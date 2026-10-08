#!/bin/bash
# 构建 VibeKey-F3 固件（HCPU 主工程）
# 用法: bash build_fw.sh [build|clean]
set -e

PY_ENV="/c/Users/l/.sifli/envs/default/b1ffa931b7485f8607540b6a2f6a9a53bbefe3696b9db68845be1c3c5cff1ab9/python"
GCC_BIN="/c/Users/l/.sifli/tools/arm-none-eabi-gcc/14.2.1/bin"
SDK_EXE="/c/Users/l/.sifli/tools/sdk-exe/0.1.1"
SDK="A:/OpenSiFli/SiFli-SDK/main"

export SIFLI_SDK="$SDK"
export SIFLI_SDK_PATH="$SDK"
export RTT_CC="gcc"
export RTT_EXEC_PATH="C:/Users/l/.sifli/tools/arm-none-eabi-gcc/14.2.1/bin"
export ENV_ROOT="C:\\Users\\l\\.sifli\\envs\\default\\b1ffa931b7485f8607540b6a2f6a9a53bbefe3696b9db68845be1c3c5cff1ab9\\python"
export PYTHONPATH="$SDK/tools/build"
export PATH="$PY_ENV/Scripts:$SDK_EXE:$GCC_BIN:$SDK/tools:/mingw64/bin:/usr/bin:/bin:$PATH"

cd "A:/VibeKey-F3/Firmware/project" || exit 1

CMD="$1"
if [ -z "$CMD" ]; then CMD="build"; fi

case "$CMD" in
  build)
    if command -v sdk.py >/dev/null 2>&1; then
      sdk.py build
    else
      scons -j8
    fi
    ;;
  clean)
    scons -c
    ;;
  *)
    echo "unknown cmd: $CMD"
    exit 1
    ;;
esac
