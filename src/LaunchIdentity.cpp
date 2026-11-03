#ifdef _WIN32
#include <windows.h>
#endif

#include <limits>
#include <stdexcept>

#include "LaunchIdentity.h"
#include "SqliteHelpers.h"

std::string xlibrary::path_utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::filesystem::path xlibrary::utf8_path(const std::string &path) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
}

std::string xlibrary::path_identity(const std::filesystem::path &path, const bool resolve_links) {
    if (path.empty() || !path.is_absolute())
        throw std::invalid_argument("Launch path must be absolute");
    auto normalized = path.lexically_normal();
    if (resolve_links) {
        std::error_code error;
        const auto canonical = std::filesystem::canonical(path, error);
        if (!error)
            normalized = canonical;
    }
#ifdef _WIN32
    const auto native = normalized.generic_wstring();
    if (native.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Launch path is too long");
    const auto count = static_cast<int>(native.size());
    const auto required =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(), count, nullptr, 0, nullptr, nullptr, 0);
    if (required == 0)
        throw std::runtime_error("Cannot normalize Windows launch path");
    std::wstring folded(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(), count, folded.data(), required, nullptr,
                      nullptr, 0) != required)
        throw std::runtime_error("Cannot normalize Windows launch path");
    normalized = folded;
#endif
    return path_utf8(normalized);
}

sqlite3_int64 xlibrary::ensure_launch_configuration(sqlite3 *const connection, const std::filesystem::path &path,
                                                    const std::string &arguments,
                                                    const std::filesystem::path &working_directory) {
    const auto launch_path = path_utf8(path);
    const auto directory = path_utf8(working_directory);
    if (launch_path.find('\0') != std::string::npos || directory.find('\0') != std::string::npos ||
        arguments.find('\0') != std::string::npos)
        throw std::invalid_argument("Launch configuration contains a null character");
    const auto invocation = path_identity(path, false);
    {
        // Исчезнувший или переназначенный симлинк не меняет идентичность при сохранении тегов карточки.
        SqliteStatement existing(connection, "SELECT id FROM launch_configuration WHERE invocation_key=? "
                                             "AND arguments=? AND working_directory=? LIMIT 1;");
        existing.bind(1, invocation);
        existing.bind(2, arguments);
        existing.bind(3, directory);
        if (existing.next())
            return existing.number(0);
    }
    const auto key = path_identity(path);
    const auto directory_key = working_directory.empty() ? std::string{} : path_identity(working_directory);
    const std::string kind = path.extension() == ".desktop" ? "desktop" : "executable";
    SqliteStatement create_target(connection, "INSERT INTO launch_target(kind,identity_key) VALUES(?,?) "
                                              "ON CONFLICT(kind,identity_key) DO NOTHING;");
    create_target.bind(1, kind);
    create_target.bind(2, key);
    create_target.next();
    SqliteStatement find_target(connection, "SELECT id FROM launch_target WHERE kind=? AND identity_key=?;");
    find_target.bind(1, kind);
    find_target.bind(2, key);
    if (!find_target.next())
        throw std::runtime_error("Launch target was not saved");
    const auto target_id = find_target.number(0);
    // Путь вызова влияет на argv[0] и каталог по умолчанию, даже у симлинков одного EXE.
    SqliteStatement create_configuration(connection, R"(
        INSERT INTO launch_configuration(target_id,path,arguments,working_directory,working_directory_key,invocation_key)
        VALUES(?,?,?,?,?,?) ON CONFLICT(target_id,invocation_key,arguments,working_directory_key) DO NOTHING;
    )");
    create_configuration.bind(1, target_id);
    create_configuration.bind(2, launch_path);
    create_configuration.bind(3, arguments);
    create_configuration.bind(4, directory);
    create_configuration.bind(5, directory_key);
    create_configuration.bind(6, invocation);
    create_configuration.next();
    SqliteStatement find_configuration(connection, "SELECT id FROM launch_configuration WHERE target_id=? "
                                                   "AND invocation_key=? AND arguments=? AND working_directory_key=?;");
    find_configuration.bind(1, target_id);
    find_configuration.bind(2, invocation);
    find_configuration.bind(3, arguments);
    find_configuration.bind(4, directory_key);
    if (!find_configuration.next())
        throw std::runtime_error("Launch configuration was not saved");
    return find_configuration.number(0);
}
