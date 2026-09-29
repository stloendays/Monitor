cmake_minimum_required(VERSION 3.24)
project(MonitorHubCpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.82 REQUIRED)

add_library(monitor_hub_core
    src/core.cpp
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

include(CTest)
if(BUILD_TESTING)
    add_executable(monitor_hub_core_tests tests/core_tests.cpp)
    target_link_libraries(monitor_hub_core_tests PRIVATE monitor_hub_core)
    add_test(NAME monitor_hub_core_tests COMMAND monitor_hub_core_tests)
endif()
