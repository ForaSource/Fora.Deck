#include <QCoreApplication>
#ifdef _WIN32
#include <windows.h>
#endif

#include <limits>
#include <stdexcept>

#include "LaunchIdentity.h"
#include "PathHelpers.h"
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
#ifdef _WIN32
    const auto effective_directory =
        filesystem_path(effective_working_directory(path_text(path), path_text(working_directory)));
    const auto directory_key = path_identity(effective_directory);
    {
        // Match older catalogs with an omitted directory or the EXE itself as the directory.
        SqliteStatement existing(connection, "SELECT c.id,c.path,c.working_directory FROM launch_configuration c "
                                             "WHERE c.invocation_key=? AND c.arguments=? "
                                             "ORDER BY EXISTS(SELECT 1 FROM applications a "
                                             "WHERE a.configuration_id=c.id) DESC,c.id;");
        existing.bind(1, invocation);
        existing.bind(2, arguments);
        while (existing.next()) {
            const auto stored_directory = effective_working_directory(existing.qtext(1), existing.qtext(2));
            if (path_identity(filesystem_path(stored_directory)) == directory_key)
                return existing.number(0);
        }
    }
#else
    const auto directory_key = working_directory.empty() ? std::string{} : path_identity(working_directory);
#endif
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

#include "Application.h"

sqlite3_int64 xlibrary::ensure_launch_configuration(sqlite3 *const connection, const app_record &record) {
    Application::validate_launch_target(record);
    sqlite3_int64 configuration_id = 0;
    const auto kind = Application::target_kind(record).toStdString();
    if (Application::file_target(record)) {
        configuration_id =
            ensure_launch_configuration(connection, filesystem_path(record.path), record.arguments.toStdString(),
                                        filesystem_path(record.working_directory));
    } else {
        const auto key = (kind == "aumid" ? record.path.toCaseFolded() : record.path).toStdString();
        SqliteStatement create_target(
            connection,
            "INSERT INTO launch_target(kind,identity_key) VALUES(?,?) ON CONFLICT(kind,identity_key) DO NOTHING;");
        create_target.bind(1, kind);
        create_target.bind(2, key);
        create_target.next();
        SqliteStatement target(connection, "SELECT id FROM launch_target WHERE kind=? AND identity_key=?;");
        target.bind(1, kind);
        target.bind(2, key);
        if (!target.next())
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Launch target was not saved").toStdString());
        const auto target_id = target.number(0);
        SqliteStatement create_configuration(
            connection,
            "INSERT INTO "
            "launch_configuration(target_id,path,arguments,working_directory,working_directory_key,invocation_key) "
            "VALUES(?,?,'','','',?) ON CONFLICT(target_id,invocation_key,arguments,working_directory_key) DO NOTHING;");
        create_configuration.bind(1, target_id);
        create_configuration.bind(2, record.path);
        create_configuration.bind(3, key);
        create_configuration.next();
        SqliteStatement find(connection, "SELECT id FROM launch_configuration WHERE target_id=? AND invocation_key=?;");
        find.bind(1, target_id);
        find.bind(2, key);
        if (!find.next())
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Launch configuration was not saved").toStdString());
        configuration_id = find.number(0);
    }
    // Location is discovery metadata: moving a Steam library must preserve the card and target IDs.
    SqliteStatement metadata(
        connection,
        "UPDATE launch_configuration SET installation_directory=?,icon_path=?,executable_path=? WHERE id=?;");
    metadata.bind(1, record.installation_directory);
    metadata.bind(2, record.icon_path);
    metadata.bind(3, record.executable_path);
    metadata.bind(4, configuration_id);
    metadata.next();
    return configuration_id;
}
