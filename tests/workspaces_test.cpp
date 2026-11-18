#include <mutex>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "WorkspaceCoordinator.h"

namespace {
class FakeBackend final : public WorkspaceWindowBackend {
    std::mutex lock;
    workspace_desktop desktop;

public:
    explicit FakeBackend(workspace_desktop desktop) : desktop(std::move(desktop)) {}
    void windows(QVector<workspace_window> windows) {
        std::scoped_lock guard(lock);
        desktop.windows = std::move(windows);
    }
    workspace_desktop snapshot() override {
        std::scoped_lock guard(lock);
        return desktop;
    }
    bool activate(const workspace_window &window) override {
        std::scoped_lock guard(lock);
        return std::ranges::any_of(desktop.windows,
                                   [&](const auto &candidate) { return same_workspace_window(window, candidate); });
    }
    bool place(const workspace_window &window, const QRect &frame, const bool maximized) override {
        std::scoped_lock guard(lock);
        for (auto &candidate : desktop.windows)
            if (same_workspace_window(window, candidate)) {
                candidate.frame = frame;
                candidate.maximized = maximized;
                return true;
            }
        return false;
    }
};

app_record application() {
    app_record record;
    record.id = 17;
    record.name = "Application";
    record.path = QCoreApplication::applicationFilePath();
    record.target_kind = "executable";
    return record;
}

workspace_window window(const quintptr handle = 1) {
    return {handle, 42, 7, application().path, "Window", {300, 100, 400, 400}};
}

workspace_item app_item() {
    workspace_item item;
    item.application_id = application().id;
    return item;
}

workspace_desktop desktop_with_windows(QVector<workspace_window> windows = {}) {
    return {true, true, {}, {{"primary", {0, 0, 1000, 800}, true}}, std::move(windows)};
}
}

class WorkspacesTest : public QObject {
    Q_OBJECT

private slots:
    void capture_confidence() {
        const auto test_app = application();
        auto other = test_app;
        other.id++;
        other.name = test_app.name;
        other.arguments = "--another-profile";
        auto unknown = window(2);
        unknown.executable = "/unrecognized";
        const auto desktop = desktop_with_windows({window(), unknown});
        const auto candidates = workspace_capture(desktop, {test_app});
        QCOMPARE(candidates.size(), 2);
        QCOMPARE(candidates.front().item->application_id, test_app.id);
        QVERIFY(candidates.front().item->placement.has_value());
        QVERIFY(!candidates.back().item);
        QVERIFY(!workspace_capture(desktop, {test_app, other}).front().item);
        QVERIFY(workspace_capture({}, {test_app}).isEmpty());
    }

    void normalized_work_area() {
        const QVector<workspace_monitor> monitors{{"primary", {-1920, 60, 1920, 1020}, true},
                                                  {"small", {0, 40, 360, 180}, false}};
        auto candidate = window();
        candidate.frame = {-2000, 20, 480, 1000};
        const auto placement = workspace_capture_placement(candidate, monitors);
        QVERIFY(placement.has_value());
        QCOMPARE(placement->monitor_id, QString("primary"));
        QCOMPARE(placement->geometry.x(), 0.0);
        QCOMPARE(placement->geometry.y(), 0.0);
        const auto *fallback = workspace_find_monitor("disconnected", monitors);
        QCOMPARE(fallback->id, QString("primary"));
        workspace_placement corner{"small", {0.99, 0.99, 0.01, 0.01}};
        const auto clamped = workspace_restore_frame(corner, monitors.back().work_area);
        QVERIFY(monitors.back().work_area.contains(clamped));
        QCOMPARE(clamped.size(), QSize(160, 96));
    }

    void reuses_and_verifies_placement() {
        auto backend = std::make_unique<FakeBackend>(desktop_with_windows({window()}));
        auto *controlled = backend.get();
        int launch_count = 0;
        WorkspaceCoordinator runs(
            std::move(backend), [&](const auto &, const auto &, bool, quint64, const auto &) { return ++launch_count; },
            [](const auto &) { return false; });
        auto item = app_item();
        item.placement = workspace_placement{"removed", {0.5, 0, 0.5, 1}, false};
        const auto id = runs.prepare({1, "Work", {item}}, {application()});
        QTRY_VERIFY(runs.awaiting_confirmation());
        QVERIFY(runs.start(id));
        QTRY_COMPARE(runs.outcome(), workspace_outcome::completed);
        QCOMPARE(launch_count, 0);
        QVERIFY(runs.report().front().reused);
        QVERIFY(runs.report().front().placement_verified);
        QCOMPARE(controlled->snapshot().windows.front().frame, QRect(500, 0, 500, 800));
    }

    void ambiguous_reuse_is_skipped() {
        auto backend = std::make_unique<FakeBackend>(desktop_with_windows({window(), window(2)}));
        int launches = 0;
        WorkspaceCoordinator runs(
            std::move(backend), [&](const auto &, const auto &, bool, quint64, const auto &) { return ++launches; },
            [](const auto &) { return false; });
        const auto id = runs.prepare({1, "Work", {app_item()}}, {application()});
        QTRY_VERIFY(runs.awaiting_confirmation());
        runs.start(id);
        QTRY_COMPARE(runs.outcome(), workspace_outcome::failed);
        QCOMPARE(runs.report().front().state, workspace_step_state::skipped);
        QCOMPARE(launches, 0);
    }

    void cancel_ignores_late_acceptance() {
        quint64 launch_count = 0;
        WorkspaceCoordinator::launch_token old_token;
        auto backend = std::make_unique<FakeBackend>(desktop_with_windows());
        WorkspaceCoordinator runs(
            std::move(backend),
            [&](const auto &, const auto &, bool, quint64, const auto &token) {
                old_token = token;
                return ++launch_count;
            },
            [](const auto &) { return false; }, 2000);
        const workspace_record record{1, "Work", {app_item()}};
        const auto first = runs.prepare(record, {application()});
        QCOMPARE(runs.prepare(record, {application()}), 0U);
        QTRY_VERIFY(runs.awaiting_confirmation());
        QVERIFY(runs.start(first));
        QTRY_COMPARE(launch_count, 1U);
        runs.cancel();
        QCOMPARE(old_token->load(), 0U);
        QCOMPARE(runs.outcome(), workspace_outcome::cancelled);
        const auto second = runs.prepare(record, {application()});
        QVERIFY(second > first);
        QTRY_VERIFY(runs.awaiting_confirmation());
        QVERIFY(runs.start(second));
        QTRY_COMPARE(launch_count, 2U);
        runs.launch_finished({1, "workspace_launch", operation_state::completed, {}});
        QCOMPARE(runs.report().front().state, workspace_step_state::launching);
        runs.launch_finished({2, "workspace_launch", operation_state::completed, {}}, window().pid);
        QCOMPARE(runs.report().front().state, workspace_step_state::waiting);
        runs.cancel();
    }

    void waits_for_a_window_then_times_out() {
        auto backend = std::make_unique<FakeBackend>(desktop_with_windows());
        WorkspaceCoordinator runs(
            std::move(backend), [](const auto &, const auto &, bool, quint64, const auto &) { return 1; },
            [](const auto &) { return false; }, 50);
        const auto id = runs.prepare({1, "Work", {app_item()}}, {application()});
        QTRY_VERIFY(runs.awaiting_confirmation());
        runs.start(id);
        QTRY_COMPARE(runs.report().front().state, workspace_step_state::launching);
        runs.launch_finished({1, "workspace_launch", operation_state::completed, {}}, window().pid);
        QTRY_COMPARE(runs.outcome(), workspace_outcome::partial);
        QCOMPARE(runs.report().front().state, workspace_step_state::unverified);
        QVERIFY(runs.report().front().opened);
    }

    void only_places_windows_from_the_launched_process() {
        auto backend = std::make_unique<FakeBackend>(desktop_with_windows({window(1)}));
        auto *controlled = backend.get();
        int requests = 0;
        WorkspaceCoordinator runs(
            std::move(backend),
            [&](const auto &, const auto &, bool fresh, quint64, const auto &) {
                requests += fresh;
                return 7;
            },
            [](const auto &) { return false; }, 2000);
        auto item = app_item();
        item.reuse_existing = false;
        item.placement = workspace_placement{"primary", {0.5, 0, 0.5, 1}};
        const auto id = runs.prepare({1, "Work", {item}}, {application()});
        QTRY_VERIFY(runs.awaiting_confirmation());
        QVERIFY(runs.start(id));
        QTRY_COMPARE(requests, 1);
        auto unrelated = window(2);
        unrelated.pid++;
        controlled->windows({window(1), unrelated});
        auto launched = window(3);
        launched.pid += 2;
        runs.launch_finished({7, "workspace_launch", operation_state::completed, {}}, launched.pid);
        QTest::qWait(100);
        QCOMPARE(runs.report().front().state, workspace_step_state::waiting);
        controlled->windows({window(1), unrelated, launched});
        QTRY_COMPARE(runs.outcome(), workspace_outcome::completed);
        const auto current = controlled->snapshot().windows;
        QCOMPARE(current[0].frame, window(1).frame);
        QCOMPARE(current[1].frame, unrelated.frame);
        QCOMPARE(current[2].frame, QRect(500, 0, 500, 800));
    }

    void typed_resource_open() {
        QTemporaryDir temp;
        QList<QUrl> urls;
        auto backend = std::make_unique<FakeBackend>(workspace_desktop{});
        WorkspaceCoordinator runs(std::move(backend), {}, [&](const QUrl &url) {
            urls.append(url);
            return true;
        });
        workspace_item file;
        file.kind = workspace_item_kind::file;
        file.target = temp.filePath("actual file.txt");
        QFile document(file.target);
        QVERIFY(document.open(QIODevice::WriteOnly));
        document.close();
        auto missing = file;
        missing.target = temp.filePath("missing.txt");
        workspace_item folder;
        folder.kind = workspace_item_kind::folder;
        folder.target = temp.path();
        workspace_item url;
        url.kind = workspace_item_kind::url;
        url.target = "https://example.org/path?q=1";
        const auto id = runs.prepare({1, "Work", {file, missing, folder, url}}, {});
        QTRY_VERIFY(runs.awaiting_confirmation());
        QVERIFY(urls.isEmpty());
        runs.start(id);
        QTRY_COMPARE(runs.outcome(), workspace_outcome::partial);
        QCOMPARE(urls,
                 QList<QUrl>({QUrl::fromLocalFile(file.target), QUrl::fromLocalFile(folder.target), QUrl(url.target)}));
        QCOMPARE(runs.report()[1].state, workspace_step_state::skipped);
    }
};

QTEST_GUILESS_MAIN(WorkspacesTest)
#include "workspaces_test.moc"
