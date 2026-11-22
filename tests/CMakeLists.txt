find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

function(foradeck_add_test name)
    add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/desktop")
    target_link_libraries(${name} PRIVATE foradeck_core Qt6::Test)
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endfunction()

foradeck_add_test(update_checker_test update_checker_test.cpp
    ../desktop/UpdateChecker.cpp ../desktop/UpdateChecker.h)
target_link_libraries(update_checker_test PRIVATE Qt6::Network Qt6::Widgets)

foradeck_add_test(core_test core_test.cpp)
foradeck_add_test(catalog_test catalog_test.cpp)
foradeck_add_test(file_manager_test file_manager_test.cpp)
foradeck_add_test(launch_test launch_test.cpp)
foradeck_add_test(profile_settings_test profile_settings_test.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(FORADECK_DBUS_RUN_SESSION dbus-run-session REQUIRED)
    add_executable(linux_portal_test linux_portal_test.cpp
        ../desktop/LinuxIntegration.cpp ../desktop/PlatformIntegration.h)
    set_target_properties(linux_portal_test PROPERTIES AUTOMOC ON)
    target_include_directories(linux_portal_test PRIVATE "${PROJECT_SOURCE_DIR}/desktop")
    target_link_libraries(linux_portal_test PRIVATE foradeck_core Qt6::Widgets Qt6::DBus Qt6::Test)
    add_test(NAME linux_portal_test COMMAND "${FORADECK_DBUS_RUN_SESSION}" -- $<TARGET_FILE:linux_portal_test>)
    set_tests_properties(linux_portal_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_SESSION_TYPE=wayland" TIMEOUT 60)
endif()

foradeck_add_test(path_search_test path_search_test.cpp)
target_link_libraries(path_search_test PRIVATE Qt6::Widgets)

foradeck_add_test(library_model_test library_model_test.cpp
    ../desktop/LibraryModel.cpp ../desktop/LibraryModel.h ../desktop/appearance.cpp)
target_link_libraries(library_model_test PRIVATE Qt6::Widgets Qt6::Concurrent)

foradeck_add_test(ui_registry_test ui_registry_test.cpp)
target_link_libraries(ui_registry_test PRIVATE Qt6::Widgets)

foradeck_add_test(desktop_motion_test desktop_motion_test.cpp)
target_link_libraries(desktop_motion_test PRIVATE foradeck_desktop)
# The combined GUI scenarios can take more than a minute with debug Qt on Windows.
set_tests_properties(desktop_motion_test PROPERTIES TIMEOUT 120)

foradeck_add_test(notification_banner_test notification_banner_test.cpp
    ../desktop/NotificationBanner.cpp ../desktop/appearance.cpp)
target_link_libraries(notification_banner_test PRIVATE Qt6::Widgets)

option(FORADECK_BUILD_BENCHMARKS "Build optional local poll timing tool (not installed)" OFF)
if(FORADECK_BUILD_BENCHMARKS)
    add_executable(poll_benchmark poll_benchmark.cpp)
    target_link_libraries(poll_benchmark PRIVATE foradeck_core)
endif()

foradeck_add_test(workspaces_test workspaces_test.cpp
    ../desktop/WorkspaceCapture.cpp ../desktop/WorkspaceCapture.h ../desktop/WorkspaceWindowBackend.h
    ../desktop/WorkspaceCoordinator.cpp ../desktop/WorkspaceCoordinator.h)
target_link_libraries(workspaces_test PRIVATE Qt6::Concurrent)
