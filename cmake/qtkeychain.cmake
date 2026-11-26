# Keep dependency options scoped to the function so they do not affect Fora.Deck settings.
function(foradeck_add_qtkeychain)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_WITH_QT5 OFF)
    set(BUILD_TRANSLATIONS OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_TEST_APPLICATION OFF)
    set(BUILD_QTQUICK_DEMO OFF)
    set(USE_CREDENTIAL_STORE ON)
    set(USE_COMPAT_NAMING_SCHEME OFF)
    set(LIBSECRET_SUPPORT ON)
    FetchContent_Declare(qtkeychain
            URL https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/0.17.0.tar.gz
            URL_HASH SHA256=3b85c3929034b0a99da777130c34d99f006fcd3a9d56564159399a33fee0e504
    )
    FetchContent_MakeAvailable(qtkeychain)
    set_property(DIRECTORY "${qtkeychain_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_compile_definitions(qt6keychain PUBLIC QTKEYCHAIN_NO_EXPORT)
    if(WIN32)
        set(foradeck_keychain_license_dir "licenses")
    else()
        # QtKeychain loads libsecret via QLibrary; the packager cannot find it through DT_NEEDED.
        find_library(foradeck_secret_library NAMES secret-1 REQUIRED)
        set(foradeck_secret_library "${foradeck_secret_library}" PARENT_SCOPE)
        set(foradeck_keychain_license_dir "${CMAKE_INSTALL_DATADIR}/Fora.Deck/licenses")
    endif()
    file(READ "${qtkeychain_SOURCE_DIR}/COPYING" foradeck_keychain_license)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qtkeychain-notice.txt"
            "QtKeychain 0.17.0 (https://github.com/frankosterfeld/qtkeychain)\n"
            "Copyright (C) 2011-2015 Frank Osterfeld <frank.osterfeld@gmail.com>\n"
            "Copyright (C) 2016 Mathias Hasselmann <mathias.hasselmann@kdab.com>\n\n"
            "${foradeck_keychain_license}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qtkeychain-notice.txt"
            DESTINATION "${foradeck_keychain_license_dir}" RENAME QtKeychain-BSD-3-Clause.txt)
endfunction()

foradeck_add_qtkeychain()
