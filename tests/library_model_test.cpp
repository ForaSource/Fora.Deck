#include <algorithm>
#include <QImage>
#include <QLocale>
#include <QtTest>

#include "LibraryModel.h"

namespace desktop_platform {
QImage application_icon(const QString &) {
    return {};
}
}

class LibraryModelTest : public QObject {
    Q_OBJECT
    static app_record application(qint64 id, const QString &name) {
        app_record record;
        record.id = id;
        record.name = name;
        record.target_kind = "steam";
        record.path = QString::number(id);
        return record;
    }

    static QList<qint64> visible_ids(const LibraryFilter &filter) {
        QList<qint64> ids;
        for (int row = 0; row < filter.rowCount(); ++row)
            ids.append(filter.index(row, 0).data(LibraryModel::id_role).toLongLong());
        return ids;
    }

private slots:
    void combined_section_tags_and_query() {
        auto match = application(1, "Studio");
        match.favorite = true;
        match.aliases = {"editor"};
        match.tags = {"Work", "Team", "Desktop"};
        auto not_favorite = match;
        not_favorite.id = 2;
        not_favorite.favorite = false;
        auto missing_sidebar_tag = match;
        missing_sidebar_tag.id = 3;
        missing_sidebar_tag.tags.removeAll("Team");
        auto missing_query_tag = match;
        missing_query_tag.id = 4;
        missing_query_tag.tags.removeAll("Desktop");
        auto excluded = match;
        excluded.id = 5;
        excluded.tags.append("games");
        auto other_name = match;
        other_name.id = 6;
        other_name.aliases.clear();

        LibraryModel model;
        model.replace({other_name, excluded, missing_query_tag, missing_sidebar_tag, not_favorite, match});
        LibraryFilter filter;
        filter.setSourceModel(&model);
        filter.set_tag_section("favorites", {"work", "TEAM"});
        filter.set_query("editor #desktop -#games");
        QCOMPARE(visible_ids(filter), QList<qint64>{1});
        filter.set_tag_section("library", {"work", "TEAM"});
        QCOMPARE(visible_ids(filter), QList<qint64>({1, 2}));
        filter.set_section("library");
        QCOMPARE(visible_ids(filter), QList<qint64>({1, 3, 2}));
        filter.set_query({});
        QCOMPARE(filter.rowCount(), 6);
    }

    void running_section_recomputes_best_match() {
        auto exact = application(1, "Visual");
        auto suggestion = application(2, "Visuql");
        suggestion.running = true;
        LibraryModel model;
        model.replace({exact, suggestion});
        LibraryFilter filter;
        filter.setSourceModel(&model);
        filter.set_query("visual");
        QCOMPARE(visible_ids(filter), QList<qint64>{1});
        filter.set_section("running");
        QCOMPARE(visible_ids(filter), QList<qint64>{2});

        exact.running = true;
        model.update_state({exact});
        QCOMPARE(visible_ids(filter), QList<qint64>{1});
        exact.running = false;
        model.update_state({exact});
        QCOMPARE(visible_ids(filter), QList<qint64>{2});
        model.replace({suggestion, exact});
        QCOMPARE(visible_ids(filter), QList<qint64>{2});
    }

    void relevance_precedes_favorites_usage_and_selected_sort() {
        auto exact = application(1, "Editor");
        auto favorite = application(2, "Editor Five");
        favorite.favorite = true;
        auto frequent = application(3, "Editor Four");
        frequent.launch_count = 8;
        frequent.last_launch = 10;
        auto recent = application(4, "Editor Three");
        recent.launch_count = 2;
        recent.last_launch = 30;
        auto older = application(5, "Editor One");
        older.launch_count = 2;
        older.last_launch = 20;
        auto tied = application(6, "Editor Two");
        tied.launch_count = older.launch_count;
        tied.last_launch = older.last_launch;

        LibraryModel model;
        model.replace({older, recent, tied, frequent, favorite, exact});
        LibraryFilter filter;
        filter.setSourceModel(&model);
        filter.set_query("editor");
        QCOMPARE(visible_ids(filter), QList<qint64>({1, 2, 3, 4, 5, 6}));
        filter.sort(0, Qt::DescendingOrder);
        QCOMPARE(visible_ids(filter), QList<qint64>({1, 2, 3, 4, 6, 5}));
    }

    void catalog_sort_modes_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QList<qint64>>("ascending");
        QTest::addColumn<QList<qint64>>("descending");
        QTest::newRow("name") << 0 << QList<qint64>{2, 4, 3, 1} << QList<qint64>{1, 3, 4, 2};
        QTest::newRow("last-launch") << 1 << QList<qint64>{1, 3, 2, 4} << QList<qint64>{4, 2, 3, 1};
        QTest::newRow("favorites-first") << 2 << QList<qint64>{2, 3, 4, 1} << QList<qint64>{3, 2, 1, 4};
        QTest::newRow("tags") << 3 << QList<qint64>{1, 4, 3, 2} << QList<qint64>{2, 3, 4, 1};
        QTest::newRow("status") << 4 << QList<qint64>{4, 2, 1, 3} << QList<qint64>{3, 1, 2, 4};
    }

    void catalog_sort_modes() {
        QFETCH(int, mode);
        QFETCH(QList<qint64>, ascending);
        QFETCH(QList<qint64>, descending);
        auto zulu = application(1, "Zulu");
        zulu.last_launch = 30;
        zulu.tags = {"Alpha"};
        zulu.available = false;
        zulu.volume_unavailable = true;
        auto alpha = application(2, "Alpha");
        alpha.last_launch = 10;
        alpha.favorite = true;
        auto delta = application(3, "Delta");
        delta.last_launch = 20;
        delta.favorite = true;
        delta.tags = {"Zulu"};
        delta.available = false;
        auto bravo = application(4, "Bravo");
        bravo.tags = {"Delta"};
        bravo.running = true;

        LibraryModel model;
        model.replace({zulu, alpha, delta, bravo});
        LibraryFilter filter;
        filter.setSourceModel(&model);
        filter.set_sort_mode(mode);
        QCOMPARE(visible_ids(filter), ascending);
        filter.set_sort_mode(mode, Qt::DescendingOrder);
        QCOMPARE(visible_ids(filter), descending);
    }

    void process_usage_updates_and_visibility() {
        auto app = application(1, "Studio");
        app.running = true;
        LibraryModel model;
        model.replace({app});
        const auto index = model.index(0, 0);
        const auto usage = [&] { return index.data(LibraryModel::process_usage_role).toString(); };
        const auto tooltip = [&] { return index.data(Qt::ToolTipRole).toString(); };
        QCOMPARE(usage(), QString::fromUtf8("CPU: — · RAM: —"));
        QVERIFY(tooltip().endsWith('\n' + usage()));

        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        app.cpu_percent = 12.5;
        app.memory_bytes = 3 * 1024 * 1024;
        model.update_state({app});
        QVERIFY(!changes.isEmpty());
        const auto roles = changes.takeLast().at(2).value<QList<int>>();
        QVERIFY(roles.isEmpty() || roles.contains(LibraryModel::process_usage_role));
        QVERIFY(roles.isEmpty() || roles.contains(Qt::ToolTipRole));
        const auto cpu = QString("CPU: %1%").arg(QLocale{}.toString(12.5, 'f', 1));
        const auto ram = QString("RAM: %1").arg(QLocale{}.formattedDataSize(app.memory_bytes, 1));
        QCOMPARE(usage(), cpu + QString::fromUtf8(" · ") + ram);
        QVERIFY(tooltip().endsWith('\n' + usage()));

        changes.clear();
        model.set_process_display(true, false);
        QVERIFY(!changes.isEmpty());
        QCOMPARE(usage(), ram);
        QVERIFY(!tooltip().contains(cpu));
        model.set_process_display(false, true);
        QCOMPARE(usage(), cpu);
        QVERIFY(!tooltip().contains(ram));
        model.set_process_display(false, false);
        QVERIFY(usage().isEmpty());
        QVERIFY(!tooltip().contains(cpu));
        QVERIFY(!tooltip().contains(ram));
        QVERIFY(index.data(LibraryModel::running_role).toBool());
        model.set_process_display(true, true);
        app.running = false;
        model.update_state({app});
        QVERIFY(usage().isEmpty());
        QVERIFY(!tooltip().contains(cpu));
        QVERIFY(!tooltip().contains(ram));
        QVERIFY(!index.data(LibraryModel::running_role).toBool());
    }

    void indexed_lookup_after_reset() {
        LibraryModel model;
        QVector<app_record> records;
        for (int row = 0; row < 10000; ++row) {
            app_record record;
            record.id = row + 1;
            record.name = QString("Application %1").arg(row);
            record.target_kind = "steam";
            record.path = QString::number(row + 1);
            records.append(record);
        }
        model.replace(records);
        for (int repeat = 0; repeat < 10; ++repeat)
            for (int row = 0; row < records.size(); ++row) {
                QCOMPARE(model.row_for_id(records[row].id), row);
                QCOMPARE(model.find(records[row].id)->name, records[row].name);
            }
        QCOMPARE(model.row_for_id(-1), -1);
        QVERIFY(!model.find(-1));
        QVERIFY(!model.data(model.index(model.row_for_id(-1), 0), Qt::DecorationRole).isValid());
        std::reverse(records.begin(), records.end());
        model.replace(records);
        QCOMPARE(model.row_for_id(10000), 0);
        QCOMPARE(model.row_for_id(1), 9999);
        records.removeLast();
        model.replace(records);
        QVERIFY(!model.find(1));
        QCOMPARE(model.row_for_id(1), -1);
        app_record state = records.front();
        state.running = true;
        model.update_state({state});
        QCOMPARE(model.row_for_id(10000), 0);
        QVERIFY(model.find(10000)->running);
        model.replace({});
        QCOMPARE(model.row_for_id(10000), -1);
        QVERIFY(!model.find(10000));
    }
};
QTEST_MAIN(LibraryModelTest)
#include "library_model_test.moc"
