cmake_minimum_required(VERSION 3.24)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" MONITOR_HUB_VERSION LIMIT_COUNT 1)
string(STRIP "${MONITOR_HUB_VERSION}" MONITOR_HUB_VERSION)
if(NOT MONITOR_HUB_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "Invalid root VERSION: '${MONITOR_HUB_VERSION}'")
endif()

project(MonitorHubCpp VERSION ${MONITOR_HUB_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.82 REQUIRED)

add_library(monitor_hub_core
    src/core.cpp
    src/control_orchestrator.cpp
    src/dispatch_worker.cpp
    src/deterministic_handlers.cpp
    src/command_control.cpp
    src/event_store.cpp
    src/overview.cpp
    src/notification_outbox.cpp
    src/setup_request.cpp
    src/claude_cli.cpp
    src/windows_probe.cpp
    src/boost_json.cpp
)
target_include_directories(monitor_hub_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${Boost_INCLUDE_DIRS}
)
target_compile_definitions(
    monitor_hub_core
    PUBLIC MONITOR_HUB_VERSION="${PROJECT_VERSION}"
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

add_executable(monitor_hub_control src/control_main.cpp)
target_link_libraries(monitor_hub_control PRIVATE monitor_hub_core)
install(TARGETS monitor_hub_control RUNTIME DESTINATION bin)

add_executable(monitor_hub_worker src/worker_main.cpp)
target_link_libraries(monitor_hub_worker PRIVATE monitor_hub_core)
install(TARGETS monitor_hub_worker RUNTIME DESTINATION bin)

add_executable(monitor_hub_orchestrator src/orchestrator_main.cpp)
target_link_libraries(monitor_hub_orchestrator PRIVATE monitor_hub_core)
install(TARGETS monitor_hub_orchestrator RUNTIME DESTINATION bin)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../hub/claude_statusline_bridge.py"
    DESTINATION share/monitor_hub
)

option(MONITOR_HUB_BUILD_QT "Build the Qt 6 desktop UI" OFF)
if(MONITOR_HUB_BUILD_QT)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
    qt_add_executable(monitor_hub_qt WIN32
        src/qt_main.cpp
        src/qt_main_window.cpp
        src/qt_desktop_controller.cpp
        src/qt_desktop_settings.cpp
        src/qt_app_logger.cpp
    )
    qt_add_resources(monitor_hub_qt monitor_hub_resources
        PREFIX "/monitor_hub"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/monitor_hub.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/monitor_hub.svg"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/styles/modern.qss"
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
    add_dependencies(monitor_hub_qt monitor_hub_orchestrator)
    if(MSVC)
        target_compile_options(monitor_hub_qt PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(monitor_hub_qt PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS monitor_hub_qt RUNTIME DESTINATION bin)
endif()

include(CTest)
if(BUILD_TESTING)
    if(MONITOR_HUB_BUILD_QT)
        add_executable(
            monitor_hub_qt_desktop_settings_tests
            tests/qt_desktop_settings_tests.cpp
            src/qt_desktop_settings.cpp
            src/qt_app_logger.cpp
        )
        target_include_directories(
            monitor_hub_qt_desktop_settings_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_link_libraries(
            monitor_hub_qt_desktop_settings_tests
            PRIVATE Qt6::Core Qt6::Gui
        )
        add_test(
            NAME monitor_hub_qt_desktop_settings_tests
            COMMAND monitor_hub_qt_desktop_settings_tests
        )
        if(MSVC)
            target_compile_options(
                monitor_hub_qt_desktop_settings_tests
                PRIVATE /W4 /permissive- /utf-8
            )
        else()
            target_compile_options(
                monitor_hub_qt_desktop_settings_tests
                PRIVATE -Wall -Wextra -Wpedantic
            )
        endif()
    endif()

    add_executable(monitor_hub_core_tests tests/core_tests.cpp)
    target_link_libraries(monitor_hub_core_tests PRIVATE monitor_hub_core)
    add_test(NAME monitor_hub_core_tests COMMAND monitor_hub_core_tests)

    add_executable(
        monitor_hub_command_control_tests
        tests/command_control_tests.cpp
    )
    target_link_libraries(
        monitor_hub_command_control_tests
        PRIVATE monitor_hub_core
    )
    add_test(
        NAME monitor_hub_command_control_tests
        COMMAND monitor_hub_command_control_tests
    )

    add_executable(
        monitor_hub_dispatch_worker_tests
        tests/dispatch_worker_tests.cpp
    )
    target_link_libraries(
        monitor_hub_dispatch_worker_tests
        PRIVATE monitor_hub_core
    )
    add_test(
        NAME monitor_hub_dispatch_worker_tests
        COMMAND monitor_hub_dispatch_worker_tests
    )

    add_executable(
        monitor_hub_handler_fixture
        tests/handler_fixture.cpp
    )

    add_executable(
        monitor_hub_deterministic_handler_tests
        tests/deterministic_handler_tests.cpp
    )
    target_link_libraries(
        monitor_hub_deterministic_handler_tests
        PRIVATE monitor_hub_core
    )
    add_test(
        NAME monitor_hub_deterministic_handler_tests
        COMMAND monitor_hub_deterministic_handler_tests
                $<TARGET_FILE:monitor_hub_handler_fixture>
    )

    add_executable(
        monitor_hub_setup_policy_bootstrap_tests
        tests/setup_policy_bootstrap_tests.cpp
    )
    target_link_libraries(
        monitor_hub_setup_policy_bootstrap_tests
        PRIVATE monitor_hub_core
    )
    add_test(
        NAME monitor_hub_setup_policy_bootstrap_tests
        COMMAND monitor_hub_setup_policy_bootstrap_tests
    )

    add_executable(
        monitor_hub_control_orchestrator_tests
        tests/control_orchestrator_tests.cpp
    )
    target_link_libraries(
        monitor_hub_control_orchestrator_tests
        PRIVATE monitor_hub_core
    )
    add_test(
        NAME monitor_hub_control_orchestrator_tests
        COMMAND monitor_hub_control_orchestrator_tests
    )

    add_executable(
        monitor_hub_recovery_projection_tests
        tests/recovery_projection_tests.cpp
    )
    target_link_libraries(
        monitor_hub_recovery_projection_tests
        PRIVATE monitor_hub_core
    )
    add_test(
        NAME monitor_hub_recovery_projection_tests
        COMMAND monitor_hub_recovery_projection_tests
    )

    add_test(
        NAME monitor_hub_registered_handlers_cli
        COMMAND monitor_hub_control --registered-handlers
    )
endif()
