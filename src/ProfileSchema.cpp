#include <chrono>
#include <QCoreApplication>
#include <set>
#include <vector>

#include "Application.h"
#include "LaunchIdentity.h"
#include "ProfileSchema.h"
#include "SqliteHelpers.h"

namespace {
using xlibrary::sqlite_execute;
using xlibrary::SqliteStatement;

void check_integrity(sqlite3 *const connection) {
    SqliteStatement check(connection, "PRAGMA quick_check;");
    if (!check.next() || check.text(0) != "ok" || check.next())
        throw std::runtime_error("Profile database is damaged (quick_check)");
    SqliteStatement foreign_keys(connection, "PRAGMA foreign_key_check;");
    if (foreign_keys.next())
        throw std::runtime_error("Profile database contains broken references (foreign_key_check)");
}

void require_unique(sqlite3 *const connection, const char *const table, const std::vector<std::string> &columns) {
    SqliteStatement indexes(connection, ("PRAGMA index_list(" + std::string(table) + ");").c_str());
    while (indexes.next()) {
        if (indexes.number(2) == 0 || indexes.number(4) != 0)
            continue;
        auto name = indexes.text(1);
        std::string escaped;
        for (const auto character : name) {
            escaped += character;
            if (character == '\'')
                escaped += '\'';
        }
        SqliteStatement info(connection, ("PRAGMA index_info('" + escaped + "');").c_str());
        std::vector<std::string> actual;
        while (info.next())
            actual.push_back(info.text(2));
        if (actual == columns)
            return;
    }
    throw std::runtime_error("Profile schema is missing a uniqueness constraint in " + std::string(table));
}

void require_reference(sqlite3 *const connection, const char *const table, const char *const column,
                       const char *const target, const char *const on_delete) {
    SqliteStatement references(connection, ("PRAGMA foreign_key_list(" + std::string(table) + ");").c_str());
    while (references.next()) {
        if (references.text(2) == target && references.text(3) == column && references.text(4) == "id" &&
            references.text(6) == on_delete)
            return;
    }
    throw std::runtime_error("Profile schema is missing a reference in " + std::string(table));
}

void validate_columns(sqlite3 *const connection, const int version) {
    SqliteStatement base(connection,
                         version < 3 ? "SELECT a.id,a.name,a.path,t.id,t.name,l.application_id,l.tag_id "
                                       "FROM applications a,tags t,application_tags l LIMIT 0;"
                                     : "SELECT a.id,a.name,a.configuration_id,t.id,t.name,l.application_id,l.tag_id "
                                       "FROM applications a,tags t,application_tags l LIMIT 0;");
    base.next();
    require_unique(connection, "tags", {"name"});
    require_unique(connection, "application_tags", {"application_id", "tag_id"});
    require_reference(connection, "application_tags", "application_id", "applications", "CASCADE");
    require_reference(connection, "application_tags", "tag_id", "tags", "CASCADE");
    if (version >= 2) {
        SqliteStatement desktop(connection, version < 3
                                                ? "SELECT d.application_id,d.arguments,d.working_directory,d.favorite,"
                                                  "d.last_launch,s.id,s.name,p.scenario_id,p.position,p.application_id,"
                                                  "p.delay_ms,p.enabled FROM desktop_application d,desktop_scenario s,"
                                                  "desktop_scenario_step p LIMIT 0;"
                                                : "SELECT d.application_id,d.favorite,d.last_launch,s.id,s.name,"
                                                  "p.scenario_id,p.position,p.application_id,p.delay_ms,p.enabled "
                                                  "FROM desktop_application d,desktop_scenario s,"
                                                  "desktop_scenario_step p LIMIT 0;");
        desktop.next();
        require_unique(connection, "desktop_scenario_step", {"scenario_id", "position"});
        require_reference(connection, "desktop_application", "application_id", "applications", "CASCADE");
        require_reference(connection, "desktop_scenario_step", "application_id", "applications", "CASCADE");
        require_reference(connection, "desktop_scenario_step", "scenario_id", "desktop_scenario", "CASCADE");
    }
    if (version >= 3) {
        SqliteStatement launches(connection, R"(
            SELECT t.id,t.kind,t.identity_key,c.id,c.target_id,c.path,c.arguments,c.working_directory,
                   c.working_directory_key,c.invocation_key,s.configuration_id,s.kind,s.source_key
            FROM launch_target t,launch_configuration c,discovery_source s LIMIT 0;
        )");
        launches.next();
        require_unique(connection, "launch_target", {"kind", "identity_key"});
        require_unique(connection, "launch_configuration",
                       {"target_id", "invocation_key", "arguments", "working_directory_key"});
        require_unique(connection, "discovery_source", {"configuration_id", "kind", "source_key"});
        require_reference(connection, "applications", "configuration_id", "launch_configuration", "NO ACTION");
        require_reference(connection, "launch_configuration", "target_id", "launch_target", "NO ACTION");
        require_reference(connection, "discovery_source", "configuration_id", "launch_configuration", "CASCADE");
        SqliteStatement missing(connection, "SELECT 1 FROM applications WHERE configuration_id IS NULL LIMIT 1;");
        if (missing.next())
            throw std::runtime_error("Profile contains a card without a launch configuration");
    }
    if (version >= 7) {
        SqliteStatement profiles(connection, "SELECT d.default_launch_profile,p.launch_profile FROM "
                                             "desktop_application d,desktop_scenario_step p LIMIT 0;");
        profiles.next();
        SqliteStatement values(connection, "SELECT default_launch_profile FROM desktop_application "
                                           "UNION ALL SELECT launch_profile FROM desktop_scenario_step;");
        while (values.next())
            if (!Application::valid_launch_profile_id(values.qtext(0)))
                throw std::runtime_error("Profile contains an invalid browser launch profile");
    }
    if (version >= 4) {
        SqliteStatement aliases(connection, "SELECT application_id,position,name FROM application_alias LIMIT 0;");
        aliases.next();
        require_unique(connection, "application_alias", {"application_id", "position"});
        require_reference(connection, "application_alias", "application_id", "applications", "CASCADE");
        SqliteStatement filters(connection,
                                "SELECT id,name,query,section,tag,sort_mode,sort_order FROM saved_filter LIMIT 0;");
        filters.next();
        require_unique(connection, "saved_filter", {"name"});
        SqliteStatement invalid_filters(connection, R"(
            SELECT 1 FROM saved_filter WHERE id<=0 OR name IS NULL OR length(trim(name))=0 OR length(name)>128
                OR instr(name,char(0))>0 OR query IS NULL OR length(query)>32768 OR instr(query,char(0))>0
                OR section IS NULL OR section NOT IN ('library','favorites','running')
                OR tag IS NULL OR length(tag)>64 OR instr(tag,char(0))>0
                OR sort_mode IS NULL OR sort_mode NOT BETWEEN 0 AND 4
                OR sort_order IS NULL OR sort_order NOT IN (0,1) LIMIT 1;
        )");
        if (invalid_filters.next())
            throw std::runtime_error("Profile contains an invalid saved filter");
        SqliteStatement invalid_aliases(connection, R"(
            SELECT 1 FROM application_alias WHERE position<0 OR position IS NULL OR name IS NULL
                OR length(trim(name))=0 OR length(name)>256 OR instr(name,char(0))>0
                OR instr(name,char(10))>0 OR instr(name,char(13))>0
            UNION ALL SELECT 1 FROM application_alias GROUP BY application_id HAVING count(*)>32 LIMIT 1;
        )");
        if (invalid_aliases.next())
            throw std::runtime_error("Profile contains invalid search aliases");
    }
    if (version >= 5) {
        SqliteStatement metadata(connection,
                                 "SELECT installation_directory,icon_path FROM launch_configuration LIMIT 0;");
        metadata.next();
        SqliteStatement invalid(connection, "SELECT 1 FROM launch_target WHERE kind NOT IN "
                                            "('executable','desktop','system_uri','aumid','steam') LIMIT 1;");
        if (invalid.next())
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Profile contains an unsupported launch target type")
                    .toStdString());
        SqliteStatement targets(
            connection, "SELECT t.kind,c.path,c.arguments,c.working_directory,c.installation_directory,c.icon_path "
                        "FROM launch_configuration c JOIN launch_target t ON t.id=c.target_id;");
        while (targets.next()) {
            app_record record;
            record.target_kind = targets.qtext(0);
            record.path = targets.qtext(1);
            record.arguments = targets.qtext(2);
            record.working_directory = targets.qtext(3);
            Application::validate_launch_target(record);
            for (int column = 4; column <= 5; ++column)
                if (targets.qtext(column).size() > catalog_limits::text_length ||
                    targets.qtext(column).contains(QChar::Null))
                    throw std::runtime_error(
                        QCoreApplication::translate("XLibrary", "Profile contains invalid launch target metadata")
                            .toStdString());
        }
    }
    if (version >= 6) {
        SqliteStatement shortcuts(connection,
                                  "SELECT application_id,scenario_id,sequence FROM target_shortcut LIMIT 0;");
        shortcuts.next();
        require_unique(connection, "target_shortcut", {"application_id"});
        require_unique(connection, "target_shortcut", {"scenario_id"});
        require_unique(connection, "target_shortcut", {"sequence"});
        require_reference(connection, "target_shortcut", "application_id", "applications", "CASCADE");
        require_reference(connection, "target_shortcut", "scenario_id", "desktop_scenario", "CASCADE");
        SqliteStatement invalid(connection, R"(
            SELECT 1 FROM target_shortcut WHERE (application_id IS NULL)=(scenario_id IS NULL)
                OR coalesce(application_id,scenario_id)<=0 OR sequence IS NULL
                OR length(sequence)=0 OR length(sequence)>128 OR instr(sequence,char(0))>0 LIMIT 1;
        )");
        if (invalid.next())
            throw std::runtime_error("Profile contains an invalid target shortcut");
    }
    check_integrity(connection);
}

int infer_legacy_version(sqlite3 *const connection) {
    std::set<std::string> tables;
    SqliteStatement schema(connection,
                           "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';");
    while (schema.next()) {
        const auto name = schema.text(0);
        if (!name.starts_with("_xlibrary_profile_"))
            tables.insert(name);
    }
    if (tables.empty())
        return 0;
    const std::set<std::string> base{"applications", "tags", "application_tags"};
    const std::set<std::string> desktop{
        "applications", "tags", "application_tags", "desktop_application", "desktop_scenario", "desktop_scenario_step"};
    const auto version = tables == base ? 1 : tables == desktop ? 2 : -1;
    if (version == -1)
        throw std::runtime_error("Unrecognized or incomplete unversioned profile schema; restore a backup");
    validate_columns(connection, version);
    return version;
}

void migrate_base(sqlite3 *const connection) {
    sqlite_execute(connection, R"(
        CREATE TABLE applications(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,path TEXT);
        CREATE TABLE tags(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL UNIQUE);
        CREATE TABLE application_tags(
            application_id INTEGER NOT NULL REFERENCES applications(id) ON DELETE CASCADE,
            tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,PRIMARY KEY(application_id,tag_id));
        CREATE INDEX application_tags_tag_id ON application_tags(tag_id);
    )");
}

void migrate_desktop(sqlite3 *const connection) {
    sqlite_execute(connection, R"(
        CREATE INDEX desktop_applications_name ON applications(name);
        CREATE INDEX desktop_applications_path ON applications(path);
        CREATE TABLE desktop_application(
            application_id INTEGER PRIMARY KEY REFERENCES applications(id) ON DELETE CASCADE,
            arguments TEXT NOT NULL DEFAULT '',working_directory TEXT NOT NULL DEFAULT '',
            favorite INTEGER NOT NULL DEFAULT 0,last_launch INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE desktop_scenario(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL);
        CREATE TABLE desktop_scenario_step(
            scenario_id INTEGER NOT NULL REFERENCES desktop_scenario(id) ON DELETE CASCADE,
            position INTEGER NOT NULL,application_id INTEGER NOT NULL REFERENCES applications(id) ON DELETE CASCADE,
            delay_ms INTEGER NOT NULL CHECK(delay_ms BETWEEN 0 AND 3600000),enabled INTEGER NOT NULL,
            PRIMARY KEY(scenario_id,position));
    )");
}

void migrate_identity(sqlite3 *const connection) {
    sqlite_execute(connection, R"(
        CREATE TABLE launch_target(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,identity_key TEXT NOT NULL,
            UNIQUE(kind,identity_key));
        CREATE TABLE launch_configuration(
            id INTEGER PRIMARY KEY,target_id INTEGER NOT NULL REFERENCES launch_target(id),
            path TEXT NOT NULL,arguments TEXT NOT NULL,working_directory TEXT NOT NULL,
            working_directory_key TEXT NOT NULL,invocation_key TEXT NOT NULL,
            UNIQUE(target_id,invocation_key,arguments,working_directory_key));
        CREATE TABLE discovery_source(
            configuration_id INTEGER NOT NULL REFERENCES launch_configuration(id) ON DELETE CASCADE,
            kind TEXT NOT NULL,source_key TEXT NOT NULL,PRIMARY KEY(configuration_id,kind,source_key));
        ALTER TABLE applications ADD COLUMN configuration_id INTEGER REFERENCES launch_configuration(id);
    )");
    struct legacy_card {
        sqlite3_int64 id;
        std::string path;
        std::string arguments;
        std::string directory;
    };
    std::vector<legacy_card> cards;
    {
        SqliteStatement records(connection,
                                "SELECT a.id,a.path,coalesce(d.arguments,''),coalesce(d.working_directory,'') "
                                "FROM applications a LEFT JOIN desktop_application d ON d.application_id=a.id;");
        while (records.next())
            cards.push_back({records.number(0), records.text(1), records.text(2), records.text(3)});
    }
    for (const auto &card : cards) {
        const auto configuration_id = xlibrary::ensure_launch_configuration(
            connection, xlibrary::utf8_path(card.path), card.arguments, xlibrary::utf8_path(card.directory));
        SqliteStatement update(connection, "UPDATE applications SET configuration_id=? WHERE id=?;");
        update.bind(1, configuration_id);
        update.bind(2, card.id);
        update.next();
    }
    sqlite_execute(connection, R"(
        DROP INDEX IF EXISTS desktop_applications_path;
        ALTER TABLE applications DROP COLUMN path;
        ALTER TABLE desktop_application DROP COLUMN arguments;
        ALTER TABLE desktop_application DROP COLUMN working_directory;
        CREATE INDEX application_configuration ON applications(configuration_id);
        CREATE TRIGGER application_configuration_insert BEFORE INSERT ON applications
            WHEN NEW.configuration_id IS NULL BEGIN SELECT RAISE(ABORT,'A card requires a launch configuration'); END;
        CREATE TRIGGER application_configuration_update BEFORE UPDATE OF configuration_id ON applications
            WHEN NEW.configuration_id IS NULL BEGIN SELECT RAISE(ABORT,'A card requires a launch configuration'); END;
    )");
}
}

int xlibrary::read_schema_version(sqlite3 *const connection) {
    SqliteStatement version(connection, "PRAGMA user_version;");
    if (!version.next())
        throw std::runtime_error("Cannot read profile schema version");
    return static_cast<int>(version.number(0));
}

void xlibrary::validate_profile(sqlite3 *const connection) {
    const auto version = read_schema_version(connection);
    if (version != profile_schema_version)
        throw std::runtime_error("Unsupported profile schema version " + std::to_string(version));
    validate_columns(connection, version);
}

void xlibrary::backup_database(sqlite3 *const connection, const std::filesystem::path &destination) {
    if (std::filesystem::exists(destination))
        throw std::runtime_error("Backup destination already exists");
    auto partial = destination;
    partial += ".partial";
    if (std::filesystem::exists(partial))
        throw std::runtime_error("Backup temporary file already exists");
    try {
        {
            sqlite3 *raw = nullptr;
            const auto path = path_utf8(partial);
            const auto status =
                sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
            const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> output(raw, &sqlite3_close_v2);
            if (status != SQLITE_OK)
                throw std::runtime_error("Cannot create profile backup");
            sqlite3_busy_timeout(output.get(), 1500);
            auto *backup = sqlite3_backup_init(output.get(), "main", connection, "main");
            if (!backup)
                throw std::runtime_error("Cannot initialize profile backup");
            const auto copied = sqlite3_backup_step(backup, -1);
            const auto finished = sqlite3_backup_finish(backup);
            if (copied != SQLITE_DONE || finished != SQLITE_OK)
                throw std::runtime_error("Cannot finish profile backup");
            check_integrity(output.get());
        }
        std::filesystem::rename(partial, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        throw;
    }
}

void xlibrary::migrate_profile(sqlite3 *const connection, const std::filesystem::path &path) {
    const auto recorded = read_schema_version(connection);
    if (recorded < 0 || recorded > profile_schema_version)
        throw std::runtime_error("Profile schema " + std::to_string(recorded) + " is newer than supported schema " +
                                 std::to_string(profile_schema_version) +
                                 "; update XLibrary. The profile was not changed.");
    auto version = recorded == 0 ? infer_legacy_version(connection) : recorded;
    if (version != 0)
        validate_columns(connection, version);
    if (recorded == profile_schema_version)
        return;
    if (version > 0) {
        const auto timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        auto backup = path;
        backup +=
            ".before-schema-v" + std::to_string(profile_schema_version) + "." + std::to_string(timestamp) + ".bak";
        backup_database(connection, backup);
    }
    sqlite_execute(connection, "BEGIN IMMEDIATE;");
    try {
        if (read_schema_version(connection) != recorded)
            throw std::runtime_error("Profile schema changed concurrently; reopen the profile");
        while (version < profile_schema_version) {
            switch (version) {
            case 0:
                migrate_base(connection);
                break;
            case 1:
                migrate_desktop(connection);
                break;
            case 2:
                migrate_identity(connection);
                break;
            case 3:
                sqlite_execute(connection, R"(
                    CREATE TABLE application_alias(
                        application_id INTEGER NOT NULL REFERENCES applications(id) ON DELETE CASCADE,
                        position INTEGER NOT NULL, name TEXT NOT NULL,
                        PRIMARY KEY(application_id,position));
                    CREATE TABLE saved_filter(
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        name TEXT NOT NULL UNIQUE COLLATE NOCASE,
                        query TEXT NOT NULL, section TEXT NOT NULL, tag TEXT NOT NULL,
                        sort_mode INTEGER NOT NULL CHECK(sort_mode BETWEEN 0 AND 4),
                        sort_order INTEGER NOT NULL CHECK(sort_order IN (0,1)));
                )");
                break;
            case 4:
                sqlite_execute(connection, R"(
                    ALTER TABLE launch_configuration ADD COLUMN installation_directory TEXT NOT NULL DEFAULT '';
                    ALTER TABLE launch_configuration ADD COLUMN icon_path TEXT NOT NULL DEFAULT '';
                )");
                break;
            case 5:
                sqlite_execute(connection, R"(
                    CREATE TABLE target_shortcut(
                        application_id INTEGER UNIQUE REFERENCES applications(id) ON DELETE CASCADE,
                        scenario_id INTEGER UNIQUE REFERENCES desktop_scenario(id) ON DELETE CASCADE,
                        sequence TEXT NOT NULL UNIQUE CHECK(length(sequence) BETWEEN 1 AND 128),
                        CHECK((application_id IS NULL)!=(scenario_id IS NULL)));
                )");
                break;
            case 6:
                sqlite_execute(connection, R"(
                    ALTER TABLE desktop_application ADD COLUMN default_launch_profile TEXT NOT NULL DEFAULT '';
                    ALTER TABLE desktop_scenario_step ADD COLUMN launch_profile TEXT NOT NULL DEFAULT '';
                )");
                break;
            default:
                throw std::runtime_error("Missing profile migration");
            }
            ++version;
            validate_columns(connection, version);
            sqlite_execute(connection, ("PRAGMA user_version=" + std::to_string(version) + ";").c_str());
        }
        validate_profile(connection);
        sqlite_execute(connection, "COMMIT;");
    } catch (...) {
        sqlite3_exec(connection, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}
