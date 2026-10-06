cmake_minimum_required(VERSION 3.16)

project(VibeKeyF3Studio VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 生成 app_icon.rc：用绝对路径引用 ico，避免 rc.exe 路径解析问题
set(APPICON_ABSPATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.ico")
configure_file(resources/app_icon.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc @ONLY)

find_package(Qt6 REQUIRED COMPONENTS Quick Widgets SerialPort QuickControls2)

qt_standard_project_setup(REQUIRES 6.10)

qt_add_executable(appVibeKeyF3Studio
    src/main.cpp
    src/VibeKeyCommon.h
    src/AppVersion.h
    src/AppUpdater.h
    src/AppUpdater.cpp
    src/PortDetector.h
    src/PortDetector.cpp
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
    ${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc
)

# C++ sources live in src/ and include their headers by bare name ("PortDetector.h"),
# so add src/ to the include path.
target_include_directories(appVibeKeyF3Studio PRIVATE src)

qt_add_qml_module(appVibeKeyF3Studio
    URI VibeKeyF3Studio
    QML_FILES
        qml/Main.qml
)

set_target_properties(appVibeKeyF3Studio PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
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
