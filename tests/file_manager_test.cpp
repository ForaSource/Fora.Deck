#include <memory>
#include <optional>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "FileManager.h"
#include "SqliteHelpers.h"

namespace {
bool write_file(const QString &path, const QByteArray &contents = "fixture") {
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly) &&
           file.write(contents) == contents.size() && file.flush();
}

template <typename Result, typename Start> Result wait_for_result(Start start) {
    std::optional<Result> result;
    QObject receiver;
    start(&receiver, [&](Result value) { result.emplace(std::move(value)); });
    if (!QTest::qWaitFor([&] { return result.has_value(); }, 5000)) {
        QTest::qFail("FileManager did not deliver the result", __FILE__, __LINE__);
        return {};
    }
    return std::move(*result);
}

QVector<file_match> saved_files(FileManager &manager) {
    return wait_for_result<QVector<file_match>>(
        [&](QObject *receiver, auto completed) { manager.saved_files(receiver, completed); });
}

QHash<QString, qint64> directory_dates(FileManager &manager, const QString &path) {
    return wait_for_result<QHash<QString, qint64>>(
        [&](QObject *receiver, auto completed) { manager.directory_dates(path, receiver, completed); });
}
}

class FileManagerTest : public QObject {
    Q_OBJECT
private slots:
    void accepted_content_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("uppercase-document") << "draft.PDF" << QByteArray("%PDF") << true;
        QTest::newRow("utf16-text") << "notes.txt" << QByteArray::fromHex("fffe4100") << true;
        QTest::newRow("binary-media") << "model.blend" << QByteArray::fromHex("4200") << true;
        QTest::newRow("binary-text") << "notes.txt" << QByteArray::fromHex("410042") << false;
        QTest::newRow("hidden-name") << ".draft.txt" << QByteArray("notes") << false;
        QTest::newRow("editor-lock") << "~draft.docx" << QByteArray("notes") << false;
        QTest::newRow("temporary-download") << "draft.part.pdf" << QByteArray("%PDF") << false;
        QTest::newRow("unsupported-type") << "notes.bin" << QByteArray("notes") << false;
        QTest::newRow("renamed-windows-binary") << "draft.pdf" << QByteArray("MZ") << false;
        QTest::newRow("renamed-linux-binary") << "draft.pdf" << QByteArray::fromHex("7f454c46") << false;
        QTest::newRow("renamed-database") << "draft.pdf" << QByteArray("SQLite format 3") << false;
    }

    void accepted_content() {
        QFETCH(QString, name);
        QFETCH(QByteArray, contents);
        QFETCH(bool, accepted);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath(name);
        QVERIFY(write_file(path, contents));
        QCOMPARE(FileManager::accepts_file(path), accepted);
        QCOMPARE(FileManager::can_open(path), accepted);
    }

    void batch_save_reports_each_path_and_keeps_overlapping_writes() {
        QTemporaryDir temporary;
        const auto first = temporary.filePath("one/draft.txt");
        const auto second = temporary.filePath("two/draft.txt");
        const auto third = temporary.filePath("third.txt");
        const auto excluded = temporary.filePath("excluded.exe");
        QVERIFY(write_file(first));
        QVERIFY(write_file(second));
        QVERIFY(write_file(third));
        QVERIFY(write_file(excluded));
        FileManager manager(temporary.path());
        QSignalSpy updated(&manager, &FileManager::saved_paths_changed);
        QObject receiver;
        std::optional<file_save_result> initial;
        std::optional<file_save_result> overlap;
        manager.save_files({first, first, second, excluded}, &receiver,
                           [&](file_save_result result) { initial = std::move(result); });
        manager.save_files({second, third}, &receiver,
                           [&](file_save_result result) { overlap = std::move(result); });
        QTRY_VERIFY(initial && overlap);
        QCOMPARE(initial->added.size(), 2);
        QCOMPARE(initial->rejected.size(), 1);
        QCOMPARE(overlap->already_saved.size(), 1);
        QCOMPARE(overlap->added.size(), 1);
        QCOMPARE(updated.size(), 2);
        QCOMPARE(manager.persisted_saved_paths().size(), 3);
    }

    void batch_rolls_back_and_can_retry_after_write_error() {
        QTemporaryDir temporary;
        const auto first = temporary.filePath("first.txt");
        const auto blocked = temporary.filePath("blocked.txt");
        const auto vanished = temporary.filePath("vanished.txt");
        QVERIFY(write_file(first));
        QVERIFY(write_file(blocked));
        QVERIFY(write_file(vanished));
        FileManager manager(temporary.path());
        const auto inspected = wait_for_result<file_save_result>(
            [&](QObject *receiver, auto completed) { manager.inspect_saved_files({vanished}, receiver, completed); });
        QCOMPARE(inspected.added, QStringList{FileManager::saved_path_identity(vanished)});
        QVERIFY(QFile::remove(vanished));
        sqlite3 *raw = nullptr;
        QCOMPARE(sqlite3_open(temporary.filePath("file-index.sqlite").toUtf8().constData(), &raw), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        xlibrary::sqlite_execute(db.get(), "CREATE TRIGGER blocked_document BEFORE INSERT ON saved "
                                           "WHEN NEW.path LIKE '%/blocked.txt' "
                                           "BEGIN SELECT RAISE(ABORT, 'blocked document'); END");
        QSignalSpy updated(&manager, &FileManager::saved_paths_changed);
        const auto failure = wait_for_result<file_save_result>([&](QObject *receiver, auto completed) {
            manager.save_files({first, blocked, vanished}, receiver, completed);
        });
        QVERIFY(failure.error.contains("blocked document"));
        QVERIFY(failure.added.isEmpty());
        QVERIFY(manager.persisted_saved_paths().isEmpty());
        QCOMPARE(updated.size(), 0);
        xlibrary::sqlite_execute(db.get(), "DROP TRIGGER blocked_document");
        const auto retry = wait_for_result<file_save_result>([&](QObject *receiver, auto completed) {
            manager.save_files({first, blocked, vanished}, receiver, completed);
        });
        QVERIFY(retry.error.isEmpty());
        QCOMPARE(retry.added.size(), 2);
        QVERIFY(retry.rejected.contains(FileManager::saved_path_identity(vanished)));
        QCOMPARE(updated.size(), 1);
        QCOMPARE(manager.persisted_saved_paths().size(), 2);
    }

    void closing_receiver_does_not_cancel_confirmed_write() {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("notes.txt");
        QVERIFY(write_file(path));
        QStringList paths;
        {
            FileManager manager(temporary.path());
            {
                QObject receiver;
                manager.save_files({path}, &receiver, [](file_save_result) {
                    QFAIL("A destroyed receiver must not see the result");
                });
            }
            manager.shutdown();
            QCoreApplication::processEvents();
            paths = manager.persisted_saved_paths();
            QCOMPARE(paths, QStringList{FileManager::saved_path_identity(path)});
        }
        FileManager restored(temporary.path(), paths);
        QCOMPARE(restored.persisted_saved_paths(), QStringList{FileManager::saved_path_identity(path)});
    }

    void saved_paths_survive_restart_independently_of_favorites() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto profile = temporary.filePath("profile");
        const auto path = temporary.filePath("draft.md");
        QVERIFY(write_file(path));
        QStringList paths;
        QVariantMap metadata;
        {
            FileManager manager(profile);
            QSignalSpy saved(&manager, &FileManager::saved_paths_changed);
            manager.update_metadata(path, {"Work"}, true);
            QVERIFY(manager.persisted_saved_paths().isEmpty());
            manager.set_saved(path, true);
            manager.set_saved(path, true);
            QTRY_VERIFY(!manager.is_saving());
            QVERIFY(!saved.isEmpty());
            paths = saved.last().first().toStringList();
            QCOMPARE(paths.size(), 1);
            QCOMPARE(QFileInfo(paths.first()), QFileInfo(path));
            QCOMPARE(manager.persisted_saved_paths(), paths);
            manager.update_metadata(path, {"Work"}, false);
            const auto entries = saved_files(manager);
            QCOMPARE(entries.size(), 1);
            QVERIFY(entries.first().saved);
            QVERIFY(entries.first().available);
            QCOMPARE(entries.first().path, paths.first());
            metadata = manager.metadata_values();
        }
        QVERIFY(QFile::remove(path));
        FileManager restored(profile, paths);
        restored.restore_metadata(metadata);
        QCOMPARE(restored.tags_for(path), QStringList{"Work"});
        QVERIFY(!restored.is_favorite(path));
        const auto entries = saved_files(restored);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().path, paths.first());
        QVERIFY(!entries.first().available);
        QVERIFY(!FileManager::can_open(path));
        QVERIFY(write_file(path));
        restored.set_saved(path, false);
        QTRY_VERIFY(!restored.is_saving());
        QVERIFY(saved_files(restored).isEmpty());
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(restored.tags_for(path), QStringList{"Work"});
    }

    void rejected_save_preserves_other_saved_entries() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto saved = temporary.filePath("saved.txt");
        const auto rejected = temporary.filePath("broken.tmp.txt");
        QVERIFY(write_file(saved));
        QVERIFY(write_file(rejected));
        FileManager manager(temporary.filePath("profile"), {saved});
        QSignalSpy errors(&manager, &FileManager::error);
        manager.set_saved(rejected, true);
        QTRY_VERIFY(!manager.is_saving());
        QCOMPARE(errors.size(), 1);
        const auto entries = saved_files(manager);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(QFileInfo(entries.first().path), QFileInfo(saved));
    }

    void shutdown_finishes_queued_saved_file_changes_in_order() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto first = temporary.filePath("first.txt");
        const auto second = temporary.filePath("second.txt");
        QVERIFY(write_file(first));
        QVERIFY(write_file(second));
        FileManager manager(temporary.filePath("profile"));
        manager.set_saved(first, true);
        manager.set_saved(second, true);
        manager.set_saved(first, false);
        manager.shutdown();
        const auto paths = manager.persisted_saved_paths();
        QCOMPARE(paths.size(), 1);
        QCOMPARE(QFileInfo(paths.first()), QFileInfo(second));
    }

    void excluded_sources_hide_saved_matches_without_removing_saved_paths() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath("report.md");
        QVERIFY(write_file(path));
        FileManager manager(temporary.filePath("profile"), {path});
        // Personal folders and TEMP may be redirected to a network share outside the local drives.
        auto exclusions = FileManager::volume_roots();
        for (const auto location :
             {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation})
            exclusions.append(QStandardPaths::writableLocation(location));
        exclusions.append(temporary.path());
        manager.configure(file_search_mode::basic, {temporary.path()}, exclusions);
        const auto entries = wait_for_result<QVector<file_match>>([&](QObject *receiver, auto completed) {
            const quint64 request = 42;
            manager.search(
                "*.md", request, receiver,
                [completed](quint64 delivered, QVector<file_match> matches) {
                    QCOMPARE(delivered, quint64(42));
                    completed(std::move(matches));
                },
                std::make_shared<std::atomic<quint64>>(request));
        });
        QVERIFY(entries.isEmpty());
        QTRY_VERIFY(manager.status().startsWith("Indexed "));
        QVERIFY(manager.sources().isEmpty());
        manager.configure(file_search_mode::off);
        QVERIFY(!manager.is_enabled());
        const auto saved = saved_files(manager);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(QFileInfo(saved.first().path), QFileInfo(path));
        QVERIFY(saved.first().available);
    }

    void search_honors_large_limit_and_refills_after_rejected_files() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QStringList paths;
        for (int index = 0; index < 240; ++index) {
            const auto path = temporary.filePath(QString("draft-%1.txt").arg(index, 3, 10, QLatin1Char('0')));
            QVERIFY(write_file(path, index < 10 ? QByteArray("MZ") : QByteArray("notes")));
            paths.append(path);
        }
        FileManager manager(temporary.filePath("profile"), paths);
        QStringList exclusions;
        for (const auto location :
             {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation})
            exclusions.append(QStandardPaths::writableLocation(location));
        manager.configure(file_search_mode::basic, {}, exclusions);
        for (const auto &path : paths)
            manager.update_metadata(path, {}, true);
        const auto entries = wait_for_result<QVector<file_match>>([&](QObject *receiver, auto completed) {
            manager.search({}, 1, receiver,
                           [completed](quint64, QVector<file_match> matches) { completed(std::move(matches)); },
                           std::make_shared<std::atomic<quint64>>(1), {}, {}, true, 225, 1);
        });
        QCOMPARE(entries.size(), 225);
        QCOMPARE(entries.first().name, QString("draft-010.txt"));
        QCOMPARE(entries.last().name, QString("draft-234.txt"));
    }

    void filename_queries_data() {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("extension-and-tag") << ".pdf #WORK -#archive" << QStringList{"Report[4].PDF"};
        QTest::newRow("literal-brackets-in-mask") << "report[?].pdf #work" << QStringList{"Report[4].PDF"};
        QTest::newRow("single-character-mask") << "memo-??.txt" << QStringList{"memo-12.txt"};
        QTest::newRow("extension-with-name") << "memo.txt" << QStringList{"memo-12.txt", "memo-123.txt"};
        QTest::newRow("unknown-tag") << "*.pdf #missing" << QStringList{};
        QTest::newRow("unknown-exclusion") << "*.pdf -#missing" << QStringList{"Report[4].PDF", "Report4.pdf"};
    }

    void filename_queries() {
        QFETCH(QString, query);
        QFETCH(QStringList, expected);
        QVector<file_match> files;
        for (const QString &name : {"Report[4].PDF", "Report4.pdf", "memo-12.txt", "memo-123.txt", "memo.docx"}) {
            file_match entry;
            entry.name = name;
            entry.tags = {"work"};
            if (name == "Report4.pdf")
                entry.tags.append("Archive");
            files.append(entry);
        }
        QStringList names;
        for (const auto &match : FileManager::filter_matches(query, files))
            names.append(match.name);
        QCOMPARE(names, expected);
    }

    void tag_renaming_merges_equivalent_names_and_preserves_other_files() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto first = temporary.filePath("one.txt");
        const auto second = temporary.filePath("two.txt");
        FileManager manager(temporary.filePath("profile"));
        manager.update_metadata(first, {"Work", "Review"}, true);
        manager.update_metadata(second, {"WORK", "Final"}, false);
        manager.rename_tag("work", "Review");
        QCOMPARE(manager.tags_for(first), QStringList{"Review"});
        QCOMPARE(manager.tags_for(second), (QStringList{"Final", "Review"}));
        QCOMPARE(manager.tag_names(), (QStringList{"Final", "Review"}));
        manager.update_metadata(first, {"invalid tag"}, false);
        QCOMPARE(manager.tags_for(first), QStringList{"Review"});
        QVERIFY(manager.is_favorite(first));
        manager.rename_tag("review", {});
        QVERIFY(manager.tags_for(first).isEmpty());
        QVERIFY(manager.is_favorite(first));
        QCOMPARE(manager.tags_for(second), QStringList{"Final"});
    }

    void restoring_metadata_sanitizes_tags_and_ignores_relative_paths() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath("draft.txt");
        const auto previous = temporary.filePath("previous.txt");
        FileManager manager(temporary.filePath("profile"));
        manager.update_metadata(previous, {"old"}, true);
        const QVariantMap properties{{"tags", QStringList{"Work", "work", "bad tag", "Final"}}, {"favorite", true}};
        manager.restore_metadata({{path, properties}, {"relative.md", properties}});
        QCOMPARE(manager.metadata_values().size(), 1);
        QCOMPARE(manager.tags_for(path), (QStringList{"Work", "Final"}));
        QVERIFY(manager.is_favorite(path));
        QVERIFY(manager.tags_for(previous).isEmpty());
        QVERIFY(!manager.is_favorite(previous));
    }

    void old_index_recovers_added_dates_only_for_immediate_children() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto folder = temporary.filePath("records_10%");
        QVERIFY(QDir().mkpath(folder));
        const auto path = QDir(folder).filePath("draft.txt");
        const auto nested = QDir(folder).filePath("sub/draft.txt");
        const auto neighbor = temporary.filePath("recordsX10x/sibling.txt");
        {
            // This was the persisted file index before first-seen timestamps were introduced.
            sqlite3 *raw = nullptr;
            QCOMPARE(sqlite3_open(temporary.filePath("file-index.sqlite").toUtf8().constData(), &raw), SQLITE_OK);
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
            xlibrary::sqlite_execute(raw, "CREATE TABLE files(path TEXT PRIMARY KEY,name TEXT NOT NULL,"
                                          "scope TEXT NOT NULL,seen INTEGER NOT NULL)");
            for (const auto &entry : {path, nested, neighbor}) {
                auto key = entry;
#ifdef Q_OS_WIN
                key = key.toCaseFolded();
#endif
                xlibrary::SqliteStatement insert(raw, "INSERT INTO files VALUES(?,?,'old',1234567)");
                insert.bind(1, key);
                insert.bind(2, QFileInfo(entry).fileName());
                insert.next();
            }
        }
        FileManager manager(temporary.path());
        const auto dates = directory_dates(manager, folder);
        QCOMPARE(dates.size(), 1);
        QCOMPARE(QFileInfo(dates.constBegin().key()).fileName(), QString("draft.txt"));
        QCOMPARE(dates.constBegin().value(), qint64(1234567));
        QCOMPARE(directory_dates(manager, path), dates);
        QVERIFY(write_file(path));
        manager.set_saved(path, true);
        QTRY_VERIFY(!manager.is_saving());
        QCOMPARE(directory_dates(manager, folder), dates);
    }
};

QTEST_GUILESS_MAIN(FileManagerTest)
#include "file_manager_test.moc"
