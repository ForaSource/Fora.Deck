#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include "Application.h"
#include "WorkspaceCapture.h"

namespace {
QString executable_key(QString canonical) {
    canonical = QDir::fromNativeSeparators(canonical);
#ifdef Q_OS_WIN
    canonical = canonical.toCaseFolded();
#endif
    return canonical;
}

QString canonical_executable(const QString &path) {
    return path.isEmpty() ? QString{} : executable_key(QFileInfo(path).canonicalFilePath());
}

const workspace_monitor *monitor_for_frame(const QRect &frame, const QVector<workspace_monitor> &monitors) {
    const workspace_monitor *best = nullptr;
    qint64 largest = 0;
    for (const auto &monitor : monitors) {
        const auto overlap = monitor.work_area.intersected(frame);
        const auto area = qint64(overlap.width()) * overlap.height();
        if (area > largest) {
            largest = area;
            best = &monitor;
        }
    }
    return best;
}
}

bool same_workspace_window(const workspace_window &left, const workspace_window &right) {
    return left.handle && left.handle == right.handle && left.pid > 0 && left.pid == right.pid && left.process_birth &&
           left.process_birth == right.process_birth && !left.executable.isEmpty() &&
           left.executable == right.executable;
}

QString workspace_executable(const app_record &app) {
    const auto kind = Application::target_kind(app);
    if (kind == "executable" || kind == "steam")
        return canonical_executable(Application::process_path(app));
    if (kind == "desktop") {
        try {
            return canonical_executable(Application::resolve_application(app.path).executable_path);
        } catch (const std::exception &) {
            return {};
        }
    }
    return {};
}

bool workspace_can_reuse(const app_record &app, const QString &profile) {
    const auto selected = profile.isEmpty() ? app.default_launch_profile : profile;
    if ((!selected.isEmpty() && selected != "configured") || !app.arguments.trimmed().isEmpty() ||
        (!app.working_directory.isEmpty() && app.working_directory != QFileInfo(app.path).absolutePath()) ||
        !Application::launch_modes(app).isEmpty() || Application::target_kind(app) == "steam")
        return false;
    if (Application::target_kind(app) != "desktop")
        return true;
    auto command = QProcess::splitCommand(Application::configured_arguments(app));
    if (command.isEmpty())
        return false;
    command.removeFirst();
    return std::all_of(command.cbegin(), command.cend(),
                       [](const QString &arg) { return arg == "%f" || arg == "%F" || arg == "%u" || arg == "%U"; });
}

QVector<workspace_window> workspace_matching_windows(const workspace_desktop &desktop, const QString &executable) {
    QVector<workspace_window> matches;
    if (desktop.can_capture && !executable.isEmpty())
        for (const auto &window : desktop.windows)
            if (window.handle && window.pid > 0 && window.process_birth && !window.minimized &&
                executable_key(window.executable) == executable)
                matches.append(window);
    return matches;
}

QVector<workspace_capture_candidate> workspace_capture(const workspace_desktop &desktop,
                                                       const QVector<app_record> &applications) {
    if (!desktop.can_capture)
        return {};
    QHash<QString, QVector<const app_record *>> by_executable;
    for (const auto &app : applications)
        if (const auto executable = workspace_executable(app); !executable.isEmpty())
            by_executable[executable].append(&app);
    QVector<workspace_capture_candidate> candidates;
    for (const auto &window : desktop.windows) {
        workspace_capture_candidate row;
        row.window = window;
        const auto *monitor = monitor_for_frame(window.frame, desktop.monitors);
        if (monitor)
            row.monitor = monitor->id;
        const auto matching_apps = by_executable.value(executable_key(window.executable));
        if (!window.handle || window.pid <= 0 || !window.process_birth || window.minimized || matching_apps.isEmpty()) {
            row.issue = QCoreApplication::translate("Workspaces", "Unrecognized window");
        } else if (matching_apps.size() != 1 || !workspace_can_reuse(*matching_apps.front(), {})) {
            row.issue = QCoreApplication::translate("Workspaces", "Launch configuration could not be identified");
        } else {
            workspace_item item;
            item.application_id = matching_apps.front()->id;
            item.display_name = matching_apps.front()->name.left(256);
            item.display_name.removeIf([](const QChar c) { return c.category() == QChar::Other_Control; });
            if (desktop.can_place)
                item.placement = workspace_capture_placement(window, desktop.monitors);
            row.item = std::move(item);
        }
        candidates.append(std::move(row));
    }
    return candidates;
}

std::optional<workspace_placement> workspace_capture_placement(const workspace_window &window,
                                                               const QVector<workspace_monitor> &monitors) {
    const auto *monitor = monitor_for_frame(window.frame, monitors);
    if (!monitor || window.minimized)
        return std::nullopt;
    const auto &area = monitor->work_area;
    const auto frame = window.frame.intersected(area);
    if (window.maximized)
        return workspace_placement{monitor->id, {0, 0, 1, 1}, true};
    return workspace_placement{monitor->id,
                               {double(frame.x() - area.x()) / area.width(),
                                double(frame.y() - area.y()) / area.height(), double(frame.width()) / area.width(),
                                double(frame.height()) / area.height()},
                               false};
}

const workspace_monitor *workspace_find_monitor(const QString &id, const QVector<workspace_monitor> &monitors) {
    const auto match = std::ranges::find(monitors, id, &workspace_monitor::id);
    if (match != monitors.cend() && match->work_area.isValid())
        return &*match;
    for (const auto &monitor : monitors)
        if (monitor.primary && monitor.work_area.isValid())
            return &monitor;
    for (const auto &monitor : monitors)
        if (monitor.work_area.isValid())
            return &monitor;
    return nullptr;
}

QRect workspace_restore_frame(const workspace_placement &placement, const QRect &area) {
    if (!area.isValid())
        return {};
    const auto &relative = placement.geometry;
    const auto width = qBound(qMin(160, area.width()), qRound(relative.width() * area.width()), area.width());
    const auto height = qBound(qMin(96, area.height()), qRound(relative.height() * area.height()), area.height());
    const auto x = qBound(0, qRound(relative.x() * area.width()), area.width() - width);
    const auto y = qBound(0, qRound(relative.y() * area.height()), area.height() - height);
    return {area.x() + x, area.y() + y, width, height};
}
