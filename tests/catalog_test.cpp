#include <algorithm>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "Manager.h"

namespace {
app_record steam_card(const QString &name, const QString &app_id) {
    app_record record;
    record.name = name;
    record.target_kind = "steam";
    record.path = app_id;
    return record;
}

bool write_file(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.flush();
}
}

class CatalogTest : public QObject {
    Q_OBJECT
private slots:
    void invalid_json_preserves_catalog_data() {
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<QString>("diagnostic");
        QTest::newRow("malformed") << QByteArray("{") << "offset";
        QTest::newRow("root-array") << QByteArray("[]") << "root";
        QTest::newRow("unsupported-format") << QByteArray(R"({"format":2,"applications":[]})") << "format";
        QTest::newRow("string-format") << QByteArray(R"({"format":"1","applications":[]})") << "format";
        QTest::newRow("invalid-collection") << QByteArray(R"({"format":1,"applications":{}})") << "applications";

        const auto invalid_record = [](const char *label, const QJsonValue &record, const QString &field) {
            const QJsonObject first{{"name", "Valid first card"}, {"path", "10"}, {"target_kind", "steam"}};
            const QJsonObject root{{"format", 1}, {"applications", QJsonArray{first, record}}};
            QTest::newRow(label) << QJsonDocument(root).toJson(QJsonDocument::Compact)
                                 << QString("Record 2, field %1:").arg(field);
        };
        const QJsonObject second{{"name", "Second"}, {"path", "20"}, {"target_kind", "steam"}};
        const auto invalid_field = [&](const char *label, const QString &field, const QJsonValue &value,
                                       const QString &reported_field) {
            auto row = second;
            row.insert(field, value);
            invalid_record(label, row, reported_field);
        };
        invalid_record("non-object", 1, "applications[]");
        invalid_record("missing-path", QJsonObject{{"name", "Missing target"}}, "path");
        invalid_field("blank-name", "name", "  ", "name");
        invalid_field("null-string", "arguments", QJsonValue::Null, "arguments");
        invalid_field("numeric-favorite", "favorite", 1, "favorite");
        invalid_field("tags-not-array", "tags", "work", "tags");
        invalid_field("invalid-tag", "tags", QJsonArray{"ok", "two words"}, "tags[2]");
        invalid_field("multiline-alias", "aliases", QJsonArray{"one\ntwo"}, "aliases");
        invalid_field("relative-workdir", "working_directory", "relative", "working_directory");
        invalid_field("steam-id-overflow", "path", "4294967296", "application");
        invalid_field("steam-arguments", "arguments", "--flag", "application");
        invalid_field("invalid-profile", "default_launch_profile", "profile:", "application");
        QByteArray oversized(R"({"format":1,"applications":[]})");
        oversized.append(QByteArray(32 * 1024 * 1024 + 1 - oversized.size(), ' '));
        QTest::newRow("byte-limit") << oversized << "32 MiB";
        QByteArray too_many(R"({"format":1,"applications":[)");
        too_many += QByteArray("{},").repeated(50000) + "{}]}";
        QTest::newRow("application-limit") << too_many << "50000";
    }

    void invalid_json_preserves_catalog() {
        QFETCH(QByteArray, contents);
        QFETCH(QString, diagnostic);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath("catalog.json");
        QVERIFY(write_file(path, contents));
        Manager manager;
        manager.store = std::make_unique<Database>(temporary.filePath("profile.db"));
        const auto id = manager.store->save_application(steam_card("Existing", "30"));
        QSignalSpy previews(&manager, &Manager::preview_ready);
        QSignalSpy results(&manager, &Manager::result);
        manager.execute(
            7, "import", [&](Manager &worker) { worker.import_catalog(path); }, operation_error_source::filesystem);
        QCOMPARE(previews.size(), 0);
        QCOMPARE(results.size(), 1);
        const auto result = results.front().front().value<operation_result>();
        QCOMPARE(result.id, 7);
        QCOMPARE(result.state, operation_state::error);
        QCOMPARE(result.error.source, operation_error_source::validation);
        QVERIFY2(result.error.user_message.contains(diagnostic), qPrintable(result.error.user_message));
        const auto apps = manager.store->read().applications;
        QCOMPARE(apps.size(), 1);
        QCOMPARE(apps.front().id, id);
        QCOMPARE(apps.front().name, QString("Existing"));
    }

    void export_and_preview_card_metadata() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Manager manager;
        manager.store = std::make_unique<Database>(temporary.filePath("profile.db"));
        app_record app;
        app.name = "Editor";
        app.path = temporary.filePath("editor.exe");
        app.arguments = "--open \"two words\"";
        app.working_directory = temporary.path();
        app.default_launch_profile = "profile:Default";
        app.tags = {"work"};
        app.aliases = {"first alias", "editor alias"};
        app.favorite = true;
        const auto id = manager.store->save_application(app);
        manager.store->record_launch(id);
        manager.catalog = manager.store->read();
        const auto path = temporary.filePath("catalog.json");
        manager.export_catalog(path);

        QFile export_file(path);
        QVERIFY(export_file.open(QIODevice::ReadOnly));
        const auto exported = QJsonDocument::fromJson(export_file.readAll()).object();
        QCOMPARE(exported.value("format").toInt(), 1);
        const auto cards = exported.value("applications").toArray();
        QCOMPARE(cards.size(), 1);
        const auto card = cards.first().toObject();
        QVERIFY(!card.contains("id"));
        QVERIFY(!card.contains("launch_count"));
        QVERIFY(!card.contains("last_launch"));
        export_file.close();

        Manager destination;
        destination.store = std::make_unique<Database>(temporary.filePath("destination.db"));
        QSignalSpy previews(&destination, &Manager::preview_ready);
        destination.import_catalog(path);
        QCOMPARE(previews.size(), 1);
        QVERIFY(destination.store->read().applications.isEmpty());
        const auto records = previews.front().at(1).value<QVector<app_record>>();
        QCOMPARE(records.size(), 1);
        const auto &preview = records.front();
        QCOMPARE(preview.name, app.name);
        QCOMPARE(preview.path, app.path);
        QCOMPARE(preview.target_kind, QString("executable"));
        QCOMPARE(preview.arguments, app.arguments);
        QCOMPARE(preview.working_directory, app.working_directory);
        QCOMPARE(preview.default_launch_profile, app.default_launch_profile);
        QCOMPARE(preview.tags, app.tags);
        QCOMPARE(preview.aliases, app.aliases);
        QCOMPARE(preview.favorite, app.favorite);
    }

    void legacy_catalog_with_only_required_fields() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto target = temporary.filePath("editor.exe");
        const QJsonObject app{{"name", "Editor"}, {"path", target}};
        const auto path = temporary.filePath("legacy.json");
        QVERIFY(write_file(path, QJsonDocument(QJsonObject{{"format", 1}, {"applications", QJsonArray{app}}})
                                     .toJson(QJsonDocument::Compact)));
        Manager manager;
        manager.store = std::make_unique<Database>(temporary.filePath("profile.db"));
        QSignalSpy previews(&manager, &Manager::preview_ready);
        manager.import_catalog(path);
        QCOMPARE(previews.size(), 1);
        const auto records = previews.front().at(1).value<QVector<app_record>>();
        QCOMPARE(records.size(), 1);
        QCOMPARE(manager.store->import_records(records).added, 1);
        const auto saved = manager.store->read().applications;
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.front().target_kind, QString("executable"));
        QCOMPARE(saved.front().path, target);
        QCOMPARE(saved.front().name, QString("Editor"));
        QVERIFY(!saved.front().favorite);
        QVERIFY(saved.front().tags.isEmpty());
        QVERIFY(saved.front().aliases.isEmpty());
    }

    void duplicate_policy_for_existing_and_repeated_cards_data() {
        QTest::addColumn<bool>("replace");
        QTest::newRow("skip") << false;
        QTest::newRow("replace") << true;
    }

    void duplicate_policy_for_existing_and_repeated_cards() {
        QFETCH(bool, replace);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Database db(temporary.filePath("profile.db"));
        const auto original = steam_card("Original", "10");
        const auto id = db.save_application(original);
        db.record_launch(id);
        const auto last_launch = db.read().applications.front().last_launch;
        const auto scenario_id = db.save_scenario({0, "Session", {{id, 230, true, {}}}});
        auto updated = original;
        updated.name = "Imported";
        updated.tags = {"new-tag"};
        updated.aliases = {"new alias"};
        updated.favorite = true;
        auto first = steam_card("First", "20");
        auto last = first;
        last.name = "Last";
        const auto counts =
            db.import_records({updated, first, last}, nullptr, {}, nullptr,
                              replace ? import_conflict_policy::replace_metadata : import_conflict_policy::skip);
        QCOMPARE(counts.added, 1);
        QCOMPARE(counts.updated, replace ? 2 : 0);
        QCOMPARE(counts.duplicates, replace ? 0 : 2);
        const auto snapshot = db.read();
        QCOMPARE(snapshot.applications.size(), 2);
        const auto &existing = snapshot.applications.front();
        const auto &expected = replace ? updated : original;
        QCOMPARE(existing.id, id);
        QCOMPARE(existing.name, expected.name);
        QCOMPARE(existing.tags, expected.tags);
        QCOMPARE(existing.aliases, expected.aliases);
        QCOMPARE(existing.favorite, expected.favorite);
        QCOMPARE(existing.launch_count, 1);
        QCOMPARE(existing.last_launch, last_launch);
        QCOMPARE(snapshot.applications.back().name, replace ? last.name : first.name);
        QCOMPARE(snapshot.scenarios.size(), 1);
        QCOMPARE(snapshot.scenarios.front().id, scenario_id);
        QCOMPARE(snapshot.scenarios.front().steps.size(), 1);
        QCOMPARE(snapshot.scenarios.front().steps.front().application_id, id);
    }

    void bulk_metadata_preserves_mixed_assignments_and_rolls_back_stale_selection() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Database db(temporary.filePath("profile.db"));
        auto first = steam_card("First", "10");
        first.tags = {"private", "Shared"};
        first.favorite = true;
        const auto first_id = db.save_application(first);
        auto second = steam_card("Second", "20");
        second.tags = {"Shared"};
        const auto second_id = db.save_application(second);
        db.update_metadata({first_id, second_id}, {"added"}, {"SHARED"}, -1);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, db.update_metadata({first_id, 99999}, {"lost"}, {}, 0));
        const auto snapshot = db.read();
        QCOMPARE(snapshot.applications.size(), 2);
        QCOMPARE(snapshot.applications.front().tags, (QStringList{"added", "private"}));
        QVERIFY(snapshot.applications.front().favorite);
        QCOMPARE(snapshot.applications.back().tags, QStringList{"added"});
        QVERIFY(!snapshot.applications.back().favorite);
        QVERIFY(!snapshot.tags.contains("lost"));
    }

    void undo_restores_metadata_shortcut_and_ordered_scenario_steps() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Database db(temporary.filePath("profile.db"));
        auto removed = steam_card("Removed", "10");
        removed.tags = {"work"};
        removed.aliases = {"first", "second"};
        removed.favorite = true;
        const auto id = db.save_application(removed);
        const auto keep_id = db.save_application(steam_card("Kept", "20"));
        db.save_scenario({0, "Session", {{id, 50, false, "private"}, {keep_id, 20, true, {}}, {id, 0, true, {}}}});
        db.record_launch(id);
        db.save_target_shortcut(false, id, "Ctrl+Alt+T");
        const auto before = db.read();
        db.remove_application(id);
        QVERIFY(db.can_undo_removal());
        const auto after = db.read();
        QCOMPARE(after.applications.size(), 1);
        QCOMPARE(after.applications.front().id, keep_id);
        QVERIFY(after.target_shortcuts.isEmpty());
        QCOMPARE(after.scenarios.size(), 1);
        QCOMPARE(after.scenarios.front().steps.size(), 1);
        QCOMPARE(after.scenarios.front().steps.front().application_id, keep_id);

        db.undo_removal();
        QVERIFY(!db.can_undo_removal());
        const auto restored = db.read();
        QCOMPARE(restored.applications.size(), 2);
        const auto &app = restored.applications.front();
        QCOMPARE(app.id, id);
        QCOMPARE(app.name, removed.name);
        QCOMPARE(app.tags, removed.tags);
        QCOMPARE(app.aliases, removed.aliases);
        QVERIFY(app.favorite);
        QCOMPARE(app.launch_count, 1);
        QCOMPARE(app.last_launch, before.applications.front().last_launch);
        QCOMPARE(restored.target_shortcuts.size(), 1);
        QCOMPARE(restored.target_shortcuts.front().target_id, id);
        QCOMPARE(restored.target_shortcuts.front().sequence, QString("Ctrl+Alt+T"));
        QCOMPARE(restored.scenarios.size(), 1);
        const auto &steps = restored.scenarios.front().steps;
        const auto &expected = before.scenarios.front().steps;
        QCOMPARE(steps.size(), expected.size());
        for (qsizetype i = 0; i < steps.size(); ++i) {
            QCOMPARE(steps[i].application_id, expected[i].application_id);
            QCOMPARE(steps[i].delay_ms, expected[i].delay_ms);
            QCOMPARE(steps[i].enabled, expected[i].enabled);
            QCOMPARE(steps[i].launch_profile, expected[i].launch_profile);
        }
    }

    void moved_folder_recovery_rechecks_files_and_preserves_card_identity() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto old_root = directory.filePath("old");
        const auto new_root = directory.filePath("moved");
        QVERIFY(QDir().mkpath(new_root));
        Database db(directory.filePath("profile.db"));
        app_record app;
        app.name = "Editor";
        app.path = QDir(old_root).filePath("editor.exe");
        app.working_directory = old_root;
        app.arguments = "--offline";
        app.favorite = true;
        app.tags = {"work"};
        const auto editor_id = db.save_application(app);
        db.record_launch(editor_id);
        const auto scenario_id = db.save_scenario({0, "Desk", {{editor_id, 0, true, {}}}});
        auto second = app;
        second.name = "Viewer";
        second.path = QDir(old_root).filePath("viewer.exe");
        db.save_application(second);
        auto neighbor = app;
        neighbor.name = "Outside";
        neighbor.path = directory.filePath("old-other/editor.exe");
        db.save_application(neighbor);

        for (const auto &name : {"editor.exe", "viewer.exe"}) {
            QFile file(QDir(new_root).filePath(name));
            QVERIFY(write_file(file.fileName(), "fixture"));
#ifdef Q_OS_LINUX
            QVERIFY(file.setPermissions(file.permissions() | QFileDevice::ExeOwner));
#endif
        }
        const auto preview = db.preview_relocation(old_root, new_root);
        QCOMPARE(preview.size(), 2);
        for (const auto &candidate : preview) {
            QVERIFY(candidate.issue.isEmpty());
            QCOMPARE(candidate.new_directory, new_root);
        }
        const auto editor = std::ranges::find(preview, editor_id, &path_relocation::application_id);
        QVERIFY(editor != preview.end());
        QCOMPARE(editor->new_path, QDir(new_root).filePath("editor.exe"));

        // The second replacement disappears after preview. Applying the batch must roll back the first.
        const auto viewer = std::ranges::find(preview, QString("Viewer"), &path_relocation::name);
        QVERIFY(viewer != preview.end());
        QVERIFY(QFile::remove(viewer->new_path));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, db.apply_relocation({*editor, *viewer}));
        const auto unchanged = db.read().applications;
        const auto original = std::ranges::find(unchanged, editor_id, &app_record::id);
        QVERIFY(original != unchanged.end());
        QCOMPARE(original->path, app.path);
        db.apply_relocation({*editor});
        const auto recovered = db.read();
        const auto moved = std::ranges::find(recovered.applications, editor_id, &app_record::id);
        QVERIFY(moved != recovered.applications.end());
        QCOMPARE(moved->path, editor->new_path);
        QCOMPARE(moved->working_directory, new_root);
        QCOMPARE(moved->arguments, app.arguments);
        QCOMPARE(moved->tags, app.tags);
        QVERIFY(moved->favorite);
        QCOMPARE(moved->launch_count, 1);
        QCOMPARE(recovered.scenarios.size(), 1);
        QCOMPARE(recovered.scenarios.front().id, scenario_id);
        QCOMPARE(recovered.scenarios.front().steps.size(), 1);
        QCOMPARE(recovered.scenarios.front().steps.front().application_id, editor_id);
    }

    void undo_conflicts_preserve_intervening_changes_data() {
        QTest::addColumn<bool>("edit_scenario");
        QTest::newRow("linked-scenario-edited") << true;
        QTest::newRow("launch-configuration-readded") << false;
    }

    void undo_conflicts_preserve_intervening_changes() {
        QFETCH(bool, edit_scenario);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        Database db(temporary.filePath("profile.db"));
        const auto first_id = db.save_application(steam_card("First", "10"));
        const auto second_id = db.save_application(steam_card("Second", "20"));
        const auto keep_id = db.save_application(steam_card("Kept", "30"));
        const auto scenario_id = db.save_scenario({0, "Session", {{first_id, 0, true, {}}, {keep_id, 50, true, {}}}});
        db.remove_applications({first_id, second_id});
        qint64 replacement_id = 0;
        if (edit_scenario)
            db.save_scenario({scenario_id, "Session", {{keep_id, 200, true, {}}}});
        else
            replacement_id = db.save_application(steam_card("Replacement", "20"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, db.undo_removal());
        const auto snapshot = db.read();
        QCOMPARE(snapshot.applications.size(), edit_scenario ? 1 : 2);
        QCOMPARE(snapshot.applications.front().id, keep_id);
        if (!edit_scenario) {
            QCOMPARE(snapshot.applications.back().id, replacement_id);
            QCOMPARE(snapshot.applications.back().name, QString("Replacement"));
        }
        QCOMPARE(snapshot.scenarios.size(), 1);
        QCOMPARE(snapshot.scenarios.front().steps.size(), 1);
        QCOMPARE(snapshot.scenarios.front().steps.front().application_id, keep_id);
        QCOMPARE(snapshot.scenarios.front().steps.front().delay_ms, edit_scenario ? 200 : 50);
    }
};

QTEST_GUILESS_MAIN(CatalogTest)
#include "catalog_test.moc"
