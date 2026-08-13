cmake_minimum_required(VERSION 3.16)

# 使用当前目录的 basename 作为项目名称：dde-clip-wink
project(dde-clip-wink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6 Widgets：托盘控件与 QClipboard 均依赖 QtWidgets
# Qt6 DBus：监听 DDE 剪贴板守护进程的信号
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)

# dde-tray-loader 开发包，提供 PluginsItemInterfaceV2 等接口头文件
find_package(DdeTrayLoader CONFIG REQUIRED)

# 源文件：插件入口 + 剪贴板管理 + 托盘图标控件
set(PLUGIN_SOURCES
    src/ddeclipwinkplugin.cpp
    src/clipboardmanager.cpp
    src/clipboardpreviewwidget.cpp
)

set(PLUGIN_HEADERS
    src/ddeclipwinkplugin.h
    src/clipboardmanager.h
    src/clipboardpreviewwidget.h
)

# 插件以 MODULE 形式编译，dde-tray-loader 通过 QPluginLoader 加载
add_library(dde-clip-wink MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

# 引入 DDE 托盘接口头文件目录
# DdeTrayLoaderConfig.cmake 会设置 DDE_TRAY_LOADER_INCLUDE_DIR
if(DEFINED DDE_TRAY_LOADER_INCLUDE_DIR)
    target_include_directories(dde-clip-wink PRIVATE ${DDE_TRAY_LOADER_INCLUDE_DIR})
endif()

target_include_directories(dde-clip-wink PRIVATE src)

target_link_libraries(dde-clip-wink PRIVATE
    Qt6::Widgets
    Qt6::DBus
)

# 安装到 dde-tray-loader 的系统托盘插件目录
# 与 airplane-mode / bluetooth / sound 等插件同级
install(TARGETS dde-clip-wink
    LIBRARY DESTINATION lib/dde-dock/plugins/system-trays
)
