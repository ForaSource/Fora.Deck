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

DesktopService::DesktopService(QObject *const parent, const QString &profile_path, const QString &database_path)
    : QObject(parent), worker(new Manager(database_path, request_sequence)), model(this), filter(this),
      settings((profile_path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                       : profile_path) +
                   "/settings.ini",
               QSettings::IniFormat) {
    qRegisterMetaType<QVector<app_record>>();
    qRegisterMetaType<QVector<discovery_issue>>();
    qRegisterMetaType<operation_state>();
    qRegisterMetaType<operation_error_source>();
    qRegisterMetaType<operation_error>();
    qRegisterMetaType<operation_result>();
    qRegisterMetaType<profile_manifest>();
    qRegisterMetaType<QVector<path_relocation>>();
    library_request_id = begin_operation("library");
    worker->known_volume_roots = settings.value("availability/volume_roots").toStringList();
    filter.setSourceModel(&model);
    discovery_pool.setMaxThreadCount(1);
    discovery_pool.setExpiryTimeout(5000);
    worker->moveToThread(&worker_thread);
    connect(worker, &Manager::volumes_observed, this, [this](const QStringList &roots) {
        if (!profile_busy() && !restart_pending)
            settings.setValue("availability/volume_roots", roots);
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
        if (!current_tag.isEmpty() && !snapshot.tags.contains(current_tag))
            navigate(current_section);
        model.replace(value.applications);
        if (!model.find(selected_id))
            selected_id = 0;
        selected_ids.removeIf([this](const qint64 id) { return !model.find(id); });
        emit catalog_changed();
        emit selection_changed();
        emit selections_changed();
    });
    connect(worker, &Manager::state_changed, this, [this](const QVector<app_record> &applications) {
        snapshot.applications = applications;
        model.update_state(applications);
        emit selection_changed();
    });
    connect(worker, &Manager::launched, this,
            [this](const qint64 id, const operation_result &result, const quint64 from_scenario) {
                launching.remove(id);
                if (!from_scenario)
                    emit application_launch_finished(id, result);
                if (result.state == operation_state::completed && !from_scenario)
                    emit launch_succeeded();
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
            launch(pending_steps[step_index].application_id, false, true);
        }
    });
    worker_thread.setObjectName("catalog_worker");
    worker_thread.start();
}

DesktopService::~DesktopService() {
    ++*scenario_generation;
    auto expected = discovery_phase::running;
    discovery_cancelled->phase.compare_exchange_strong(expected, discovery_phase::cancelling);
    discovery_cancelled->requested.store(true);
    scenario_timer.stop();
    discovery_pool.clear();
    discovery_pool.waitForDone();
    worker_thread.quit();
    worker_thread.wait();
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
    const auto mode = new_instance ? desktop_platform::launch_mode::new_instance
                      : settings.value("behavior/relaunch", "activate").toString() == "activate"
                          ? desktop_platform::launch_mode::activate
                          : desktop_platform::launch_mode::launch;
    return enqueue(
        "launch",
        [id, mode, generation, token = scenario_generation](Manager &worker) {
            worker.launch_record(id, mode, generation, token, desktop_platform::launch_application);
        },
        operation_error_source::platform);
}

void DesktopService::open_folder(const qint64 id) {
    if (const auto *record = model.find(id))
        enqueue(
            "folder", [path = record->path](Manager &) { desktop_platform::open_folder(path); },
            operation_error_source::platform);
}

void DesktopService::run_scenario(const qint64 id) {
    if (profile_busy() || restart_pending)
        return;
    if (is_scenario_active()) {
        notify(QCoreApplication::translate("XLibrary", "A scenario is already running. Stop it first."));
        return;
    }
    for (const auto &record : snapshot.scenarios) {
        if (record.id == id) {
            pending_steps.clear();
            for (const auto &step : record.steps)
                if (step.enabled)
                    pending_steps.append(step);
            if (pending_steps.isEmpty()) {
                notify(QCoreApplication::translate("XLibrary", "The scenario has no enabled applications."));
                return;
            }
            scenario_name = record.name;
            ++*scenario_generation;
            step_index = 0;
            scenario_status = operation_state::loading;
            scenario_request_id = begin_operation("run_scenario");
            next_scenario_step();
            return;
        }
    }
}

void DesktopService::next_scenario_step() {
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
    scenario_timer.start(pending_steps[step_index].delay_ms);
}

void DesktopService::cancel_scenario() {
    if (!is_scenario_active())
        return;
    finish_scenario(operation_state::cancelled);
    emit scenario_changed(QCoreApplication::translate(
        "XLibrary", "The scenario stopped. Applications already opened will keep running."));
}

void DesktopService::finish_scenario(const operation_state state, const operation_error &error) {
    ++*scenario_generation;
    scenario_status = state;
    scenario_timer.stop();
    publish_result({scenario_request_id, "run_scenario", state, error});
}

bool DesktopService::begin_catalog_preview(const QString &operation) {
    if (is_discovering() || !is_ready() || profile_busy() || restart_pending)
        return false;
    preview_operation = operation;
    reviewing = false;
    preview_records.clear();
    preview_issues.clear();
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
        records.removeIf([&](const app_record &record) {
            const auto key = launch_configuration_key(record.path, record.arguments, record.working_directory);
            auto excluded = exclusions.contains(key);
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
    scan_progress = {90, 0, preview_records.size(),
                     QCoreApplication::translate("XLibrary", "Review applications before import")};
    emit discovery_progress_changed();
    emit catalog_preview_ready();
}

QString DesktopService::preview_details() const {
    return discovery_details(QCoreApplication::translate("XLibrary", "Preview issues: %1").arg(preview_issues.size()),
                             preview_issues);
}

void DesktopService::clear_discovery_exclusions() {
    if (profile_busy() || restart_pending || is_discovering())
        return;
    settings.remove("sources/excluded_configurations");
    settings.sync();
    notify(QCoreApplication::translate("XLibrary", "Discovery exclusions cleared."));
}

void DesktopService::apply_catalog_preview(const QVector<int> &rows, const bool remember_exclusions,
                                           import_conflict_policy policy) {
    if (!reviewing || !is_discovering())
        return;
    const QSet<int> selected_rows(rows.cbegin(), rows.cend());
    QVector<app_record> applications;
    auto issues = preview_issues;
    auto exclusions = settings.value("sources/excluded_configurations").toStringList();
    for (int row = 0; row < preview_records.size(); ++row) {
        const auto &record = preview_records[row];
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
         report_progress](Manager &worker) mutable {
            worker.apply_catalog_preview(applications, std::move(issues), cancelled, policy, report_progress);
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

void DesktopService::discover() {
    if (is_discovering() || !is_ready() || profile_busy() || restart_pending)
        return;
    const bool registry = settings.value("sources/registry", true).toBool();
    const bool start_menu = settings.value("sources/start_menu", true).toBool();
    if (!registry && !start_menu) {
        notify(QCoreApplication::translate("XLibrary", "Select discovery sources in the library settings."));
        return;
    }
    if (!begin_catalog_preview("discover"))
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
                notify(message);
                watcher->deleteLater();
                return;
            }
            present_catalog_preview(std::move(result.first), std::move(result.second));
        } catch (const DiscoveryCancelled &) {
            const auto message = discovery_summary(
                0, {}, QCoreApplication::translate("XLibrary", "Scan cancelled; changes were not saved."));
            scan_report = message;
            emit discovery_report_changed();
            publish_result({discovery_request_id, "discover", operation_state::cancelled, {}});
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
    if (!is_discovering())
        return;
    auto expected = discovery_phase::running;
    if (!discovery_cancelled->phase.compare_exchange_strong(expected, discovery_phase::cancelling))
        return;
    discovery_cancelled->requested.store(true);
    if (reviewing) {
        reviewing = false;
        preview_records.clear();
        scan_report = discovery_details(
            QCoreApplication::translate("XLibrary", "Import cancelled; changes were not saved."), preview_issues);
        emit discovery_report_changed();
        publish_result({discovery_request_id, preview_operation, operation_state::cancelled, {}});
        return;
    }
    scan_progress.stage = QCoreApplication::translate("XLibrary", "Stopping the scan…");
    emit discovery_progress_changed();
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
