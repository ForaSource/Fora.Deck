#include <algorithm>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "Database.h"
#include "LaunchIdentity.h"
#include "ProfileBackup.h"
#include "ProfileSchema.h"
#include "SearchQuery.h"
#include "SqliteHelpers.h"

class CoreTest : public QObject {
    Q_OBJECT
    static app_record record() {
        app_record result;
        result.name = "Test application";
        result.path = QCoreApplication::applicationFilePath();
        result.target_kind = "executable";
        result.tags = {"work", "тест"};
        result.aliases = {"test alias"};
        result.favorite = true;
        result.default_launch_profile = "profile:Default";
        return result;
    }
private slots:
    void initTestCase() { QCoreApplication::setApplicationVersion("0.7.0"); }
    void search_data() {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QStringList>("aliases");
        QTest::addColumn<bool>("matches");
        QTest::newRow("exact") << "Visual Studio Code" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("prefix") << "vis" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("alias") << "editor" << "Visual Studio Code" << QStringList{"editor"} << true;
        QTest::newRow("layout") << "мшыгфд" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("abbreviation") << "vsc" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("transpose") << "viusal" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("typo") << "visuql" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("terms") << "visual code" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("unicode") << "редактор" << "Редактор текста" << QStringList{} << true;
        QTest::newRow("tag") << "#work" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("excluded") << "-#work" << "Visual Studio Code" << QStringList{} << false;
        QTest::newRow("unknown") << "#unknown" << "Visual Studio Code" << QStringList{} << false;
        QTest::newRow("mixed-tags") << "code #work -#games" << "Visual Studio Code" << QStringList{} << true;
        QTest::newRow("missing-term") << "visual nonexistent" << "Visual Studio Code" << QStringList{} << false;
    }
    void search() {
        QFETCH(QString, query);
        QFETCH(QString, name);
        QFETCH(QStringList, aliases);
        QFETCH(bool, matches);
        QCOMPARE(SearchQuery(query).rank(name, "code", {"work"}, aliases) >= 0, matches);
    }
    void exact_before_fuzzy() {
        const SearchQuery query("visual");
        const auto exact = query.rank("visual", "", {});
        const auto fuzzy = query.rank("visuql", "", {});
        QVERIFY(exact >= 0);
        QVERIFY(fuzzy >= 0);
        QVERIFY(exact < fuzzy);
        QCOMPARE(search_query::switch_layout("ghbdtn"), QString("привет"));
    }
    void launch_identity() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Database db(temporary.filePath("profile.db"));
        const auto executable = xlibrary::utf8_path(record().path.toStdString());
        const auto id = xlibrary::ensure_launch_configuration(db.get_connection(), executable);
        QCOMPARE(xlibrary::ensure_launch_configuration(db.get_connection(), executable), id);
        QVERIFY(xlibrary::ensure_launch_configuration(db.get_connection(), executable, "--test") != id);
        QVERIFY(xlibrary::ensure_launch_configuration(db.get_connection(), executable, "",
                                                      temporary.path().toStdString()) != id);
        const auto base = xlibrary::utf8_path(temporary.path().toStdString());
        QCOMPARE(xlibrary::path_identity(base / "a" / ".." / "b", false), xlibrary::path_identity(base / "b", false));
#ifdef Q_OS_WIN
        QCOMPARE(xlibrary::path_identity(base / "B", false), xlibrary::path_identity(base / "b", false));
#else
        QVERIFY(xlibrary::path_identity(base / "B", false) != xlibrary::path_identity(base / "b", false));
#endif
    }
    void database_integrity() {
        QTemporaryDir temporary;
        Database db(temporary.filePath("profile.db"));
        QCOMPARE(db.schema_version(), xlibrary::profile_schema_version);
        auto app = record();
        const auto id = db.save_application(app);
        QCOMPARE(db.read().applications.size(), 1);
        QVERIFY_EXCEPTION_THROWN(db.save_application(app), std::exception);
        QCOMPARE(db.read().applications.size(), 1);
        const auto loaded = db.read().applications.front();
        QCOMPARE(loaded.id, id);
        QCOMPARE(loaded.tags, app.tags);
        QCOMPARE(loaded.aliases, app.aliases);
        QVERIFY(loaded.favorite);
        QVERIFY(sqlite3_exec(db.get_connection(), "INSERT INTO application_tags VALUES(999999,999999);", nullptr,
                             nullptr, nullptr) != SQLITE_OK);
        QVERIFY(sqlite3_exec(db.get_connection(), "INSERT INTO tags(name) VALUES('work');", nullptr, nullptr,
                             nullptr) != SQLITE_OK);
        xlibrary::validate_profile(db.get_connection());
        auto invalid = app;
        invalid.name.clear();
        QVERIFY_EXCEPTION_THROWN(db.import_records({app, invalid}), std::exception);
        QCOMPARE(db.read().applications.size(), 1);
        xlibrary::validate_profile(db.get_connection());
    }
    void migration() {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("old.db");
        sqlite3 *raw = nullptr;
        QCOMPARE(sqlite3_open(path.toUtf8().constData(), &raw), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> connection(raw, sqlite3_close_v2);
        xlibrary::sqlite_execute(raw, R"(
            PRAGMA foreign_keys=ON;
            CREATE TABLE applications(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,path TEXT);
            CREATE TABLE tags(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL UNIQUE);
            CREATE TABLE application_tags(application_id INTEGER NOT NULL REFERENCES applications(id) ON DELETE CASCADE,
                tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,PRIMARY KEY(application_id,tag_id));
            INSERT INTO tags VALUES(1,'legacy');
            PRAGMA user_version=1;
        )");
        {
            xlibrary::SqliteStatement insert(raw, "INSERT INTO applications VALUES(1,'Legacy',?);");
            insert.bind(1, QCoreApplication::applicationFilePath());
            insert.next();
        }
        xlibrary::sqlite_execute(raw, "INSERT INTO application_tags VALUES(1,1);");
        connection.reset();
        Database migrated(path);
        QCOMPARE(migrated.schema_version(), xlibrary::profile_schema_version);
        const auto apps = migrated.read().applications;
        QCOMPARE(apps.size(), 1);
        QCOMPARE(apps.front().name, QString("Legacy"));
        QCOMPARE(apps.front().tags, QStringList{"legacy"});
        xlibrary::validate_profile(migrated.get_connection());
    }
    void reject_future_and_corrupt() {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("future.db");
        {
            Database db(path);
            xlibrary::sqlite_execute(db.get_connection(), "PRAGMA user_version=999;");
        }
        QVERIFY_EXCEPTION_THROWN(Database{path}, std::exception);
        sqlite3 *raw = nullptr;
        QCOMPARE(sqlite3_open(path.toUtf8().constData(), &raw), SQLITE_OK);
        QCOMPARE(xlibrary::read_schema_version(raw), 999);
        sqlite3_close_v2(raw);
        QFile damaged(temporary.filePath("damaged.db"));
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        damaged.write("not a database");
        damaged.close();
        QVERIFY_EXCEPTION_THROWN(Database{damaged.fileName()}, std::exception);
    }
    void import_cancel_rescan_and_steam() {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("profile.db");
        {
            Database db(path);
            auto steam = record();
            steam.name = "Steam fixture";
            steam.target_kind = "steam";
            steam.path = "570";
            steam.default_launch_profile.clear();
            steam.installation_directory = temporary.filePath("offline steam library");
            const auto imported = db.import_records({record(), steam});
            QCOMPARE(imported.added, 2);
            QCOMPARE(db.import_records({record(), steam}).duplicates, 2);
            discovery_control control;
            auto extra = record();
            extra.arguments = "--new-configuration";
            QVERIFY_EXCEPTION_THROWN(
                db.import_records({extra, record()}, &control,
                                  [&](qsizetype, qsizetype) { control.phase.store(discovery_phase::cancelling); }),
                DiscoveryCancelled);
            QCOMPARE(db.read().applications.size(), 2);
            xlibrary::validate_profile(db.get_connection());
        }
        Database reopened(path);
        const auto applications = reopened.read().applications;
        QCOMPARE(applications.size(), 2);
        const auto steam = std::ranges::find(applications, QString("steam"), &app_record::target_kind);
        QVERIFY(steam != applications.end());
        QCOMPARE(steam->path, QString("570"));
        QCOMPARE(steam->installation_directory, temporary.filePath("offline steam library"));
        const auto executable = std::ranges::find(applications, QString("executable"), &app_record::target_kind);
        QVERIFY(executable != applications.end());
        QCOMPARE(executable->default_launch_profile, QString("profile:Default"));
    }
    void interrupted_restore_recovery() {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("profile.db");
        const auto settings_path = temporary.filePath("settings.ini");
        const auto recovery = temporary.filePath(".xlibrary-recovery-fixture");
        QVERIFY(QDir().mkpath(recovery));
        {
            Database db(path);
            db.save_application(record());
        }
        {
            QSettings settings(settings_path, QSettings::IniFormat);
            settings.setValue("fixture", "original");
            settings.sync();
        }
        QVERIFY(QFile::copy(path, recovery + "/database.sqlite"));
        QVERIFY(QFile::copy(settings_path, recovery + "/settings.ini"));
        QJsonArray files;
        for (const auto *name : {"database.sqlite", "database.sqlite-wal", "database.sqlite-shm",
                                 "database.sqlite-journal", "settings.ini"})
            files.append(QJsonObject{
                {"name", name}, {"existed", QString(name) == "database.sqlite" || QString(name) == "settings.ini"}});
        const QJsonObject journal{{"version", 1},
                                  {"database_path", path},
                                  {"settings_path", settings_path},
                                  {"recovery_path", recovery},
                                  {"state", "prepared"},
                                  {"files", files}};
        QFile journal_file(path + ".profile-restore.json");
        QVERIFY(journal_file.open(QIODevice::WriteOnly));
        journal_file.write(QJsonDocument(journal).toJson());
        journal_file.close();
        {
            Database changed(path);
            changed.remove_application(changed.read().applications.front().id);
        }
        ProfileBackup::recover(path, settings_path);
        QCOMPARE(Database(path).read().applications.size(), 1);
        QVERIFY(!QFile::exists(journal_file.fileName()));
        ProfileBackup::recover(path, settings_path);
        QCOMPARE(Database(path).read().applications.size(), 1);
    }
    void backup_roundtrip() {
        QTemporaryDir temporary;
        const auto database_path = temporary.filePath("profile.db");
        const auto settings_path = temporary.filePath("settings.ini");
        const auto backup_path = temporary.filePath("backup.xlprofile");
        qint64 id = 0;
        {
            Database db(database_path);
            id = db.save_application(record());
            scenario_record scenario;
            scenario.name = "Test scenario";
            scenario.steps = {{id, 250, true, {}}};
            db.save_scenario(scenario);
            db.record_launch(id);
            db.save_target_shortcut(false, id, "Ctrl+Alt+T");
        }
        {
            QSettings settings(settings_path, QSettings::IniFormat);
            settings.setValue("test/setting", "saved value");
            settings.sync();
        }
        ProfileBackup::create(backup_path, database_path, settings_path);
        const auto manifest = ProfileBackup::inspect(backup_path);
        QVERIFY(!manifest.fingerprint.isEmpty());
        auto wrong_fingerprint = manifest.fingerprint;
        wrong_fingerprint[0] = static_cast<char>(wrong_fingerprint[0] ^ 1);
        QVERIFY_EXCEPTION_THROWN(ProfileBackup::restore(backup_path, database_path, settings_path, wrong_fingerprint),
                                 std::exception);
        {
            Database db(database_path);
            db.remove_application(id);
        }
        {
            QSettings changed_settings(settings_path, QSettings::IniFormat);
            changed_settings.setValue("test/setting", "changed after backup");
            changed_settings.sync();
        }
        ProfileBackup::restore(backup_path, database_path, settings_path, manifest.fingerprint);
        {
            Database restored(database_path);
            const auto snapshot = restored.read();
            QCOMPARE(snapshot.applications.size(), 1);
            QCOMPARE(snapshot.applications.front().aliases, record().aliases);
            QCOMPARE(snapshot.applications.front().tags, record().tags);
            QVERIFY(snapshot.applications.front().favorite);
            QCOMPARE(snapshot.applications.front().default_launch_profile, QString("profile:Default"));
            QVERIFY(snapshot.applications.front().last_launch > 0);
            QCOMPARE(snapshot.scenarios.size(), 1);
            QCOMPARE(snapshot.scenarios.front().steps.front().application_id, id);
            QCOMPARE(snapshot.target_shortcuts.size(), 1);
            xlibrary::validate_profile(restored.get_connection());
        }
        QSettings restored_settings(settings_path, QSettings::IniFormat);
        QCOMPARE(restored_settings.value("test/setting").toString(), QString("saved value"));
        ProfileBackup::recover(database_path, settings_path);
        QFile damaged(temporary.filePath("bad.xlprofile"));
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        damaged.write("broken archive");
        damaged.close();
        QVERIFY_EXCEPTION_THROWN(ProfileBackup::inspect(damaged.fileName()), std::exception);
        QVERIFY_EXCEPTION_THROWN(
            ProfileBackup::restore(damaged.fileName(), database_path, settings_path, manifest.fingerprint),
            std::exception);
        QCOMPARE(Database(database_path).read().applications.size(), 1);
    }
};
QTEST_GUILESS_MAIN(CoreTest)
#include "core_test.moc"
