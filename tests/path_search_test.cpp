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
