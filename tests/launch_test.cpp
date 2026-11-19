#include <QTemporaryDir>
#include <QtTest>

#include "LaunchArguments.h"
#include "Manager.h"

class LaunchTest : public QObject {
    Q_OBJECT
private slots:
    void split_arguments_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QByteArrayList>("expected");
        QTest::newRow("only-whitespace") << QByteArray(" \t\r\n") << QByteArrayList{};
        QTest::newRow("quoted-empty") << QByteArray("a \"\" '' b") << QByteArrayList{"a", "", "", "b"};
        QTest::newRow("mixed-quotes") << QByteArray("a'b c'\"d\" escaped\\ space")
                                      << QByteArrayList{"ab cd", "escaped space"};
        QTest::newRow("literal-unicode") << QByteArray("\"два слова\" '$HOME' *.pdf")
                                         << QByteArrayList{"два слова", "$HOME", "*.pdf"};
        QTest::newRow("backslash-in-quotes")
            << QByteArray("\"a\\qb\\\"c\" 'd\\e'") << QByteArrayList{"a\\qb\"c", "d\\e"};
        QTest::newRow("continued-line") << QByteArray("a\\\nb") << QByteArrayList{"ab"};
    }

    void split_arguments() {
        QFETCH(QByteArray, input);
        QFETCH(QByteArrayList, expected);
        QByteArrayList actual;
        for (const auto &argument : xlibrary::split_launch_arguments(input.toStdString()))
            actual.append(QByteArray::fromStdString(argument));
        QCOMPARE(actual, expected);
    }

    void rejects_incomplete_arguments_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("single-quote") << QByteArray("'unfinished");
        QTest::newRow("double-quote") << QByteArray("\"unfinished");
        QTest::newRow("trailing-escape") << QByteArray("unfinished\\");
        QTest::newRow("embedded-null") << QByteArray("a\0b", 3);
    }

    void rejects_incomplete_arguments() {
        QFETCH(QByteArray, input);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, xlibrary::split_launch_arguments(input.toStdString()));
    }

    void workspace_launch_data() {
        QTest::addColumn<bool>("current_generation");
        QTest::addColumn<bool>("current_configuration");
        QTest::addColumn<bool>("platform_accepts");
        QTest::addColumn<operation_state>("expected_state");
        QTest::addColumn<operation_error_source>("expected_source");
        QTest::newRow("launched") << true << true << true << operation_state::completed << operation_error_source::none;
        QTest::newRow("cancelled") << false << true << true << operation_state::cancelled
                                   << operation_error_source::none;
        QTest::newRow("edited-after-preflight")
            << true << false << true << operation_state::error << operation_error_source::validation;
        QTest::newRow("launcher-failed") << true << true << false << operation_state::error
                                         << operation_error_source::platform;
    }

    void workspace_launch() {
        QFETCH(bool, current_generation);
        QFETCH(bool, current_configuration);
        QFETCH(bool, platform_accepts);
        QFETCH(operation_state, expected_state);
        QFETCH(operation_error_source, expected_source);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Manager manager(directory.filePath("profile.db"), std::make_shared<std::atomic<quint64>>(0));
        manager.initialize(1);
        QVERIFY(manager.store);
        app_record app;
        app.path = QCoreApplication::applicationFilePath();
        app.name = "Fixture";
        const auto id = manager.store->save_application(app);
        manager.refresh();
        const auto &apps = manager.catalog.applications;
        QCOMPARE(apps.size(), 1);
        const launch_context context{launch_origin::workspace, 5,
                                     apps.front().configuration_id + (current_configuration ? 0 : 1)};
        const auto generation = std::make_shared<std::atomic<quint64>>(current_generation ? 5 : 6);
        int dispatches = 0;
        const auto launch = [&](const app_record &selected, Application::launch_mode mode) -> qint64 {
            ++dispatches;
            if (!platform_accepts)
                throw std::runtime_error("fixture launch failure");
            return selected.id == id && mode == Application::launch_mode::launch ? 42 : 0;
        };
        QSignalSpy launched(&manager, &Manager::launched);
        manager.execute(
            17, "workspace_launch",
            [&](Manager &worker) {
                worker.launch_record(id, Application::launch_mode::launch, context, generation, launch);
            },
            operation_error_source::platform);
        QCOMPARE(dispatches, current_generation && current_configuration ? 1 : 0);
        QCOMPARE(launched.count(), 1);
        const auto args = launched.takeFirst();
        QCOMPARE(args[0].toLongLong(), id);
        const auto result = args[1].value<operation_result>();
        QCOMPARE(result.id, quint64{17});
        QCOMPARE(result.operation, "workspace_launch");
        QCOMPARE(result.state, expected_state);
        QCOMPARE(result.error.source, expected_source);
        const bool succeeded = expected_state == operation_state::completed;
        QCOMPARE(args[3].toLongLong(), succeeded ? 42 : 0);
        const auto saved = manager.store->read().applications;
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.front().launch_count, succeeded ? 1 : 0);
        QCOMPARE(saved.front().last_launch > 0, succeeded);
    }

    void scenario_preflight_reports_only_enabled_invalid_steps() {
        app_record app;
        app.id = 1;
        app.name = "Editor";
        app.path = QCoreApplication::applicationFilePath();
        scenario_record scenario;
        scenario.steps = {{99, 0, false, {}}, {app.id, 0, true, {}}};
        QVERIFY(Manager::scenario_issues(scenario, {app}).isEmpty());
        scenario.steps.append({99, 0, true, {}});
        scenario.steps.append({app.id, -1, true, {}});
        const auto issues = Manager::scenario_issues(scenario, {app});
        QCOMPARE(issues.size(), 2);
        QVERIFY(issues[0].contains("Step 3"));
        QVERIFY(issues[0].contains("Missing application"));
        QVERIFY(issues[1].contains("Step 4"));
        QVERIFY(issues[1].contains("delay"));
        scenario.steps = {{app.id, 0, false, {}}};
        QVERIFY(!Manager::scenario_issues(scenario, {app}).isEmpty());
    }

    void monitoring_switches_keep_running_detection() {
        Manager manager({}, std::make_shared<std::atomic<quint64>>(0));
        app_record app;
        app.path = QCoreApplication::applicationFilePath();
        manager.catalog.applications = {app};
        const auto &current = manager.catalog.applications.front();
        manager.set_process_monitoring(false, false);
        manager.poll();
        QVERIFY(current.running);
        QVERIFY(current.memory_bytes < 0);
        QVERIFY(current.cpu_percent < 0);
        manager.set_process_monitoring(true, false);
        manager.poll();
        QVERIFY(current.running);
        QVERIFY(current.memory_bytes > 0);
        QVERIFY(current.cpu_percent < 0);
        manager.set_process_monitoring(false, true);
        manager.poll();
        QVERIFY(current.running);
        QVERIFY(current.memory_bytes < 0);
        QVERIFY(current.cpu_percent < 0);
        manager.poll();
        QVERIFY(current.cpu_percent >= 0 && current.cpu_percent <= 100);
    }
};

QTEST_GUILESS_MAIN(LaunchTest)
#include "launch_test.moc"
