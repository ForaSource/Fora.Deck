#include <algorithm>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>

#include "Manager.h"

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTextStream output(stdout);
    output << "records,first_us,min_us,median_us,max_us\n";
    for (const auto count : {0, 255, 1000, 10000}) {
        Manager manager(QString{}, std::make_shared<std::atomic<quint64>>(0));
        for (int row = 0; row < count; ++row) {
            app_record record;
            record.id = row + 1;
            record.name = QString::number(row);
            record.path = QCoreApplication::applicationFilePath();
            record.target_kind = "executable";
            record.arguments = QString::number(row);
            manager.catalog.applications.append(record);
        }
        QVector<qint64> samples;
        qint64 first = 0;
        for (int sample = 0; sample < 4; ++sample) {
            QElapsedTimer timer;
            timer.start();
            manager.poll();
            const auto microseconds = timer.nsecsElapsed() / 1000;
            if (sample == 0)
                first = microseconds;
            else
                samples.append(microseconds);
        }
        std::sort(samples.begin(), samples.end());
        output << count << ',' << first << ',' << samples.front() << ',' << samples[1] << ',' << samples.back() << '\n';
        output.flush();
    }
}
