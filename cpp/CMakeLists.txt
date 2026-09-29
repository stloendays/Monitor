cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" MONITOR_HUB_VERSION)
string(STRIP "${MONITOR_HUB_VERSION}" MONITOR_HUB_VERSION)
if(NOT MONITOR_HUB_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid VERSION: '${MONITOR_HUB_VERSION}'")
endif()
project(MonitorHubCpp VERSION ${MONITOR_HUB_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.82 REQUIRED)

add_library(monitor_hub_core
    src/core.cpp
    src/boost_json.cpp
)
target_include_directories(monitor_hub_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${Boost_INCLUDE_DIRS}
)
if(MSVC)
    target_compile_options(monitor_hub_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(monitor_hub_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(monitor_hub_cli src/main.cpp)
target_link_libraries(monitor_hub_cli PRIVATE monitor_hub_core)
target_compile_definitions(monitor_hub_cli PRIVATE MONITOR_HUB_VERSION="${MONITOR_HUB_VERSION}")

include(CTest)
if(BUILD_TESTING)
    add_executable(monitor_hub_core_tests tests/core_tests.cpp)
    target_link_libraries(monitor_hub_core_tests PRIVATE monitor_hub_core)
    add_test(NAME monitor_hub_core_tests COMMAND monitor_hub_core_tests)
endif()
