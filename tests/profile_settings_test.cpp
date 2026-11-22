#include <limits>
#include <QKeySequence>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "Database.h"
#include "ProfileBackup.h"
#include "ProfileSettings.h"

class ProfileSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void typed_values_data() {
        QTest::addColumn<QVariant>("value");
        QTest::newRow("window-geometry") << QVariant(QByteArray("abc\0xyz", 7));
        QTest::newRow("signed-limit") << QVariant::fromValue(std::numeric_limits<qint64>::min());
        QTest::newRow("unsigned-limit") << QVariant::fromValue(std::numeric_limits<quint64>::max());
        QTest::newRow("window-position") << QVariant(QRect(-1900, 20, 1120, 700));
        QTest::newRow("shortcut") << QVariant::fromValue(QKeySequence("Ctrl+Alt+Space", QKeySequence::PortableText));
        QTest::newRow("empty-path-list") << QVariant(QStringList{});
        QTest::newRow("file-metadata") << QVariant(
            QVariantMap{{"report.pdf", QVariantMap{{"tags", QStringList{"work", "draft"}}, {"favorite", true}}}});
    }

    void typed_values() {
        QFETCH(QVariant, value);
        const auto decoded = profile_settings::decode(profile_settings::encode(value));
        QCOMPARE(decoded.metaType().id(), value.metaType().id());
        QCOMPARE(decoded, value);
    }

    void malformed_setting_data() {
        QTest::addColumn<QByteArray>("encoded");
        QTest::newRow("unknown-type") << QByteArray(R"({"type":"unknown","value":"text"})");
        QTest::newRow("invalid-binary") << QByteArray(R"({"type":"QByteArray","value":"**"})");
        QTest::newRow("out-of-range-int") << QByteArray(R"({"type":"int","value":"2147483648"})");
        QTest::newRow("negative-unsigned") << QByteArray(R"({"type":"uint","value":"-1"})");
        QTest::newRow("list-element-type") << QByteArray(R"({"type":"QStringList","value":["work",42]})");
        QTest::newRow("incomplete-map-entry") << QByteArray(R"({"type":"QVariantMap","value":{"x":{}}})");
    }

    void malformed_setting() {
        QFETCH(QByteArray, encoded);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, profile_settings::decode(encoded));
    }

    void restore_keeps_local_token_state_data() {
        QTest::addColumn<bool>("local_marker");
        QTest::newRow("destination-with-token") << true;
        QTest::newRow("destination-without-token") << false;
    }

    void restore_keeps_local_token_state() {
        QFETCH(bool, local_marker);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto db_path = directory.filePath("profile.db");
        const auto ini_path = directory.filePath("profile.ini");
        const auto backup_path = directory.filePath("transfer.xlprofile");
        { Database empty_profile(db_path); }
        QCoreApplication::setApplicationVersion("1.0.0");
        const QByteArray geometry("window\0position", 15);
        {
            QSettings source(ini_path, QSettings::IniFormat);
            source.setValue("window/geometry", geometry);
            source.setValue(profile_settings::token_marker, !local_marker);
            source.sync();
            QCOMPARE(source.status(), QSettings::NoError);
        }
        ProfileBackup::create(backup_path, db_path, ini_path);
        const auto preview = ProfileBackup::inspect(backup_path);
        QCOMPARE(preview.counts.value("settings"), 1);
        {
            QSettings destination(ini_path, QSettings::IniFormat);
            destination.clear();
            destination.setValue("local/only", "previous value");
            destination.setValue(profile_settings::token_marker, local_marker);
            destination.sync();
            QCOMPARE(destination.status(), QSettings::NoError);
        }
        const QDir recovery(ProfileBackup::restore(backup_path, db_path, ini_path, preview.fingerprint));
        QSettings restored(ini_path, QSettings::IniFormat);
        QCOMPARE(restored.value("window/geometry").toByteArray(), geometry);
        QCOMPARE(restored.value(profile_settings::token_marker).toBool(), local_marker);
        QVERIFY(!restored.contains("local/only"));
        QSettings previous(recovery.filePath("settings.ini"), QSettings::IniFormat);
        QCOMPARE(previous.value("local/only").toString(), QString("previous value"));
        QCOMPARE(ProfileBackup::inspect(recovery.filePath("previous.xlprofile")).counts.value("settings"), 1);
    }
};

QTEST_GUILESS_MAIN(ProfileSettingsTest)
#include "profile_settings_test.moc"
