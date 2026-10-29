cmake_minimum_required(VERSION 3.16)
project(jlusv_gsapp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 查找自编译的 Qt 5.15 模块
find_package(Qt5 5.15 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Quick
    QuickWidgets
    Location
    Positioning
    Network
)

set(SOURCES
    src/main.cpp
    src/app_window.h
    src/app_window.cpp
    src/network/gs_client.h
    src/network/gs_client.cpp
    src/map/coord_transform.h
    src/map/coord_transform.cpp
    src/map/local_tile_server.h
    src/map/local_tile_server.cpp
    src/map/map_bridge.h
    src/map/map_bridge.cpp
    qml/qml.qrc
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/network
    src/map
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Quick
    Qt5::QuickWidgets
    Qt5::Location
    Qt5::Positioning
    Qt5::Network
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W4)
endif()
