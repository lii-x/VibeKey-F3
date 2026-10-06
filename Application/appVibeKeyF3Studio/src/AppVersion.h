#pragma once
/* ============================================================
 * 上位机应用版本号 —— 唯一来源(Single Source of Truth)
 * ------------------------------------------------------------
 * 发新版流程:
 *   1) 改这里的 APP_VERSION;
 *   2) 重新构建绿色版 + 安装包(脚本从本文件自动读取版本号);
 *   3) tools/release_gitee.py --app-setup 发布到 Gitee Release。
 * C++ 侧(AppUpdater 比对基线)直接 include 本头文件;
 * deploy/build_green.ps1 / build_all.ps1 / installer.nsi 由打包脚本
 * 读取本文件生成版本串, 不再各自硬编码。
 * ============================================================ */
#define VIBEKEY_STUDIO_VERSION "1.1.5"
