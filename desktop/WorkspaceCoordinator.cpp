#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include "Application.h"
#include "WorkspaceCoordinator.h"

namespace {
QString resource_issue(const workspace_item &item) {
    if (const auto issue = xlibrary::workspace_item_issue(item); !issue.isEmpty())
        return issue;
    if (item.kind != workspace_item_kind::file && item.kind != workspace_item_kind::folder)
        return {};
    const QFileInfo info(item.target);
    if (!(item.kind == workspace_item_kind::file ? info.isFile() : info.isDir()))
        return QCoreApplication::translate("Workspaces", "File or folder is missing.");
    if (!info.isReadable()
#ifdef Q_OS_UNIX
        || (item.kind == workspace_item_kind::folder && !info.isExecutable())
#endif
    )
        return QCoreApplication::translate("Workspaces", "Permission to open the resource is unavailable.");
    if (item.kind == workspace_item_kind::file) {
        const auto suffix = info.suffix().toCaseFolded();
        static const QStringList launchers{"exe",       "com", "bat", "cmd", "lnk", "url", "desktop",
                                           "msi",       "msp", "ps1", "scr", "pif", "cpl", "hta",
                                           "appref-ms", "vbs", "js",  "jse", "wsf", "sh"};
        if (launchers.contains(suffix) ||
            launchers.contains(QFileInfo(info.canonicalFilePath()).suffix().toCaseFolded())
#ifdef Q_OS_UNIX
            || info.isExecutable()
#endif
        )
            return QCoreApplication::translate("Workspaces", "Add executable targets from the application library.");
    }
    return {};
}

bool near_frame(const QRect &actual, const QRect &expected) {
    constexpr int tolerance = 24;
    return actual.isValid() && qAbs(actual.x() - expected.x()) <= tolerance &&
           qAbs(actual.y() - expected.y()) <= tolerance && qAbs(actual.width() - expected.width()) <= tolerance &&
           qAbs(actual.height() - expected.height()) <= tolerance;
}
}

WorkspaceCoordinator::WorkspaceCoordinator(std::unique_ptr<WorkspaceWindowBackend> backend, app_launcher launch_app,
                                           resource_launcher open_resource, const int timeout_ms, QObject *parent)
    : QObject(parent), backend(std::move(backend)), launch_app(std::move(launch_app)),
      open_resource(std::move(open_resource)), timeout_ms(timeout_ms) {
    executor.setMaxThreadCount(1);
    launch_timer.setSingleShot(true);
    connect(&launch_timer, &QTimer::timeout, this, [this] {
        if (request_token)
            request_token->store(0);
        launch_request = 0;
        complete_step(workspace_step_state::failed,
                      QCoreApplication::translate("Workspaces", "The launch request did not respond in time."));
    });
}

WorkspaceCoordinator::~WorkspaceCoordinator() {
    if (run_token)
        run_token->store(0);
    if (request_token)
        request_token->store(0);
    executor.clear();
    executor.waitForDone();
}

template <typename Work, typename Finish> void WorkspaceCoordinator::background(Work work, Finish finish) {
    using Result = std::invoke_result_t<Work>;
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [watcher, finish = std::move(finish)] {
        watcher->deleteLater();
        finish(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(&executor, std::move(work)));
}

void WorkspaceCoordinator::set_capabilities(workspace_desktop desktop) {
    desktop.windows.clear();
    capabilities = std::move(desktop);
    emit capabilities_changed();
}

void WorkspaceCoordinator::check_capabilities() {
    if (probing)
        return;
    probing = true;
    background([backend = backend] { return backend->snapshot(); },
               [this](workspace_desktop desktop) {
                   probing = false;
                   set_capabilities(std::move(desktop));
               });
}

quint64 WorkspaceCoordinator::capture(QVector<app_record> applications) {
    if (active() || capturing())
        return 0;
    const auto id = capture_id = ++next_capture;
    background(
        [backend = backend, applications = std::move(applications)] {
            auto desktop = backend->snapshot();
            return std::pair{desktop, workspace_capture(desktop, applications)};
        },
        [this, id](const auto &result) {
            capture_id = 0;
            set_capabilities(result.first);
            emit capture_ready(id, result.second);
        });
    return id;
}

WorkspaceCoordinator::Entry WorkspaceCoordinator::preflight(workspace_item item,
                                                            const QVector<app_record> &applications) {
    Entry entry;
    entry.item = std::move(item);
    if (!entry.item.enabled)
        return entry;
    entry.issue = resource_issue(entry.item);
    if (!entry.issue.isEmpty() || entry.item.kind != workspace_item_kind::application)
        return entry;
    const auto found = std::ranges::find(applications, entry.item.application_id, &app_record::id);
    if (found == applications.cend()) {
        entry.issue = QCoreApplication::translate("Workspaces", "Library application is missing.");
        return entry;
    }
    try {
        const auto profile =
            entry.item.launch_profile.isEmpty() ? found->default_launch_profile : entry.item.launch_profile;
        Application::with_launch_profile(*found, profile);
        if (!Application::target_available(*found))
            entry.issue = QCoreApplication::translate("Workspaces", "Application target is unavailable.");
        else if (!found->working_directory.isEmpty() && !QFileInfo(found->working_directory).isDir())
            entry.issue = QCoreApplication::translate("Workspaces", "Application working directory is unavailable.");
#ifdef Q_OS_UNIX
        else if (Application::target_kind(*found) == "executable" && !QFileInfo(found->path).isExecutable())
            entry.issue = QCoreApplication::translate("Workspaces", "Application lacks permission to execute.");
#endif
        if (entry.issue.isEmpty() && Application::target_kind(*found) == "desktop")
            Application::resolve_application(found->path);
        if (entry.issue.isEmpty() && !entry.item.reuse_existing && Application::file_target(*found))
            entry.issue = Application::new_instance_error(*found);
        entry.app = *found;
        entry.executable = workspace_executable(*found);
        entry.can_reuse = workspace_can_reuse(*found, profile);
    } catch (const std::exception &error) {
        entry.issue = QString::fromUtf8(error.what());
    }
    return entry;
}

quint64 WorkspaceCoordinator::prepare(workspace_record workspace, QVector<app_record> applications) {
    if (active() || capturing() || !xlibrary::workspace_record_issue(workspace).isEmpty())
        return 0;
    const auto id = ++generation;
    run_token = std::make_shared<std::atomic<quint64>>(id);
    run_clock.start();
    run_name = workspace.name;
    total_duration = 0;
    final_outcome = workspace_outcome::none;
    phase = Phase::preflight;
    steps.clear();
    entries.clear();
    index = 0;
    for (const auto &item : workspace.items) {
        workspace_step_report step;
        step.label = workspace_item_label(item, applications);
        step.state = item.enabled ? workspace_step_state::pending : workspace_step_state::disabled;
        step.placement_requested = item.enabled && item.placement.has_value();
        steps.append(std::move(step));
    }
    emit changed();
    background(
        [backend = backend, token = run_token, id, items = std::move(workspace.items),
         applications = std::move(applications)] {
            auto desktop = backend->snapshot();
            QVector<Entry> entries;
            QHash<QString, int> configurations;
            for (const auto &item : items) {
                if (token->load() != id)
                    return std::pair{std::move(desktop), QVector<Entry>{}};
                auto entry = preflight(item, applications);
                if (desktop.can_capture && item.reuse_existing && entry.can_reuse && !entry.executable.isEmpty())
                    configurations.insert(entry.executable, 0);
                entries.append(std::move(entry));
            }
            if (!configurations.isEmpty()) {
                for (const auto &app : applications) {
                    if (token->load() != id)
                        return std::pair{std::move(desktop), QVector<Entry>{}};
                    auto found = configurations.find(workspace_executable(app));
                    if (found != configurations.end())
                        ++*found;
                }
                for (auto &entry : entries)
                    entry.ambiguous_configuration = configurations.value(entry.executable) > 1;
            }
            return std::pair{std::move(desktop), std::move(entries)};
        },
        [this, id](const auto &result) {
            if (id != generation || phase != Phase::preflight)
                return;
            entries = result.second;
            set_capabilities(result.first);
            QStringList warnings;
            for (qsizetype position = 0; position < entries.size(); ++position) {
                const auto &entry = entries[position];
                if (!entry.item.enabled)
                    continue;
                QString reason = entry.issue;
                if (reason.isEmpty() && entry.app && entry.item.reuse_existing) {
                    const auto matches = workspace_matching_windows(result.first, entry.executable).size();
                    if (matches && entry.can_reuse && entry.ambiguous_configuration)
                        reason = QCoreApplication::translate(
                            "Workspaces", "The existing window matches several launch configurations. Turn off reuse "
                                          "to request a fresh launch.");
                    else if (entry.can_reuse && matches > 1)
                        reason = QCoreApplication::translate(
                            "Workspaces",
                            "Several windows match. This resource will be skipped while the match is ambiguous.");
                    else if (matches && !entry.can_reuse)
                        reason = QCoreApplication::translate("Workspaces",
                                                             "Existing windows cannot be reused with these launch "
                                                             "options. A fresh launch will be requested.");
                }
                if (reason.isEmpty() && entry.item.placement) {
                    if (!capabilities.can_place)
                        reason = capabilities.limitation.isEmpty()
                                     ? QCoreApplication::translate("Workspaces", "Window placement is unavailable.")
                                     : capabilities.limitation;
                    else if (entry.executable.isEmpty())
                        reason = QCoreApplication::translate(
                            "Workspaces", "Window identity cannot be verified for this launch configuration.");
                    else if (const auto *monitor =
                                 workspace_find_monitor(entry.item.placement->monitor_id, capabilities.monitors);
                             !monitor || (!entry.item.placement->monitor_id.isEmpty() &&
                                          monitor->id != entry.item.placement->monitor_id))
                        reason = QCoreApplication::translate(
                            "Workspaces", "Saved monitor unavailable. The primary monitor will be used if available.");
                }
                if (!reason.isEmpty())
                    warnings.append(steps[position].label + " — " + reason);
            }
            phase = Phase::confirm;
            emit changed();
            emit preflight_ready(id, warnings);
        });
    return id;
}

bool WorkspaceCoordinator::start(const quint64 id) {
    if (id != generation || phase != Phase::confirm)
        return false;
    phase = Phase::running;
    next();
    return true;
}

void WorkspaceCoordinator::next() {
    while (index < steps.size() && steps[index].state == workspace_step_state::disabled)
        ++index;
    if (index >= steps.size()) {
        const auto accepted = std::ranges::any_of(steps, [](const auto &step) { return step.opened || step.reused; });
        const auto incomplete = std::ranges::any_of(steps, [](const auto &step) {
            return step.state == workspace_step_state::failed || step.state == workspace_step_state::skipped ||
                   step.state == workspace_step_state::unverified ||
                   (step.placement_requested && !step.placement_verified);
        });
        finish(!accepted    ? workspace_outcome::failed
               : incomplete ? workspace_outcome::partial
                            : workspace_outcome::completed);
        return;
    }
    step_clock.start();
    const auto &entry = entries[index];
    if (!entry.issue.isEmpty()) {
        complete_step(workspace_step_state::skipped, entry.issue);
        return;
    }
    update_step(workspace_step_state::checking, {});
    const auto id = generation;
    const auto step = index;
    if (!entry.app) {
        background([item = entry.item] { return resource_issue(item); },
                   [this, id, step](const QString &issue) {
                       if (!current(id, step))
                           return;
                       if (!issue.isEmpty()) {
                           complete_step(workspace_step_state::skipped, issue);
                           return;
                       }
                       const auto &item = entries[index].item;
                       const auto url = item.kind == workspace_item_kind::url ? QUrl(item.target, QUrl::StrictMode)
                                                                              : QUrl::fromLocalFile(item.target);
                       const auto opened = open_resource(url);
                       steps[index].opened = opened;
                       complete_step(opened ? workspace_step_state::opened : workspace_step_state::failed,
                                     opened ? QCoreApplication::translate("Workspaces", "Open request accepted.")
                                            : QCoreApplication::translate(
                                                  "Workspaces", "The system did not accept the open request."));
                   });
        return;
    }
    background([backend = backend] { return backend->snapshot(); },
               [this, id, step](workspace_desktop desktop) {
                   if (current(id, step))
                       resolve_app(std::move(desktop));
               });
}

void WorkspaceCoordinator::resolve_app(workspace_desktop desktop) {
    const auto &entry = entries[index];
    previous_windows = workspace_matching_windows(desktop, entry.executable);
    const auto should_reuse = entry.item.reuse_existing && entry.can_reuse;
    if (should_reuse && entry.ambiguous_configuration && !previous_windows.isEmpty()) {
        complete_step(workspace_step_state::skipped,
                      QCoreApplication::translate("Workspaces",
                                                  "The existing window matches several launch configurations. Turn "
                                                  "off reuse to request a fresh launch."));
        return;
    }
    if (should_reuse && previous_windows.size() > 1) {
        complete_step(workspace_step_state::skipped,
                      QCoreApplication::translate("Workspaces",
                                                  "Several windows match. Turn off reuse to request a fresh launch."));
        return;
    }
    if (should_reuse && previous_windows.size() == 1) {
        use_window(previous_windows.front(), desktop, true);
        return;
    }
    set_capabilities(std::move(desktop));
    launched_pid = 0;
    update_step(workspace_step_state::launching, {});
    request_token = std::make_shared<std::atomic<quint64>>(generation);
    const auto selected =
        entry.item.launch_profile.isEmpty() ? entry.app->default_launch_profile : entry.item.launch_profile;
    launch_request = launch_app(*entry.app, selected.isEmpty() ? QStringLiteral("configured") : selected,
                                !entry.item.reuse_existing, generation, request_token);
    if (launch_request)
        launch_timer.start(timeout_ms);
    else
        complete_step(workspace_step_state::failed,
                      QCoreApplication::translate("Workspaces", "The application is busy."));
}

void WorkspaceCoordinator::launch_finished(const operation_result &result, const qint64 process_id) {
    if (phase != Phase::running || !launch_request || launch_request != result.id ||
        result.state == operation_state::loading)
        return;
    launch_timer.stop();
    launch_request = 0;
    request_token.reset();
    if (result.state != operation_state::completed) {
        complete_step(workspace_step_state::failed,
                      result.error.user_message.isEmpty()
                          ? QCoreApplication::translate("Workspaces", "Could not open the application.")
                          : result.error.user_message);
        return;
    }
    const auto &entry = entries[index];
    launched_pid = process_id;
    steps[index].opened = true;
    if (!capabilities.can_capture) {
        complete_step(
            workspace_step_state::opened,
            capabilities.limitation.isEmpty()
                ? QCoreApplication::translate("Workspaces", "Launch accepted. Window verification is unavailable.")
                : capabilities.limitation);
    } else if (entry.executable.isEmpty() || !launched_pid) {
        complete_step(
            workspace_step_state::unverified,
            QCoreApplication::translate("Workspaces", "Launch accepted. Window identity could not be verified."));
    } else {
        update_step(workspace_step_state::waiting, {});
        readiness_started = step_clock.elapsed();
        wait_for_window();
    }
}

void WorkspaceCoordinator::wait_for_window() {
    const auto id = generation;
    const auto step = index;
    background([backend = backend] { return backend->snapshot(); },
               [this, id, step](const workspace_desktop &desktop) {
                   if (!current(id, step))
                       return;
                   auto matches = workspace_matching_windows(desktop, entries[index].executable);
                   matches.removeIf([this](const auto &window) {
                       return window.pid != launched_pid || std::ranges::any_of(previous_windows, [&](const auto &old) {
                                  return same_workspace_window(window, old);
                              });
                   });
                   if (matches.size() == 1) {
                       use_window(matches.front(), desktop, false);
                   } else if (step_clock.elapsed() - readiness_started >= timeout_ms) {
                       complete_step(workspace_step_state::unverified,
                                     matches.size() > 1
                                         ? QCoreApplication::translate("Workspaces",
                                                                       "Launch accepted. Several new windows match.")
                                         : QCoreApplication::translate(
                                               "Workspaces", "Launch accepted. No matching window appeared in time."));
                   } else {
                       QTimer::singleShot(300, this, [this, id, step] {
                           if (current(id, step))
                               wait_for_window();
                       });
                   }
               });
}

void WorkspaceCoordinator::use_window(const workspace_window &window, const workspace_desktop &desktop,
                                      const bool reuse) {
    const auto &placement = entries[index].item.placement;
    const auto *monitor =
        placement && desktop.can_place ? workspace_find_monitor(placement->monitor_id, desktop.monitors) : nullptr;
    const auto id = generation;
    const auto step = index;
    target_window = window;
    target_frame = monitor ? workspace_restore_frame(*placement, monitor->work_area) : QRect{};
    placement_monitor = monitor ? monitor->id : QString{};
    placement_note = placement && monitor && !placement->monitor_id.isEmpty() && monitor->id != placement->monitor_id
                         ? QCoreApplication::translate("Workspaces", "The saved monitor was unavailable.")
                         : QString{};
    if (target_frame.isValid())
        update_step(workspace_step_state::positioning, {});
    background(
        [backend = backend, token = run_token, id, window, reuse, frame = target_frame,
         maximize = placement && placement->maximized] {
            const auto activated = !reuse || (token->load() == id && backend->activate(window));
            const auto moved = frame.isValid() && token->load() == id && backend->place(window, frame, maximize);
            return std::pair{activated, moved};
        },
        [this, id, step, reuse, unsupported = desktop.limitation,
         needs_placement = placement.has_value()](const auto &result) {
            if (!current(id, step))
                return;
            steps[index].reused = reuse;
            steps[index].placement_attempted = target_frame.isValid();
            if (result.second) {
                placement_started = step_clock.elapsed();
                if (!result.first)
                    placement_note += ' ' + QCoreApplication::translate("Workspaces", "Window activation was refused.");
                verify_placement();
                return;
            }
            QString detail;
            if (!result.first)
                detail = QCoreApplication::translate("Workspaces", "The existing window could not be activated.");
            if (needs_placement) {
                const auto issue = target_frame.isValid()
                                       ? QCoreApplication::translate("Workspaces", "Window placement was refused.")
                                   : unsupported.isEmpty()
                                       ? QCoreApplication::translate("Workspaces", "Window placement is unavailable.")
                                       : unsupported;
                detail = detail.isEmpty() ? issue : detail + ' ' + issue;
            }
            complete_step(!result.first ? workspace_step_state::unverified
                          : reuse       ? workspace_step_state::reused
                                        : workspace_step_state::ready,
                          detail);
        });
}

void WorkspaceCoordinator::verify_placement() {
    const auto id = generation;
    const auto step = index;
    background([backend = backend] { return backend->snapshot(); },
               [this, id, step](const workspace_desktop &desktop) {
                   if (!current(id, step))
                       return;
                   const auto window = std::ranges::find_if(desktop.windows, [this](const auto &candidate) {
                       return same_workspace_window(candidate, target_window) && !candidate.minimized;
                   });
                   const auto *monitor = workspace_find_monitor(placement_monitor, desktop.monitors);
                   const auto maximized = entries[index].item.placement->maximized;
                   if (window != desktop.windows.cend() && window->maximized == maximized && monitor &&
                       monitor->id == placement_monitor &&
                       (maximized ? monitor->work_area.contains(window->frame.center())
                                  : near_frame(window->frame, target_frame))) {
                       steps[index].placement_verified = true;
                       const auto detail =
                           QCoreApplication::translate("Workspaces", "Window positioned on %1.").arg(placement_monitor);
                       complete_step(workspace_step_state::positioned, (detail + ' ' + placement_note).trimmed());
                   } else if (step_clock.elapsed() - placement_started >= 2000) {
                       complete_step(steps[index].reused ? workspace_step_state::reused : workspace_step_state::ready,
                                     QCoreApplication::translate(
                                         "Workspaces", "The window manager did not confirm the requested placement."));
                   } else {
                       QTimer::singleShot(250, this, [this, id, step] {
                           if (current(id, step))
                               verify_placement();
                       });
                   }
               });
}

bool WorkspaceCoordinator::current(const quint64 id, const qsizetype step) const {
    return phase == Phase::running && id == generation && step == index;
}

void WorkspaceCoordinator::update_step(const workspace_step_state state, const QString &detail) {
    steps[index].state = state;
    steps[index].detail = detail;
    emit changed();
}

void WorkspaceCoordinator::complete_step(const workspace_step_state state, const QString &detail) {
    steps[index].duration_ms = step_clock.elapsed();
    const auto id = generation;
    const auto step = index;
    update_step(state, detail);
    if (!current(id, step))
        return;
    ++index;
    next();
}

void WorkspaceCoordinator::finish(const workspace_outcome outcome) {
    launch_timer.stop();
    launch_request = 0;
    if (request_token)
        request_token->store(0);
    request_token.reset();
    run_token->store(0);
    phase = Phase::idle;
    total_duration = run_clock.elapsed();
    final_outcome = outcome;
    emit changed();
}

void WorkspaceCoordinator::cancel() {
    if (!active())
        return;
    for (qsizetype position = index; position < steps.size(); ++position) {
        auto &step = steps[position];
        if (step.state != workspace_step_state::disabled) {
            step.state = workspace_step_state::cancelled;
            step.detail = QCoreApplication::translate("Workspaces", "Run cancelled. Open applications stay open.");
        }
    }
    finish(workspace_outcome::cancelled);
}

QString workspace_item_label(const workspace_item &item, const QVector<app_record> &applications) {
    if (item.kind != workspace_item_kind::application)
        return item.display_name.isEmpty() ? item.target : item.display_name;
    const auto app = std::ranges::find(applications, item.application_id, &app_record::id);
    return app != applications.cend()    ? app->name
           : item.display_name.isEmpty() ? QCoreApplication::translate("Workspaces", "Missing application")
                                         : item.display_name;
}

QString workspace_step_text(const workspace_step_state state) {
    switch (state) {
    case workspace_step_state::pending:
        return QCoreApplication::translate("Workspaces", "Pending");
    case workspace_step_state::disabled:
        return QCoreApplication::translate("Workspaces", "Disabled");
    case workspace_step_state::checking:
        return QCoreApplication::translate("Workspaces", "Checking");
    case workspace_step_state::launching:
        return QCoreApplication::translate("Workspaces", "Opening");
    case workspace_step_state::waiting:
        return QCoreApplication::translate("Workspaces", "Waiting for window");
    case workspace_step_state::positioning:
        return QCoreApplication::translate("Workspaces", "Positioning window");
    case workspace_step_state::opened:
        return QCoreApplication::translate("Workspaces", "Open request accepted");
    case workspace_step_state::ready:
        return QCoreApplication::translate("Workspaces", "Window ready");
    case workspace_step_state::reused:
        return QCoreApplication::translate("Workspaces", "Reused existing window");
    case workspace_step_state::positioned:
        return QCoreApplication::translate("Workspaces", "Window positioned");
    case workspace_step_state::unverified:
        return QCoreApplication::translate("Workspaces", "Unverified");
    case workspace_step_state::skipped:
        return QCoreApplication::translate("Workspaces", "Skipped");
    case workspace_step_state::failed:
        return QCoreApplication::translate("Workspaces", "Failed");
    case workspace_step_state::cancelled:
        return QCoreApplication::translate("Workspaces", "Cancelled");
    }
    return {};
}

QString workspace_outcome_text(const workspace_outcome outcome) {
    switch (outcome) {
    case workspace_outcome::none:
        return {};
    case workspace_outcome::completed:
        return QCoreApplication::translate("Workspaces", "Workspace completed");
    case workspace_outcome::partial:
        return QCoreApplication::translate("Workspaces", "Workspace partially completed");
    case workspace_outcome::failed:
        return QCoreApplication::translate("Workspaces", "Workspace failed");
    case workspace_outcome::cancelled:
        return QCoreApplication::translate("Workspaces", "Workspace cancelled");
    }
    return {};
}
