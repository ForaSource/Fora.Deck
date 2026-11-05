find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

function(xlibrary_add_test name)
    add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/desktop")
    target_link_libraries(${name} PRIVATE xlibrary_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endfunction()

xlibrary_add_test(update_checker_test update_checker_test.cpp
    ../desktop/UpdateChecker.cpp ../desktop/UpdateChecker.h)
target_link_libraries(update_checker_test PRIVATE Qt6::Network Qt6::Widgets)

xlibrary_add_test(core_test core_test.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(XLIBRARY_DBUS_RUN_SESSION dbus-run-session REQUIRED)
    add_executable(linux_portal_test linux_portal_test.cpp
        ../desktop/LinuxIntegration.cpp ../desktop/PlatformIntegration.h)
    set_target_properties(linux_portal_test PROPERTIES AUTOMOC ON)
    target_include_directories(linux_portal_test PRIVATE "${PROJECT_SOURCE_DIR}/desktop")
    target_link_libraries(linux_portal_test PRIVATE xlibrary_core Qt6::Widgets Qt6::DBus Qt6::Test)
    add_test(NAME linux_portal_test COMMAND "${XLIBRARY_DBUS_RUN_SESSION}" -- $<TARGET_FILE:linux_portal_test>)
    set_tests_properties(linux_portal_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_SESSION_TYPE=wayland" TIMEOUT 60)
endif()

xlibrary_add_test(path_search_test path_search_test.cpp)
target_link_libraries(path_search_test PRIVATE Qt6::Widgets)

xlibrary_add_test(library_model_test library_model_test.cpp
    ../desktop/LibraryModel.cpp ../desktop/LibraryModel.h ../desktop/appearance.cpp)
target_link_libraries(library_model_test PRIVATE Qt6::Widgets Qt6::Concurrent)

xlibrary_add_test(ui_registry_test ui_registry_test.cpp)
target_link_libraries(ui_registry_test PRIVATE Qt6::Widgets)
