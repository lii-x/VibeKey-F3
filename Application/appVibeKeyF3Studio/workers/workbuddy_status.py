#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读取 WorkBuddy 运行时状态，输出一行 JSON 供 Qt 端（WorkBuddyMonitor）消费。

状态来源（"完成才空闲"范式）：
  busy  = 进程在跑 且 (会话在 planning/working/刚回完缓冲期 或 自动化任务近期在更新)；
          用状态文件记住"最近一次 busy 时刻"，只要保持期(HOLD_MS=15s)内报过 busy 且进程
          仍在跑，就继续 busy——容忍 DB 静默间隙(思考/等待/planning 记录未建)，绝不闪 idle。
          "明确完成"优先：最近会话 status 已转 completed 且超过 COMPLETE_GRACE_MS=1.5s 缓冲，
          直接空闲，不依赖保持期，确保"任务完成"弹窗出现后很快灭灯。
  await = 弹出审批/可选项对话框等待用户输入（09-10 新增）：会话 status=="planning"，
          或存在带时间戳的哨兵文件 wb_awaiting.json（代理弹 AskUserQuestion 时写入）。
          优先于 busy/idle/error，上位机据此黄灯闪烁。
  alert = 高风险命令审批弹窗（10-03 新增）：WorkBuddy 弹出"检测到批量删除操作/高风险命令"
          这类确认框时，**会话 status 仍是 working**（实测，见下），planning 与 wb_awaiting.json
          都抓不到，故只能用工作副产物反查。优先于所有其它状态，上位机据此红灯闪烁。
  "完成"信号：会话 status 转 completed 且超过 1.5s 缓冲 -> 空闲；自动化任务 updated_at 停更
          超过 15s -> 空闲。不再用 automation_runtime_state.running 标志（会卡死在 1）。
  error = 最近一次 automation_runs 失败(result_success=0) 且距今 < 3 分钟。
  idle  = 其他（进程不在 / 明确完成 / 超过保持期无任何活动）。

会话生命周期实测：planning(等待/规划) -> working(生成) -> completed(回完，时间戳刷新为完成时刻)。

10-03 实测（高风险审批为何抓不到）：
  在 AskUserQuestion 弹窗期间以 1s 间隔采样 110s，session.status **恒为 working**，
  wb_awaiting.json 从未出现。审批状态走 A2A IPC 消息 codebuddy.tool_permission，
  只在进程内传递、**不落盘**（audit-log 里也没有 approval 事件），外部无法直接订阅。
  唯一可观测的落地产物：审批期间 WorkBuddy 会在 %TEMP%/codebuddy-safe-delete/ 下写
  report-<pid>-<ts>-<rand>.jsonl（源码 ensureSafeDeleteReportPath()，仅 bash/node/powershell/python
  运行时启用），弹窗结束后目录被清空。

注意：被 Qt 的 QProcess 拉起时 stdout 默认是 GBK，任务名里可能含 emoji/中文，
因此这里强制 UTF-8 输出，并兜底用 ensure_ascii=True，保证 worker 永不崩溃。
"""
import sys
import os
import sqlite3
import subprocess
import time
import datetime
import json
import csv
import io

DB_PATH = os.path.expandvars(r"%USERPROFILE%\.workbuddy\workbuddy.db")


def _reconfigure_stdio():
    """强制 stdout/stderr 为 UTF-8，避免 Windows 下 GBK 编码崩溃。"""
    for s in (sys.stdout, sys.stderr):
        try:
            if hasattr(s, "reconfigure"):
                s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def _emit(obj):
    """输出一行 JSON；任何编码异常都兜底为 ascii，绝不让 worker 崩溃。"""
    text = json.dumps(obj, ensure_ascii=False)
    try:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()
    except Exception:
        sys.stdout.write(json.dumps(obj, ensure_ascii=True) + "\n")
        sys.stdout.flush()


def _now_ms():
    return int(time.time() * 1000)


# ---- 高风险命令审批检测（10-03 新增）--------------------------------------
# WorkBuddy 弹出"检测到批量删除操作 / 高风险命令"确认框时，会在
#   %TEMP%\codebuddy-safe-delete\report-<pid>-<ts>-<rand>.jsonl
# 写审计报告；**弹窗结束后该目录被清空**（源码 ensureSafeDeleteReportPath()）。
# 这是外部唯一可观测的落地信号 —— 审批状态本身走 A2A IPC，不落盘。
SAFEDEL_DIR = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"),
                          "codebuddy-safe-delete")
# ⚠️ 判据是"**文件存在**"，不是"文件新鲜"（10-03 实测踩过的坑）：
#   最初用 mtime < 8s 判定，实测 3s 轮询时 alert 只亮了几秒就消失 ——
#   因为审批期间 WorkBuddy 并**不持续追加**报告，mtime 停在创建那一刻。
#   既然弹窗结束 WorkBuddy 会清空整个目录，"存在即审批中"才是准确语义，
#   也天然免疫轮询间隔。mtime 只保留给 detail 显示，不再作判据。
SAFEDEL_MAX_AGE_MS = 30 * 60 * 1000   # 仅防"目录被漏删"的极端残留，30 分钟


def _high_risk_approval_pending():
    """%TEMP%/codebuddy-safe-delete/ 下是否有 report-*.jsonl（= 审批弹窗中）。

    返回 (pending: bool, detail: str)。任何异常都返回 (False, "")，
    绝不让检测失败影响状态输出。
    """
    try:
        if not os.path.isdir(SAFEDEL_DIR):
            return False, ""
        now = _now_ms()
        newest = 0
        name = ""
        try:
            entries = os.scandir(SAFEDEL_DIR)
        except Exception:
            return False, ""
        with entries as it:
            for e in it:
                if not e.is_file():
                    continue
                if not e.name.startswith("report-"):
                    continue
                try:
                    mt = e.stat().st_mtime
                except Exception:
                    continue
                if mt > newest:
                    newest, name = mt, e.name
        if name and (now - int(newest * 1000)) < SAFEDEL_MAX_AGE_MS:
            return True, name
    except Exception:
        pass
    return False, ""


def _rel_time(ms):
    if not ms:
        return ""
    secs = (_now_ms() - ms) / 1000.0
    if secs < 0:
        secs = 0
    if secs < 60:
        return "刚刚"
    if secs < 3600:
        return "%d分钟前" % int(secs / 60)
    if secs < 86400:
        return "%d小时前" % int(secs / 3600)
    return datetime.datetime.fromtimestamp(ms / 1000).strftime("%m-%d %H:%M")


def _workbuddy_proc_alive():
    """WorkBuddy.exe 进程是否在运行。"""
    try:
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq WorkBuddy.exe", "/NH"],
            capture_output=True, text=True, timeout=5, errors="ignore",
        )
        return "WorkBuddy.exe" in r.stdout
    except Exception:
        return False


def _ms(v):
    """把可能是秒/毫秒的时间戳归一化为毫秒（created_at/updated_at 均为毫秒）。"""
    if not v:
        return 0
    return int(v) if v > 1e12 else int(v) * 1000


# 状态保持：记录最近一次判定为 busy 的时间戳，用于容忍 DB 静默间隙（思考/等待/
# planning 记录未建等），避免在这些瞬间闪 idle。只有明确的"完成"信号才清空。
STATE_FILE = os.path.expandvars(r"%USERPROFILE%\.workbuddy\wb_monitor_state.json")
HOLD_MS = 15 * 1000             # busy 状态保持时长，覆盖瞬时静默 / planning 记录未建
COMPLETE_GRACE_MS = 1500        # 回完(转 completed)后仅亮 1.5s 缓冲，超过即视为明确完成


def _load_state():
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _save_state(obj):
    try:
        with open(STATE_FILE, "w", encoding="utf-8") as f:
            json.dump(obj, f)
    except Exception:
        pass


def _load_last_busy():
    return float(_load_state().get("last_busy_ts", 0))


def _save_last_busy(ts):
    st = _load_state()
    st["last_busy_ts"] = ts
    _save_state(st)


def _load_last_alert():
    """上次检测到高风险审批的时刻（ms）。用于 1.2s 迟滞，防红灯闪断。"""
    return float(_load_state().get("last_alert_ts", 0))


def _save_last_alert(ts):
    st = _load_state()
    st["last_alert_ts"] = ts
    _save_state(st)


def read_state():
    result = {
        "status": "idle",
        "task": None,
        "active": False,
        "alert": False,
        "alertDetail": "",
        "recent": [],
        "todayRuns": 0,
        "todaySuccess": 0,
        "todayFail": 0,
        "lastError": None,
    }
    if not os.path.exists(DB_PATH):
        _emit(result)
        return

    try:
        con = sqlite3.connect(DB_PATH)
        cur = con.cursor()

        # 1) 最近运行（用于 error 判定 / 最近任务 / 今日统计）
        #    最近任务只取 7 天内，避免把几个月前的旧记录仍显示为"最近"。
        #    created_at/updated_at 在 DB 中为毫秒戳，cutoff 用毫秒比较。
        recent_cutoff_ms = _now_ms() - 7 * 24 * 60 * 60 * 1000
        cur.execute(
            "SELECT thread_title, result_success, status, created_at, updated_at "
            "FROM automation_runs "
            "WHERE created_at >= ? OR updated_at >= ? "
            "ORDER BY max(created_at, updated_at) DESC LIMIT 5",
            (recent_cutoff_ms, recent_cutoff_ms),
        )
        rows = cur.fetchall()
        recent = []
        last = rows[0] if rows else None
        for t in rows:
            recent.append({
                "name": t[0] if t[0] else "(未命名任务)",
                "success": bool(t[1]),
                "timeText": _rel_time(_ms(t[4]) or _ms(t[3])),
            })

        # 今日统计（created_at 为毫秒戳）
        today_start = int(
            datetime.datetime.combine(datetime.date.today(), datetime.time.min).timestamp()
            * 1000
        )
        cur.execute(
            "SELECT count(*), "
            "sum(CASE WHEN result_success = 1 THEN 1 ELSE 0 END), "
            "sum(CASE WHEN result_success = 0 THEN 1 ELSE 0 END) "
            "FROM automation_runs WHERE created_at >= ?",
            (today_start,),
        )
        c = cur.fetchone()
        todayRuns = c[0] or 0
        todaySuccess = c[1] or 0
        todayFail = c[2] or 0

        last_error = None
        cur.execute(
            "SELECT last_error FROM automation_runtime_state "
            "WHERE last_error IS NOT NULL AND last_error != '' LIMIT 1"
        )
        le = cur.fetchone()
        if le:
            last_error = le[0]

        # 活跃会话（交互式对话 / 等待模型响应）——"完成才空闲"范式：
        #   实测会话生命周期 status：planning(等待/规划) -> working(生成) -> completed(回完，
        #   且 updated_at/last_activity_at 刷新为完成时刻)。判定只认"是否已完成"：
        #     - planning / working（任何年龄）：一律 busy（即"任务在进行中"）；
        #     - completed 且 (now - la) < COMPLETE_GRACE_MS：刚回完的缓冲期，仍 busy；
        #     - completed 且已超过缓冲：明确完成 -> 空闲（这就是"只有完成才空闲"）。
        #   不靠时间窗口限制 planning/working，避免长任务思考间隙被误判空闲；真正的真空闲
        #   （无对话，只有陈旧 completed）自然落到"超过缓冲"分支 -> 空闲。
        DONE_STATES = ("completed", "terminated", "Terminated")
        session_title = None
        session_active = False
        last_session_status = None   # 传递给判定段，用于"明确完成"优先判断
        last_session_la = 0
        try:
            cur.execute(
                "SELECT title, status, updated_at, last_activity_at FROM sessions "
                "ORDER BY max(updated_at, last_activity_at) DESC LIMIT 1"
            )
            srow = cur.fetchone()
            if srow:
                now = _now_ms()
                st = srow[1]
                la = _ms(srow[3]) if srow[3] else 0
                last_session_status = st
                last_session_la = la
                if st not in DONE_STATES:
                    session_active = True          # planning / working -> 进行中
                elif la and (now - la) < COMPLETE_GRACE_MS:
                    session_active = True          # 刚回完的缓冲期
                if session_active:
                    session_title = srow[0] if srow[0] else None
        except Exception:
            session_active = False

        con.close()

        # 3) 状态判定——"完成才空闲" + 状态保持
        #    实时 busy 证据（进程必须在跑）：
        #      ① 会话 planning/working 或 刚回完缓冲期内；
        #      ② 自动化任务进行中：最近 automation_runs.updated_at 近 AUTO_FRESH_MS 有更新。
        #    注意：不再用 automation_runtime_state.running 标志单独决定 busy——该标志会卡死
        #    在 1，导致无实际活动时也长亮（之前的"假忙碌"根因）。
        #    明确完成优先：若最近会话已转 completed 且超过 COMPLETE_GRACE_MS（1.5s）缓冲，
        #    直接 idle，不再用状态保持，确保"任务完成"弹窗出现后不久灯即熄灭。
        #    保持逻辑：仅用于非明确完成场景（planning 记录未建、思考/等待静默间隙），只要
        #    近期(HOLD_MS=15s 内)报过 busy 且进程仍在跑，就继续 busy，绝不闪 idle。
        proc_alive = _workbuddy_proc_alive()
        AUTO_FRESH_MS = 15 * 1000
        recent_run_fresh = False
        if last:
            ru = _ms(last[4]) or _ms(last[3])
            recent_run_fresh = (_now_ms() - ru) < AUTO_FRESH_MS

        now_busy = proc_alive and (session_active or recent_run_fresh)

        now = _now_ms()
        # "明确完成": 最近会话是 completed/terminated 且已非"刚完成"(>1.5s)
        completed_done = (
            last_session_status in DONE_STATES
            and last_session_la
            and (now - last_session_la) > COMPLETE_GRACE_MS
        )

        last_busy = _load_last_busy()
        if now_busy:
            busy = True              # 实时有活动(会话进行中/自动化运行) -> 忙
            _save_last_busy(now)
        elif completed_done:
            busy = False             # 明确完成 -> 立刻空闲
        elif proc_alive and (now - last_busy) < HOLD_MS:
            busy = True              # 保持期内的静默间隙，继续忙
        else:
            busy = False

        task_name = None
        if busy:
            status = "busy"
            if session_active and session_title:
                task_name = session_title
            elif last:
                task_name = last[0] if last[0] else None
        elif last and last[1] == 0 and (_now_ms() - _ms(last[3])) < 3 * 60 * 1000:
            status = "error"
            task_name = last[0] if last else None
        else:
            status = "idle"

        # 4) 审批/可选项等待态（"await"）——优先于上述 busy/idle/error 判定。
        #    触发信号（任一满足即视为「WorkBuddy 正在等待用户输入」）：
        #      a) 会话 status == "planning"：宿主在等待/规划（等价「等待用户」）时写入；
        #      b) 哨兵文件 wb_awaiting.json：代理弹出 AskUserQuestion 等审批/可选项
        #         对话框时写入，含 {ts, prompt}；陈旧(>AWAIT_TIMEOUT_MS)忽略，防滞留。
        #    上位机据此显示黄灯闪烁，区别于 working 的红呼吸。
        AWAIT_TIMEOUT_MS = 120 * 1000
        await_prompt = None
        is_await = (last_session_status == "planning")
        if not is_await:
            sentinel = os.path.expandvars(r"%USERPROFILE%\.workbuddy\wb_awaiting.json")
            if os.path.exists(sentinel):
                try:
                    with open(sentinel, "r", encoding="utf-8") as f:
                        sj = json.load(f)
                    if (_now_ms() - int(sj.get("ts", 0))) < AWAIT_TIMEOUT_MS:
                        is_await = True
                        await_prompt = sj.get("prompt") or None
                except Exception:
                    pass
        if is_await:
            status = "await"
            if await_prompt:
                task_name = await_prompt

        # 5) 高风险命令审批（"alert"）——10-03 新增，优先级最高。
        #    "检测到批量删除操作 / 高风险命令"这类弹窗期间 status 恒为 working，
        #    planning 与 wb_awaiting.json 都抓不到（实测 110s 采样确认），
        #    唯一外部信号是 %TEMP%\codebuddy-safe-delete\ 下的新鲜 report-*.jsonl。
        #    加 1.2s 迟滞：弹窗刚出现/刚消失的那一轮不立即翻转，避免红灯闪断。
        ALERT_LINGER_MS = 1200
        alert_hit, alert_detail = _high_risk_approval_pending()
        last_alert = _load_last_alert()
        if alert_hit:
            _save_last_alert(_now_ms())
            is_alert = True
        else:
            is_alert = (_now_ms() - last_alert) < ALERT_LINGER_MS
        if is_alert:
            status = "alert"
            task_name = "等待确认：高风险命令" if alert_hit else task_name

        result.update({
            "status": status,
            "task": task_name,
            "active": bool(busy) or is_await or is_alert,
            "alert": bool(is_alert),
            "alertDetail": alert_detail if alert_hit else "",
            "recent": recent,
            "todayRuns": todayRuns,
            "todaySuccess": todaySuccess,
            "todayFail": todayFail,
            "lastError": last_error,
        })
    except Exception as e:
        # 任何 DB 异常都不应让 worker 崩溃；回退默认 idle 并带上错误提示
        result["lastError"] = "db_error: %s" % e

    _emit(result)


if __name__ == "__main__":
    _reconfigure_stdio()
    read_state()
