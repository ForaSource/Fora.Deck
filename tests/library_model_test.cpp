#include <algorithm>
#include <QImage>
#include <QtTest>

#include "LibraryModel.h"

namespace desktop_platform {
QImage application_icon(const QString &) {
    return {};
}
}

class LibraryModelTest : public QObject {
    Q_OBJECT
private slots:
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
