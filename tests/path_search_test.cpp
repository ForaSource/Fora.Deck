#include <future>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "SearchResults.h"

class PathSearchTest : public QObject {
    Q_OBJECT
    static void touch(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Cannot create path fixture");
    }
private slots:
    void working_files_apply_metadata_and_keep_directories_navigable() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("folder"));
        const auto report = directory.filePath("report.PDF");
        const auto unrelated = directory.filePath("other.txt");
        touch(report);
        touch(unrelated);
        touch(directory.filePath("program.exe"));
        const auto key = normalized_path(report);
        const QVariantMap metadata{{key, QVariantMap{{"tags", QStringList{"Work", "Team"}}, {"favorite", true}}},
                                   {normalized_path(unrelated), QVariantMap{{"tags", QStringList{"Work"}}}}};
        const auto generation = std::make_shared<std::atomic<quint64>>(1);
        const auto lookup = [&](const QStringList &tags, bool favorites_only) {
            return search_results::directory_entries(directory.path(), generation, 1, {{key, {3, 50}}}, {key}, true,
                                                     metadata, tags, favorites_only, {{key, 25}});
        };
        const auto entries = lookup({}, false);
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries.front().type, search_entry::kind::directory);
        QCOMPARE(entries[1].path, report);
        QCOMPARE(entries[1].extension, "pdf");
        QCOMPARE(entries[1].added, 25);
        QCOMPARE(entries[1].last_used, 50);
        QCOMPARE(entries[1].use_count, 3);
        QVERIFY(entries[1].saved);
        QVERIFY(entries[1].favorite);
        QCOMPARE(entries[2].path, unrelated);
        for (const auto &filtered : {lookup({"work", "team"}, false), lookup({}, true)}) {
            QCOMPARE(filtered.size(), 2);
            QCOMPARE(filtered[0].type, search_entry::kind::directory);
            QCOMPARE(filtered[1].path, report);
        }
        const auto no_files = lookup({"unknown"}, true);
        QCOMPARE(no_files.size(), 1);
        QCOMPARE(no_files.front().type, search_entry::kind::directory);
    }

    void calculator_data() {
        QTest::addColumn<QString>("query");
        QTest::addColumn<bool>("recognized");
        QTest::addColumn<QVariant>("expected");
        QTest::newRow("precedence") << "2 + 3 * 4" << true << QVariant(14.);
        QTest::newRow("right-associative-power") << "2**3^2" << true << QVariant(512.);
        QTest::newRow("negative-power-base") << "-2^2" << true << QVariant(-4.);
        QTest::newRow("decimal-comma") << "(1,5 + 2) × 4 ÷ 2" << true << QVariant(7.);
        QTest::newRow("root-and-remainder") << "sqrt(81) % 4" << true << QVariant(1.);
        QTest::newRow("divide-by-zero") << "42 / 0" << true << QVariant{};
        QTest::newRow("negative-root") << "sqrt(-9)" << true << QVariant{};
        QTest::newRow("incomplete") << "2 + (" << true << QVariant{};
        QTest::newRow("ordinary-search") << "report 2026" << false << QVariant{};
    }

    void calculator() {
        QFETCH(QString, query);
        QFETCH(bool, recognized);
        QFETCH(QVariant, expected);
        const auto calculation = search_query::calculate(query);
        QCOMPARE(calculation.recognized, recognized);
        QCOMPARE(calculation.value.has_value(), expected.isValid());
        if (expected.isValid())
            QCOMPARE(*calculation.value, expected.toDouble());
    }

    void ordering_identity_and_prefix() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(QDir(temporary.path()).mkdir("z-directory"));
        touch(temporary.filePath("a-file"));
        touch(temporary.filePath(".hidden"));
        touch(temporary.filePath("Prefix-one"));
        auto generation = std::make_shared<std::atomic<quint64>>(1);
        const auto entries = search_results::directory_entries(temporary.path() + '/', generation, 1);
        QCOMPARE(entries.size(), 4);
        QCOMPARE(entries.front().type, search_entry::kind::directory);
        QCOMPARE(entries.front().path, temporary.filePath("z-directory"));
        QCOMPARE(entries[1].title, QString(".hidden"));
        const auto prefixed = search_results::directory_entries(temporary.filePath("pre"), generation, 1);
        QCOMPARE(prefixed.size(), 1);
        QCOMPARE(prefixed.front().path, temporary.filePath("Prefix-one"));
    }
    void bounded_and_cancellable() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        for (int i = 0; i < search_results::scan_limit + 10; ++i)
            touch(temporary.filePath(QString("file-%1").arg(i, 5, 10, QLatin1Char('0'))));
        auto generation = std::make_shared<std::atomic<quint64>>(1);
        const auto entries = search_results::directory_entries(temporary.path() + '/', generation, 1);
        QCOMPARE(entries.size(), search_results::result_limit + 1);
        QCOMPARE(entries.back().type, search_entry::kind::open_folder);
        QCOMPARE(entries.back().path, temporary.path());
        for (qsizetype i = 1; i < entries.size() - 1; ++i)
            QVERIFY(entries[i - 1].title < entries[i].title);
        const auto missing_prefix = search_results::directory_entries(temporary.filePath("zz-no-match"), generation, 1);
        QCOMPARE(missing_prefix.size(), 1);
        QCOMPARE(missing_prefix.front().type, search_entry::kind::open_folder);
        QVERIFY(search_results::directory_entries(temporary.path(), generation, 0).isEmpty());
        auto worker = std::async(std::launch::async,
                                 [&] { return search_results::directory_entries(temporary.path(), generation, 1); });
        generation->store(2);
        QVERIFY(worker.get().isEmpty());
        const auto next = search_results::directory_entries(temporary.filePath("file-0000"), generation, 2);
        QVERIFY(!next.isEmpty());
        QVERIFY(next.front().title.startsWith("file-0000") || next.front().type == search_entry::kind::open_folder);
    }
    void unavailable() {
        QTemporaryDir temporary;
        auto generation = std::make_shared<std::atomic<quint64>>(1);
        auto result = search_results::directory_entries(temporary.filePath("missing/"), generation, 1);
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.front().type, search_entry::kind::message);
#ifndef Q_OS_WIN
        const auto closed = temporary.filePath("closed");
        QVERIFY(QDir().mkpath(closed));
        QVERIFY(QFile::setPermissions(closed, QFileDevice::WriteOwner));
        result = search_results::directory_entries(closed + '/', generation, 1);
        QVERIFY(
            QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.front().type, search_entry::kind::message);
#endif
    }
};
QTEST_GUILESS_MAIN(PathSearchTest)
#include "path_search_test.moc"
