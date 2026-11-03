#include <memory>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <stdexcept>

#include "Database.h"
#include "path_helpers.h"
#include "profile_backup.h"
#include "profile_settings.h"
#include "ProfileSchema.h"

namespace {
constexpr auto format_id = "io.github.LapplandJulia.XLibrary.profile";
constexpr int format_version = 1;
constexpr auto manifest_table = "_xlibrary_profile_manifest";
constexpr auto counts_table = "_xlibrary_profile_counts";
constexpr auto settings_table = "_xlibrary_profile_settings";
constexpr qsizetype settings_bytes_limit = 64 * 1024 * 1024;
constexpr qsizetype settings_count_limit = 100000;
constexpr qsizetype journal_bytes_limit = 1024 * 1024;

class InvalidDatabase : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const QString &message) {
    throw std::runtime_error(message.toStdString());
}

void check_sqlite(int code, sqlite3 *connection) {
    if (code == SQLITE_OK)
        return;
    const auto message = connection ? sqlite3_errmsg(connection) : "Cannot open profile database";
    if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB)
        throw InvalidDatabase(message);
    throw std::runtime_error(message);
}

class Connection {
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> connection{nullptr, &sqlite3_close_v2};

public:
    explicit Connection(const QString &path, bool writable = false) {
        sqlite3 *raw = nullptr;
        const auto utf8 = path.toUtf8();
        const auto result =
            sqlite3_open_v2(utf8.constData(), &raw,
                            writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY, nullptr);
        connection.reset(raw);
        check_sqlite(result, raw);
        sqlite3_busy_timeout(raw, 5000);
        sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, static_cast<int>(settings_bytes_limit));
        check_sqlite(sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr), raw);
        execute("PRAGMA trusted_schema=OFF;");
    }
    sqlite3 *get() const { return connection.get(); }
    void execute(const QByteArray &sql) const {
        check_sqlite(sqlite3_exec(get(), sql.constData(), nullptr, nullptr, nullptr), get());
    }
};

class Statement {
    sqlite3 *connection;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{nullptr, &sqlite3_finalize};

public:
    Statement(sqlite3 *db, const QByteArray &sql) : connection(db) {
        sqlite3_stmt *raw = nullptr;
        const auto result = sqlite3_prepare_v2(db, sql.constData(), -1, &raw, nullptr);
        statement.reset(raw);
        check_sqlite(result, db);
    }
    void bind(int index, const QString &value) {
        const auto utf8 = value.toUtf8();
        check_sqlite(sqlite3_bind_text64(statement.get(), index, utf8.constData(),
                                         static_cast<sqlite3_uint64>(utf8.size()), SQLITE_TRANSIENT, SQLITE_UTF8),
                     connection);
    }
    void bind(int index, qint64 value) { check_sqlite(sqlite3_bind_int64(statement.get(), index, value), connection); }
    void bind(int index, const QByteArray &value) {
        check_sqlite(sqlite3_bind_blob64(statement.get(), index, value.constData(),
                                         static_cast<sqlite3_uint64>(value.size()), SQLITE_TRANSIENT),
                     connection);
    }
    bool next() {
        const auto result = sqlite3_step(statement.get());
        if (result != SQLITE_ROW && result != SQLITE_DONE)
            check_sqlite(result, connection);
        return result == SQLITE_ROW;
    }
    void reset() {
        check_sqlite(sqlite3_reset(statement.get()), connection);
        check_sqlite(sqlite3_clear_bindings(statement.get()), connection);
    }
    int type(int column) const { return sqlite3_column_type(statement.get(), column); }
    QString text(int column) const {
        if (type(column) != SQLITE_TEXT)
            fail("Malformed profile text field");
        if (sqlite3_column_bytes(statement.get(), column) > 32768)
            fail("A profile text field is too large");
        const auto *value = sqlite3_column_text(statement.get(), column);
        return QString::fromUtf8(reinterpret_cast<const char *>(value), sqlite3_column_bytes(statement.get(), column));
    }
    QByteArray bytes(int column) const {
        if (type(column) != SQLITE_BLOB)
            fail("Malformed profile settings payload");
        const auto size = sqlite3_column_bytes(statement.get(), column);
        if (size > settings_bytes_limit)
            fail("Profile setting is too large");
        return QByteArray(static_cast<const char *>(sqlite3_column_blob(statement.get(), column)), size);
    }
    qint64 number(int column) const {
        if (type(column) != SQLITE_INTEGER)
            fail("Malformed profile integer field");
        return sqlite3_column_int64(statement.get(), column);
    }
};

QString absolute_path(const QString &path) {
    if (path.isEmpty() || path.contains(QChar::Null))
        fail("A profile path is empty or malformed");
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool same_path(const QString &first, const QString &second) {
    const QFileInfo a(first), b(second);
    if (a.exists() && b.exists() && a.canonicalFilePath() == b.canonicalFilePath())
        return true;
    return QString::compare(absolute_path(first), absolute_path(second), path_case_sensitivity) == 0;
}

void validate_destinations(const QString &database_path, const QString &settings_path) {
    if (same_path(database_path, settings_path))
        fail("The database and settings require different paths");
    for (const auto *suffix : {"-wal", "-shm", "-journal", ".profile-restore.json", ".profile-restore.lock"}) {
        if (same_path(settings_path, database_path + QLatin1String(suffix)))
            fail("The settings file cannot replace a database journal");
    }
    for (const auto &path : {database_path, settings_path}) {
        const QFileInfo info(path);
        if (!info.dir().exists() || info.isSymLink() || (info.exists() && !info.isFile()))
            fail("A profile destination is not a regular file in an existing directory");
    }
}

void ensure_distinct_backup(const QString &backup_path, const QString &database_path, const QString &settings_path) {
    if (same_path(backup_path, database_path) || same_path(backup_path, settings_path))
        fail("Choose a profile backup outside the active database and settings files");
    for (const auto *suffix : {"-wal", "-shm", "-journal", ".profile-restore.json", ".profile-restore.lock"}) {
        if (same_path(backup_path, database_path + QLatin1String(suffix)))
            fail("A profile backup cannot replace a database journal");
    }
}

void copy_file(const QString &source, const QString &destination) {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        fail("Cannot read profile file: " + source);
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        fail("Cannot prepare profile file: " + destination);
    output.setPermissions(input.permissions());
    while (!input.atEnd()) {
        const auto bytes = input.read(1024 * 1024);
        if (bytes.isEmpty() && input.error() != QFileDevice::NoError)
            fail("Cannot read profile file: " + source);
        if (output.write(bytes) != bytes.size())
            fail("Cannot write profile file: " + destination);
    }
    if (!output.commit())
        fail("Cannot commit profile file: " + destination);
}

void remove_file(const QString &path) {
    if (QFileInfo::exists(path) && !QFile::remove(path))
        fail("Cannot remove profile file: " + path);
}

void require_self_contained(const QString &path) {
    for (const auto *suffix : {"-wal", "-shm", "-journal"}) {
        if (QFileInfo(path + QLatin1String(suffix)).size() > 0)
            fail("An XLibrary profile backup must be a self-contained SQLite file");
    }
}

QByteArray fingerprint(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("Cannot read profile backup");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        fail("Cannot calculate profile backup fingerprint");
    return hash.result();
}

void snapshot(const QString &source, const QString &destination) {
    Connection input(source);
    Connection output(destination, true);
    auto *backup = sqlite3_backup_init(output.get(), "main", input.get(), "main");
    if (!backup) {
        check_sqlite(sqlite3_errcode(output.get()), output.get());
        fail("Cannot initialize the profile database snapshot");
    }
    QElapsedTimer timer;
    timer.start();
    int result = SQLITE_OK;
    do {
        result = sqlite3_backup_step(backup, 256);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED)
            QThread::msleep(20);
    } while (result == SQLITE_OK || ((result == SQLITE_BUSY || result == SQLITE_LOCKED) && timer.elapsed() < 5000));
    const auto finished = sqlite3_backup_finish(backup);
    if (result != SQLITE_DONE)
        check_sqlite(result, output.get());
    check_sqlite(finished, output.get());
    output.execute("PRAGMA journal_mode=DELETE;");
}

qint64 schema_version(sqlite3 *connection) {
    Statement version(connection, "PRAGMA user_version;");
    if (!version.next())
        fail("Missing profile schema version");
    return version.number(0);
}

void integrity_check(sqlite3 *connection) {
    Statement check(connection, "PRAGMA integrity_check;");
    if (!check.next() || check.text(0) != "ok" || check.next())
        throw InvalidDatabase("Profile database integrity check failed");
}

QByteArray quote_identifier(const QString &name) {
    if (name.contains(QChar::Null))
        fail("Malformed profile table name");
    auto escaped = name.toUtf8();
    escaped.replace("\"", "\"\"");
    return '"' + escaped + '"';
}

bool service_table(const QString &name) {
    return name == QLatin1String(manifest_table) || name == QLatin1String(counts_table) ||
           name == QLatin1String(settings_table);
}

QMap<QString, qint64> data_counts(sqlite3 *connection) {
    QMap<QString, qint64> counts;
    Statement tables(connection,
                     "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;");
    while (tables.next()) {
        const auto name = tables.text(0);
        if (service_table(name)) {
            if (name == QLatin1String(settings_table)) {
                Statement settings_count(connection, "SELECT COUNT(*) FROM _xlibrary_profile_settings;");
                if (settings_count.next())
                    counts.insert("settings", settings_count.number(0));
            }
            continue;
        }
        Statement count(connection, "SELECT COUNT(*) FROM " + quote_identifier(name) + ';');
        if (!count.next())
            fail("Cannot count profile data");
        counts.insert(name, count.number(0));
    }
    if (counts.contains("desktop_application")) {
        Statement history(connection, "SELECT COUNT(*) FROM desktop_application WHERE last_launch > 0;");
        if (history.next())
            counts.insert("history", history.number(0));
    }
    return counts;
}

void validate_staged_database(const QString &path) {
    {
        Connection connection(path);
        integrity_check(connection.get());
        const auto version = schema_version(connection.get());
        if (version < 0 || version > xlibrary::profile_schema_version)
            throw InvalidDatabase("This profile database requires a newer XLibrary version");
    }
    try {
        Database store(path);
        (void)store.read();
        Connection connection(path);
        xlibrary::validate_profile(connection.get());
        integrity_check(connection.get());
    } catch (const InvalidDatabase &) {
        throw;
    } catch (const std::exception &error) {
        throw InvalidDatabase(error.what());
    }
}

profile_manifest read_manifest(sqlite3 *connection) {
    Statement row(connection, "SELECT format_id, format_version, application_version, created_utc, schema_version "
                              "FROM _xlibrary_profile_manifest;");
    if (!row.next() || row.text(0) != QLatin1String(format_id))
        fail("This file is not an XLibrary profile backup");
    const auto version = row.number(1);
    if (version != format_version)
        fail("Unsupported XLibrary profile backup format");
    profile_manifest result;
    result.format_version = static_cast<int>(version);
    result.application_version = row.text(2);
    result.created_utc = row.text(3);
    const auto schema = row.number(4);
    if (schema <= 0 || schema > xlibrary::profile_schema_version)
        fail("This profile backup requires a newer or unsupported database schema");
    result.schema_version = static_cast<int>(schema);
    const auto date = QDateTime::fromString(result.created_utc, Qt::ISODateWithMs);
    if (result.application_version.isEmpty() || result.application_version.size() > 256 || !date.isValid() ||
        !result.created_utc.endsWith('Z') || row.next())
        fail("Malformed XLibrary profile manifest");
    if (schema_version(connection) != result.schema_version)
        fail("The profile manifest and database schema do not match");
    Statement counts(connection, "SELECT name, row_count FROM _xlibrary_profile_counts;");
    while (counts.next()) {
        const auto name = counts.text(0);
        const auto count = counts.number(1);
        if (name.isEmpty() || name.size() > 256 || count < 0 || result.counts.contains(name) ||
            result.counts.size() > 1024)
            fail("Malformed profile data counts");
        result.counts.insert(name, count);
    }
    if (result.counts != data_counts(connection))
        fail("Profile data counts do not match the manifest");
    return result;
}

profile_settings::values read_settings(sqlite3 *connection) {
    profile_settings::values result;
    qsizetype total_bytes = 0;
    Statement settings(connection, "SELECT key, value FROM _xlibrary_profile_settings ORDER BY key;");
    while (settings.next()) {
        const auto key = settings.text(0);
        const auto bytes = settings.bytes(1);
        total_bytes += bytes.size();
        if (key.isEmpty() || key.contains(QChar::Null) || key.size() > 32768 || result.contains(key) ||
            result.size() >= settings_count_limit || total_bytes > settings_bytes_limit ||
            key.compare(QLatin1String(profile_settings::token_marker), Qt::CaseInsensitive) == 0)
            fail("Malformed profile settings");
        result.insert(key, profile_settings::decode(bytes));
    }
    profile_settings::validate_keys(result);
    return result;
}

struct staged_profile {
    profile_manifest manifest;
    profile_settings::values settings;
};

staged_profile prepare_backup(const QString &source, const QString &staged_database) {
    if (!QFileInfo(source).isFile())
        fail("Select an existing XLibrary profile backup");
    require_self_contained(source);
    const auto before = fingerprint(source);
    copy_file(source, staged_database);
    if (!QFile::setPermissions(staged_database, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        fail("Cannot make the staged profile database writable");
    if (before != fingerprint(staged_database) || before != fingerprint(source))
        fail("The profile backup changed while it was being read");
    staged_profile result;
    {
        Connection connection(staged_database);
        // Версии проверяются до Database: его конструктор имеет право мигрировать только staging.
        result.manifest = read_manifest(connection.get());
        integrity_check(connection.get());
        result.settings = read_settings(connection.get());
    }
    {
        Connection connection(staged_database, true);
        connection.execute("DROP TABLE _xlibrary_profile_settings; DROP TABLE _xlibrary_profile_counts; "
                           "DROP TABLE _xlibrary_profile_manifest;");
        connection.execute("PRAGMA journal_mode=DELETE;");
    }
    validate_staged_database(staged_database);
    result.manifest.fingerprint = before;
    return result;
}

QString journal_path(const QString &database_path) {
    return database_path + ".profile-restore.json";
}

struct preserved_file {
    QString target;
    QString saved_name;
    bool existed = false;
};

QList<preserved_file> profile_files(const QString &database_path, const QString &settings_path) {
    return {{database_path, "database.sqlite", false},
            {database_path + "-wal", "database.sqlite-wal", false},
            {database_path + "-shm", "database.sqlite-shm", false},
            {database_path + "-journal", "database.sqlite-journal", false},
            {settings_path, "settings.ini", false}};
}

void write_json(const QString &path, const QJsonObject &object) {
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        fail("Cannot save profile recovery journal");
}

QJsonObject read_json(const QString &path) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() > journal_bytes_limit)
        fail("Cannot read profile recovery journal");
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail("Malformed profile recovery journal");
    return document.object();
}

void rollback_files(const QString &database_path, const QString &recovery_path, const QList<preserved_file> &files) {
    // Каждый повтор начинает с исходной БД и её журналов, даже если прежний rollback тоже прервался.
    for (const auto &file : files) {
        if (file.existed && !QFileInfo(QDir(recovery_path).filePath(file.saved_name)).isFile())
            fail("A preserved profile file is missing; recovery directory: " + recovery_path);
    }
    for (const auto *suffix : {"-wal", "-shm", "-journal"})
        remove_file(database_path + QLatin1String(suffix));
    for (const auto &file : files) {
        if (file.existed)
            copy_file(QDir(recovery_path).filePath(file.saved_name), file.target);
        else
            remove_file(file.target);
    }
}

void recover_locked(const QString &database_path, const QString &settings_path) {
    const auto journal = journal_path(database_path);
    if (!QFileInfo::exists(journal))
        return;
    const auto object = read_json(journal);
    const auto recovery_path = object.value("recovery_path").toString();
    const QFileInfo recovery_info(recovery_path);
    const auto parent = QFileInfo(database_path).absolutePath();
    auto files = profile_files(database_path, settings_path);
    const auto saved = object.value("files").toArray();
    const auto state = object.value("state").toString();
    if (object.value("version").toInt() != 1 || object.value("database_path").toString() != database_path ||
        object.value("settings_path").toString() != settings_path || recovery_info.absolutePath() != parent ||
        recovery_info.isSymLink() || !recovery_info.fileName().startsWith(".xlibrary-recovery-") ||
        saved.size() != files.size() || (state != "prepared" && state != "committed"))
        fail("The profile recovery journal does not match this profile");
    for (qsizetype index = 0; index < files.size(); ++index) {
        if (!saved[index].isObject())
            fail("Malformed profile recovery journal");
        const auto entry = saved[index].toObject();
        if (entry.value("name").toString() != files[index].saved_name || !entry.value("existed").isBool())
            fail("Malformed profile recovery journal");
        files[index].existed = entry.value("existed").toBool();
    }
    if (state == "prepared")
        rollback_files(database_path, recovery_path, files);
    remove_file(journal);
}

void lock_profile(QLockFile &lock) {
    if (!lock.tryLock(0))
        fail("Another profile recovery operation is running");
}
}

void ProfileBackup::create(const QString &backup_path, const QString &database_path, const QString &settings_path) {
    ensure_distinct_backup(backup_path, database_path, settings_path);
    if (!QFileInfo(backup_path).dir().exists())
        fail("The profile backup directory does not exist");
    QTemporaryDir staging(QFileInfo(backup_path).absolutePath() + "/.xlibrary-backup-XXXXXX");
    if (!staging.isValid())
        fail("Cannot create a profile backup staging directory");
    const auto staged_database = staging.filePath("profile.sqlite");
    snapshot(database_path, staged_database);
    {
        Connection connection(staged_database);
        Statement reserved(connection.get(),
                           "SELECT name FROM sqlite_master WHERE name IN "
                           "('_xlibrary_profile_manifest','_xlibrary_profile_counts','_xlibrary_profile_settings');");
        if (reserved.next())
            fail("The active profile contains reserved backup tables");
    }
    validate_staged_database(staged_database);
    const auto settings = profile_settings::read(settings_path);
    {
        Connection connection(staged_database, true);
        auto counts = data_counts(connection.get());
        counts.insert("settings", settings.size());
        connection.execute(
            "BEGIN IMMEDIATE; CREATE TABLE _xlibrary_profile_manifest ("
            "format_id TEXT NOT NULL, format_version INTEGER NOT NULL, application_version TEXT NOT NULL, "
            "created_utc TEXT NOT NULL, schema_version INTEGER NOT NULL);"
            "CREATE TABLE _xlibrary_profile_counts (name TEXT PRIMARY KEY NOT NULL, row_count INTEGER NOT NULL);"
            "CREATE TABLE _xlibrary_profile_settings (key TEXT PRIMARY KEY NOT NULL, value BLOB NOT NULL);");
        Statement manifest(connection.get(), "INSERT INTO _xlibrary_profile_manifest VALUES (?1,?2,?3,?4,?5);");
        manifest.bind(1, QLatin1String(format_id));
        manifest.bind(2, static_cast<qint64>(format_version));
        const auto version = QCoreApplication::applicationVersion();
        if (version.isEmpty())
            fail("The application version is unavailable");
        manifest.bind(3, version);
        manifest.bind(4, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        manifest.bind(5, static_cast<qint64>(xlibrary::profile_schema_version));
        (void)manifest.next();
        Statement count(connection.get(), "INSERT INTO _xlibrary_profile_counts VALUES (?1,?2);");
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            count.bind(1, it.key());
            count.bind(2, it.value());
            (void)count.next();
            count.reset();
        }
        Statement setting(connection.get(), "INSERT INTO _xlibrary_profile_settings VALUES (?1,?2);");
        qsizetype total_bytes = 0;
        if (settings.size() > settings_count_limit)
            fail("The profile has too many settings");
        for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
            const auto bytes = profile_settings::encode(it.value());
            total_bytes += bytes.size();
            if (it.key().isEmpty() || it.key().size() > 32768 || total_bytes > settings_bytes_limit)
                fail("The profile settings exceed backup format limits");
            setting.bind(1, it.key());
            setting.bind(2, bytes);
            (void)setting.next();
            setting.reset();
        }
        connection.execute("COMMIT; PRAGMA journal_mode=DELETE;");
        integrity_check(connection.get());
    }
    require_self_contained(absolute_path(backup_path));
    copy_file(staged_database, absolute_path(backup_path));
}

profile_manifest ProfileBackup::inspect(const QString &path) {
    QTemporaryDir staging;
    if (!staging.isValid())
        fail("Cannot create a profile inspection directory");
    return prepare_backup(absolute_path(path), staging.filePath("profile.sqlite")).manifest;
}

QString ProfileBackup::restore(const QString &path, const QString &database_path, const QString &settings_path,
                               const QByteArray &expected_fingerprint) {
    const auto database = absolute_path(database_path);
    const auto settings = absolute_path(settings_path);
    validate_destinations(database, settings);
    ensure_distinct_backup(path, database, settings);
    if (expected_fingerprint.size() != 32)
        fail("Inspect the profile backup before restoring it");
    QLockFile lock(database + ".profile-restore.lock");
    lock_profile(lock);
    recover_locked(database, settings);
    QTemporaryDir database_staging(QFileInfo(database).absolutePath() + "/.xlibrary-restore-XXXXXX");
    QTemporaryDir settings_staging(QFileInfo(settings).absolutePath() + "/.xlibrary-restore-XXXXXX");
    if (!database_staging.isValid() || !settings_staging.isValid())
        fail("Cannot prepare the profile restoration");
    const auto staged_database = database_staging.filePath("database.sqlite");
    const auto staged_settings = settings_staging.filePath("settings.ini");
    auto prepared = prepare_backup(absolute_path(path), staged_database);
    if (prepared.manifest.fingerprint != expected_fingerprint || fingerprint(path) != expected_fingerprint)
        fail("The profile backup changed after preview; inspect it again");
    bool local_token = false;
    bool local_settings_valid = true;
    if (QFileInfo::exists(settings)) {
        QSettings local(settings, QSettings::IniFormat);
        local.setFallbacksEnabled(false);
        local_token = local.value(profile_settings::token_marker, false).toBool();
        // Повреждённый INI сохраняется raw; неизвестный маркер не объявляет секрет существующим.
        if (local.status() != QSettings::NoError) {
            local_token = false;
            local_settings_valid = false;
        }
    }
    prepared.settings.insert(profile_settings::token_marker, local_token);
    prepared.settings.insert("profile/autostart_pending", true);
    profile_settings::write(staged_settings, prepared.settings);
    const auto recovery_path = QFileInfo(database).absolutePath() + "/.xlibrary-recovery-" +
                               QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ") + "-" +
                               QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(recovery_path))
        fail("Cannot create the profile recovery directory");
    QFile::setPermissions(recovery_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    auto files = profile_files(database, settings);
    bool journal_written = false;
    try {
        QJsonArray saved;
        for (auto &file : files) {
            file.existed = QFileInfo::exists(file.target);
            if (file.existed) {
                if (!QFileInfo(file.target).isFile())
                    fail("A current profile file is not a regular file");
                copy_file(file.target, QDir(recovery_path).filePath(file.saved_name));
            }
            saved.append(QJsonObject{{"name", file.saved_name}, {"existed", file.existed}});
        }
        if (files.front().existed && local_settings_valid) {
            try {
                // SQLite может восстановить hot journal или изменить shm даже при чтении.
                // Для полной копии используем ещё один экземпляр, сохраняя raw-файлы побайтно.
                QTemporaryDir previous_staging(recovery_path + "/.previous-XXXXXX");
                if (!previous_staging.isValid())
                    fail("Cannot prepare the previous profile snapshot");
                for (const auto &file : files) {
                    if (file.existed) {
                        const auto copied = previous_staging.filePath(file.saved_name);
                        copy_file(QDir(recovery_path).filePath(file.saved_name), copied);
                        if (!QFile::setPermissions(copied, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
                            fail("Cannot prepare the previous profile snapshot file");
                    }
                }
                const auto previous_database = previous_staging.filePath("database.sqlite");
                {
                    Connection previous(previous_database, true);
                    integrity_check(previous.get());
                }
                create(QDir(recovery_path).filePath("previous.xlprofile"), previous_database,
                       previous_staging.filePath("settings.ini"));
            } catch (const InvalidDatabase &) {
                // Для повреждённой БД единственная честная копия — исходные файлы вместе с журналами.
                write_json(QDir(recovery_path).filePath("raw-recovery.json"),
                           {{"full_backup_available", false}, {"reason", "unreadable_or_unsupported_profile"}});
            }
        } else if (!local_settings_valid) {
            write_json(QDir(recovery_path).filePath("raw-recovery.json"), {{"settings_valid", false}});
        }
        QJsonObject journal{{"version", 1},
                            {"database_path", database},
                            {"settings_path", settings},
                            {"recovery_path", recovery_path},
                            {"files", saved},
                            {"state", "prepared"}};
        write_json(journal_path(database), journal);
        journal_written = true;
        for (const auto *suffix : {"-wal", "-shm", "-journal"})
            remove_file(database + QLatin1String(suffix));
        copy_file(staged_database, database);
        copy_file(staged_settings, settings);
        journal.insert("state", "committed");
        write_json(journal_path(database), journal);
        journal_written = false;
        // После durable commit ошибка удаления журнала не превращает успешную замену в rollback.
        (void)QFile::remove(journal_path(database));
    } catch (const std::exception &error) {
        QString rollback_error;
        if (journal_written) {
            try {
                rollback_files(database, recovery_path, files);
                remove_file(journal_path(database));
            } catch (const std::exception &rollback) {
                rollback_error = QString::fromUtf8(rollback.what());
            }
        }
        auto message = QString::fromUtf8(error.what()) + "\nProfile recovery directory: " + recovery_path;
        if (!rollback_error.isEmpty())
            message += "\nRollback is incomplete: " + rollback_error;
        fail(message);
    }
    return recovery_path;
}

void ProfileBackup::recover(const QString &database_path, const QString &settings_path) {
    const auto database = absolute_path(database_path);
    const auto settings = absolute_path(settings_path);
    if (!QFileInfo::exists(journal_path(database)))
        return;
    validate_destinations(database, settings);
    QLockFile lock(database + ".profile-restore.lock");
    lock_profile(lock);
    recover_locked(database, settings);
}
