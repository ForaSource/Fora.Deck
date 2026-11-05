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
