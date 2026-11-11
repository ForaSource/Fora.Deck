# CPack executes this section only when explicitly creating a package.
if(XLIBRARY_CPACK_PROJECT_CONFIG)
    if(CPACK_GENERATOR STREQUAL "DEB")
        # System Qt from testing/unstable also belongs to dpkg but is incompatible with the release baseline.
        if(CPACK_XLIBRARY_QT_VERSION VERSION_GREATER "6.8.2")
            message(FATAL_ERROR
                    "DEB packaging for Debian 13 requires Qt <= 6.8.2; "
                    "this build uses Qt ${CPACK_XLIBRARY_QT_VERSION}. "
                    "Reconfigure and rebuild in a clean build directory on Debian 13 "
                    "with its system Qt 6.8.2, or package this build as AppImage. "
                    "Do not lower the generated package dependencies manually.")
        endif()
        if(NOT CPACK_XLIBRARY_QT_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
            message(FATAL_ERROR "DEB packaging requires shared system Qt libraries.")
        endif()
        find_program(xlibrary_dpkg_query NAMES dpkg-query REQUIRED)
        foreach(xlibrary_qt_library IN LISTS CPACK_XLIBRARY_QT_LIBRARIES)
            file(REAL_PATH "${xlibrary_qt_library}" xlibrary_qt_real_library)
            execute_process(COMMAND "${xlibrary_dpkg_query}" --search "${xlibrary_qt_real_library}"
                    RESULT_VARIABLE xlibrary_qt_owner_result
                    OUTPUT_QUIET ERROR_QUIET)
            if(NOT xlibrary_qt_owner_result EQUAL 0)
                message(FATAL_ERROR
                        "DEB packaging requires distribution Qt libraries managed by dpkg. "
                        "${xlibrary_qt_real_library} is not owned by an installed package. "
                        "Configure a separate build with the target distribution's system Qt; "
                        "use AppImage to distribute an SDK Qt build.")
            endif()
        endforeach()
    endif()
    return()
endif()

get_target_property(xlibrary_qmake Qt6::qmake LOCATION)
get_target_property(xlibrary_qt_core_library Qt6::Core LOCATION)
configure_file("${CMAKE_CURRENT_LIST_DIR}/linux_build_config.json.in"
        "${CMAKE_CURRENT_BINARY_DIR}/linux-packaging-config.json" @ONLY)

install(FILES "${CMAKE_CURRENT_LIST_DIR}/debian/copyright"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/xlibrary")

set(CPACK_PACKAGE_NAME "xlibrary")
set(CPACK_PACKAGE_VENDOR "lappland-open")
set(CPACK_PACKAGE_CONTACT "lappland-open (https://github.com/lappland-open)" CACHE STRING
        "Package maintainer contact (name and email for distribution releases)")
set(CPACK_PACKAGE_VERSION "${XLIBRARY_RELEASE_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Application launcher and organizer")
set(CPACK_PACKAGE_DESCRIPTION
        "XLibrary organizes desktop applications, tags and launch scenarios in a Qt interface.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ForaSource/Fora.Deck")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_DEBIAN_PACKAGE_NAME "xlibrary")
set(CPACK_DEBIAN_PACKAGE_VERSION "${XLIBRARY_DEBIAN_VERSION}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Qt platform plugins are loaded dynamically and are invisible to dpkg-shlibdeps.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, qt6-wayland, xdg-utils, libsecret-1-0")

foreach(xlibrary_qt_module IN ITEMS Core Gui Widgets Concurrent Network DBus)
    get_target_property(xlibrary_qt_library Qt6::${xlibrary_qt_module} LOCATION)
    list(APPEND CPACK_XLIBRARY_QT_LIBRARIES "${xlibrary_qt_library}")
endforeach()
get_target_property(CPACK_XLIBRARY_QT_LIBRARY_TYPE Qt6::Core TYPE)
set(CPACK_XLIBRARY_QT_VERSION "${Qt6_VERSION}")
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/linux-cpack-project.cmake")
file(WRITE "${CPACK_PROJECT_CONFIG_FILE}"
        "set(XLIBRARY_CPACK_PROJECT_CONFIG ON)\ninclude([==[${CMAKE_CURRENT_LIST_FILE}]==])\n")

include(CPack)
