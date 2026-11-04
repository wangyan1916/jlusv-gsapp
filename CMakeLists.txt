cmake_minimum_required(VERSION 3.16)
project(jlusv_gsapp VERSION 1.1.5 LANGUAGES CXX)

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
target_compile_definitions(${PROJECT_NAME} PRIVATE GSAPP_VERSION="${PROJECT_VERSION}")

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

include(CTest)
if(BUILD_TESTING)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(test_gs_client
        tests/test_gs_client.cpp
        src/network/gs_client.cpp src/network/gs_client.h
        src/map/map_bridge.cpp src/map/map_bridge.h
        src/map/coord_transform.cpp src/map/coord_transform.h
    )
    target_include_directories(test_gs_client PRIVATE src)
    target_link_libraries(test_gs_client PRIVATE Qt5::Core Qt5::Network Qt5::Positioning Qt5::Test)
    if(MSVC)
        target_compile_options(test_gs_client PRIVATE /utf-8 /W4)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        include(InstallRequiredSystemLibraries)
        foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            add_custom_command(TARGET test_gs_client POST_BUILD COMMAND ${CMAKE_COMMAND} -E
                copy_if_different "${runtime}" "$<TARGET_FILE_DIR:test_gs_client>")
        endforeach()
    endif()
    if(WIN32)
        foreach(module Core Network Positioning Test)
            add_custom_command(TARGET test_gs_client POST_BUILD COMMAND ${CMAKE_COMMAND} -E
                copy_if_different "$<TARGET_FILE:Qt5::${module}>" "$<TARGET_FILE_DIR:test_gs_client>")
        endforeach()
    endif()
    add_test(NAME gs_client_local_gateway COMMAND test_gs_client -o "${CMAKE_CURRENT_BINARY_DIR}/gs-client-tests.txt,txt")
    set_tests_properties(gs_client_local_gateway PROPERTIES TIMEOUT 90)
endif()
