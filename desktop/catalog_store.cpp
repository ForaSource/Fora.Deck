#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <stdexcept>

#include "catalog_store.h"
#include "LaunchIdentity.h"
#include "localization.h"
#include "path_helpers.h"
#include "ProfileSchema.h"
#include "search_query.h"

void CatalogStore::validate_application(const app_record &record) {
    if (record.name.trimmed().isEmpty() || record.name.size() > 256 || !QFileInfo(record.path).isAbsolute() ||
        record.name.contains(QChar::Null) || record.path.contains(QChar::Null) ||
        record.arguments.contains(QChar::Null) || record.working_directory.contains(QChar::Null)) {
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Invalid application name or path.").toStdString());
    }
#ifdef Q_OS_WIN
    if (QFileInfo(record.path).suffix().isEmpty())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "A Windows target requires a filename extension.").toStdString());
#endif
    if (!record.working_directory.isEmpty() && !QFileInfo(record.working_directory).isAbsolute()) {
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The working directory must be an absolute path.").toStdString());
    }
    if (record.path.size() > catalog_limits::text_length || record.arguments.size() > catalog_limits::text_length ||
        record.working_directory.size() > catalog_limits::text_length ||
        record.tags.size() > catalog_limits::tags_per_application) {
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Paths and arguments: up to 32768 characters; tags: up to 256.")
                .toStdString());
    }
    if (record.aliases.size() > 32)
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Use up to 32 aliases, each on one line with up to 256 characters.")
                .toStdString());
    for (const auto &alias : record.aliases)
        if (alias.trimmed().isEmpty() || alias.size() > 256 || alias.contains(QChar::Null) || alias.contains('\n') ||
            alias.contains('\r'))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary",
                                            "Use up to 32 aliases, each on one line with up to 256 characters.")
                    .toStdString());
    for (const auto &tag : record.tags) {
        if (!valid_tag_name(tag))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Tag: up to 64 characters, without spaces, commas, or #.")
                    .toStdString());
    }
}

namespace {
class Statement {
    sqlite3 *connection;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{nullptr, &sqlite3_finalize};

public:
    Statement(sqlite3 *connection, const char *sql) : connection(connection) {
        sqlite3_stmt *raw = nullptr;
        const auto result = sqlite3_prepare_v2(connection, sql, -1, &raw, nullptr);
        statement.reset(raw);
        if (result != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(connection));
        }
    }
    void bind(int index, const QString &value) {
        const auto utf8 = value.toUtf8();
        if (sqlite3_bind_text64(statement.get(), index, utf8.constData(), utf8.size(), SQLITE_TRANSIENT, SQLITE_UTF8) !=
            SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(connection));
        }
    }
    void bind(int index, qint64 value) {
        if (sqlite3_bind_int64(statement.get(), index, value) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(connection));
        }
    }
    bool next() {
        const auto result = sqlite3_step(statement.get());
        if (result != SQLITE_ROW && result != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(connection));
        }
        return result == SQLITE_ROW;
    }
    QString text(int column) const {
        const auto *value = sqlite3_column_text(statement.get(), column);
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value),
                                         sqlite3_column_bytes(statement.get(), column))
                     : QString{};
    }
    qint64 number(int column) const { return sqlite3_column_int64(statement.get(), column); }
};

class Transaction {
    sqlite3 *connection;
    bool committed = false;

public:
    explicit Transaction(sqlite3 *connection) : connection(connection) {
        if (sqlite3_exec(connection, "SAVEPOINT desktop_write;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(connection));
        }
    }
    ~Transaction() {
        if (!committed) {
            sqlite3_exec(connection, "ROLLBACK TO desktop_write; RELEASE desktop_write;", nullptr, nullptr, nullptr);
        }
    }
    void commit() {
        if (sqlite3_exec(connection, "RELEASE desktop_write;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(connection));
        }
        committed = true;
    }
};
}

void CatalogStore::execute(const char *sql) const {
    if (sqlite3_exec(database.get_connection(), sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database.get_connection()));
    }
}

CatalogStore::CatalogStore(const QString &database_path) : database(filesystem_path(database_path), false) {
    xlibrary::validate_profile(database.get_connection());
}

int CatalogStore::schema_version() const {
    return xlibrary::read_schema_version(database.get_connection());
}

void CatalogStore::backup_database(const QString &destination) const {
    xlibrary::backup_database(database.get_connection(), filesystem_path(destination));
}

QString CatalogStore::restore_backup(const QString &source, const QString &destination) {
    const auto fail = [](const QString &message) { throw std::runtime_error(message.toStdString()); };
    if (!QFileInfo(source).isFile() ||
        QFileInfo(source).canonicalFilePath() == QFileInfo(destination).canonicalFilePath())
        fail(QCoreApplication::translate("XLibrary", "Select a different, existing SQLite backup file."));
    const auto parent = QFileInfo(destination).absolutePath();
    if (!QDir().mkpath(parent))
        fail(QCoreApplication::translate("XLibrary", "Could not create the library folder."));
    QTemporaryDir staging(parent + "/.xlibrary-recovery-XXXXXX");
    if (!staging.isValid())
        fail(QCoreApplication::translate("XLibrary", "Could not create a recovery folder."));
    const auto restored = staging.filePath("restored.db");
    {
        sqlite3 *raw_source = nullptr;
        const auto opened = sqlite3_open_v2(source.toUtf8().constData(), &raw_source, SQLITE_OPEN_READONLY, nullptr);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> input(raw_source, &sqlite3_close_v2);
        if (opened != SQLITE_OK)
            fail(QCoreApplication::translate("XLibrary", "Could not open the SQLite backup."));
        Statement check(input.get(), "PRAGMA quick_check;");
        if (!check.next() || check.text(0) != "ok")
            fail(QCoreApplication::translate("XLibrary", "The SQLite backup is damaged."));
        Statement schema(input.get(), "SELECT a.id,a.name,t.id,t.name,at.application_id,at.tag_id "
                                      "FROM applications a, tags t, application_tags at LIMIT 0;");
        schema.next();
        sqlite3 *raw_destination = nullptr;
        const auto created = sqlite3_open(restored.toUtf8().constData(), &raw_destination);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> output(raw_destination, &sqlite3_close_v2);
        if (created != SQLITE_OK)
            fail(QCoreApplication::translate("XLibrary", "Could not prepare the restored library."));
        auto *backup = sqlite3_backup_init(output.get(), "main", input.get(), "main");
        if (!backup)
            fail(QCoreApplication::translate("XLibrary", "Could not read the SQLite backup."));
        const auto copied = sqlite3_backup_step(backup, -1);
        const auto finished = sqlite3_backup_finish(backup);
        if (copied != SQLITE_DONE || finished != SQLITE_OK)
            fail(QCoreApplication::translate("XLibrary", "Could not copy the SQLite backup."));
    }
    // Проверяем схему и миграцию в отдельном файле, прежде чем трогать исходную БД.
    {
        CatalogStore candidate(restored);
        candidate.read();
    }
    QVector<QPair<QString, QString>> moved;
    for (const auto &suffix : {QString{}, QString("-wal"), QString("-shm"), QString("-journal")}) {
        const auto original = destination + suffix;
        if (!QFileInfo::exists(original))
            continue;
        const auto saved = staging.filePath("previous.db" + suffix);
        if (!QFile::rename(original, saved)) {
            for (auto it = moved.crbegin(); it != moved.crend(); ++it)
                QFile::rename(it->second, it->first);
            // При сбое отката сохраняем папку с исходными файлами для ручного восстановления.
            staging.setAutoRemove(false);
            fail(QCoreApplication::translate("XLibrary", "Could not preserve the current library. Recovery files: %1")
                     .arg(staging.path()));
        }
        moved.append({original, saved});
    }
    if (!QFile::rename(restored, destination)) {
        for (auto it = moved.crbegin(); it != moved.crend(); ++it)
            QFile::rename(it->second, it->first);
        staging.setAutoRemove(false);
        fail(QCoreApplication::translate("XLibrary", "Could not install the restored library. Recovery files: %1")
                 .arg(staging.path()));
    }
    if (!moved.isEmpty()) {
        staging.setAutoRemove(false);
        return staging.path();
    }
    return {};
}

catalog_snapshot CatalogStore::read() const {
    catalog_snapshot result;
    auto *connection = database.get_connection();
    Statement applications(connection, R"(
        SELECT a.id, a.name, c.path, c.arguments, c.working_directory,
               coalesce(d.favorite,0), coalesce(d.last_launch,0),c.id,c.target_id
        FROM applications a JOIN launch_configuration c ON c.id=a.configuration_id
        LEFT JOIN desktop_application d ON a.id = d.application_id ORDER BY a.id;
    )");
    QHash<qint64, qsizetype> rows;
    while (applications.next()) {
        app_record record;
        record.id = applications.number(0);
        record.name = applications.text(1);
        record.path = QDir::fromNativeSeparators(applications.text(2));
        record.arguments = applications.text(3);
        record.working_directory = applications.text(4);
        record.favorite = applications.number(5) != 0;
        record.last_launch = applications.number(6);
        record.configuration_id = applications.number(7);
        record.target_id = applications.number(8);
        record.available = QFileInfo(record.path).isFile();
        rows.insert(record.id, result.applications.size());
        result.applications.append(std::move(record));
    }
    Statement aliases(connection,
                      "SELECT application_id,name FROM application_alias ORDER BY application_id,position;");
    while (aliases.next()) {
        const auto row = rows.constFind(aliases.number(0));
        if (row != rows.cend())
            result.applications[*row].aliases.append(aliases.text(1));
    }
    Statement filters(
        connection,
        "SELECT id,name,query,section,tag,sort_mode,sort_order FROM saved_filter ORDER BY name COLLATE NOCASE,id;");
    while (filters.next())
        result.filters.append({filters.number(0), filters.text(1), filters.text(2), filters.text(3), filters.text(4),
                               static_cast<int>(filters.number(5)), static_cast<Qt::SortOrder>(filters.number(6))});
    Statement sources(connection, "SELECT a.id,s.source_key FROM applications a JOIN discovery_source s "
                                  "ON s.configuration_id=a.configuration_id ORDER BY s.kind,s.source_key;");
    while (sources.next()) {
        const auto row = rows.constFind(sources.number(0));
        if (row != rows.cend()) {
            auto &record = result.applications[*row];
            record.discovery_sources.append(sources.text(1));
            if (record.discovery_source.isEmpty())
                record.discovery_source = sources.text(1);
        }
    }
    Statement tags(connection, "SELECT name FROM tags ORDER BY name COLLATE NOCASE;");
    while (tags.next()) {
        result.tags.append(tags.text(0));
    }
    Statement links(
        connection,
        "SELECT at.application_id, t.name FROM application_tags at JOIN tags t ON t.id = at.tag_id ORDER BY t.name;");
    while (links.next()) {
        const auto row = rows.constFind(links.number(0));
        if (row != rows.cend()) {
            result.applications[*row].tags.append(links.text(1));
        }
    }
    Statement scenarios(connection, "SELECT id, name FROM desktop_scenario ORDER BY name;");
    while (scenarios.next()) {
        scenario_record record{scenarios.number(0), scenarios.text(1), {}};
        Statement steps(connection, "SELECT application_id, delay_ms, enabled FROM desktop_scenario_step WHERE "
                                    "scenario_id = ? ORDER BY position;");
        steps.bind(1, record.id);
        while (steps.next()) {
            record.steps.append({steps.number(0), static_cast<int>(steps.number(1)), steps.number(2) != 0});
        }
        result.scenarios.append(std::move(record));
    }
    return result;
}

void CatalogStore::write_metadata(qint64 id, const app_record &record) {
    auto *connection = database.get_connection();
    Statement metadata(connection, R"(
        INSERT INTO desktop_application(application_id, favorite) VALUES(?,?)
        ON CONFLICT(application_id) DO UPDATE SET favorite=excluded.favorite;
    )");
    metadata.bind(1, id);
    metadata.bind(2, record.favorite);
    metadata.next();
    Statement clear_aliases(connection, "DELETE FROM application_alias WHERE application_id=?;");
    clear_aliases.bind(1, id);
    clear_aliases.next();
    for (qsizetype position = 0; position < record.aliases.size(); ++position) {
        Statement alias(connection, "INSERT INTO application_alias(application_id,position,name) VALUES(?,?,?);");
        alias.bind(1, id);
        alias.bind(2, static_cast<qint64>(position));
        alias.bind(3, record.aliases[position]);
        alias.next();
    }
    Statement clear(connection, "DELETE FROM application_tags WHERE application_id = ?;");
    clear.bind(1, id);
    clear.next();
    QHash<QString, QString> known_tags;
    Statement existing_tags(connection, "SELECT name FROM tags;");
    while (existing_tags.next()) {
        const auto name = existing_tags.text(0);
        known_tags.insert(name.toCaseFolded(), name);
    }
    for (const auto &tag : record.tags) {
        const auto canonical_tag = known_tags.value(tag.toCaseFolded(), tag);
        Statement create(connection, "INSERT OR IGNORE INTO tags(name) VALUES(?);");
        create.bind(1, canonical_tag);
        create.next();
        Statement link(connection, "INSERT OR IGNORE INTO application_tags SELECT ?, id FROM tags WHERE name = ?;");
        link.bind(1, id);
        link.bind(2, canonical_tag);
        link.next();
        known_tags.insert(tag.toCaseFolded(), canonical_tag);
    }
}

void CatalogStore::write_source(qint64 configuration_id, const app_record &record) {
    if (record.discovery_source.isEmpty())
        return;
    Statement source(database.get_connection(),
                     "INSERT OR IGNORE INTO discovery_source(configuration_id,kind,source_key) VALUES(?,?,?);");
    source.bind(1, configuration_id);
    source.bind(2, QFileInfo(record.discovery_source).suffix().compare("desktop", Qt::CaseInsensitive) == 0
                       ? QString("desktop")
                   : QFileInfo(record.discovery_source).suffix().compare("lnk", Qt::CaseInsensitive) == 0
                       ? QString("shortcut")
                       : QString("discovery"));
    source.bind(3, record.discovery_source);
    source.next();
}

qint64 CatalogStore::save_application(const app_record &record) {
    validate_application(record);
    auto *connection = database.get_connection();
    Transaction transaction(connection);
    const auto configuration_id =
        xlibrary::ensure_launch_configuration(connection, filesystem_path(record.path), record.arguments.toStdString(),
                                              filesystem_path(record.working_directory));
    qint64 id = record.id;
    if (id == 0) {
        Statement insert(connection, "INSERT INTO applications(name,configuration_id) VALUES(?,?);");
        insert.bind(1, record.name);
        insert.bind(2, configuration_id);
        insert.next();
        id = sqlite3_last_insert_rowid(connection);
        write_source(configuration_id, record);
    } else {
        Statement update(connection, "UPDATE applications SET name=?,configuration_id=? WHERE id=?;");
        update.bind(1, record.name);
        update.bind(2, configuration_id);
        update.bind(3, id);
        update.next();
        if (sqlite3_changes(connection) == 0)
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "The application has already been removed from the library.")
                    .toStdString());
    }
    write_metadata(id, record);
    transaction.commit();
    return id;
}

void CatalogStore::remove_application(qint64 id) {
    remove_applications({id});
}

void CatalogStore::remove_applications(const QVector<qint64> &ids) {
    const QSet<qint64> selected(ids.cbegin(), ids.cend());
    if (selected.isEmpty())
        return;
    auto *connection = database.get_connection();
    Transaction transaction(connection);
    auto backup = read();
    QHash<qint64, qint64> configurations;
    for (const auto &record : backup.applications)
        configurations.insert(record.id, record.configuration_id);
    backup.applications.erase(std::remove_if(backup.applications.begin(), backup.applications.end(),
                                             [&](const app_record &record) { return !selected.contains(record.id); }),
                              backup.applications.end());
    if (backup.applications.size() != selected.size())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The selection changed. Select applications again.").toStdString());
    backup.scenarios.erase(std::remove_if(backup.scenarios.begin(), backup.scenarios.end(),
                                          [&](const scenario_record &record) {
                                              return std::none_of(record.steps.cbegin(), record.steps.cend(),
                                                                  [&](const scenario_step &step) {
                                                                      return selected.contains(step.application_id);
                                                                  });
                                          }),
                           backup.scenarios.end());
    for (const auto id : selected) {
        Statement statement(connection, "DELETE FROM applications WHERE id=?;");
        statement.bind(1, id);
        statement.next();
    }
    transaction.commit();
    last_removal = std::move(backup);
    removal_configurations = std::move(configurations);
}

void CatalogStore::undo_removal() {
    if (!can_undo_removal())
        return;
    auto *connection = database.get_connection();
    Transaction transaction(connection);
    const auto current = read();
    QSet<qint64> deleted_ids;
    for (const auto &record : last_removal.applications)
        deleted_ids.insert(record.id);
    const auto fail = [] {
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary",
                                        "The library or a linked scenario changed. Undo would overwrite changes.")
                .toStdString());
    };
    for (const auto &before : last_removal.scenarios) {
        const auto now = std::find_if(current.scenarios.cbegin(), current.scenarios.cend(),
                                      [&](const scenario_record &record) { return record.id == before.id; });
        if (now == current.scenarios.cend())
            fail();
        qsizetype index = 0;
        for (const auto &step : before.steps) {
            if (deleted_ids.contains(step.application_id))
                continue;
            if (index >= now->steps.size())
                fail();
            const auto &other = now->steps[index++];
            if (step.application_id != other.application_id || step.delay_ms != other.delay_ms ||
                step.enabled != other.enabled)
                fail();
        }
        if (index != now->steps.size())
            fail();
    }
    for (const auto &record : last_removal.applications) {
        Statement conflict(connection, "SELECT id FROM applications WHERE id=? OR configuration_id=?;");
        conflict.bind(1, record.id);
        conflict.bind(2, record.configuration_id);
        while (conflict.next())
            if (conflict.number(0) == record.id ||
                removal_configurations.value(conflict.number(0)) != record.configuration_id)
                fail();
        Statement insert(connection, "INSERT INTO applications(id,name,configuration_id) VALUES(?,?,?);");
        insert.bind(1, record.id);
        insert.bind(2, record.name);
        insert.bind(3, record.configuration_id);
        insert.next();
        write_metadata(record.id, record);
        Statement history(connection, "UPDATE desktop_application SET last_launch=? WHERE application_id=?;");
        history.bind(1, record.last_launch);
        history.bind(2, record.id);
        history.next();
    }
    for (const auto &scenario : last_removal.scenarios) {
        Statement clear(connection, "DELETE FROM desktop_scenario_step WHERE scenario_id=?;");
        clear.bind(1, scenario.id);
        clear.next();
        for (qsizetype index = 0; index < scenario.steps.size(); ++index) {
            const auto &step = scenario.steps[index];
            Statement insert(connection, "INSERT INTO desktop_scenario_step VALUES(?,?,?,?,?);");
            insert.bind(1, scenario.id);
            insert.bind(2, index);
            insert.bind(3, step.application_id);
            insert.bind(4, step.delay_ms);
            insert.bind(5, step.enabled);
            insert.next();
        }
    }
    transaction.commit();
    last_removal = {};
    removal_configurations.clear();
}

void CatalogStore::update_metadata(const QVector<qint64> &ids, const QStringList &add_tags,
                                   const QStringList &remove_tags, int favorite) {
    const QSet<qint64> selected(ids.cbegin(), ids.cend());
    if (selected.isEmpty())
        return;
    Transaction transaction(database.get_connection());
    const auto current = read();
    qsizetype updated_count = 0;
    for (auto record : current.applications) {
        if (!selected.contains(record.id))
            continue;
        for (const auto &tag : remove_tags)
            record.tags.removeIf([&](const QString &name) { return name.toCaseFolded() == tag.toCaseFolded(); });
        for (const auto &tag : add_tags)
            if (!record.tags.contains(tag, Qt::CaseInsensitive))
                record.tags.append(tag);
        if (favorite >= 0)
            record.favorite = favorite != 0;
        validate_application(record);
        write_metadata(record.id, record);
        ++updated_count;
    }
    if (updated_count != selected.size())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The selection changed. Select applications again.").toStdString());
    transaction.commit();
}

void CatalogStore::save_filter(const saved_filter &record) {
    if (record.name.trimmed().isEmpty() || record.name.size() > 128 || record.name.contains(QChar::Null) ||
        record.query.size() > catalog_limits::text_length || record.query.contains(QChar::Null) ||
        (record.section != "library" && record.section != "favorites" && record.section != "running") ||
        (!record.tag.isEmpty() && !valid_tag_name(record.tag)) || record.sort_mode < 0 || record.sort_mode > 4 ||
        (record.sort_order != Qt::AscendingOrder && record.sort_order != Qt::DescendingOrder))
        throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Invalid saved filter.").toStdString());
    Transaction transaction(database.get_connection());
    Statement names(database.get_connection(), "SELECT id,name FROM saved_filter;");
    while (names.next())
        if (names.number(0) != record.id && names.text(1).toCaseFolded() == record.name.toCaseFolded())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "A filter with this name already exists.").toStdString());
    Statement write(database.get_connection(),
                    record.id
                        ? "UPDATE saved_filter SET name=?,query=?,section=?,tag=?,sort_mode=?,sort_order=? WHERE id=?;"
                        : "INSERT INTO saved_filter(name,query,section,tag,sort_mode,sort_order) VALUES(?,?,?,?,?,?);");
    write.bind(1, record.name);
    write.bind(2, record.query);
    write.bind(3, record.section);
    write.bind(4, record.tag);
    write.bind(5, record.sort_mode);
    write.bind(6, static_cast<int>(record.sort_order));
    if (record.id)
        write.bind(7, record.id);
    write.next();
    transaction.commit();
}

void CatalogStore::remove_filter(qint64 id) {
    Statement remove(database.get_connection(), "DELETE FROM saved_filter WHERE id=?;");
    remove.bind(1, id);
    remove.next();
}

void CatalogStore::update_filter_tags(const QString &old_name, const QString &new_name) {
    if (old_name.isEmpty())
        return;
    Statement filters(database.get_connection(), "SELECT id,query,tag FROM saved_filter;");
    while (filters.next()) {
        const auto query = search_query::rewrite_tag(filters.text(1), old_name, new_name);
        const auto tag = filters.text(2).compare(old_name, Qt::CaseInsensitive) == 0 ? new_name : filters.text(2);
        Statement update(database.get_connection(), "UPDATE saved_filter SET query=?,tag=? WHERE id=?;");
        update.bind(1, query);
        update.bind(2, tag);
        update.bind(3, filters.number(0));
        update.next();
    }
}

void CatalogStore::save_tag(const QString &old_name, const QString &new_name) {
    if (!valid_tag_name(new_name))
        throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Invalid tag name.").toStdString());
    Transaction transaction(database.get_connection());
    Statement existing(database.get_connection(), "SELECT name FROM tags;");
    while (existing.next()) {
        if (existing.text(0) != old_name && existing.text(0).toCaseFolded() == new_name.toCaseFolded()) {
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "A tag with this name already exists.").toStdString());
        }
    }
    Statement statement(database.get_connection(), old_name.isEmpty() ? "INSERT INTO tags(name) VALUES(?);"
                                                                      : "UPDATE tags SET name = ? WHERE name = ?;");
    statement.bind(1, new_name);
    if (!old_name.isEmpty()) {
        statement.bind(2, old_name);
    }
    statement.next();
    update_filter_tags(old_name, new_name);
    transaction.commit();
}

void CatalogStore::remove_tag(const QString &name) {
    Transaction transaction(database.get_connection());
    Statement statement(database.get_connection(), "DELETE FROM tags WHERE name = ?;");
    statement.bind(1, name);
    statement.next();
    update_filter_tags(name, {});
    transaction.commit();
}

qint64 CatalogStore::save_scenario(const scenario_record &record) {
    if (record.name.trimmed().isEmpty() || record.name.size() > 128 || record.steps.isEmpty() ||
        record.steps.size() > 1000) {
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Enter a scenario name and between 1 and 1000 steps.")
                .toStdString());
    }
    auto *connection = database.get_connection();
    Transaction transaction(connection);
    auto id = record.id;
    if (id == 0) {
        Statement insert(connection, "INSERT INTO desktop_scenario(name) VALUES(?);");
        insert.bind(1, record.name);
        insert.next();
        id = sqlite3_last_insert_rowid(connection);
    } else {
        Statement update(connection, "UPDATE desktop_scenario SET name = ? WHERE id = ?;");
        update.bind(1, record.name);
        update.bind(2, id);
        update.next();
        Statement clear(connection, "DELETE FROM desktop_scenario_step WHERE scenario_id = ?;");
        clear.bind(1, id);
        clear.next();
    }
    for (qsizetype index = 0; index < record.steps.size(); ++index) {
        const auto &step = record.steps[index];
        Statement insert(connection, "INSERT INTO desktop_scenario_step VALUES(?,?,?,?,?);");
        insert.bind(1, id);
        insert.bind(2, index);
        insert.bind(3, step.application_id);
        insert.bind(4, step.delay_ms);
        insert.bind(5, step.enabled);
        insert.next();
    }
    transaction.commit();
    return id;
}

void CatalogStore::remove_scenario(qint64 id) {
    Statement statement(database.get_connection(), "DELETE FROM desktop_scenario WHERE id = ?;");
    statement.bind(1, id);
    statement.next();
}

void CatalogStore::record_launch(qint64 id) {
    Statement statement(database.get_connection(),
                        "INSERT INTO desktop_application(application_id,last_launch) VALUES(?,?) ON "
                        "CONFLICT(application_id) DO UPDATE SET last_launch=excluded.last_launch;");
    statement.bind(1, id);
    statement.bind(2, QDateTime::currentSecsSinceEpoch());
    statement.next();
}

QString CatalogStore::path() const {
    return path_text(database.get_file_path());
}

void CatalogStore::clear_history() {
    execute("UPDATE desktop_application SET last_launch = 0;");
}

catalog_import_result CatalogStore::import_records(const QVector<app_record> &records, discovery_control *control,
                                                   const std::function<void(qsizetype, qsizetype)> &progress,
                                                   QVector<discovery_issue> *issues, import_conflict_policy policy) {
    QVector<const app_record *> candidates;
    candidates.reserve(records.size());
    for (const auto &record : records) {
        if (control && control->phase.load() == discovery_phase::cancelling)
            throw DiscoveryCancelled();
        try {
            validate_application(record);
            candidates.append(&record);
        } catch (const std::invalid_argument &error) {
            if (!issues)
                throw;
            issues->append(discovery_issue{record.discovery_source.isEmpty() ? record.path : record.discovery_source,
                                           QString::fromUtf8(error.what())});
        }
    }
    Transaction transaction(database.get_connection());
    catalog_import_result result;
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        if (control && control->phase.load() == discovery_phase::cancelling)
            throw DiscoveryCancelled();
        if (progress)
            progress(index, candidates.size());
        const auto &record = *candidates[index];
        const auto configuration_id = xlibrary::ensure_launch_configuration(
            database.get_connection(), filesystem_path(record.path), record.arguments.toStdString(),
            filesystem_path(record.working_directory));
        write_source(configuration_id, record);
        Statement duplicate(database.get_connection(), "SELECT id FROM applications WHERE configuration_id=? LIMIT 1;");
        duplicate.bind(1, configuration_id);
        if (duplicate.next()) {
            if (policy == import_conflict_policy::replace_metadata) {
                const auto id = duplicate.number(0);
                Statement update(database.get_connection(), "UPDATE applications SET name=? WHERE id=?;");
                update.bind(1, record.name);
                update.bind(2, id);
                update.next();
                write_metadata(id, record);
                ++result.updated;
                continue;
            }
            ++result.duplicates;
            if (issues)
                issues->append(discovery_issue{
                    record.discovery_source.isEmpty() ? record.path : record.discovery_source,
                    QCoreApplication::translate("XLibrary", "This launch configuration is already in the library."),
                    discovery_issue_kind::duplicate});
            continue;
        }
        Statement insert(database.get_connection(), "INSERT INTO applications(name,configuration_id) VALUES(?,?);");
        insert.bind(1, record.name);
        insert.bind(2, configuration_id);
        insert.next();
        const auto id = sqlite3_last_insert_rowid(database.get_connection());
        write_metadata(id, record);
        ++result.added;
    }
    if (control) {
        // The only cancellation boundary: a request before this point rolls back the entire transaction.
        auto expected = discovery_phase::running;
        if (!control->phase.compare_exchange_strong(expected, discovery_phase::committing))
            throw DiscoveryCancelled();
    }
    transaction.commit();
    if (progress)
        progress(candidates.size(), candidates.size());
    return result;
}

QVector<path_relocation> CatalogStore::preview_relocation(const QString &old_root, const QString &new_root) const {
    if (!QFileInfo(old_root).isAbsolute() || !QFileInfo(new_root).isAbsolute() || !QFileInfo(new_root).isDir())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Choose an absolute old folder and an existing new folder.")
                .toStdString());
    const auto old_prefix = QDir::cleanPath(QDir::fromNativeSeparators(old_root));
    const auto new_prefix = QDir::cleanPath(QDir::fromNativeSeparators(new_root));
    const auto replace_root = [&](const QString &path) -> QString {
        if (path.isEmpty())
            return {};
        const auto clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
        if (clean.compare(old_prefix, path_case_sensitivity) == 0)
            return new_prefix;
        const auto prefix = old_prefix.endsWith('/') ? old_prefix : old_prefix + '/';
        if (!clean.startsWith(prefix, path_case_sensitivity))
            return path;
        return QDir(new_prefix).filePath(clean.mid(prefix.size()));
    };
    QVector<path_relocation> result;
    const auto snapshot = read();
    for (const auto &record : snapshot.applications) {
        if (record.available)
            continue;
        const auto replacement = replace_root(record.path);
        if (replacement == record.path)
            continue;
        path_relocation change{record.id,
                               record.name,
                               record.path,
                               replacement,
                               record.working_directory,
                               replace_root(record.working_directory),
                               {}};
        if (!QFileInfo(replacement).isFile())
            change.issue = QCoreApplication::translate("XLibrary", "The replacement file was not found.");
#ifdef Q_OS_LINUX
        else if (QFileInfo(replacement).suffix() != "desktop" && !QFileInfo(replacement).isExecutable())
            change.issue = QCoreApplication::translate("XLibrary", "The replacement file is not executable.");
#endif
        if (change.issue.isEmpty() && !change.new_directory.isEmpty() && !QFileInfo(change.new_directory).isDir())
            change.issue = QCoreApplication::translate("XLibrary", "The replacement working directory was not found.");
        change.uses_existing_configuration =
            std::any_of(snapshot.applications.cbegin(), snapshot.applications.cend(), [&](const app_record &other) {
                return other.id != record.id && normalized_path(other.path) == normalized_path(change.new_path) &&
                       other.arguments == record.arguments &&
                       normalized_path(other.working_directory) == normalized_path(change.new_directory);
            });
        result.append(std::move(change));
    }
    return result;
}

void CatalogStore::apply_relocation(const QVector<path_relocation> &changes) {
    if (changes.isEmpty())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "No paths selected for recovery.").toStdString());
    Transaction transaction(database.get_connection());
    const auto current = read();
    QSet<qint64> seen;
    for (const auto &change : changes) {
        const auto record = std::find_if(current.applications.cbegin(), current.applications.cend(),
                                         [&](const app_record &item) { return item.id == change.application_id; });
        if (record == current.applications.cend() || record->path != change.old_path ||
            record->working_directory != change.old_directory || seen.contains(change.application_id))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The library changed. Preview path recovery again.")
                    .toStdString());
        seen.insert(change.application_id);
        if (!change.issue.isEmpty() || !QFileInfo(change.new_path).isFile())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "A replacement application file is unavailable.")
                    .toStdString());
#ifdef Q_OS_LINUX
        if (QFileInfo(change.new_path).suffix() != "desktop" && !QFileInfo(change.new_path).isExecutable())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The replacement file is not executable.").toStdString());
#endif
        auto updated = *record;
        updated.path = change.new_path;
        updated.working_directory = change.new_directory;
        validate_application(updated);
        if (!updated.working_directory.isEmpty() && !QFileInfo(updated.working_directory).isDir())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The replacement working directory was not found.")
                    .toStdString());
        const auto configuration_id = xlibrary::ensure_launch_configuration(
            database.get_connection(), filesystem_path(updated.path), updated.arguments.toStdString(),
            filesystem_path(updated.working_directory));
        Statement sources(database.get_connection(),
                          "INSERT OR IGNORE INTO discovery_source(configuration_id,kind,source_key) "
                          "SELECT ?,kind,source_key FROM discovery_source WHERE configuration_id=?;");
        sources.bind(1, configuration_id);
        sources.bind(2, record->configuration_id);
        sources.next();
        Statement update(database.get_connection(), "UPDATE applications SET configuration_id=? WHERE id=?;");
        update.bind(1, configuration_id);
        update.bind(2, change.application_id);
        update.next();
    }
    transaction.commit();
}
