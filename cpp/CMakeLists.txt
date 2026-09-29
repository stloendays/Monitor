cmake_minimum_required(VERSION 3.24)
project(MonitorHubCpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.82 REQUIRED)

add_library(monitor_hub_core
    src/core.cpp
    src/overview.cpp
    src/windows_probe.cpp
    src/boost_json.cpp
)
target_include_directories(monitor_hub_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${Boost_INCLUDE_DIRS}
)
if(WIN32)
    target_link_libraries(monitor_hub_core PRIVATE taskschd wbemuuid ole32 oleaut32)
endif()
if(MSVC)
    target_compile_options(monitor_hub_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(monitor_hub_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(monitor_hub_cli src/main.cpp)
target_link_libraries(monitor_hub_cli PRIVATE monitor_hub_core)
install(TARGETS monitor_hub_cli RUNTIME DESTINATION bin)

option(MONITOR_HUB_BUILD_QT "Build the Qt 6 desktop UI" OFF)
if(MONITOR_HUB_BUILD_QT)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
    qt_add_executable(monitor_hub_qt WIN32
        src/qt_main.cpp
        src/qt_main_window.cpp
        src/qt_desktop_controller.cpp
        src/qt_desktop_settings.cpp
    )
    qt_add_resources(monitor_hub_qt monitor_hub_resources
        PREFIX "/monitor_hub"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/monitor_hub.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/monitor_hub.svg"
    )
    if(WIN32)
        enable_language(RC)
        set(MONITOR_HUB_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/monitor_hub.ico")
        configure_file(
            resources/monitor_hub.rc.in
            "${CMAKE_CURRENT_BINARY_DIR}/monitor_hub.rc"
            @ONLY
        )
        target_sources(monitor_hub_qt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/monitor_hub.rc")
    endif()
    target_link_libraries(monitor_hub_qt
        PRIVATE
            monitor_hub_core
            Qt6::Widgets
            Qt6::Network
    )
    target_compile_definitions(monitor_hub_qt PRIVATE MONITOR_HUB_VERSION="${PROJECT_VERSION}")
    if(MSVC)
        target_compile_options(monitor_hub_qt PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(monitor_hub_qt PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS monitor_hub_qt RUNTIME DESTINATION bin)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(monitor_hub_core_tests tests/core_tests.cpp)
    target_link_libraries(monitor_hub_core_tests PRIVATE monitor_hub_core)
    add_test(NAME monitor_hub_core_tests COMMAND monitor_hub_core_tests)
endif()
