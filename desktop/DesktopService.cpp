#include <algorithm>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <QUrl>

#include "ApplicationPaths.h"
#include "DesktopService.h"
#include "GithubTokenStore.h"
#include "localization.h"
#include "Manager.h"
#include "PathHelpers.h"
#include "PlatformIntegration.h"
#include "ProfileBackup.h"
#include "ProfileSchema.h"
#include "ProfileSettings.h"

DesktopService::DesktopService(QObject *const parent, const QString &profile_path, const QString &database_path)
    : QObject(parent), worker(new Manager(database_path, request_sequence)), model(this), filter(this),
      settings((profile_path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                       : profile_path) +
                   "/settings.ini",
               QSettings::IniFormat) {
    // Detect existing profiles before the worker opens a new database or the UI writes defaults.
    if (!settings.contains("onboarding/completed") &&
        (QFileInfo(settings.fileName()).isFile() || (!database_path.isEmpty() && QFileInfo(database_path).isFile())))
        settings.setValue("onboarding/completed", true);
    files = new FileManager(QFileInfo(settings.fileName()).absolutePath(), settings.value("files/saved").toStringList(),
                            this);
    connect(files, &FileManager::error, this, &DesktopService::notify);
    connect(files, &FileManager::busy_changed, this, &DesktopService::profile_state_changed);
    connect(files, &FileManager::saved_paths_changed, this, [this](const QStringList &paths) {
        settings.setValue("files/saved", paths);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            notify(QCoreApplication::translate("XLibrary", "Could not save the settings file."));
    });
    QTimer::singleShot(0, this, [this] {
        files->configure(FileManager::mode_from_setting(settings.value("files/mode", "off").toString()),
                         settings.value("files/roots").toStringList(),
                         settings.value("files/exclusions").toStringList());
    });
    startup_inventory_pending = settings.value("onboarding/completed", false).toBool();
    updates.set_channel(settings.value("updates/channel", "stable").toString());
    connect(&updates, &UpdateChecker::channel_changed, this,
            [this] { settings.setValue("updates/channel", updates.channel()); });
    qRegisterMetaType<QVector<app_record>>();
    qRegisterMetaType<QVector<discovery_issue>>();
    qRegisterMetaType<operation_state>();
    qRegisterMetaType<operation_error_source>();
    qRegisterMetaType<operation_error>();
    qRegisterMetaType<operation_result>();
    qRegisterMetaType<profile_manifest>();
    qRegisterMetaType<QVector<path_relocation>>();
    connect(this, &DesktopService::catalog_changed, this, &DesktopService::sync_target_hotkeys);
    connect(this, &DesktopService::appearance_changed, this, &DesktopService::sync_target_hotkeys);
    connect(this, &DesktopService::operation_changed, this, [this](const operation_result &result) {
        if (!pending_shortcut_request || result.id != pending_shortcut_request ||
            result.state == operation_state::loading)
            return;
        if (result.state != operation_state::completed) {
            // Keep the old OS registration until persistence succeeds, so rollback cannot lose it.
            target_hotkeys[pending_shortcut_key] = std::move(previous_target_hotkey);
            if (!previous_target_shortcut_error.isEmpty()) {
                registered_target_shortcuts.remove(pending_shortcut_key);
                refused_target_shortcuts.insert(pending_shortcut_key, previous_refused_target_shortcut.isEmpty()
                                                                          ? previous_target_shortcut
                                                                          : previous_refused_target_shortcut);
                target_shortcut_errors.insert(pending_shortcut_key, previous_target_shortcut_error);
                notify(previous_target_shortcut_error);
            } else if (previous_target_shortcut.isEmpty())
                registered_target_shortcuts.remove(pending_shortcut_key);
            else
                registered_target_shortcuts.insert(pending_shortcut_key, previous_target_shortcut);
        }
        previous_target_hotkey.reset();
        previous_target_shortcut_error.clear();
        previous_refused_target_shortcut = {};
        pending_shortcut_request = 0;
        pending_shortcut_key.clear();
        sync_target_hotkeys();
        emit target_shortcuts_changed();
    });
    library_request_id = begin_operation("library");
    worker->known_volume_roots = settings.value("availability/volume_roots").toStringList();
    worker->verified_locations = settings.value("availability/verified_locations").toMap();
    filter.setSourceModel(&model);
    launch_profile_pool.setMaxThreadCount(1);
    launch_profile_pool.setExpiryTimeout(5000);
    connect(this, &DesktopService::catalog_changed, this, &DesktopService::warm_launch_profiles);
    discovery_pool.setMaxThreadCount(1);
    discovery_pool.setExpiryTimeout(5000);
    worker->moveToThread(&worker_thread);
    connect(worker, &Manager::volumes_observed, this, [this](const QStringList &roots) {
        if (!profile_busy() && !restart_pending)
            settings.setValue("availability/volume_roots", roots);
    });
    connect(worker, &Manager::locations_observed, this, [this](const QVariantMap &locations) {
        if (!profile_busy() && !restart_pending)
            settings.setValue("availability/verified_locations", locations);
    });
    connect(this, &DesktopService::library_open_requested, worker, &Manager::initialize, Qt::QueuedConnection);
    connect(this, &DesktopService::library_restore_requested, worker, &Manager::restore, Qt::QueuedConnection);
    connect(&worker_thread, &QThread::started, worker,
            [worker = worker, id = library_request_id] { worker->initialize(id); });
    connect(&worker_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &Manager::location_changed, this, [this](const QString &path) { library_file = path; });
    connect(worker, &Manager::schema_changed, this, [this](const int version) { current_schema_version = version; });
    connect(worker, &Manager::profile_inspected, this, &DesktopService::profile_preview_ready);
    connect(worker, &Manager::paths_inspected, this, &DesktopService::paths_preview_ready);
    connect(worker, &Manager::profile_replaced, this, [this](const QString &recovery, const bool previous_autostart) {
        restart_pending = true;
        settings.sync();
        QString failure;
        const auto restored_autostart = settings.value("behavior/autostart", false).toBool();
        if (restored_autostart != previous_autostart && !desktop_platform::set_autostart(restored_autostart, failure)) {
            const auto id = begin_operation("autostart");
            publish_result({id,
                            "autostart",
                            operation_state::error,
                            {QCoreApplication::translate(
                                 "XLibrary", "The profile was restored, but autostart could not be updated."),
                             failure, operation_error_source::platform}});
        } else {
            settings.remove("profile/autostart_pending");
            settings.sync();
        }
        emit profile_state_changed();
        notify(QCoreApplication::translate(
                   "XLibrary", "Profile restored. Restart XLibrary to apply all settings. Previous profile: %1")
                   .arg(recovery));
    });
    connect(worker, &Manager::result, this, &DesktopService::publish_result);
    connect(worker, &Manager::notification, this, &DesktopService::message);
    connect(worker, &Manager::discovery_dismissed, this, [this](const QStringList &dismissed) {
        if (profile_busy() || restart_pending)
            return;
        auto targets = settings.value("sources/dismissed_targets").toStringList();
        targets.append(dismissed);
        targets.removeDuplicates();
        settings.setValue("sources/dismissed_targets", targets);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            notify(QCoreApplication::translate("XLibrary", "Could not save discovery exclusions."));
    });
    connect(worker, &Manager::discovery_report_ready, this, [this](const QString &report) {
        scan_report = report;
        emit discovery_report_changed();
    });
    connect(worker, &Manager::preview_ready, this,
            [this](const quint64 id, const QVector<app_record> &records, const QVector<discovery_issue> &issues) {
                if (id != discovery_request_id)
                    return;
                if (discovery_cancelled->requested.load()) {
                    publish_result({id, preview_operation, operation_state::cancelled, {}});
                    return;
                }
                present_catalog_preview(records, issues);
            });
    connect(worker, &Manager::inventory_ready, this,
            [this](const quint64 id, const QVector<app_record> &records, const QVector<app_record> &removed,
                   const QVector<discovery_issue> &issues) {
                if (id != discovery_request_id || !is_discovering())
                    return;
                if (discovery_cancelled->requested.load()) {
                    publish_result({id, preview_operation, operation_state::cancelled, {}});
                    return;
                }
                missing_records = removed;
                present_catalog_preview(records, issues);
            });
    connect(worker, &Manager::undo_changed, this, [this](const bool available) {
        undo_available = available;
        emit undo_removal_changed();
    });
    connect(worker, &Manager::resolved, this, &DesktopService::application_resolved);
    connect(worker, &Manager::application_saved, this, &DesktopService::application_saved);
    connect(worker, &Manager::scenario_saved, this, &DesktopService::scenario_saved);
    connect(worker, &Manager::tag_renamed, this, [this](const QString &old_name, const QString &new_name) {
        if (old_name.isEmpty())
            return;
        filter.set_query(search_query::rewrite_tag(filter.text(), old_name, new_name));
        if (tag_name_key(current_tag) == tag_name_key(old_name))
            navigate(current_section, new_name);
    });
    connect(worker, &Manager::loaded, this, [this](const catalog_snapshot &value) {
        snapshot = value;
        clear_stale_scenario_run();
        if (!current_tag.isEmpty() && !snapshot.tags.contains(current_tag))
            navigate(current_section);
        model.replace(value.applications);
        invalidate_scenario_checks();
        if (!model.find(selected_id))
            selected_id = 0;
        selected_ids.removeIf([this](const qint64 id) { return !model.find(id); });
        emit catalog_changed();
        emit selection_changed();
        emit selections_changed();
    });
    connect(worker, &Manager::state_changed, this, [this](const QVector<app_record> &applications) {
        const bool availability_changed =
            std::any_of(applications.cbegin(), applications.cend(), [this](const auto &app) {
                const auto *previous = model.find(app.id);
                return !previous || previous->available != app.available;
            });
        snapshot.applications = applications;
        model.update_state(applications);
        if (availability_changed)
            invalidate_scenario_checks();
        emit selection_changed();
    });
    connect(worker, &Manager::launched, this,
            [this](const qint64 id, const operation_result &result, const quint64 from_scenario) {
                launching.remove(id);
                if (!from_scenario)
                    emit application_launch_finished(id, result);
                if (result.state == operation_state::completed && !from_scenario)
                    emit launch_succeeded();
                if (from_scenario && from_scenario == scenario_run_generation && step_index < step_states.size()) {
                    step_states[step_index] = result.state == operation_state::completed   ? "success"
                                              : result.state == operation_state::cancelled ? "skipped"
                                                                                           : "error";
                    step_errors[step_index] = result.error.user_message;
                    clear_stale_scenario_run();
                    emit scenario_steps_changed();
                }
                if (!from_scenario || !is_scenario_active() || from_scenario != scenario_generation->load())
                    return;
                if (result.state != operation_state::completed) {
                    finish_scenario(result.state, result.error);
                    emit scenario_changed(QCoreApplication::translate(
                        "XLibrary", "The scenario stopped because an application could not be launched."));
                    return;
                }
                ++step_index;
                next_scenario_step();
            });
    scenario_timer.setSingleShot(true);
    connect(&updates, &UpdateChecker::operation_changed, this, [this](const operation_result &result) {
        if (result.state == operation_state::loading) {
            update_source_request_id = result.id;
            update_request_id = begin_operation(result.operation);
        } else if (result.id == update_source_request_id) {
            auto mapped = result;
            mapped.id = update_request_id;
            publish_result(mapped);
        }
    });
    connect(&scenario_timer, &QTimer::timeout, this, [this] {
        if (is_scenario_active() && step_index < pending_steps.size()) {
            const auto &step = pending_steps[step_index];
            step_states[step_index] = "launching";
            emit scenario_steps_changed();
            quint64 request = 0;
            if (step.launch_profile.isEmpty())
                request = request_launch(step.application_id, false, true);
            else
                request = request_launch_mode(step.application_id, Application::launch_mode::launch, true,
                                              step.launch_profile);
            if (!request && is_scenario_active()) {
                step_states[step_index] = "error";
                finish_scenario(operation_state::error);
            }
        }
    });
    worker_thread.setObjectName("catalog_worker");
    worker_thread.start();
}

DesktopService::~DesktopService() {
    files->shutdown();
    if (!restart_pending) {
        try {
            settings.setValue("files/saved", files->persisted_saved_paths());
            settings.sync();
        } catch (const std::exception &e) {
            qWarning("Cannot flush saved file paths: %s", e.what());
        }
    }
    ++*scenario_generation;
    auto expected = discovery_phase::running;
    discovery_cancelled->phase.compare_exchange_strong(expected, discovery_phase::cancelling);
    discovery_cancelled->requested.store(true);
    scenario_timer.stop();
    discovery_pool.clear();
    discovery_pool.waitForDone();
    launch_profile_queue.clear();
    launch_profile_pool.waitForDone();
    worker_thread.quit();
    worker_thread.wait();
}

void DesktopService::refresh_availability() {
    if (!is_ready() || profile_busy() || restart_required())
        return;
    enqueue("availability_refresh", [](Manager &manager) { manager.poll(true); }, operation_error_source::platform);
}

void DesktopService::retry_library() {
    if (is_ready() || is_opening())
        return;
    const auto id = begin_operation("library");
    emit library_open_requested(id);
}

void DesktopService::open_profile_folder() {
    enqueue(
        "folder", [path = settings.fileName()](Manager &) { desktop_platform::open_folder(path); },
        operation_error_source::platform);
}

void DesktopService::restore_library(const QString &path) {
    if (is_ready() || is_opening() || path.isEmpty())
        return;
    const auto id = begin_operation("library");
    emit library_restore_requested(id, path);
}

quint64 DesktopService::begin_operation(const QString &operation) {
    const auto id = ++*request_sequence;
    publish_result({id, operation, operation_state::loading, {}});
    return id;
}

void DesktopService::publish_result(const operation_result &result) {
    if (pending_exclusions_request_id != 0 && result.id == pending_exclusions_request_id &&
        (result.state == operation_state::completed || result.state == operation_state::cancelled ||
         result.state == operation_state::error)) {
        auto exclusions = std::move(pending_exclusions);
        pending_exclusions.clear();
        pending_exclusions_request_id = 0;
        if (result.state == operation_state::completed) {
            exclusions.append(settings.value("sources/excluded_configurations").toStringList());
            exclusions.removeDuplicates();
            settings.setValue("sources/excluded_configurations", exclusions);
            settings.sync();
            if (settings.status() != QSettings::NoError) {
                last_failure = {QCoreApplication::translate(
                                    "XLibrary", "Import completed, but discovery exclusions could not be saved."),
                                "QSettings sync failed for discovery exclusions", operation_error_source::filesystem};
                emit message(last_failure.user_message);
            }
        }
    }
    if (result.state == operation_state::error)
        last_failure = result.error;
    if (result.operation.startsWith("profile_")) {
        if (result.state == operation_state::loading)
            profile_request_id = result.id;
        if (result.id == profile_request_id) {
            profile_status = result.state;
            GithubTokenStore::lock_profile(settings, profile_busy() || restart_pending);
            emit profile_state_changed();
        }
    }
    if (result.operation == "library") {
        if (result.state == operation_state::loading)
            library_request_id = result.id;
        if (result.id != library_request_id)
            return;
        library_status = result.state;
        opening_error = result.error;
        emit catalog_changed();
        if (is_ready() && startup_inventory_pending && !QCoreApplication::arguments().contains("--check-startup")) {
            QTimer::singleShot(0, this, [this] {
                if (!startup_inventory_pending)
                    return;
                startup_inventory_pending = false;
                if (!is_discovering())
                    discover(true);
            });
        }
    }
    if (result.id == missing_removal_request_id && result.state != operation_state::loading) {
        missing_removal_request_id = 0;
        missing_records.clear();
        reviewing_missing = false;
        if (result.state == operation_state::completed)
            continue_inventory_review();
        else
            publish_result({discovery_request_id, preview_operation, result.state, {}});
    }
    if (result.id == discovery_request_id) {
        discovery_state = result.state;
        if (result.state != operation_state::loading)
            reviewing = false;
        if (result.state == operation_state::completed) {
            scan_progress.percent = 100;
            scan_progress.stage = QCoreApplication::translate("XLibrary", "Scan complete");
            emit discovery_progress_changed();
        }
        emit discovery_changed();
    }
    emit operation_changed(result);
    // Ошибку запуска уже показывает отдельный результат launch; сценарий сохраняет её для своих подписчиков.
    if (result.state == operation_state::error && result.operation != "run_scenario" &&
        !(startup_scan && (result.id == discovery_request_id || result.operation == "remove_missing")) &&
        !result.error.user_message.isEmpty())
        emit message(result.error.user_message);
    if (result.state != operation_state::loading)
        emit operation_finished(result.operation,
                                result.state == operation_state::completed || result.state == operation_state::ready);
}

quint64 DesktopService::enqueue(const QString &operation, std::function<void(Manager &)> task,
                                const operation_error_source source, const quint64 request_id) {
    const auto id = request_id ? request_id : begin_operation(operation);
    if ((profile_busy() || restart_pending) && !operation.startsWith("profile_") && operation != "folder" &&
        operation != "diagnostics") {
        QTimer::singleShot(0, this, [this, id, operation] {
            publish_result(
                {id,
                 operation,
                 operation_state::error,
                 {QCoreApplication::translate("XLibrary", "The profile is being restored or requires a restart."),
                  "Profile changes are paused", operation_error_source::library}});
        });
        return id;
    }
    const auto queued = QMetaObject::invokeMethod(
        worker,
        [worker = worker, id, operation, source, task = std::move(task)] {
            worker->execute(id, operation, task, source);
        },
        Qt::QueuedConnection);
    if (!queued) {
        QTimer::singleShot(0, this, [this, id, operation] {
            publish_result({id,
                            operation,
                            operation_state::error,
                            {QCoreApplication::translate("XLibrary", "The operation could not be queued."),
                             "Catalog worker invocation failed", operation_error_source::internal}});
        });
    }
    return id;
}

void DesktopService::select(const qint64 id) {
    if (selected_id != id) {
        selected_id = id;
        if (const auto *record = model.find(id))
            launch_profiles(*record);
        emit selection_changed();
    }
}

void DesktopService::navigate(const QString &section, const QString &tag) {
    current_section = section;
    current_tag = tag;
    filter.set_section(section, tag);
    emit navigation_changed();
}

quint64 DesktopService::save_application(app_record record) {
    return enqueue("application", [record = std::move(record)](Manager &worker) { worker.save_application(record); });
}

quint64 DesktopService::resolve_application(const QString &path) {
    return enqueue(
        "resolve", [path](Manager &worker) { worker.resolve_application(path); }, operation_error_source::platform);
}

void DesktopService::select_applications(const QVector<qint64> &ids) {
    if (selected_ids == ids)
        return;
    selected_ids = ids;
    emit selections_changed();
}

void DesktopService::remove_application(const qint64 id) {
    remove_applications({id});
}

void DesktopService::remove_applications(const QVector<qint64> &ids) {
    if (ids.isEmpty())
        return;
    if (is_scenario_active()) {
        notify(QCoreApplication::translate("XLibrary", "Stop the scenario before removing applications."));
        return;
    }
    enqueue("remove_application", [ids](Manager &worker) { worker.remove_applications(ids); });
}

void DesktopService::undo_removal() {
    if (is_scenario_active()) {
        notify(QCoreApplication::translate("XLibrary", "Stop the scenario before restoring applications."));
        return;
    }
    enqueue("undo_removal", [](Manager &worker) { worker.undo_removal(); });
}

quint64 DesktopService::update_metadata(const QVector<qint64> &ids, const QStringList &add_tags,
                                        const QStringList &remove_tags, const int favorite) {
    return enqueue("metadata", [ids, add_tags, remove_tags, favorite](Manager &worker) {
        worker.update_metadata(ids, add_tags, remove_tags, favorite);
    });
}

quint64 DesktopService::save_filter(saved_filter record) {
    return enqueue("filter", [record = std::move(record)](Manager &worker) { worker.save_filter(record); });
}

void DesktopService::remove_filter(const qint64 id) {
    enqueue("remove_filter", [id](Manager &worker) { worker.remove_filter(id); });
}

void DesktopService::apply_filter(const saved_filter &record) {
    navigate(record.section, record.tag);
    filter.set_query(record.query);
    filter.set_sort_mode(record.sort_mode, record.sort_order);
}

void DesktopService::clear_filters() {
    navigate("library");
    filter.set_query({});
    filter.set_sort_mode(0);
}

quint64 DesktopService::save_tag(const QString &old_name, const QString &new_name) {
    return enqueue("tag", [old_name, new_name](Manager &worker) { worker.save_tag(old_name, new_name); });
}

void DesktopService::remove_tag(const QString &name) {
    enqueue("remove_tag", [name](Manager &worker) { worker.remove_tag(name); });
}

quint64 DesktopService::save_scenario(scenario_record record) {
    return enqueue("scenario", [record = std::move(record)](Manager &worker) { worker.save_scenario(record); });
}

void DesktopService::remove_scenario(const qint64 id) {
    enqueue("remove_scenario", [id](Manager &worker) { worker.remove_scenario(id); });
}

void DesktopService::launch(const qint64 id, const bool new_instance, const bool scenario) {
    request_launch(id, new_instance, scenario);
}

quint64 DesktopService::request_launch(const qint64 id, const bool new_instance, const bool scenario) {
    const auto mode = new_instance ? desktop_platform::launch_mode::new_instance
                      : settings.value("behavior/relaunch", "activate").toString() == "activate"
                          ? desktop_platform::launch_mode::activate
                          : desktop_platform::launch_mode::launch;
    return request_launch_mode(id, mode, scenario);
}

quint64 DesktopService::request_launch_profile(const qint64 id, const QString &profile) {
    return request_launch_mode(id, Application::launch_mode::launch, false, profile);
}

quint64 DesktopService::request_launch_mode(const qint64 id, const Application::launch_mode mode, const bool scenario,
                                            const QString &profile) {
    if (!is_ready() || !id || profile_busy() || restart_pending)
        return 0;
    if (launching.contains(id)) {
        if (scenario) {
            cancel_scenario();
            notify(QCoreApplication::translate(
                "XLibrary", "The application is still starting. Try the scenario again after it has started."));
        }
        return 0;
    }
    launching.insert(id);
    const auto generation = scenario ? scenario_generation->load() : 0;
    return enqueue(
        "launch",
        [id, mode, generation, token = scenario_generation, profile](Manager &worker) {
            worker.launch_record(id, mode, generation, token, desktop_platform::launch_application, profile);
        },
        operation_error_source::platform);
}

void DesktopService::open_folder(const qint64 id) {
    if (const auto *record = model.find(id);
        record && (Application::file_target(*record) || !record->installation_directory.isEmpty()))
        enqueue(
            "folder",
            [path = record->installation_directory.isEmpty() ? record->path : record->installation_directory + "/."](
                Manager &) { desktop_platform::open_folder(path); },
            operation_error_source::platform);
}

void DesktopService::run_scenario(const qint64 id) {
    if (!is_ready() || !id || profile_busy() || restart_pending)
        return;
    if (is_scenario_active()) {
        notify(QCoreApplication::translate("XLibrary", "A scenario is already running. Stop it first."));
        return;
    }
    pending_scenario_id = id;
    emit scenario_changed(QCoreApplication::translate("XLibrary", "Checking scenario…"));
    check_scenario(id, true);
}

void DesktopService::start_scenario(const qint64 id) {
    if (!is_ready() || profile_busy() || restart_pending)
        return;
    for (const auto &record : snapshot.scenarios) {
        if (record.id == id) {
            if (std::none_of(record.steps.cbegin(), record.steps.cend(),
                             [](const auto &step) { return step.enabled; })) {
                notify(QCoreApplication::translate("XLibrary", "The scenario has no enabled applications."));
                return;
            }
            pending_steps = record.steps;
            step_states.clear();
            step_errors.clear();
            for (const auto &step : pending_steps) {
                step_states.append(step.enabled ? "waiting" : "skipped");
                step_errors.append(QString{});
            }
            running_scenario_id = id;
            scenario_name = record.name;
            scenario_run_generation = ++*scenario_generation;
            step_index = 0;
            scenario_status = operation_state::loading;
            scenario_request_id = begin_operation("run_scenario");
            next_scenario_step();
            return;
        }
    }
}

QStringList DesktopService::scenario_issues(const qint64 id) const {
    return scenario_check_results.value(id);
}

void DesktopService::invalidate_scenario_checks() {
    scenario_check_results.clear();
    scenario_check_requests.clear();
    if (pending_scenario_id)
        check_scenario(pending_scenario_id, true);
    emit scenario_check_changed(0);
}

void DesktopService::check_scenario(const qint64 id, const bool force) {
    if (!id || !is_ready() || profile_busy() || restart_pending ||
        (!force && (scenario_check_results.contains(id) || scenario_check_requests.contains(id))))
        return;
    const auto found = std::find_if(snapshot.scenarios.cbegin(), snapshot.scenarios.cend(),
                                    [id](const auto &record) { return record.id == id; });
    const auto record = found == snapshot.scenarios.cend() ? scenario_record{} : *found;
    const auto request = ++scenario_check_sequence;
    scenario_check_results.remove(id);
    scenario_check_requests.insert(id, request);
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, id, request] {
        watcher->deleteLater();
        if (scenario_check_requests.value(id) != request)
            return;
        scenario_check_requests.remove(id);
        QStringList issues;
        try {
            issues = watcher->result();
        } catch (const std::exception &) {
            issues = {QCoreApplication::translate("XLibrary", "Could not check the scenario.")};
        }
        scenario_check_results.insert(id, issues);
        emit scenario_check_changed(id);
        if (pending_scenario_id != id)
            return;
        pending_scenario_id = 0;
        if (!issues.isEmpty()) {
            emit scenario_changed(QCoreApplication::translate("XLibrary", "Fix the scenario before running it:"));
            notify(QCoreApplication::translate("XLibrary", "Fix the scenario before running it:") + "\n" +
                   issues.first());
            return;
        }
        start_scenario(id);
    });
    watcher->setFuture(QtConcurrent::run(&launch_profile_pool, [record, applications = snapshot.applications] {
        if (!record.id)
            return QStringList{QCoreApplication::translate("XLibrary", "The scenario was not found.")};
        return Manager::scenario_issues(record, applications);
    }));
}

void DesktopService::next_scenario_step() {
    while (step_index < pending_steps.size() && !pending_steps[step_index].enabled)
        ++step_index;
    if (step_index >= pending_steps.size()) {
        finish_scenario(operation_state::completed);
        emit scenario_changed(QCoreApplication::translate("XLibrary", "Scenario “") + scenario_name +
                              QCoreApplication::translate("XLibrary", "” completed."));
        return;
    }
    emit scenario_changed(QCoreApplication::translate("XLibrary", "%1 · step %2 of %3")
                              .arg(scenario_name)
                              .arg(step_index + 1)
                              .arg(pending_steps.size()));
    step_states[step_index] = pending_steps[step_index].delay_ms > 0 ? "delaying" : "waiting";
    emit scenario_steps_changed();
    scenario_timer.start(qMax(0, pending_steps[step_index].delay_ms));
}

void DesktopService::cancel_scenario() {
    if (!is_scenario_active())
        return;
    if (pending_scenario_id)
        pending_scenario_id = 0;
    else
        finish_scenario(operation_state::cancelled);
    emit scenario_changed(QCoreApplication::translate(
        "XLibrary", "The scenario stopped. Applications already opened will keep running."));
}

void DesktopService::finish_scenario(const operation_state state, const operation_error &error) {
    ++*scenario_generation;
    scenario_status = state;
    scenario_timer.stop();
    for (auto &step_state : step_states)
        if (step_state == "waiting" || step_state == "delaying")
            step_state = "skipped";
    clear_stale_scenario_run();
    emit scenario_steps_changed();
    publish_result({scenario_request_id, "run_scenario", state, error});
}

void DesktopService::clear_stale_scenario_run() {
    if (is_scenario_active() || !running_scenario_id || step_states.contains("launching"))
        return;
    const auto found = std::find_if(snapshot.scenarios.cbegin(), snapshot.scenarios.cend(),
                                    [this](const auto &record) { return record.id == running_scenario_id; });
    const auto same_step = [](const scenario_step &left, const scenario_step &right) {
        return left.application_id == right.application_id && left.delay_ms == right.delay_ms &&
               left.enabled == right.enabled && left.launch_profile == right.launch_profile;
    };
    if (found == snapshot.scenarios.cend() || !std::equal(pending_steps.cbegin(), pending_steps.cend(),
                                                          found->steps.cbegin(), found->steps.cend(), same_step)) {
        running_scenario_id = 0;
        pending_steps.clear();
        step_states.clear();
        step_errors.clear();
    }
}

QString DesktopService::scenario_step_status(const qsizetype index) const {
    if (index < 0 || index >= step_states.size())
        return {};
    const auto &state = step_states[index];
    QString text;
    if (state == "waiting")
        text = QCoreApplication::translate("XLibrary", "Waiting");
    else if (state == "delaying")
        text = QCoreApplication::translate("XLibrary", "Delay");
    else if (state == "launching")
        text = QCoreApplication::translate("XLibrary", "Launching");
    else if (state == "success")
        text = QCoreApplication::translate("XLibrary", "Success");
    else if (state == "error")
        text = QCoreApplication::translate("XLibrary", "Error");
    else
        text = QCoreApplication::translate("XLibrary", "Skipped");
    if (!step_errors[index].isEmpty())
        text += ": " + step_errors[index];
    return text;
}

bool DesktopService::begin_catalog_preview(const QString &operation, const bool at_startup) {
    if (is_discovering() || !is_ready() || profile_busy() || restart_pending)
        return false;
    preview_operation = operation;
    reviewing = false;
    preview_records.clear();
    preview_issues.clear();
    missing_records.clear();
    reviewing_missing = false;
    startup_scan = at_startup;
    discovery_state = operation_state::loading;
    discovery_request_id = begin_operation(operation);
    discovery_cancelled = std::make_shared<discovery_control>();
    scan_progress = {-1, 0, 0, QCoreApplication::translate("XLibrary", "Preparing import preview…")};
    emit discovery_changed();
    emit discovery_progress_changed();
    return true;
}

void DesktopService::present_catalog_preview(QVector<app_record> records, QVector<discovery_issue> issues) {
    if (is_scan_preview()) {
        const auto exclusions = settings.value("sources/excluded_configurations").toStringList();
        const auto dismissed = settings.value("sources/dismissed_targets").toStringList();
        QSet<QString> known;
        for (const auto &record : snapshot.applications)
            known.insert(Application::launch_key(record));
        records.removeIf([&](const app_record &record) {
            if (known.contains(Application::launch_key(record)))
                return true;
            const auto key = launch_configuration_key(record.path, record.arguments, record.working_directory);
            auto excluded = exclusions.contains(key) || dismissed.contains(Application::target_key(record));
#ifdef Q_OS_WIN
            // Keep exclusions saved before empty and default directories became equivalent.
            if (!excluded && key == launch_configuration_key(record.path, record.arguments, {}))
                excluded = exclusions.contains(launch_configuration_key(record.path, record.arguments, {}, false));
#endif
            if (!excluded)
                return false;
            issues.append({record.discovery_source, QCoreApplication::translate("XLibrary", "Excluded by the user."),
                           discovery_issue_kind::skipped});
            return true;
        });
    }
    preview_records = std::move(records);
    preview_issues = std::move(issues);
    reviewing = true;
    if (!missing_records.isEmpty()) {
        reviewing_missing = true;
        if (!settings.value("sources/confirm_removal", true).toBool()) {
            // Let an uninstaller/updater finish before the independent check at commit time.
            QTimer::singleShot(1500, this, [this, cancelled = discovery_cancelled] {
                if (cancelled != discovery_cancelled || cancelled->requested.load() || !reviewing_missing)
                    return;
                QVector<int> rows;
                for (int row = 0; row < missing_records.size(); ++row)
                    rows.append(row);
                apply_catalog_preview(rows, false);
            });
            return;
        }
        emit catalog_preview_ready();
        return;
    }
    continue_inventory_review();
}

void DesktopService::continue_inventory_review() {
    if (is_scan_preview() && preview_records.isEmpty()) {
        scan_report =
            discovery_details(QCoreApplication::translate("XLibrary", "No new applications found."), preview_issues);
        emit discovery_report_changed();
        publish_result({discovery_request_id, preview_operation, operation_state::completed, {}});
        if (!startup_scan)
            notify(QCoreApplication::translate("XLibrary", "No new applications found."));
        return;
    }
    reviewing = true;
    scan_progress = {90, 0, preview_records.size(),
                     QCoreApplication::translate("XLibrary", "Review applications before import")};
    emit discovery_progress_changed();
    emit catalog_preview_ready();
}

void DesktopService::keep_missing_applications() {
    if (!reviewing_missing || missing_removal_request_id)
        return;
    reviewing_missing = false;
    missing_records.clear();
    // Open the next dialog after the current dialog has processed rejection.
    QTimer::singleShot(0, this, [this] {
        if (is_discovering() && !discovery_cancelled->requested.load())
            continue_inventory_review();
    });
}

QString DesktopService::preview_details() const {
    return discovery_details(QCoreApplication::translate("XLibrary", "Preview issues: %1").arg(preview_issues.size()),
                             preview_issues);
}

void DesktopService::clear_discovery_exclusions() {
    if (profile_busy() || restart_pending || is_discovering())
        return;
    settings.remove("sources/excluded_configurations");
    settings.remove("sources/dismissed_targets");
    settings.sync();
    notify(QCoreApplication::translate("XLibrary", "Discovery exclusions cleared."));
}

void DesktopService::apply_catalog_preview(const QVector<int> &rows, const bool remember_exclusions,
                                           import_conflict_policy policy) {
    if (!reviewing || !is_discovering())
        return;
    const QSet<int> selected_rows(rows.cbegin(), rows.cend());
    if (reviewing_missing) {
        if (is_scenario_active()) {
            notify(QCoreApplication::translate("XLibrary", "Stop the scenario before removing applications."));
            keep_missing_applications();
            return;
        }
        QVector<app_record> removed;
        for (int row = 0; row < missing_records.size(); ++row)
            if (selected_rows.contains(row))
                removed.append(missing_records[row]);
        reviewing = false;
        missing_removal_request_id =
            enqueue("remove_missing", [removed, cancelled = discovery_cancelled](Manager &worker) {
                worker.remove_missing_applications(removed, cancelled);
            });
        emit discovery_changed();
        return;
    }
    QVector<app_record> applications;
    auto issues = preview_issues;
    auto exclusions = settings.value("sources/excluded_configurations").toStringList();
    const auto dismissed = settings.value("sources/dismissed_targets").toStringList();
    for (int row = 0; row < preview_records.size(); ++row) {
        const auto &record = preview_records[row];
        if (is_scan_preview() && dismissed.contains(Application::target_key(record)))
            continue;
        if (selected_rows.contains(row)) {
            applications.append(record);
        } else {
            issues.append({record.discovery_source.isEmpty() ? record.path : record.discovery_source,
                           QCoreApplication::translate("XLibrary", "Not selected for import."),
                           discovery_issue_kind::skipped});
            if (remember_exclusions && is_scan_preview())
                exclusions.append(launch_configuration_key(record.path, record.arguments, record.working_directory));
        }
    }
    if (remember_exclusions && is_scan_preview()) {
        exclusions.removeDuplicates();
        pending_exclusions = std::move(exclusions);
        pending_exclusions_request_id = discovery_request_id;
    }
    reviewing = false;
    preview_records.clear();
    scan_progress.stage = QCoreApplication::translate("XLibrary", "Adding applications to the library");
    emit discovery_progress_changed();
    if (!is_json_preview())
        policy = import_conflict_policy::skip;
    const auto report_progress = [this, request_id = discovery_request_id,
                                  last_percent = -1](const qsizetype processed, const qsizetype total) mutable {
        const auto percent = total > 0 ? 90 + static_cast<int>(9.0 * processed / total) : 99;
        if (percent == last_percent)
            return;
        last_percent = percent;
        QMetaObject::invokeMethod(
            this,
            [this, request_id, processed, total, percent] {
                if (request_id != discovery_request_id || !is_discovering() || discovery_cancelled->requested.load())
                    return;
                scan_progress = {percent, processed, total,
                                 QCoreApplication::translate("XLibrary", "Adding applications to the library")};
                emit discovery_progress_changed();
            },
            Qt::QueuedConnection);
    };
    enqueue(
        preview_operation,
        [applications = std::move(applications), issues = std::move(issues), cancelled = discovery_cancelled, policy,
         report_progress, quiet = startup_scan](Manager &worker) mutable {
            worker.apply_catalog_preview(applications, std::move(issues), cancelled, policy, report_progress, quiet);
        },
        operation_error_source::sqlite, discovery_request_id);
}

void DesktopService::preview_files(const QStringList &paths) {
    if (paths.isEmpty() || !begin_catalog_preview("files"))
        return;
    enqueue(
        "files", [paths, cancelled = discovery_cancelled](Manager &worker) { worker.preview_files(paths, cancelled); },
        operation_error_source::platform, discovery_request_id);
}

void DesktopService::discover(const bool at_startup) {
    if (is_discovering() || !is_ready() || profile_busy() || restart_pending)
        return;
    const bool registry = settings.value("sources/registry", true).toBool();
    const bool start_menu = settings.value("sources/start_menu", true).toBool();
    if (!registry && !start_menu && !at_startup) {
        notify(QCoreApplication::translate("XLibrary", "Select discovery sources in the library settings."));
        return;
    }
    if (!begin_catalog_preview("discover", at_startup))
        return;
    scan_progress = {-1, 0, 0, QCoreApplication::translate("XLibrary", "Preparing the application list…")};
    emit discovery_changed();
    // Updates from both worker threads reach the UI only when the percentage or stage changes.
    const std::function<void(const discovery_progress &)> report_progress =
        [this, cancelled = discovery_cancelled, last_percent = -2,
         last_stage = QString{}](const discovery_progress &progress) mutable {
            if (progress.percent == last_percent && progress.stage == last_stage)
                return;
            last_percent = progress.percent;
            last_stage = progress.stage;
            QMetaObject::invokeMethod(
                this,
                [this, cancelled, progress] {
                    if (!is_discovering() || cancelled != discovery_cancelled || cancelled->requested.load() ||
                        progress.percent < scan_progress.percent)
                        return;
                    scan_progress = progress;
                    emit discovery_progress_changed();
                },
                Qt::QueuedConnection);
        };
    auto *watcher = new QFutureWatcher<std::pair<QVector<app_record>, QVector<discovery_issue>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        try {
            auto result = watcher->result();
            if (discovery_cancelled->requested.load()) {
                const auto message = discovery_summary(
                    0, result.second,
                    QCoreApplication::translate("XLibrary", "Scan cancelled; changes were not saved."));
                scan_report = discovery_details(message, result.second);
                emit discovery_report_changed();
                publish_result({discovery_request_id, "discover", operation_state::cancelled, {}});
                if (!startup_scan)
                    notify(message);
                watcher->deleteLater();
                return;
            }
            enqueue(
                "discover",
                [records = std::move(result.first), issues = std::move(result.second), cancelled = discovery_cancelled](
                    Manager &worker) { worker.review_inventory(records, issues, cancelled); },
                operation_error_source::filesystem, discovery_request_id);
        } catch (const DiscoveryCancelled &) {
            const auto message = discovery_summary(
                0, {}, QCoreApplication::translate("XLibrary", "Scan cancelled; changes were not saved."));
            scan_report = message;
            emit discovery_report_changed();
            publish_result({discovery_request_id, "discover", operation_state::cancelled, {}});
            if (!startup_scan)
                notify(message);
        } catch (const std::exception &error) {
            const QVector<discovery_issue> issues{{"Discovery", QString::fromUtf8(error.what())}};
            const auto message = discovery_summary(
                0, issues, QCoreApplication::translate("XLibrary", "Scan failed; changes were not saved."));
            scan_report = discovery_details(message, issues);
            emit discovery_report_changed();
            publish_result({discovery_request_id,
                            "discover",
                            operation_state::error,
                            {message, QString::fromUtf8(error.what()), operation_error_source::platform}});
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&discovery_pool, [registry, start_menu, cancelled = discovery_cancelled,
                                                           report_progress] {
        QVector<discovery_issue> issues;
        auto applications = Application::discover(registry, start_menu, issues, cancelled->requested, report_progress);
        return std::make_pair(std::move(applications), std::move(issues));
    }));
}

void DesktopService::cancel_discovery() {
    if (!can_cancel_discovery())
        return;
    auto expected = discovery_phase::running;
    if (!discovery_cancelled->phase.compare_exchange_strong(expected, discovery_phase::cancelling))
        return;
    discovery_cancelled->requested.store(true);
    if (reviewing) {
        reviewing = false;
        preview_records.clear();
        missing_records.clear();
        reviewing_missing = false;
        scan_report = discovery_details(
            QCoreApplication::translate("XLibrary", "Import cancelled; changes were not saved."), preview_issues);
        emit discovery_report_changed();
        publish_result({discovery_request_id, preview_operation, operation_state::cancelled, {}});
        return;
    }
    scan_progress.stage = QCoreApplication::translate("XLibrary", "Stopping the scan…");
    emit discovery_progress_changed();
    if (!startup_scan)
        notify(QCoreApplication::translate("XLibrary", "Stopping application discovery…"));
}

void DesktopService::export_catalog(const QString &path) {
    enqueue("export", [path](Manager &worker) { worker.export_catalog(path); }, operation_error_source::filesystem);
}

void DesktopService::import_catalog(const QString &path) {
    if (!begin_catalog_preview("import"))
        return;
    enqueue(
        "import", [path](Manager &worker) { worker.import_catalog(path); }, operation_error_source::filesystem,
        discovery_request_id);
}

void DesktopService::clear_history() {
    enqueue("history", [](Manager &worker) { worker.clear_history(); });
}

void DesktopService::record_file_use(const QString &path) {
    if (!is_ready() || profile_busy() || restart_pending)
        return;
    enqueue("history", [path](Manager &worker) {
        worker.store->record_file_use(path);
        worker.refresh();
    });
}

void DesktopService::notify(const QString &text) {
    emit message(text);
}

bool DesktopService::safe_mode() const {
    return QCoreApplication::arguments().contains("--safe-mode");
}

QString DesktopService::diagnostics_text() const {
    const auto mode = application_paths::portable_mode() ? QCoreApplication::translate("XLibrary", "Portable")
                                                         : QCoreApplication::translate("XLibrary", "Installed");
    QString text = QCoreApplication::translate("XLibrary", "Product version: %1\nSchema version: %2 (supported: "
                                                           "%3)\nDatabase: %4\nSettings: %5\nMode: %6\nSafe mode: %7")
                       .arg(QCoreApplication::applicationVersion())
                       .arg(current_schema_version)
                       .arg(xlibrary::profile_schema_version)
                       .arg(library_file, settings.fileName(), mode,
                            safe_mode() ? QCoreApplication::translate("XLibrary", "Yes")
                                        : QCoreApplication::translate("XLibrary", "No"));
    if (last_failure.source != operation_error_source::none)
        text += QCoreApplication::translate("XLibrary", "\nLast error (source %1): %2\nTechnical reason: %3")
                    .arg(operation_source_name(last_failure.source))
                    .arg(last_failure.user_message, last_failure.technical_reason);
    return text;
}

quint64 DesktopService::backup_profile(const QString &path) {
    if (path.isEmpty() || profile_busy() || is_discovering() || is_scenario_active()) {
        notify(QCoreApplication::translate(
            "XLibrary", "Wait for the current profile operation or stop the scan and scenario first."));
        return 0;
    }
    settings.sync();
    const auto settings_ok = settings.status() == QSettings::NoError;
    return enqueue(
        "profile_backup",
        [path, database_path = library_file, settings_path = settings.fileName(), settings_ok](Manager &worker) {
            worker.backup_profile(path, database_path, settings_path, settings_ok);
        },
        operation_error_source::filesystem);
}

quint64 DesktopService::preview_profile(const QString &path) {
    if (path.isEmpty() || profile_busy())
        return 0;
    if (is_discovering() || is_scenario_active()) {
        notify(QCoreApplication::translate(
            "XLibrary", "Wait for the current profile operation or stop the scan and scenario first."));
        return 0;
    }
    return enqueue(
        "profile_preview", [path](Manager &worker) { worker.preview_profile(path); },
        operation_error_source::filesystem);
}

quint64 DesktopService::restore_profile(const QString &path, const QByteArray &fingerprint) {
    if (path.isEmpty() || profile_busy() || is_opening() || is_discovering() || is_scenario_active() ||
        !launching.isEmpty() || GithubTokenStore::pending_changes(settings)) {
        notify(QCoreApplication::translate(
            "XLibrary", "Wait for the current profile operation or stop the scan and scenario first."));
        return 0;
    }
    settings.sync();
    const auto settings_ok = settings.status() != QSettings::AccessError;
    const auto library_id = begin_operation("library");
    const auto db_path = library_file;
    return enqueue(
        "profile_restore",
        [path, fingerprint, database_path = db_path, settings_path = settings.fileName(), settings_ok, library_id,
         old_autostart = settings.value("behavior/autostart", false).toBool()](Manager &worker) {
            worker.restore_profile(path, database_path, settings_path, fingerprint, settings_ok, library_id,
                                   old_autostart);
        },
        operation_error_source::filesystem);
}

quint64 DesktopService::export_diagnostics(const QString &path) {
    // Только служебные поля: произвольные настройки, сообщения и причины ошибок могут содержать секреты.
    const auto report =
        QJsonDocument(QJsonObject{{"format", 1},
                                  {"application_version", QCoreApplication::applicationVersion()},
                                  {"schema_version", current_schema_version},
                                  {"supported_schema_version", xlibrary::profile_schema_version},
                                  {"database_path", library_file},
                                  {"settings_path", settings.fileName()},
                                  {"mode", application_paths::portable_mode() ? "portable" : "installed"},
                                  {"safe_mode", safe_mode()},
                                  {"library_state", static_cast<int>(library_status)},
                                  {"last_error_source", operation_source_name(last_failure.source)},
                                  {"has_error", last_failure.source != operation_error_source::none}})
            .toJson();
    return enqueue(
        "diagnostics", [path, report](Manager &worker) { worker.export_diagnostics(path, report); },
        operation_error_source::filesystem);
}

quint64 DesktopService::preview_paths(const QString &old_root, const QString &new_root) {
    if (profile_busy() || restart_pending || is_discovering() || is_scenario_active() || !launching.isEmpty()) {
        notify(QCoreApplication::translate(
            "XLibrary", "Wait for the current profile operation or stop the scan and scenario first."));
        return 0;
    }
    return enqueue("profile_paths_preview",
                   [old_root, new_root](Manager &worker) { worker.preview_paths(old_root, new_root); });
}

quint64 DesktopService::recover_paths(const QVector<path_relocation> &changes) {
    if (profile_busy() || restart_pending || is_discovering() || is_scenario_active() || !launching.isEmpty()) {
        notify(QCoreApplication::translate(
            "XLibrary", "Wait for the current profile operation or stop the scan and scenario first."));
        return 0;
    }
    return enqueue("profile_paths_recovery", [changes](Manager &worker) { worker.recover_paths(changes); });
}

bool DesktopService::reset_settings() {
    if (!is_ready() || profile_busy() || restart_pending || is_discovering() || is_scenario_active() ||
        !launching.isEmpty() || pending_shortcut_request || GithubTokenStore::pending_changes(settings) ||
        updates.is_busy()) {
        notify(QCoreApplication::translate("XLibrary", "Finish current operations before resetting settings."));
        return false;
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        notify(QCoreApplication::translate("XLibrary", "Could not save the settings file."));
        return false;
    }
    QVariantMap previous;
    for (const auto &key : settings.allKeys())
        previous.insert(key, settings.value(key));
    const bool autostart = settings.value("behavior/autostart", false).toBool();
    QString error;
    if (autostart && !desktop_platform::set_autostart(false, error)) {
        notify(error);
        return false;
    }
    settings.clear();
    // Credentials belong to the system store and are managed separately from preferences.
    const auto marker = QString::fromLatin1(profile_settings::token_marker);
    if (previous.contains(marker))
        settings.setValue(marker, previous.value(marker));
    settings.setValue("onboarding/completed", true);
    if (previous.contains("files/saved"))
        settings.setValue("files/saved", previous.value("files/saved"));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        settings.clear();
        for (auto it = previous.cbegin(); it != previous.cend(); ++it)
            settings.setValue(it.key(), it.value());
        settings.sync();
        if (autostart && !desktop_platform::set_autostart(true, error))
            notify(error);
        notify(QCoreApplication::translate("XLibrary", "Could not save the settings file."));
        return false;
    }
    restart_pending = true;
    GithubTokenStore::lock_profile(settings, true);
    emit profile_state_changed();
    notify(QCoreApplication::translate(
        "XLibrary", "Settings reset. Restart XLibrary to apply the defaults. Your catalog was kept."));
    return true;
}

void DesktopService::restart_application() {
    if (profile_busy())
        return;
    auto arguments = QCoreApplication::arguments().mid(1);
    arguments.removeAll("--check-startup");
    if (!arguments.contains("--wait-for-profile"))
        arguments.append("--wait-for-profile");
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments,
                                 QCoreApplication::applicationDirPath())) {
        notify(QCoreApplication::translate("XLibrary", "Could not restart XLibrary. Close and open it manually."));
        return;
    }
    QCoreApplication::quit();
}

namespace {
QString shortcut_target_key(const bool scenario, const qint64 id) {
    return (scenario ? QStringLiteral("scenario/") : QStringLiteral("application/")) + QString::number(id);
}

QMap<QString, QString> builtin_shortcut_defaults() {
    return {{"shortcuts/global", desktop_platform::default_global_shortcut()},
            {"shortcuts/overlay", desktop_platform::default_overlay_shortcut()},
            {"shortcuts/search", "Ctrl+K"},
            {"shortcuts/settings", "Ctrl+,"},
            {"shortcuts/add", "Ctrl+N"},
            {"shortcuts/launch", "Return"},
            {"shortcuts/new_instance", "Ctrl+Return"},
            {"shortcuts/open_folder", "Alt+Return"},
            {"shortcuts/copy_path", "Ctrl+Shift+C"},
            {"shortcuts/remove", "Delete"}};
}

QList<QKeySequence> shortcut_aliases(const QKeySequence &sequence) {
    if (sequence.isEmpty())
        return {};
    QList<QKeySequence> result{sequence};
    if (sequence.count() == 1) {
        const auto key = sequence[0];
        if (key.key() == Qt::Key_Return || key.key() == Qt::Key_Enter)
            result.append(QKeySequence(QKeyCombination(key.keyboardModifiers(),
                                                       key.key() == Qt::Key_Return ? Qt::Key_Enter : Qt::Key_Return)));
    }
    return result;
}

QSet<QString> reserved_shortcut_sequences() {
    QSet<QString> result;
    const auto add = [&](const QKeySequence &sequence) {
        result.insert(sequence.toString(QKeySequence::PortableText));
    };
    add(QKeySequence(Qt::Key_Escape));
    add(QKeySequence(Qt::Key_Tab));
    add(QKeySequence(Qt::Key_Backtab));
    for (const auto key : {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End,
                           Qt::Key_PageUp, Qt::Key_PageDown})
        for (const auto modifiers : {Qt::KeyboardModifiers{}, Qt::KeyboardModifiers(Qt::ShiftModifier),
                                     Qt::KeyboardModifiers(Qt::ControlModifier),
                                     Qt::KeyboardModifiers(Qt::ControlModifier | Qt::ShiftModifier)})
            add(QKeySequence(QKeyCombination(modifiers, key)));
    for (const auto key : {QKeySequence::Copy, QKeySequence::Cut, QKeySequence::Paste, QKeySequence::Undo,
                           QKeySequence::Redo, QKeySequence::SelectAll})
        for (const auto &sequence : QKeySequence::keyBindings(key))
            add(sequence);
    return result;
}
}

QKeySequence DesktopService::target_shortcut(const bool scenario, const qint64 id) const {
    const auto key = shortcut_target_key(scenario, id);
    if (pending_shortcut_request && key == pending_shortcut_key)
        return registered_target_shortcuts.value(key);
    for (const auto &binding : snapshot.target_shortcuts)
        if (binding.scenario == scenario && binding.target_id == id)
            return QKeySequence::fromString(binding.sequence, QKeySequence::PortableText);
    return {};
}

bool DesktopService::validate_shortcut_configuration(const QVariantMap &values, const QString &ignore_target,
                                                     const QKeySequence &candidate, QString &error) const {
    auto sequences = reserved_shortcut_sequences();
    const auto add = [&](const QKeySequence &sequence) {
        if (sequence.isEmpty())
            return true;
        if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown) {
            error = QCoreApplication::translate("XLibrary", "Use a single key combination.");
            return false;
        }
        const auto aliases = shortcut_aliases(sequence);
        for (const auto &alias : aliases)
            if (sequences.contains(alias.toString(QKeySequence::PortableText))) {
                error = QCoreApplication::translate("XLibrary", "The shortcut %1 is already assigned to another action "
                                                                "or reserved for text editing and search.")
                            .arg(sequence.toString(QKeySequence::NativeText));
                return false;
            }
        for (const auto &alias : aliases)
            sequences.insert(alias.toString(QKeySequence::PortableText));
        return true;
    };
    const auto defaults = builtin_shortcut_defaults();
    for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
        const auto value = values.contains(it.key()) ? values.value(it.key()) : settings.value(it.key(), it.value());
        if (!add(QKeySequence::fromString(value.toString(), QKeySequence::PortableText)))
            return false;
    }
    QSet<QString> visited;
    for (const auto &binding : snapshot.target_shortcuts) {
        const auto key = shortcut_target_key(binding.scenario, binding.target_id);
        visited.insert(key);
        const auto sequence = pending_shortcut_request && key == pending_shortcut_key
                                  ? registered_target_shortcuts.value(key)
                                  : QKeySequence::fromString(binding.sequence, QKeySequence::PortableText);
        if (key != ignore_target && !add(sequence))
            return false;
    }
    // A newly assigned shortcut may not have reached the catalog worker yet.
    if (pending_shortcut_request && !visited.contains(pending_shortcut_key) && pending_shortcut_key != ignore_target &&
        !add(registered_target_shortcuts.value(pending_shortcut_key)))
        return false;
    if (pending_shortcut_request && !previous_target_shortcut.isEmpty() &&
        previous_target_shortcut != registered_target_shortcuts.value(pending_shortcut_key) &&
        !add(previous_target_shortcut))
        return false;
    return add(candidate);
}

bool DesktopService::validate_shortcuts(const QVariantMap &values, QString &error) const {
    return validate_shortcut_configuration(values, {}, {}, error);
}

bool DesktopService::validate_target_shortcut(const bool scenario, const qint64 id, const QKeySequence &sequence,
                                              QString &error) const {
    if (!sequence.isEmpty()) {
        if (sequence.count() != 1) {
            error = QCoreApplication::translate("XLibrary", "Use a single key combination.");
            return false;
        }
        const auto combination = sequence[0];
        const auto key = combination.key();
        if (!((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9) ||
              (key >= Qt::Key_F1 && key <= Qt::Key_F24) || key == Qt::Key_Space)) {
            error = QCoreApplication::translate("XLibrary", "Use a letter, digit, F1–F24, or Space.");
            return false;
        }
        const auto modifiers = combination.keyboardModifiers();
        if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) ||
            (modifiers & ~(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier))) {
            error = QCoreApplication::translate("XLibrary", "Add Ctrl, Alt, or Super to the global shortcut.");
            return false;
        }
    }
    return validate_shortcut_configuration({}, shortcut_target_key(scenario, id), sequence, error);
}

bool DesktopService::set_target_shortcut(const bool scenario, const qint64 id, const QKeySequence &sequence,
                                         QString &error) {
    if (!is_ready() || profile_busy() || restart_pending || pending_shortcut_request) {
        error = QCoreApplication::translate("XLibrary", "Wait for the current library operation to finish.");
        return false;
    }
    const auto exists = scenario ? std::any_of(snapshot.scenarios.cbegin(), snapshot.scenarios.cend(),
                                               [id](const auto &record) { return record.id == id; })
                                 : model.find(id) != nullptr;
    if (id <= 0 || !exists) {
        error = QCoreApplication::translate("XLibrary", "The shortcut target no longer exists.");
        return false;
    }
    if (!validate_target_shortcut(scenario, id, sequence, error))
        return false;
    const auto previous = target_shortcut(scenario, id);
    const auto key = shortcut_target_key(scenario, id);
    if (previous == sequence && (sequence.isEmpty() || registered_target_shortcuts.value(key) == sequence))
        return true;
    auto candidate = std::make_unique<GlobalHotkey>();
    connect(candidate.get(), &GlobalHotkey::registration_failed, this,
            [this, key, source = candidate.get()](const QString &message) {
                shortcut_registration_failed(source, key, message);
            });
    connect(candidate.get(), &GlobalHotkey::activated, this, [this, scenario, id] {
        if (!is_ready() || profile_busy() || restart_pending)
            return;
        if (scenario)
            run_scenario(id);
        else
            launch(id);
    });
    if (!candidate->set_sequence(sequence, error))
        return false;
    previous_refused_target_shortcut = refused_target_shortcuts.value(key);
    previous_target_shortcut_error =
        previous_refused_target_shortcut.isEmpty() ? QString{} : target_shortcut_errors.value(key);
    refused_target_shortcuts.remove(key);
    target_shortcut_errors.remove(key);
    previous_target_hotkey = std::move(target_hotkeys[key]);
    target_hotkeys[key] = std::move(candidate);
    previous_target_shortcut = registered_target_shortcuts.value(key);
    pending_shortcut_key = key;
    registered_target_shortcuts.insert(key, sequence);
    pending_shortcut_request = begin_operation("target_shortcut");
    enqueue(
        "target_shortcut",
        [scenario, id, text = sequence.toString(QKeySequence::PortableText)](Manager &manager) {
            manager.store->save_target_shortcut(scenario, id, text);
            manager.refresh();
        },
        operation_error_source::sqlite, pending_shortcut_request);
    emit target_shortcuts_changed();
    return true;
}

void DesktopService::shortcut_registration_failed(GlobalHotkey *const source, const QString &key,
                                                  const QString &message) {
    // An old registration can fail while retained for a pending database rollback.
    // Keep that failure with the old instance instead of poisoning its replacement.
    if (pending_shortcut_key == key && source == previous_target_hotkey.get()) {
        previous_target_shortcut_error = message;
        return;
    }
    const auto current = target_hotkeys.find(key);
    if (current == target_hotkeys.end() || current->second.get() != source)
        return;
    refused_target_shortcuts.insert(key, registered_target_shortcuts.value(key));
    registered_target_shortcuts.remove(key);
    target_shortcut_errors.insert(key, message);
    notify(message);
    emit target_shortcuts_changed();
}

void DesktopService::sync_target_hotkeys() {
    if (pending_shortcut_request)
        return;
    QSet<QString> desired;
    for (const auto &binding : snapshot.target_shortcuts)
        desired.insert(shortcut_target_key(binding.scenario, binding.target_id));
    for (auto it = target_hotkeys.begin(); it != target_hotkeys.end();) {
        if (!desired.contains(it->first)) {
            registered_target_shortcuts.remove(it->first);
            target_shortcut_errors.remove(it->first);
            refused_target_shortcuts.remove(it->first);
            it = target_hotkeys.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &binding : snapshot.target_shortcuts) {
        const auto key = shortcut_target_key(binding.scenario, binding.target_id);
        const auto sequence = QKeySequence::fromString(binding.sequence, QKeySequence::PortableText);
        // A refusal stays inactive until an explicit save, changed binding, or application restart.
        // Catalog/theme refreshes must not reopen compositor permission dialogs.
        if (refused_target_shortcuts.contains(key) && refused_target_shortcuts.value(key) == sequence)
            continue;
        if (target_hotkeys.contains(key) && target_hotkeys.at(key) &&
            registered_target_shortcuts.value(key) == sequence)
            continue;
        QString error;
        const auto report_error = [&](const QString &text) {
            const auto signature = binding.sequence + QChar::Null + text;
            if (target_shortcut_errors.value(key) != signature) {
                target_shortcut_errors.insert(key, signature);
                notify(text);
            }
        };
        if (!validate_target_shortcut(binding.scenario, binding.target_id, sequence, error)) {
            report_error(error);
            continue;
        }
        auto &hotkey = target_hotkeys[key];
        if (!hotkey) {
            hotkey = std::make_unique<GlobalHotkey>();
            connect(hotkey.get(), &GlobalHotkey::registration_failed, this,
                    [this, key, source = hotkey.get()](const QString &message) {
                        shortcut_registration_failed(source, key, message);
                    });
            connect(hotkey.get(), &GlobalHotkey::activated, this,
                    [this, scenario = binding.scenario, id = binding.target_id] {
                        if (!is_ready() || profile_busy() || restart_pending)
                            return;
                        if (scenario)
                            run_scenario(id);
                        else
                            launch(id);
                    });
        }
        if (hotkey->set_sequence(sequence, error)) {
            registered_target_shortcuts.insert(key, sequence);
            target_shortcut_errors.remove(key);
        } else {
            report_error(error);
        }
    }
    emit target_shortcuts_changed();
}

namespace {
QString launch_profile_key(const app_record &record) {
    return Application::target_kind(record) + QChar::Null + record.path;
}
constexpr qsizetype launch_profile_cache_limit = 64;
}

QVector<launch_profile> DesktopService::launch_profiles(const app_record &record) {
    const auto modes = Application::launch_modes(record);
    if (modes.isEmpty())
        return {};
    const auto found = launch_profile_cache.constFind(launch_profile_key(record));
    if (found != launch_profile_cache.cend())
        return *found;
    queue_launch_profiles(record, true);
    return modes;
}

void DesktopService::queue_launch_profiles(const app_record &record, const bool priority) {
    const auto key = launch_profile_key(record);
    if (launch_profile_cache.contains(key) || active_launch_profile == key)
        return;
    if (pending_launch_profiles.contains(key)) {
        if (priority) {
            launch_profile_queue.removeAll(key);
            launch_profile_queue.prepend(key);
        }
        return;
    }
    if (pending_launch_profiles.size() >= launch_profile_cache_limit) {
        if (!priority)
            return;
        pending_launch_profiles.remove(launch_profile_queue.takeLast());
    }
    pending_launch_profiles.insert(key, record);
    if (priority)
        launch_profile_queue.prepend(key);
    else
        launch_profile_queue.append(key);
    next_launch_profiles();
}

void DesktopService::next_launch_profiles() {
    if (!active_launch_profile.isEmpty() || launch_profile_queue.isEmpty())
        return;
    const auto key = launch_profile_queue.takeFirst();
    const auto record = pending_launch_profiles.take(key);
    active_launch_profile = key;
    auto *watcher = new QFutureWatcher<QVector<launch_profile>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, key, path = record.path] {
        auto profiles = watcher->result();
        watcher->deleteLater();
        // The GUI uses themed icons; profile image files remain on the worker side.
        for (auto &profile : profiles)
            profile.icon_path.clear();
        while (launch_profile_cache.size() >= launch_profile_cache_limit)
            launch_profile_cache.remove(launch_profile_order.takeFirst());
        launch_profile_cache.insert(key, std::move(profiles));
        launch_profile_order.append(key);
        active_launch_profile.clear();
        emit launch_profiles_changed(path);
        next_launch_profiles();
    });
    watcher->setFuture(QtConcurrent::run(&launch_profile_pool, [record] {
        try {
            return Application::launch_profiles(record);
        } catch (const std::exception &) {
            return Application::launch_modes(record);
        }
    }));
}

void DesktopService::warm_launch_profiles() {
    qsizetype count = 0;
    for (const auto &record : snapshot.applications) {
        if (Application::launch_modes(record).isEmpty())
            continue;
        queue_launch_profiles(record, false);
        if (++count >= launch_profile_cache_limit)
            break;
    }
}
