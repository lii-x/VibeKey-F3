# build_all.ps1 — VibeKey-F3 Studio 一体化打包脚本（合并 build_all + build_green）
#
#   用法：
#       deploy\build.bat                                                  # 双击打包（推荐）
#       deploy\build.bat rebuild                                          # 双击 + 先重编 Release
#       pwsh -ExecutionPolicy Bypass -File deploy\build_all.ps1            # 等价于第一条
#       pwsh -ExecutionPolicy Bypass -File deploy\build_all.ps1 -Rebuild   # 等价于第二条
#
#   合并说明（10-03）：原 build_all.ps1 + build_green.ps1 合并。两者都做
#   "定位/构建 Release → 组装暂存目录 → 打包"，本脚本把重复部分
#   （MSVC / SDK / CMake / windeployqt 探测、Release 编译）抽成 helper，只留一份。
#
#   文件位置约定（10-03）：
#     deploy\build_all.ps1   本脚本
#     deploy\build.bat       双击入口
#     deploy\installer.nsi   NSIS 脚本（GBK 编码，makensis 按系统 ANSI 读）
#     deploy\*.exe           产出的安装包 + 日志
#     installer.nsi 里的 ${SRC} 保持 "deploy\VibeKey-F3-Studio_Green" 不变 ——
#     NSIS 的相对路径以 makensis 的 CWD 为基准，本脚本已 Push-Location 到项目根。
#
#   流程：
#     [1] 确保 Release exe 存在且比源码新
#           -Rebuild : 强制原地重编 build\release（CMakeCache 自愈 + 时间戳校验）
#           否则      : 先找现成的；找不到则自动用 cmake 构建到 build_green\
#     [2] 组装暂存目录 deploy\VibeKey-F3-Studio_Green\
#           exe + windeployqt 部署的 Qt 库 + 改名 + Python(含 winrt/pyserial)
#           + 6 个 worker 脚本 + VC++ 2022 x64 CRT
#     [3] makensis 打安装包（版本号从 src\AppVersion.h 单源读取）
#     收尾：删除暂存目录（绿色版不再单独分发，只留 Setup.exe）
#
#   日志：deploy\build_all_<时间戳>.log（-Rebuild 时前缀 rebuild_all_）

param(
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition   # ...\deploy
$root = Split-Path -Parent $scriptDir                               # 项目根
Push-Location $root
$logPrefix = if ($Rebuild) { "rebuild_all_" } else { "build_all_" }
$log = Join-Path $root ("deploy\" + $logPrefix + (Get-Date -Format yyyyMMdd_HHmmss) + ".log")
function Log($m) { Add-Content -Path $log -Value $m; Write-Output $m }

$outDir  = Join-Path $root "deploy\VibeKey-F3-Studio_Green"
$exeName = "appVibeKeyF3Studio.exe"
$renamed = Join-Path $outDir "VibeKey-F3 Studio.exe"

# ============================================================================
#  探测 helper（原先散落在两个脚本里，现合并为一份）
# ============================================================================
function Find-Windeployqt {
    $roots = @("D:\Qt", "C:\Qt", "${env:ProgramFiles}\Qt", "${env:ProgramFiles(x86)}\Qt")
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $f = Get-ChildItem -Path $r -Recurse -Filter "windeployqt.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($f) { return $f.FullName }
    }
    return ""
}

# MSVC：优先已知路径（本机），否则用 vswhere 动态找
function Resolve-VcVars {
    $known = "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $known) { return $known }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return "" }
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($p) { return (Join-Path $p "VC\Auxiliary\Build\vcvarsall.bat") }
    return ""
}

function Resolve-CMake {
    $known = "D:\Qt\Tools\CMake_64\bin\cmake.exe"
    if (Test-Path $known) { return $known }
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return ""
}

# Windows SDK bin（vcvarsall 因 SDK 在非标准位置不会挂其 PATH，需手动补 rc.exe/mt.exe）
function Resolve-SdkBin {
    $sdkRoot = "D:\Windows Kits\10\bin"
    if (Test-Path $sdkRoot) {
        $c = Get-ChildItem -Path $sdkRoot -Directory -Filter "10.*" -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($c) {
            $p = Join-Path $c.FullName "x64"
            if (Test-Path $p) { return $p }
        }
    }
    $fallback = "D:\Windows Kits\10\bin\10.0.22621.0\x64"
    if (Test-Path $fallback) { return $fallback }
    return ""
}

# 在 cmd 里跑：加载 vcvars -> 补 SDK -> 执行命令
# 用 cmd /v:on 延迟展开 !PATH!（执行期才展开，含 vcvarsall 加上的 nmake/cl.exe）。
# 关键修复：原先的 `set Path=%PATH%` 会在 cmd 解析整行时就展开，把 vcvarsall
# 刚加上的 nmake/cl.exe 抹掉，导致 cmake 探测 `nmake -?` 失败。
function Invoke-WithMSVC([string]$vcvars, [string]$sdkBin, [string]$cmdTail) {
    cmd /v:on /c "call `"$vcvars`" x64 && set PATH=$sdkBin;!PATH! && $cmdTail"
}

# 找现成的 Release exe（跳过 Debug —— Debug 需要 VS debug CRT，干净 PC 上跑不起来）
function Find-ReleaseExe {
    $pats = @(
        (Join-Path $root "build\*\release\$exeName"),
        (Join-Path $root "build\$exeName")
    )
    foreach ($p in $pats) {
        $m = Get-ChildItem -Path $p -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($m) { return $m.FullName }
    }
    $all = Get-ChildItem -Path (Join-Path $root "build") -Recurse -Filter $exeName -ErrorAction SilentlyContinue
    foreach ($m in $all) {
        if ($m.FullName -notmatch '[Dd]ebug') { return $m.FullName }
    }
    return ""
}

$step = 1
try {
    # ===================== [1] 确保 Release exe =====================
    Log "=== [1/3] ensure a fresh Release exe ==="
    $vcvars   = Resolve-VcVars
    $cmake    = Resolve-CMake
    $sdkBin   = Resolve-SdkBin
    $cmakeDir = if ($cmake) { Split-Path $cmake } else { "" }
    Log "    vcvars=$vcvars"
    Log "    cmake=$cmake  cmakeDir=$cmakeDir  sdkBin=$sdkBin"

    $ExePath = ""
    if ($Rebuild) {
        $relDir = Join-Path $root "build\release"
        $cache = Join-Path $relDir "CMakeCache.txt"
        $cacheFileDir = ""
        if (Test-Path $cache) {
            $cf = Select-String -Path $cache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$'
            if ($cf) { $cacheFileDir = ($cf.Matches[0].Groups[1].Value -replace '/', '\').TrimEnd('\') }
        }
        # 自愈：构建树被移动过时 CMake 会拒绝原地重配 -> 整树删掉干净重建
        if ($cacheFileDir -ne "" -and $cacheFileDir -ne $relDir.TrimEnd('\')) {
            Log "    build tree was created at $cacheFileDir (moved); wiping build/release"
            if (Test-Path $relDir) { Remove-Item -LiteralPath $relDir -Recurse -Force }
        } elseif (Test-Path $cache) {
            Log "    build tree cache OK: $cacheFileDir"
        }
        if (-not (Test-Path (Join-Path $relDir "CMakeCache.txt"))) {
            Log "    clean configure: cmake -S $root -B $relDir (Release + Qt prefix)"
            Invoke-WithMSVC $vcvars $sdkBin "cmake -S `"$root`" -B `"$relDir`" -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=`"D:/Qt/6.11.1/msvc2022_64`"" *>> $log
            if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (rc=$LASTEXITCODE)" }
        }
        Log "    compiling (a few minutes)..."
        Invoke-WithMSVC $vcvars $sdkBin "cmake --build build/release" *>> $log
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed (rc=$LASTEXITCODE)" }
        $ExePath = Join-Path $relDir $exeName
    }
    else {
        Log "    looking for an existing Release build (Debug is skipped)..."
        $ExePath = Find-ReleaseExe
    }

    if (-not $ExePath -or -not (Test-Path $ExePath)) {
        Log "    no Release exe found; building one with cmake + MSVC into build_green\ ..."
        if (-not $vcvars) { throw "MSVC (vcvarsall.bat) not found. Build the Release in Qt Creator, then re-run." }
        if (-not $cmake)  { throw "cmake not found. Install CMake or put it on PATH." }
        $buildDir = Join-Path $root "build_green"
        # 用 NMake Makefiles 生成器而非默认 VS(vcxproj)：后者在 MSBuild 合并环境变量时
        # 会因 Path/PATH 大小写冲突触发 MSB6001(cl.exe 启动失败)。NMake 不走 MSBuild。
        Invoke-WithMSVC $vcvars $sdkBin "cmake -B `"$buildDir`" -G `"NMake Makefiles`" -DCMAKE_PREFIX_PATH=`"D:/Qt/6.11.1/msvc2022_64`" -DCMAKE_BUILD_TYPE=Release `"$root`" && cmake --build `"$buildDir`"" *>> $log
        $found = Get-ChildItem -Path (Join-Path $buildDir "*\$exeName") -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $ExePath = $found.FullName }
    }
    if (-not $ExePath -or -not (Test-Path $ExePath)) {
        throw "Release exe not found. Build it in Qt Creator, then re-run."
    }
    # 时间戳校验：exe 不能比源码旧，否则"以为重编了其实没编"
    $srcT  = (Get-Item (Join-Path $root "src\WorkBuddyMonitor.cpp")).LastWriteTime
    $exeT  = (Get-Item $ExePath).LastWriteTime
    Log "    exe: $ExePath"
    Log "    exe built at $exeT / source edited at $srcT"
    if ($exeT -lt $srcT) { throw "Release exe is OLDER than source; rebuild did not pick up changes (use -Rebuild)" }

    # ===================== [2] 组装暂存目录 =====================
    $step = 2
    Log "=== [2/3] assemble installer content (staging dir, not a deliverable) ==="

    Log "  Locating Qt (windeployqt)..."
    $windeploy = Find-Windeployqt
    if ($windeploy -eq "") { throw "windeployqt.exe not found. Install Qt6 (msvc2022_64)." }
    Log "      $windeploy"

    # Remove-Item 在文件被占用时会 fail-closed 抛错；退化为"原地覆盖"而非中断
    if (Test-Path $outDir) {
        try { Remove-Item $outDir -Recurse -Force -ErrorAction Stop }
        catch { Log "      (warn: could not remove existing green dir; overwriting in place: $_)" }
    }
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null

    Log "  Copying exe and deploying Qt libraries (windeployqt)..."
    Copy-Item $ExePath -Destination $outDir

    # 跑 windeployqt 并抓住它的 stderr。
    # 为什么要抓：windeployqt 有两种完全不同的失败模式，**报错文本几乎一样**
    # （都是"qwindows.dll 没出来"），但处置方式相反：
    #   (a) "Unable to query qtpaths"  → windeployqt 连自己的伴生工具都启动不了。
    #       它内部用 QProcess 拉 qtpaths.exe，QProcess 必须建匿名管道；若这一步
    #       拿到 ERROR_NO_SYSTEM_RESOURCES(1450)，报的就是
    #       "QProcess: CreateFile failed. (所有的管道范例都在使用中。)"。
    #       这是**环境问题**（系统句柄/非分页池耗尽、被安全软件拦截），
    #       跟 Qt 安装、跟本项目代码都无关 —— 重启或关掉一批程序 often 就好了。
    #   (b) qtpaths 查询成功、但插件没部署 → windeployqt 静默跳过，
    #       重跑一次通常能补上（下面就是这么做的）。
    # 不区分就只会抛一句"deployment is broken"，把环境问题误导成安装包问题。
    function Invoke-Windeploy {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"   # 原生 stderr 在 EAP=Stop 下会变成终止性错误
        $cap = & $windeploy --release --qmldir (Join-Path $root "qml") $outDir 2>&1
        $code = $LASTEXITCODE
        $ErrorActionPreference = $prev
        return [pscustomobject]@{ Exit = $code; Text = (($cap | Out-String)) }
    }

    $wdq = Invoke-Windeploy
    if ($wdq.Exit -ne 0) {
        Log "      (windeployqt exit=$($wdq.Exit) - verifying below)"
    }
    # 关键校验：没有 platforms\qwindows.dll，应用会以
    # "This application failed to start because no Qt platform plugin could be
    # initialized" 启动即崩。windeployqt 在某些环境会静默跳过插件部署，
    # 所以这里大声失败，而不是发一个装完跑不起来的包。
    $qwindows = Join-Path $outDir "platforms\qwindows.dll"
    if (-not (Test-Path $qwindows)) {
        # windeployqt 自己没把 qtpaths 拉起来 —— 重试没有意义，先把归因分清。
        # 实测（不要靠猜）：直接跑一次 qtpaths。它能跑 => Qt 安装是好的，
        # 那失败就在 windeployqt 内部的 QProcess 建管道这一步 => 环境问题。
        # 它跑不了 => 才是 Qt 安装真的坏了。
        if ($wdq.Text -match "qtpaths") {
            Log "---- windeployqt stderr ----"
            foreach ($l in ($wdq.Text -split "`r?`n")) { if ($l.Trim()) { Log "  | $l" } }
            $qtpathsExe = Join-Path (Split-Path $windeploy) "qtpaths.exe"
            $probe = ""
            $probeOk = $false
            $prev2 = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                if (Test-Path $qtpathsExe) {
                    $r = & $qtpathsExe --qt-version 2>&1
                    $probe = (($r | Out-String)).Trim()
                    $probeOk = ($LASTEXITCODE -eq 0 -and $probe -match "^\d+\.\d+")
                } else { $probe = "qtpaths.exe not found next to windeployqt" }
            } catch { $probe = "threw: $_" }
            $ErrorActionPreference = $prev2
            Log "      self-test: qtpaths.exe --qt-version -> '$probe' (ok=$probeOk)"
            if ($probeOk) {
                throw ("windeployqt failed to start its own helper even though qtpaths.exe runs fine on its own" +
                       " ('$probe'). So the Qt install is GOOD and the fault is inside windeployqt's QProcess," +
                       " which needs an anonymous pipe to capture the child's output - that CreateFile is being" +
                       " refused with ERROR_NO_SYSTEM_RESOURCES (1450, '所有的管道范例都在使用中')." +
                       " This is a HOST/environment problem (system handle / nonpaged-pool exhaustion, or endpoint" +
                       " security blocking child-process creation) - NOT a broken Qt install, NOT this project." +
                       " Retry later, or reboot / close many apps first. No installer was produced.")
            }
            throw ("windeployqt cannot query its Qt installation, and qtpaths.exe itself is not usable either" +
                   " ('$probe'). The Qt install at $(Split-Path $windeploy) looks incomplete or damaged" +
                   " - run 'windeployqt --version' in a plain cmd window to see the underlying error.")
        }
        # qtpaths 查询成功、只是插件没部署出来 -> 重跑一次通常能补上
        Log "      (qwindows.dll missing after first pass - retrying windeployqt)..."
        $wdq2 = Invoke-Windeploy
        if (-not (Test-Path $qwindows)) {
            Log "---- windeployqt stderr (retry) ----"
            foreach ($l in ($wdq2.Text -split "`r?`n")) { if ($l.Trim()) { Log "  | $l" } }
            throw "platforms\qwindows.dll still missing after windeployqt. Deployment is broken; aborting."
        }
    }
    Log "      Qt platform plugin present: platforms\qwindows.dll"

    # 改名成产品显示名，让绿色版目录干净
    if (Test-Path (Join-Path $outDir $exeName)) {
        if (Test-Path $renamed) {
            # 目标可能短暂被旧实例或杀毒扫描占用；删除失败就结束占用进程重试几次，
            # 绝不让改名冲突中断整个打包（真正影响功能的是下面装的 Python BLE 依赖）。
            for ($i = 0; $i -lt 5; $i++) {
                try { Remove-Item $renamed -Force -ErrorAction Stop; break }
                catch {
                    & taskkill /F /IM "VibeKey-F3 Studio.exe" 2>$null
                    Start-Sleep -Milliseconds 400
                }
            }
        }
        try {
            Move-Item (Join-Path $outDir $exeName) $renamed -Force -ErrorAction Stop
            Log "      Renamed to: VibeKey-F3 Studio.exe"
        } catch {
            Log "      (warn: rename to product name failed ($_); keeping $exeName)"
        }
    }

    Log "  Bundling Python..."
    # 优先用托管的 64 位 3.13.12（已带 pyserial + winrt），再试常见 64 位安装。
    # 应用是 x64，打包的 Python 必须 64 位：拒绝 32 位解释器（PATH 上偶尔有），
    # 因为 winrt-* wheel 只有 64 位。
    $py = ""
    $cands = @(
        (Join-Path $env:USERPROFILE ".workbuddy\binaries\python\versions\3.13.12\python.exe"),
        "C:\Python313\python.exe",
        "C:\Python312\python.exe",
        "python",
        "python3"
    )
    foreach ($c in $cands) {
        try {
            $info = & $c -c "import sys; print(sys.executable); print('64' if sys.maxsize > 2**32 else '32')" 2>$null
            if ($LASTEXITCODE -ne 0 -or $info.Count -lt 2) { continue }
            if ($info[1] -eq '64') { $py = $c; break }
            Log "      (skipped 32-bit python: $($info[0]))"
        } catch {}
    }
    if ($py -eq "") { throw "No 64-bit Python found. Install Python 3.x, or copy a python/ folder next to the exe manually." }
    $pyExe = & $py -c "import sys; print(sys.executable)"
    $pyDir = Split-Path -Parent $pyExe
    $destPy = Join-Path $outDir "python"
    if (-not (Test-Path $destPy)) { New-Item -ItemType Directory -Path $destPy | Out-Null }
    Log "      Copying $pyDir -> $destPy (excluding docs/tests)..."
    robocopy $pyDir $destPy /E /XD Lib\test Lib\tkinter idlelib Tools Doc tcl include /XF *.pyc 2>$null | Out-Null
    $bundledPy = Join-Path $destPy "python.exe"
    if (-not (Test-Path $bundledPy)) { throw "Bundled python.exe not found after copy. Check the source Python install." }
    # 总是(重新)安装 BLE 依赖。pip 幂等（wheel 有缓存，已满足时几乎瞬间完成），
    # 这样能保证绿色版自包含 —— 无论源 Python 是否恰好带了这些包。
    # 曾有过"已存在就跳过"的捷径，结果发出去一个坏包：捆绑副本来自一个裸解释器
    # (没有 winrt/serial)，而旧副本里还留着它们，校验通过就跳过了 pip。
    # 目标机上 BLE worker 于是 ModuleNotFoundError（开发机正常是因为系统 Python 有）。
    # winrt 被拆成很多子包，worker 需要全部（winrt.windows.foundation 提供
    # IAsyncOperation，几乎每个异步 BLE 调用都要用）。
    $winrtVer = "3.2.1"
    Log "      Installing/ensuring pyserial + winrt (full BLE chain, pinned $winrtVer)..."
    # pip 会往 stderr 打一句无害的 "scripts not on PATH" WARNING。EAP=Stop 下这会变成
    # 终止性 RemoteException，即使 pip 成功也会中断整个打包。这里从源头消掉该警告
    # (--no-warn-script-location) 并放宽 EAP，只有真正的非零退出码才中止。
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $pipOut = & $bundledPy -m pip install --upgrade --no-warn-script-location pyserial `
        "winrt-runtime==$winrtVer" `
        "winrt-windows-foundation==$winrtVer" `
        "winrt-windows-foundation-collections==$winrtVer" `
        "winrt-windows-foundation-numerics==$winrtVer" `
        "winrt-windows-applicationmodel==$winrtVer" `
        "winrt-windows-applicationmodel-activation==$winrtVer" `
        "winrt-windows-devices-enumeration==$winrtVer" `
        "winrt-windows-security-cryptography==$winrtVer" `
        "winrt-windows-data-json==$winrtVer" `
        "winrt-windows-devices-bluetooth==$winrtVer" `
        "winrt-windows-devices-bluetooth-genericattributeprofile==$winrtVer" `
        "winrt-windows-storage-streams==$winrtVer" 2>&1
    $ErrorActionPreference = $prevEAP
    if ($LASTEXITCODE -ne 0) {
        throw "pip install of BLE deps failed: $pipOut`nThe package would crash on 'ble_led_worker.py serve' (ModuleNotFoundError: winrt). Aborting."
    }
    # 最终硬校验：BLE worker 必须能 import，否则包是坏的
    $finalCheck = & $bundledPy -c "import serial, winrt.windows.foundation, winrt.windows.devices.bluetooth, winrt.windows.devices.bluetooth.genericattributeprofile, winrt.windows.storage.streams; print('OK')" 2>$null
    if ($finalCheck -notmatch 'OK') { throw "Bundled python still cannot import the winrt BLE chain. Aborting." }
    Log "      Bundled python BLE deps verified OK."

    Log "  Copying worker scripts..."
    foreach ($f in @("ota_worker.py", "config_worker.py", "workbuddy_status.py", "ble_led_worker.py", "ble_config_worker.py", "net_worker.py")) {
        Copy-Item (Join-Path $root "workers\$f") -Destination $outDir
    }

    Log "  Bundling VC++ 2022 x64 runtime (so it runs on a clean PC without VS2022)..."
    # windeployqt 的 --compiler-runtime 在某些环境不可靠（实测静默什么都没做），
    # 所以直接从 MSVC redist 显式拷 CRT DLL。
    # 这些是 Qt6*.dll 和 python.exe 的硬依赖；缺了应用会以
    # 'no Qt platform plugin could be initialized' 启动即崩。
    $vcCrt = ""
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($vsPath) {
            $c = Get-ChildItem -Path (Join-Path $vsPath "VC\Redist\MSVC") -Recurse -Filter "vcruntime140.dll" -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC143\.CRT\\' }
            if ($c) { $vcCrt = $c[0].DirectoryName }
        }
    }
    if ($vcCrt -and (Test-Path (Join-Path $vcCrt "vcruntime140.dll"))) {
        foreach ($dll in @("concrt140.dll","msvcp140.dll","msvcp140_1.dll","msvcp140_2.dll","msvcp140_atomic_wait.dll","msvcp140_codecvt_ids.dll","vccorlib140.dll","vcruntime140.dll","vcruntime140_1.dll","vcruntime140_threads.dll")) {
            $srcDll = Join-Path $vcCrt $dll
            if (Test-Path $srcDll) { Copy-Item $srcDll -Destination $outDir }
        }
        Log "      Copied VC++ 2022 x64 CRT DLLs (portable on clean PCs)."
    } else {
        Write-Warning "VC++ 2022 x64 CRT not found via vswhere; the build may fail to start on PCs without the VS2022 runtime."
        $vcrCands = @((Join-Path $root "vc_redist.x64.exe"), (Join-Path $root "deploy\vc_redist.x64.exe"), "D:\vc_redist.x64.exe")
        $vcr = $vcrCands | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($vcr) { Copy-Item $vcr -Destination $outDir; Log "      (also copied vc_redist.x64.exe for manual install)" }
    }
    Log "      Staging ready: $outDir"

    # ===================== [3] makensis 打包 =====================
    $step = 3
    Log "=== [3/3] build NSIS installer (version from src\AppVersion.h) ==="
    # 杀残留 makensis（用原生 cmdlet，避免 taskkill 找不到进程时 stderr+非零退出码
    # 在 EAP=Stop 下被转成终止错误）
    Get-Process -Name makensis -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $verLine = Select-String -Path (Join-Path $root "src\AppVersion.h") -Pattern '#define VIBEKEY_STUDIO_VERSION "([^"]+)"'
    if (-not $verLine -or -not $verLine.Matches[0].Groups[1]) { throw "AppVersion.h 里找不到 VIBEKEY_STUDIO_VERSION" }
    $ver = $verLine.Matches[0].Groups[1].Value
    $out = Join-Path $root "deploy\VibeKey-F3_Studio_Setup_v$ver.exe"
    # installer.nsi 放在 deploy\ （与本脚本同目录）。
    #
    # ⚠️⚠️ makensis 的路径基准 —— 实测定论（10-03；三轮实测才搞清，勿再改）：
    #    makensis **默认会把 CWD 切换到脚本自身所在目录**，然后按它解析
    #    File / !system 的相对路径。对照实测（脚本在 deploy\，数据在 根\data）：
    #      不带 /NOCD，CWD=根，File "data\payload.txt"     -> no files found（按 deploy\ 找）
    #      带   /NOCD，CWD=根，File "data\payload.txt"     -> 成功（按 CWD=根 找）
    #      带   /NOCD，CWD=别处                            -> no files found
    #    ⇒ 想要"相对项目根"的路径，必须**同时**满足两点：
    #       ① 传 /NOCD（否则 makensis 强制切到脚本目录）
    #       ② 让 makensis 的 CWD == 项目根
    #
    # ⚠️ ② 不能靠 cd / Push-Location：那只改 PowerShell 自身位置，
    #    子进程继承的是 [Environment]::CurrentDirectory（= 脚本启动时的 CWD）。
    #    实测 Push-Location 到项目根后，[Environment]::CurrentDirectory 仍是
    #    A:\VibeKey-F3\Firmware ⇒ 从 Firmware 目录调用本脚本必然失败。
    #    正解是 Start-Process -WorkingDirectory（唯一可靠手段）。
    $nsi = Join-Path $root "deploy\installer.nsi"
    if (-not (Test-Path $nsi)) { throw "installer.nsi not found: $nsi" }
    # 打包前先自检暂存目录，否则报错会指向 NSIS 而不是真正原因
    if (-not (Test-Path (Join-Path $outDir "platforms\qwindows.dll"))) {
        throw "staging dir is incomplete (no platforms\qwindows.dll): $outDir"
    }
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $mk = Get-Command makensis.exe -ErrorAction SilentlyContinue
    $mkExe = if ($mk) { $mk.Source } else { "D:\NSIS\Bin\makensis.exe" }
    if (-not (Test-Path $mkExe)) {
        $ErrorActionPreference = $prevEAP
        throw "makensis.exe not found (looked on PATH and at D:\NSIS\Bin\makensis.exe)"
    }
    # 必须用 Start-Process -WorkingDirectory，**不能**用 call operator 加 -WorkingDirectory
    # （后者是 makensis 自己的参数，PowerShell 不会拦；实测它也能过，但那是碰巧 ——
    #  一旦 NSIS 将来解析到同名参数就会静默失效）。
    # 也不能靠 `cd`/Push-Location：那只改 PowerShell 自己的位置，子进程继承的是
    # [Environment]::CurrentDirectory（= 脚本启动时的 CWD），改不了。
    # /NOCD 必带：否则 makensis 强制把 CWD 切到 installer.nsi 所在目录(deploy\)，
    # ${SRC}="deploy\..." 就会变成 deploy\deploy\... -> "no files found"。
    # 配 /NOCD + -WorkingDirectory $root 才是"相对项目根解析"。
    $mkLog = Join-Path $scriptDir "makensis_build.log"
    $mkArgs = @("/NOCD", "/DVERSION=$ver", "/DOUTFILE=$out", $nsi)
    $mkProc = Start-Process -FilePath $mkExe -ArgumentList $mkArgs `
                           -WorkingDirectory $root -NoNewWindow -Wait -PassThru `
                           -RedirectStandardOutput $mkLog `
                           -RedirectStandardError  "$mkLog.err"
    $mkExit = $mkProc.ExitCode
    $ErrorActionPreference = $prevEAP
    # makensis 输出重定向到了独立文件，拼进主日志便于 build.bat 的 tail 展示
    foreach ($f in @($mkLog, "$mkLog.err")) {
        if (Test-Path $f) {
            $body = Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue
            if ($body) { Add-Content -Path $log -Value $body }
            Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
        }
    }
    if ($mkExit -ne 0) { throw "makensis failed (rc=$mkExit) - see $log" }


    if (-not (Test-Path $out)) { throw "installer not created: $out" }

    # 打完即删暂存目录（绿色版不再单独分发）
    if (Test-Path $outDir) {
        try {
            Remove-Item -LiteralPath $outDir -Recurse -Force -ErrorAction Stop
            Log "      cleaned staging dir (only Setup exe kept)"
        } catch {
            Log "      (warn: staging dir still in use, left in place: $_)"
        }
    }
    Log "=== ALL DONE: $out ($([math]::Round((Get-Item $out).Length/1MB,1)) MB) ==="
} catch {
    Log "=== FAILED at step $step : $_ ==="
    exit 1
}
