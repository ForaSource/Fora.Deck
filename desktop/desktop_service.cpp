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

#include "application_paths.h"
#include "catalog_store.h"
#include "desktop_service.h"
#include "github_token_store.h"
#include "localization.h"
#include "path_helpers.h"
#include "platform_integration.h"
#include "profile_backup.h"
#include "ProfileSchema.h"

namespace {
operation_error exception_error(const QString &operation, const std::exception &error, operation_error_source source) {
    const auto reason = QString::fromUtf8(error.what());
    QString message;
    if (source == operation_error_source::validation || source == operation_error_source::platform)
        message = reason;
    else if (operation == "library")
        message = QCoreApplication::translate("XLibrary", "Could not open the library: %1").arg(reason);
    else if (operation == "application")
        message = QCoreApplication::translate("XLibrary", "Could not save the application.");
    else if (operation == "tag")
        message = QCoreApplication::translate("XLibrary", "Could not save the tag.");
    else if (operation == "scenario")
        message = QCoreApplication::translate("XLibrary", "Could not save the scenario.");
    else
        message = QCoreApplication::translate("XLibrary", "The operation could not be completed: %1").arg(reason);
    return {message, reason, source};
}

QString discovery_summary(qsizetype added, const QVector<discovery_issue> &issues, const QString &status) {
    qsizetype duplicates = 0;
    qsizetype skipped = 0;
    qsizetype errors = 0;
    for (const auto &issue : issues) {
        switch (issue.kind) {
        case discovery_issue_kind::duplicate:
            ++duplicates;
            break;
        case discovery_issue_kind::skipped:
            ++skipped;
            break;
        case discovery_issue_kind::error:
            ++errors;
            break;
        }
    }
    return QCoreApplication::translate("XLibrary", "%1 Added: %2. Duplicates: %3. Skipped: %4. Errors: %5.")
        .arg(status)
        .arg(added)
        .arg(duplicates)
        .arg(skipped)
        .arg(errors);
}

QString discovery_details(const QString &summary, const QVector<discovery_issue> &issues) {
    QStringList details{summary};
    for (const auto &issue : issues) {
        const auto kind =
            issue.kind == discovery_issue_kind::duplicate ? QCoreApplication::translate("XLibrary", "Duplicate")
            : issue.kind == discovery_issue_kind::skipped ? QCoreApplication::translate("XLibrary", "Skipped")
                                                          : QCoreApplication::translate("XLibrary", "Error");
        details.append(QString("[%1] %2: %3").arg(kind, issue.source, issue.reason));
    }
    return details.join('\n');
}
}

class CatalogWorker : public QObject {
    Q_OBJECT
    QString database_path;
    std::shared_ptr<std::atomic<quint64>> request_sequence;

public:
    CatalogWorker(const QString &database_path, std::shared_ptr<std::atomic<quint64>> request_sequence)
        : database_path(database_path), request_sequence(std::move(request_sequence)) {}
    std::unique_ptr<CatalogStore> store;
    catalog_snapshot catalog;
    QTimer *monitor = nullptr;
    QSet<QString> last_running;
    QSet<QString> last_unavailable;
    QSet<QString> last_missing_volumes;
    QStringList known_volume_roots;
    operation_state monitor_state = operation_state::ready;
    quint64 monitor_request_id = 0;
    quint64 request_id = 0;
    operation_error_source error_source = operation_error_source::sqlite;
    operation_error task_error;
    bool defer_result = false;

    quint64 next_request_id() { return ++*request_sequence; }

    void initialize(quint64 id) {
        try {
            if (monitor) {
                monitor->stop();
                delete monitor;
                monitor = nullptr;
            }
            if (database_path.isEmpty())
                database_path = path_text(Database::get_database_path());
            emit location_changed(database_path);
            store.reset();
            store = std::make_unique<CatalogStore>(database_path);
            emit schema_changed(store->schema_version());
            refresh();
            monitor = new QTimer(this);
            monitor->setInterval(2500);
            connect(monitor, &QTimer::timeout, this, [this] { poll(); });
            monitor->start();
            emit result({id, "library", operation_state::ready, {}});
        } catch (const std::exception &error) {
            store.reset();
            sqlite3 *raw = nullptr;
            if (sqlite3_open_v2(database_path.toUtf8().constData(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
                try {
                    emit schema_changed(xlibrary::read_schema_version(raw));
                } catch (...) {
                    // Повреждённая БД не должна скрывать исходную ошибку открытия.
                }
            }
            if (raw)
                sqlite3_close_v2(raw);
            emit result({id, "library", operation_state::error,
                         exception_error("library", error, operation_error_source::sqlite)});
        }
    }
    void restore(quint64 id, const QString &path) {
        try {
            if (database_path.isEmpty())
                database_path = path_text(Database::get_database_path());
            store.reset();
            const auto preserved = CatalogStore::restore_backup(path, database_path);
            initialize(id);
            if (store)
                emit notification(preserved.isEmpty()
                                      ? QCoreApplication::translate("XLibrary", "Library restored.")
                                      : QCoreApplication::translate("XLibrary", "Library restored. Previous files: %1")
                                            .arg(preserved));
        } catch (const std::exception &error) {
            emit result({id, "library", operation_state::error,
                         exception_error("library", error, operation_error_source::filesystem)});
        }
    }
    void refresh() {
        catalog = store->read();
        for (auto &record : catalog.applications) {
            record.running = last_running.contains(normalized_path(record.path));
        }
        emit loaded(catalog);
        poll(true);
    }
    void poll(bool force = false) {
        QSet<QString> unavailable;
        QSet<QString> missing_volumes;
        const auto previous_roots = known_volume_roots;
        bool capabilities_changed = false;
        auto paths = last_running;
        try {
            for (auto &record : catalog.applications) {
                const auto error = desktop_platform::new_instance_error(record);
                if (error != record.new_instance_error) {
                    record.new_instance_error = error;
                    capabilities_changed = true;
                }
                record.available = QFileInfo(record.path).isFile();
                record.volume_unavailable = false;
                if (record.available) {
                    const QStorageInfo volume(record.path);
                    const auto root = QDir::fromNativeSeparators(volume.rootPath());
                    if (volume.isValid() && !root.isEmpty() && root != "/" && !known_volume_roots.contains(root))
                        known_volume_roots.append(root);
                } else {
                    auto roots = known_volume_roots;
#ifdef Q_OS_WIN
                    const auto path = QDir::fromNativeSeparators(record.path);
                    if (path.size() >= 3 && path[1] == ':')
                        roots.append(path.left(3));
                    else if (path.startsWith("//")) {
                        const auto end = path.indexOf('/', path.indexOf('/', 2) + 1);
                        if (end > 2)
                            roots.append(path.left(end));
                    }
#endif
                    for (const auto &root : roots) {
                        const auto prefix = root.endsWith('/') ? root : root + '/';
                        if (!record.path.startsWith(prefix, path_case_sensitivity))
                            continue;
                        const QStorageInfo volume(root);
                        if (!volume.isValid() || !volume.isReady() ||
                            normalized_path(volume.rootPath()) != normalized_path(root)) {
                            record.volume_unavailable = true;
                            missing_volumes.insert(normalized_path(record.path));
                            break;
                        }
                    }
                }
                if (!record.available)
                    unavailable.insert(normalized_path(record.path));
            }
            paths = desktop_platform::running_paths(catalog.applications);
            if (monitor_state == operation_state::error)
                emit result({monitor_request_id, "monitor", operation_state::ready, {}});
            monitor_state = operation_state::ready;
        } catch (const std::exception &error) {
            if (monitor_state != operation_state::error) {
                monitor_request_id = ++*request_sequence;
                emit result({monitor_request_id, "monitor", operation_state::error,
                             exception_error("monitor", error, operation_error_source::platform)});
            }
            monitor_state = operation_state::error;
            return;
        }
        for (auto &record : catalog.applications)
            record.running = paths.contains(normalized_path(record.path));
        if (known_volume_roots != previous_roots)
            emit volumes_observed(known_volume_roots);
        if (force || capabilities_changed || paths != last_running || unavailable != last_unavailable ||
            missing_volumes != last_missing_volumes) {
            last_running = paths;
            last_unavailable = unavailable;
            last_missing_volumes = missing_volumes;
            emit state_changed(catalog.applications);
        }
    }

signals:
    void loaded(const catalog_snapshot &catalog);
    void preview_ready(quint64 request_id, const QVector<app_record> &records, const QVector<discovery_issue> &issues);
    void undo_changed(bool available);
    void volumes_observed(const QStringList &roots);
    void location_changed(const QString &path);
    void schema_changed(int version);
    void profile_inspected(quint64 request_id, const QString &path, const profile_manifest &manifest);
    void paths_inspected(quint64 request_id, const QVector<path_relocation> &changes);
    void profile_replaced(const QString &recovery_path, bool previous_autostart);
    void state_changed(const QVector<app_record> &applications);
    void result(const operation_result &result);
    void resolved(quint64 request_id, const QString &requested_path, const app_record &record);
    void application_saved(quint64 request_id, qint64 id);
    void scenario_saved(quint64 request_id, qint64 id);
    void tag_renamed(const QString &old_name, const QString &new_name);
    void launched(qint64 id, const operation_result &result, quint64 scenario);
    void notification(const QString &message);
    void discovery_report_ready(const QString &report);
};

DesktopService::DesktopService(QObject *parent, const QString &profile_path, const QString &database_path)
    : QObject(parent), worker(new CatalogWorker(database_path, request_sequence)), model(this), filter(this),
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
    connect(worker, &CatalogWorker::volumes_observed, this, [this](const QStringList &roots) {
        if (!profile_busy() && !restart_pending)
            settings.setValue("availability/volume_roots", roots);
    });
    connect(this, &DesktopService::library_open_requested, worker, &CatalogWorker::initialize, Qt::QueuedConnection);
    connect(this, &DesktopService::library_restore_requested, worker, &CatalogWorker::restore, Qt::QueuedConnection);
    connect(&worker_thread, &QThread::started, worker,
            [worker = worker, id = library_request_id] { worker->initialize(id); });
    connect(&worker_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &CatalogWorker::location_changed, this, [this](const QString &path) { library_file = path; });
    connect(worker, &CatalogWorker::schema_changed, this, [this](int version) { current_schema_version = version; });
    connect(worker, &CatalogWorker::profile_inspected, this, &DesktopService::profile_preview_ready);
    connect(worker, &CatalogWorker::paths_inspected, this, &DesktopService::paths_preview_ready);
    connect(worker, &CatalogWorker::profile_replaced, this, [this](const QString &recovery, bool previous_autostart) {
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
    connect(worker, &CatalogWorker::result, this, &DesktopService::publish_result);
    connect(worker, &CatalogWorker::notification, this, &DesktopService::message);
    connect(worker, &CatalogWorker::discovery_report_ready, this, [this](const QString &report) {
        scan_report = report;
        emit discovery_report_changed();
    });
    connect(worker, &CatalogWorker::preview_ready, this,
            [this](quint64 id, const QVector<app_record> &records, const QVector<discovery_issue> &issues) {
                if (id != discovery_request_id)
                    return;
                if (discovery_cancelled->requested.load()) {
                    publish_result({id, preview_operation, operation_state::cancelled, {}});
                    return;
                }
                present_catalog_preview(records, issues);
            });
    connect(worker, &CatalogWorker::undo_changed, this, [this](bool available) {
        undo_available = available;
        emit undo_removal_changed();
    });
    connect(worker, &CatalogWorker::resolved, this, &DesktopService::application_resolved);
    connect(worker, &CatalogWorker::application_saved, this, &DesktopService::application_saved);
    connect(worker, &CatalogWorker::scenario_saved, this, &DesktopService::scenario_saved);
    connect(worker, &CatalogWorker::tag_renamed, this, [this](const QString &old_name, const QString &new_name) {
        if (old_name.isEmpty())
            return;
        filter.set_query(search_query::rewrite_tag(filter.text(), old_name, new_name));
        if (current_tag.compare(old_name, Qt::CaseInsensitive) == 0)
            navigate(current_section, new_name);
    });
    connect(worker, &CatalogWorker::loaded, this, [this](const catalog_snapshot &value) {
        snapshot = value;
        if (!current_tag.isEmpty() && !snapshot.tags.contains(current_tag))
            navigate(current_section);
        model.replace(value.applications);
        if (!model.find(selected_id))
            selected_id = 0;
        selected_ids.removeIf([this](qint64 id) { return !model.find(id); });
        emit catalog_changed();
        emit selection_changed();
        emit selections_changed();
    });
    connect(worker, &CatalogWorker::state_changed, this, [this](const QVector<app_record> &applications) {
        snapshot.applications = applications;
        model.update_state(applications);
        emit selection_changed();
    });
    connect(worker, &CatalogWorker::launched, this,
            [this](qint64 id, const operation_result &result, quint64 from_scenario) {
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
        "folder", [path = settings.fileName()](CatalogWorker &) { desktop_platform::open_folder(path); },
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

quint64 DesktopService::enqueue(const QString &operation, std::function<void(CatalogWorker &)> task,
                                operation_error_source source, quint64 request_id) {
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
            worker->request_id = id;
            worker->error_source = source;
            worker->task_error = {};
            worker->defer_result = false;
            try {
                if (!worker->store && operation != "folder" && operation != "profile_preview" &&
                    operation != "profile_restore" && operation != "diagnostics") {
                    worker->error_source = operation_error_source::library;
                    throw std::runtime_error(
                        QCoreApplication::translate(
                            "XLibrary",
                            "The library is unavailable. Restart the application after resolving the error.")
                            .toStdString());
                }
                task(*worker);
                if (!worker->defer_result)
                    emit worker->result({id, operation, operation_state::completed, {}});
            } catch (const DiscoveryCancelled &) {
                emit worker->result({id, operation, operation_state::cancelled, {}});
            } catch (const std::invalid_argument &error) {
                emit worker->result({id, operation, operation_state::error,
                                     exception_error(operation, error, operation_error_source::validation)});
            } catch (const std::exception &error) {
                emit worker->result({id, operation, operation_state::error,
                                     worker->task_error.source == operation_error_source::none
                                         ? exception_error(operation, error, worker->error_source)
                                         : worker->task_error});
            }
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

void DesktopService::select(qint64 id) {
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
    return enqueue("application", [record = std::move(record)](CatalogWorker &worker) {
        const auto current = std::find_if(worker.catalog.applications.cbegin(), worker.catalog.applications.cend(),
                                          [&](const app_record &item) { return item.id == record.id; });
        if (record.id == 0 || (current != worker.catalog.applications.cend() && current->path != record.path))
            desktop_platform::resolve_application(record.path);
        const auto id = worker.store->save_application(record);
        worker.refresh();
        emit worker.application_saved(worker.request_id, id);
    });
}

quint64 DesktopService::resolve_application(const QString &path) {
    return enqueue(
        "resolve",
        [path](CatalogWorker &worker) {
            emit worker.resolved(worker.request_id, path, desktop_platform::resolve_application(path));
        },
        operation_error_source::platform);
}

void DesktopService::select_applications(const QVector<qint64> &ids) {
    if (selected_ids == ids)
        return;
    selected_ids = ids;
    emit selections_changed();
}

void DesktopService::remove_application(qint64 id) {
    remove_applications({id});
}

void DesktopService::remove_applications(const QVector<qint64> &ids) {
    if (is_scenario_active()) {
        notify(QCoreApplication::translate("XLibrary", "Stop the scenario before removing applications."));
        return;
    }
    enqueue("remove_application", [ids](CatalogWorker &worker) {
        worker.store->remove_applications(ids);
        emit worker.undo_changed(worker.store->can_undo_removal());
        worker.refresh();
    });
}

void DesktopService::undo_removal() {
    if (is_scenario_active()) {
        notify(QCoreApplication::translate("XLibrary", "Stop the scenario before restoring applications."));
        return;
    }
    enqueue("undo_removal", [](CatalogWorker &worker) {
        worker.store->undo_removal();
        emit worker.undo_changed(worker.store->can_undo_removal());
        worker.refresh();
        emit worker.notification(
            QCoreApplication::translate("XLibrary", "Removed applications and scenario links restored."));
    });
}

quint64 DesktopService::update_metadata(const QVector<qint64> &ids, const QStringList &add_tags,
                                        const QStringList &remove_tags, int favorite) {
    return enqueue("metadata", [ids, add_tags, remove_tags, favorite](CatalogWorker &worker) {
        worker.store->update_metadata(ids, add_tags, remove_tags, favorite);
        worker.refresh();
    });
}

quint64 DesktopService::save_filter(saved_filter record) {
    return enqueue("filter", [record = std::move(record)](CatalogWorker &worker) {
        worker.store->save_filter(record);
        worker.refresh();
    });
}

void DesktopService::remove_filter(qint64 id) {
    enqueue("remove_filter", [id](CatalogWorker &worker) {
        worker.store->remove_filter(id);
        worker.refresh();
    });
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
    return enqueue("tag", [old_name, new_name](CatalogWorker &worker) {
        worker.store->save_tag(old_name, new_name);
        emit worker.tag_renamed(old_name, new_name);
        worker.refresh();
    });
}

void DesktopService::remove_tag(const QString &name) {
    enqueue("remove_tag", [name](CatalogWorker &worker) {
        worker.store->remove_tag(name);
        emit worker.tag_renamed(name, {});
        worker.refresh();
    });
}

quint64 DesktopService::save_scenario(scenario_record record) {
    return enqueue("scenario", [record = std::move(record)](CatalogWorker &worker) {
        const auto id = worker.store->save_scenario(record);
        worker.refresh();
        emit worker.scenario_saved(worker.request_id, id);
    });
}

void DesktopService::remove_scenario(qint64 id) {
    enqueue("remove_scenario", [id](CatalogWorker &worker) {
        worker.store->remove_scenario(id);
        worker.refresh();
    });
}

void DesktopService::launch(qint64 id, bool new_instance, bool scenario) {
    request_launch(id, new_instance, scenario);
}

quint64 DesktopService::request_launch(qint64 id, bool new_instance, bool scenario) {
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
        [id, mode, generation, token = scenario_generation](CatalogWorker &worker) {
            try {
                if (generation && token->load() != generation) {
                    throw DiscoveryCancelled();
                }
                const auto it = std::find_if(worker.catalog.applications.cbegin(), worker.catalog.applications.cend(),
                                             [id](const auto &record) { return record.id == id; });
                if (it == worker.catalog.applications.cend())
                    throw std::invalid_argument(
                        QCoreApplication::translate("XLibrary", "The application was not found in the library.")
                            .toStdString());
                desktop_platform::launch_application(*it, mode);
                // Ошибка истории не должна превращать успешный запуск в неуспешный или повторный.
                emit worker.launched(id, {worker.request_id, "launch", operation_state::completed, {}}, generation);
                try {
                    worker.store->record_launch(id);
                    worker.refresh();
                } catch (const std::exception &error) {
                    const auto reason = QString::fromUtf8(error.what());
                    emit worker.result(
                        {worker.next_request_id(),
                         "history",
                         operation_state::error,
                         {QCoreApplication::translate(
                              "XLibrary", "The application opened, but its launch history could not be saved: %1")
                              .arg(reason),
                          reason, operation_error_source::sqlite}});
                }
            } catch (const DiscoveryCancelled &) {
                emit worker.launched(id, {worker.request_id, "launch", operation_state::cancelled, {}}, generation);
                throw;
            } catch (const std::invalid_argument &error) {
                emit worker.launched(id,
                                     {worker.request_id, "launch", operation_state::error,
                                      exception_error("launch", error, operation_error_source::validation)},
                                     generation);
                throw;
            } catch (const std::exception &error) {
                emit worker.launched(id,
                                     {worker.request_id, "launch", operation_state::error,
                                      exception_error("launch", error, operation_error_source::platform)},
                                     generation);
                throw;
            }
        },
        operation_error_source::platform);
}

void DesktopService::open_folder(qint64 id) {
    if (const auto *record = model.find(id))
        enqueue(
            "folder", [path = record->path](CatalogWorker &) { desktop_platform::open_folder(path); },
            operation_error_source::platform);
}

void DesktopService::run_scenario(qint64 id) {
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

void DesktopService::finish_scenario(operation_state state, const operation_error &error) {
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
            if (!exclusions.contains(key))
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

void DesktopService::apply_catalog_preview(const QVector<int> &rows, bool remember_exclusions,
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
        settings.setValue("sources/excluded_configurations", exclusions);
        settings.sync();
    }
    reviewing = false;
    preview_records.clear();
    scan_progress.stage = QCoreApplication::translate("XLibrary", "Adding applications to the library");
    emit discovery_progress_changed();
    if (!is_json_preview())
        policy = import_conflict_policy::skip;
    const auto report_progress = [this, request_id = discovery_request_id, last_percent = -1](qsizetype processed,
                                                                                              qsizetype total) mutable {
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
         report_progress](CatalogWorker &worker) mutable {
            try {
                const auto count =
                    worker.store->import_records(applications, cancelled.get(), report_progress, &issues, policy);
                const auto message = discovery_summary(
                    count.added, issues,
                    QCoreApplication::translate("XLibrary", "Import complete. Updated: %1.").arg(count.updated));
                emit worker.discovery_report_ready(discovery_details(message, issues));
                emit worker.notification(message);
                worker.refresh();
            } catch (const DiscoveryCancelled &) {
                const auto message =
                    QCoreApplication::translate("XLibrary", "Import cancelled; changes were not saved.");
                emit worker.discovery_report_ready(discovery_details(message, issues));
                emit worker.notification(message);
                throw;
            } catch (const std::exception &error) {
                issues.append({"SQLite", QString::fromUtf8(error.what())});
                emit worker.discovery_report_ready(discovery_details(
                    QCoreApplication::translate("XLibrary", "Import failed; changes were not saved."), issues));
                throw;
            }
        },
        operation_error_source::sqlite, discovery_request_id);
}

void DesktopService::preview_files(const QStringList &paths) {
    if (paths.isEmpty() || !begin_catalog_preview("files"))
        return;
    enqueue(
        "files",
        [paths, cancelled = discovery_cancelled](CatalogWorker &worker) {
            QVector<app_record> records;
            QVector<discovery_issue> issues;
            for (const auto &path : paths) {
                if (cancelled->requested.load())
                    throw DiscoveryCancelled();
                try {
                    const QFileInfo info(path);
#ifdef Q_OS_WIN
                    const auto suffix = info.suffix().toLower();
                    if (suffix != "exe" && suffix != "lnk" && suffix != "bat" && suffix != "cmd")
                        throw std::invalid_argument(
                            QCoreApplication::translate("XLibrary", "Supported files: .exe, .lnk, .bat, .cmd.")
                                .toStdString());
#endif
                    if (!info.isFile())
                        throw std::invalid_argument(
                            QCoreApplication::translate("XLibrary", "The application file is unavailable.")
                                .toStdString());
                    auto record = desktop_platform::resolve_application(path);
                    CatalogStore::validate_application(record);
                    records.append(std::move(record));
                } catch (const std::exception &error) {
                    issues.append({path, QString::fromUtf8(error.what())});
                }
            }
            worker.defer_result = true;
            emit worker.preview_ready(worker.request_id, records, issues);
        },
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
    watcher->setFuture(
        QtConcurrent::run(&discovery_pool, [registry, start_menu, cancelled = discovery_cancelled, report_progress] {
            QVector<discovery_issue> issues;
            auto applications =
                desktop_platform::discover(registry, start_menu, issues, cancelled->requested, report_progress);
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
    enqueue(
        "export",
        [path](CatalogWorker &worker) {
            const auto too_large = [] {
                return std::invalid_argument(
                    QCoreApplication::translate(
                        "XLibrary", "The JSON catalog exceeds 32 MiB or 50000 applications. Export was not saved. "
                                    "Use a database backup for a larger library.")
                        .toStdString());
            };
            if (worker.catalog.applications.size() > catalog_limits::applications)
                throw too_large();
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Could not open the export file.").toStdString());
            qint64 written = 0;
            const auto write = [&](const QByteArray &contents) {
                if (contents.size() > catalog_limits::json_bytes - written)
                    throw too_large();
                if (file.write(contents) != contents.size())
                    throw std::runtime_error(
                        QCoreApplication::translate("XLibrary", "Could not save the export.").toStdString());
                written += contents.size();
            };
            write("{\"format\":1,\"applications\":[");
            bool first = true;
            for (const auto &record : worker.catalog.applications) {
                try {
                    CatalogStore::validate_application(record);
                } catch (const std::invalid_argument &error) {
                    throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Cannot export %1: %2")
                                                    .arg(record.path, QString::fromUtf8(error.what()))
                                                    .toStdString());
                }
                const auto contents =
                    QJsonDocument(QJsonObject{{"name", record.name},
                                              {"path", record.path},
                                              {"arguments", record.arguments},
                                              {"working_directory", record.working_directory},
                                              {"favorite", record.favorite},
                                              {"tags", QJsonArray::fromStringList(record.tags)},
                                              {"aliases", QJsonArray::fromStringList(record.aliases)}})
                        .toJson(QJsonDocument::Compact);
                if (!first)
                    write(",");
                first = false;
                write(contents);
            }
            write("]}\n");
            if (!file.commit())
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Could not save the export.").toStdString());
            emit worker.notification(QCoreApplication::translate("XLibrary", "Library exported."));
        },
        operation_error_source::filesystem);
}

void DesktopService::import_catalog(const QString &path) {
    if (!begin_catalog_preview("import"))
        return;
    enqueue(
        "import",
        [path](CatalogWorker &worker) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Could not read the import file.").toStdString());
            const auto contents = file.read(catalog_limits::json_bytes + 1);
            if (file.error() != QFileDevice::NoError)
                throw std::runtime_error(file.errorString().toStdString());
            worker.error_source = operation_error_source::validation;
            if (contents.size() > catalog_limits::json_bytes)
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "The JSON catalog exceeds 32 MiB. Import cancelled.")
                        .toStdString());
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(contents, &error);
            if (error.error != QJsonParseError::NoError)
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "JSON error at offset %1: %2. Import cancelled.")
                        .arg(error.offset)
                        .arg(error.errorString())
                        .toStdString());
            const auto root = document.object();
            if (!document.isObject())
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Field root: expected a JSON object. Import cancelled.")
                        .toStdString());
            if (!root.value("format").isDouble() || root.value("format").toDouble() != 1)
                throw std::runtime_error(QCoreApplication::translate(
                                             "XLibrary", "Field format: only version 1 is supported. Import cancelled.")
                                             .toStdString());
            if (!root.value("applications").isArray())
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Field applications: expected an array. Import cancelled.")
                        .toStdString());
            const auto applications = root.value("applications").toArray();
            if (applications.size() > catalog_limits::applications)
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary",
                                                "The JSON catalog exceeds 50000 applications. Import cancelled.")
                        .toStdString());
            QVector<app_record> records;
            for (const auto &value : applications) {
                const auto fail = [&](const QString &field, const QString &reason) {
                    throw std::runtime_error(
                        QCoreApplication::translate(
                            "XLibrary", "Record %1, field %2: %3. Import cancelled; changes were not saved.")
                            .arg(records.size() + 1)
                            .arg(field, reason)
                            .toStdString());
                };
                if (!value.isObject())
                    fail("applications[]", QCoreApplication::translate("XLibrary", "expected an object"));
                const auto object = value.toObject();
                const auto text = [&](const QString &field, bool required = false) {
                    const auto item = object.value(field);
                    if ((!item.isUndefined() || required) && !item.isString())
                        fail(field, QCoreApplication::translate("XLibrary", "expected a string"));
                    const auto result = item.toString();
                    if (result.contains(QChar::Null))
                        fail(field, QCoreApplication::translate("XLibrary", "null characters are not allowed"));
                    return result;
                };
                app_record record;
                record.name = text("name", true).trimmed();
                record.path = QDir::fromNativeSeparators(text("path", true));
                record.arguments = text("arguments");
                record.working_directory = text("working_directory");
                if (object.contains("favorite") && !object.value("favorite").isBool())
                    fail("favorite", QCoreApplication::translate("XLibrary", "expected a boolean"));
                record.favorite = object.value("favorite").toBool();
                if (object.contains("tags") && !object.value("tags").isArray())
                    fail("tags", QCoreApplication::translate("XLibrary", "expected an array of strings"));
                const auto tags = object.value("tags").toArray();
                if (tags.size() > catalog_limits::tags_per_application)
                    fail("tags", QCoreApplication::translate("XLibrary", "at most 256 tags are allowed"));
                qsizetype tag_index = 0;
                for (const auto &tag : tags) {
                    const auto field = QString("tags[%1]").arg(++tag_index);
                    if (!tag.isString())
                        fail(field, QCoreApplication::translate("XLibrary", "expected a string"));
                    const auto name = tag.toString().trimmed();
                    if (!valid_tag_name(name))
                        fail(field,
                             QCoreApplication::translate("XLibrary", "a tag must contain 1 to 64 characters, without "
                                                                     "spaces, commas, #, or null characters"));
                    record.tags.append(name);
                }
                record.tags.removeDuplicates();
                if (object.contains("aliases") && !object.value("aliases").isArray())
                    fail("aliases", QCoreApplication::translate("XLibrary", "expected an array of strings"));
                const auto aliases = object.value("aliases").toArray();
                if (aliases.size() > 32)
                    fail("aliases",
                         QCoreApplication::translate(
                             "XLibrary", "Use up to 32 aliases, each on one line with up to 256 characters."));
                for (const auto &alias : aliases) {
                    if (!alias.isString() || alias.toString().trimmed().isEmpty() || alias.toString().size() > 256 ||
                        alias.toString().contains(QChar::Null) || alias.toString().contains('\n') ||
                        alias.toString().contains('\r'))
                        fail("aliases",
                             QCoreApplication::translate(
                                 "XLibrary", "Use up to 32 aliases, each on one line with up to 256 characters."));
                    const auto name = alias.toString().trimmed();
                    if (!record.aliases.contains(name, Qt::CaseInsensitive))
                        record.aliases.append(name);
                }
                if (record.name.isEmpty() || record.name.size() > 256)
                    fail("name", QCoreApplication::translate("XLibrary", "the name must contain 1 to 256 characters"));
                if (!QFileInfo(record.path).isAbsolute())
                    fail("path", QCoreApplication::translate("XLibrary", "expected an absolute path"));
#ifdef Q_OS_WIN
                if (QFileInfo(record.path).suffix().isEmpty())
                    fail("path",
                         QCoreApplication::translate("XLibrary", "a Windows target requires a filename extension"));
#endif
                if (!record.working_directory.isEmpty() && !QFileInfo(record.working_directory).isAbsolute())
                    fail("working_directory",
                         QCoreApplication::translate("XLibrary", "expected an empty string or an absolute path"));
                try {
                    CatalogStore::validate_application(record);
                } catch (const std::invalid_argument &error) {
                    fail("application", QString::fromUtf8(error.what()));
                }
                records.append(std::move(record));
            }
            worker.error_source = operation_error_source::sqlite;
            worker.defer_result = true;
            emit worker.preview_ready(worker.request_id, records, {});
        },
        operation_error_source::filesystem, discovery_request_id);
}

void DesktopService::clear_history() {
    enqueue("history", [](CatalogWorker &worker) {
        worker.store->clear_history();
        worker.refresh();
        emit worker.notification(QCoreApplication::translate("XLibrary", "Launch history cleared."));
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
        [path, database_path = library_file, settings_path = settings.fileName(), settings_ok](CatalogWorker &worker) {
            if (!settings_ok)
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Could not save settings before creating a backup.")
                        .toStdString());
            ProfileBackup::create(path, database_path, settings_path);
            emit worker.notification(
                QCoreApplication::translate("XLibrary", "Full profile backup saved: %1").arg(path));
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
        "profile_preview",
        [path](CatalogWorker &worker) {
            emit worker.profile_inspected(worker.request_id, path, ProfileBackup::inspect(path));
        },
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
         old_autostart = settings.value("behavior/autostart", false).toBool()](CatalogWorker &worker) {
            if (worker.monitor)
                worker.monitor->stop();
            worker.store.reset();
            try {
                if (!settings_ok)
                    throw std::runtime_error(
                        QCoreApplication::translate("XLibrary", "Could not save settings before restoring the profile.")
                            .toStdString());
                const auto recovery = ProfileBackup::restore(path, database_path, settings_path, fingerprint);
                emit worker.profile_replaced(recovery, old_autostart);
                worker.initialize(library_id);
                if (!worker.store)
                    throw std::runtime_error(
                        QCoreApplication::translate(
                            "XLibrary", "The profile was restored but could not be opened. Restart XLibrary.")
                            .toStdString());
            } catch (...) {
                const auto failure = std::current_exception();
                // Восстановление возвращает прежний профиль до повторного открытия соединения.
                try {
                    ProfileBackup::recover(database_path, settings_path);
                    worker.initialize(library_id);
                } catch (const std::exception &recovery_error) {
                    emit worker.result(
                        {library_id, "library", operation_state::error,
                         exception_error("library", recovery_error, operation_error_source::filesystem)});
                }
                std::rethrow_exception(failure);
            }
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
        "diagnostics",
        [path, report](CatalogWorker &worker) {
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) || output.write(report) != report.size() || !output.commit())
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Could not save the diagnostic report.").toStdString());
            emit worker.notification(QCoreApplication::translate("XLibrary", "Diagnostic report saved."));
        },
        operation_error_source::filesystem);
}

quint64 DesktopService::preview_paths(const QString &old_root, const QString &new_root) {
    if (profile_busy() || restart_pending || is_discovering() || is_scenario_active() || !launching.isEmpty()) {
        notify(QCoreApplication::translate(
            "XLibrary", "Wait for the current profile operation or stop the scan and scenario first."));
        return 0;
    }
    return enqueue("profile_paths_preview", [old_root, new_root](CatalogWorker &worker) {
        emit worker.paths_inspected(worker.request_id, worker.store->preview_relocation(old_root, new_root));
    });
}

quint64 DesktopService::recover_paths(const QVector<path_relocation> &changes) {
    if (profile_busy() || restart_pending || is_discovering() || is_scenario_active() || !launching.isEmpty()) {
        notify(QCoreApplication::translate(
            "XLibrary", "Wait for the current profile operation or stop the scan and scenario first."));
        return 0;
    }
    return enqueue("profile_paths_recovery", [changes](CatalogWorker &worker) {
        worker.store->apply_relocation(changes);
        worker.refresh();
        emit worker.notification(
            QCoreApplication::translate("XLibrary", "Application paths recovered: %1.").arg(changes.size()));
    });
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

#include "desktop_service.moc"
