#ifndef PYTHONLOCATOR_H
#define PYTHONLOCATOR_H

#include <QString>
#include <QStringList>

/* 定位一个"能真正干活"的 Python 解释器。
 *
 * 为什么不能只探测 `--version`
 * ============================
 * macOS 上从 Finder / Dock / `open App.app` 启动的 GUI 进程继承 launchd 的
 * PATH（`/usr/bin:/bin:/usr/sbin:/sbin`），其中的 `python3` 是系统自带的
 * `/usr/bin/python3` —— 能跑，但**没有 bleak / pyserial**。
 * 旧实现只验证"能不能启动"，于是选中它，随后每个 worker 都以
 * `ModuleNotFoundError` 崩溃（exit=1、stdout 为空），C++ 侧把"无输出"
 * 判成 DISCONNECTED ⇒ 界面显示设备未连接。
 * 从终端启动时 PATH 里 anaconda / homebrew 在前，一切正常 —— 这就是
 * "同一份二进制，换个启动方式就失效"的真正原因。
 *
 * 因此这里按 requiredModules 实际 import 一次做校验，并把各平台常见的
 * 绝对路径也纳入候选（GUI 启动时 PATH 里根本没有这些目录）。
 *
 * requiredModules 为空时退化为"能启动即可"（stdlib-only 的 worker 用它）。
 * 元素支持 "模块" 或 "模块:符号" 两种写法：
 *   "serial"             -> import serial
 *   "bleak:BleakAdapter" -> from bleak import BleakAdapter（校验 API 版本）
 * 结果按模块集合缓存：多个 Manager 构造时不会重复探测。
 * 若没有任何候选满足依赖，退回能启动的第一个候选并打 warning ——
 * 行为不比旧实现更差，留下的日志足以定位问题。
 *
 * macOS 分发包会把两套自包含 Python 放在
 * Contents/Resources/python/<arch>/bin/python3，本函数优先使用它们。
 */
QString findPythonInterpreter(const QStringList &requiredModules);

#endif // PYTHONLOCATOR_H
