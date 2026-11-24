cmake_minimum_required(VERSION 3.16)

# macOS 构建基线（可用 -D 覆盖）：
#   - 官方 Qt 6.8.x 的 macOS 包是 universal2(x86_64 + arm64)，minos 12.0；
#   - 本应用最低支持 macOS 14.0（官方 Qt 12.0 基线之上，覆盖 14/15）；
#   - 默认同时构建 Intel 与 Apple Silicon，产物为 universal2。
# 注意：这两项必须在 project() 之前设置，否则 CMake 已按默认值生成编译参数。
if(APPLE)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
    endif()
endif()

project(VibeKeyF3Studio VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 版本号唯一来源: src/AppVersion.h（与 Windows 打包脚本一致，避免 bundle 元数据漂移）
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/AppVersion.h" _ver_line
     REGEX "#define VIBEKEY_STUDIO_VERSION")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ver_match "${_ver_line}")
set(APP_VERSION "${CMAKE_MATCH_1}")
if(NOT APP_VERSION)
    message(WARNING "AppVersion.h 中未解析到 VIBEKEY_STUDIO_VERSION，回退到 ${PROJECT_VERSION}")
    set(APP_VERSION "${PROJECT_VERSION}")
endif()

# 生成 app_icon.rc：用绝对路径引用 ico，避免 rc.exe 路径解析问题（仅 Windows 使用）
if(WIN32)
    set(APPICON_ABSPATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.ico")
    configure_file(resources/app_icon.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc @ONLY)
endif()

find_package(Qt6 REQUIRED COMPONENTS Quick Widgets SerialPort QuickControls2)

# macOS 26 SDK 已移除 AGL.framework（系统里只剩空壳，没有二进制）。
# 官方 Qt 6.8 的 FindWrapOpenGL.cmake 仍会无条件追加 "-framework AGL"，
# 导致链接报 "framework 'AGL' not found"。Qt 已经链接 OpenGL.framework，
# AGL 是历史遗留项；从 WrapOpenGL 的接口里去掉即可。
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_vibekey_wrapopengl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_vibekey_wrapopengl_libs)
        # find_library() 可能把 /System/Library/Frameworks/AGL.framework
        # 作为绝对路径塞进来（CMake 最后仍渲染成 -framework AGL），
        # 所以路径形式和 -framework 形式都要清掉。
        string(REPLACE "-framework AGL" "" _vibekey_wrapopengl_libs "${_vibekey_wrapopengl_libs}")
        string(REPLACE "-framework;AGL" "" _vibekey_wrapopengl_libs "${_vibekey_wrapopengl_libs}")
        string(REPLACE "AGL.framework" "" _vibekey_wrapopengl_libs "${_vibekey_wrapopengl_libs}")
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_vibekey_wrapopengl_libs}")
    endif()
endif()

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(appVibeKeyF3Studio
    src/main.cpp
    src/VibeKeyCommon.h
    src/AppVersion.h
    src/AppUpdater.h
    src/AppUpdater.cpp
    src/PortDetector.h
    src/PortDetector.cpp
    src/PythonLocator.h
    src/PythonLocator.cpp
    src/OtaManager.h
    src/OtaManager.cpp
    src/ReleasesModel.h
    src/ReleasesModel.cpp
    src/KeyConfigManager.h
    src/KeyConfigManager.cpp
    src/WorkBuddyMonitor.h
    src/WorkBuddyMonitor.cpp
    src/LightMonitor.h
    src/LightMonitor.cpp
    src/SystemTray.h
    src/SystemTray.cpp
    resources/appicon.qrc
)

if(WIN32)
    target_sources(appVibeKeyF3Studio PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc)
endif()

# C++ sources live in src/ and include their headers by bare name ("PortDetector.h"),
# so add src/ to the include path.
target_include_directories(appVibeKeyF3Studio PRIVATE src)

qt_add_qml_module(appVibeKeyF3Studio
    URI VibeKeyF3Studio
    QML_FILES
        qml/Main.qml
)

set_target_properties(appVibeKeyF3Studio PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${APP_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${APP_VERSION}
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.vibekey.f3studio"
    MACOSX_BUNDLE_BUNDLE_NAME "VibeKey-F3 Studio"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/MacOSXBundleInfo.plist.in"
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appVibeKeyF3Studio
    PRIVATE Qt6::Quick Qt6::Widgets Qt6::SerialPort Qt6::QuickControls2
)

# Copy runtime files to build directory
add_custom_command(TARGET appVibeKeyF3Studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/workers/ota_worker.py"
        "$<TARGET_FILE_DIR:appVibeKeyF3Studio>/ota_worker.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/workers/config_worker.py"
        "$<TARGET_FILE_DIR:appVibeKeyF3Studio>/config_worker.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/workers/workbuddy_status.py"
        "$<TARGET_FILE_DIR:appVibeKeyF3Studio>/workbuddy_status.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/workers/ble_led_worker.py"
        "$<TARGET_FILE_DIR:appVibeKeyF3Studio>/ble_led_worker.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/workers/ble_config_worker.py"
        "$<TARGET_FILE_DIR:appVibeKeyF3Studio>/ble_config_worker.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/workers/net_worker.py"
        "$<TARGET_FILE_DIR:appVibeKeyF3Studio>/net_worker.py"
    VERBATIM
)

install(TARGETS appVibeKeyF3Studio
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---------------------------------------------------------------------------
# macOS: 从 resources/appicon.png 生成 .icns 并放进 bundle。
# 仓库里只有 ICO/小尺寸 PNG，不额外提交二进制图标；用 sips + iconutil 在构建期
# 合成多尺寸 iconset。换图标只需替换 resources/appicon.png（建议 ≥512px 方图）。
# ---------------------------------------------------------------------------
if(APPLE)
    set(_iconset "${CMAKE_CURRENT_BINARY_DIR}/appicon.iconset")
    set(_icns "${CMAKE_CURRENT_BINARY_DIR}/appicon.icns")
    set(_png "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.png")

    add_custom_command(
        OUTPUT ${_icns}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_iconset}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_iconset}
        # 用 sips 直接从源图生成各尺寸，避免逐级缩放引入锯齿
        COMMAND sips -z 16 16      ${_png} --out ${_iconset}/icon_16x16.png
        COMMAND sips -z 32 32      ${_png} --out ${_iconset}/icon_16x16@2x.png
        COMMAND sips -z 32 32      ${_png} --out ${_iconset}/icon_32x32.png
        COMMAND sips -z 64 64      ${_png} --out ${_iconset}/icon_32x32@2x.png
        COMMAND sips -z 128 128    ${_png} --out ${_iconset}/icon_128x128.png
        COMMAND sips -z 256 256    ${_png} --out ${_iconset}/icon_128x128@2x.png
        COMMAND sips -z 256 256    ${_png} --out ${_iconset}/icon_256x256.png
        COMMAND sips -z 512 512    ${_png} --out ${_iconset}/icon_256x256@2x.png
        COMMAND sips -z 512 512    ${_png} --out ${_iconset}/icon_512x512.png
        COMMAND iconutil -c icns ${_iconset} -o ${_icns}
        DEPENDS ${_png}
        COMMENT "Generating appicon.icns from resources/appicon.png"
        VERBATIM
    )
    add_custom_target(appVibeKeyF3Studio_icns DEPENDS ${_icns})

    add_custom_command(TARGET appVibeKeyF3Studio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_icns}
            "$<TARGET_BUNDLE_CONTENT_DIR:appVibeKeyF3Studio>/Resources/appicon.icns"
        DEPENDS ${_icns}
        VERBATIM
    )
    add_dependencies(appVibeKeyF3Studio appVibeKeyF3Studio_icns)
endif()
