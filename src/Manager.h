#ifndef XLIBRARY_MANAGER_H
#define XLIBRARY_MANAGER_H

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QObject>
#include <QStorageInfo>
#include <QTimer>
#include <vector>

#include "Application.h"
#include "Database.h"
#include "Heartbeat.h"
#include "OperationTypes.h"
#include "PathHelpers.h"
#include "ProfileBackup.h"
#include "ProfileSchema.h"

// Owns the catalog and its worker-thread lifecycle for both the backend API and GUI.
class Manager : public QObject {
    Q_OBJECT
    QString database_path;
    QElapsedTimer availability_age;
    std::unique_ptr<QFileSystemWatcher> availability_watcher;
    bool availability_invalidated = true;
    // Measured filesystem/volume work dominates large catalogs; process polling stays at 2500 ms.
    static constexpr qint64 availability_interval_ms = 10000;
    static constexpr qsizetype availability_watch_limit = 128;
    std::shared_ptr<std::atomic<quint64>> request_sequence = std::make_shared<std::atomic<quint64>>(0);

public:
    Manager(const QString &database_path, std::shared_ptr<std::atomic<quint64>> request_sequence)
        : database_path(database_path), request_sequence(std::move(request_sequence)) {}
    std::unique_ptr<Database> store;
    catalog_snapshot catalog;
    QTimer *monitor = nullptr;
    QSet<QString> last_running;
    QSet<QString> last_unavailable;
    QSet<QString> last_missing_volumes;
    QStringList known_volume_roots;
    QVariantMap verified_locations;
    operation_state monitor_state = operation_state::ready;
    quint64 monitor_request_id = 0;
    quint64 request_id = 0;
    operation_error_source error_source = operation_error_source::sqlite;
    operation_error task_error;
    bool defer_result = false;

    quint64 next_request_id() { return ++*request_sequence; }

    void initialize(quint64 id);
    void restore(quint64 id, const QString &path);
    void refresh();
    void poll(bool force = false);

signals:
    void loaded(const catalog_snapshot &catalog);
    void preview_ready(quint64 request_id, const QVector<app_record> &records, const QVector<discovery_issue> &issues);
    void inventory_ready(quint64 request_id, const QVector<app_record> &records, const QVector<app_record> &removed,
                         const QVector<discovery_issue> &issues);
    void undo_changed(bool available);
    void volumes_observed(const QStringList &roots);
    void locations_observed(const QVariantMap &locations);
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
    void discovery_dismissed(const QStringList &targets);

private:
    // Keep Application's shared process handles when the value-based API is used.
    mutable std::vector<Application> applications;
    static std::string to_lower(std::string data);

public:
    void save_application(const app_record &record);
    void resolve_application(const QString &path);
    void remove_applications(const QVector<qint64> &ids, bool remember = true);
    void undo_removal();
    void update_metadata(const QVector<qint64> &ids, const QStringList &add_tags, const QStringList &remove_tags,
                         int favorite);
    void save_filter(const saved_filter &record);
    void remove_filter(qint64 id);
    void save_tag(const QString &old_name, const QString &new_name);
    void remove_tag(const QString &name);
    void save_scenario(const scenario_record &record);
    void remove_scenario(qint64 id);
    void export_catalog(const QString &path);
    void import_catalog(const QString &path);
    void clear_history();
    void preview_profile(const QString &path);
    void preview_paths(const QString &old_root, const QString &new_root);
    void recover_paths(const QVector<path_relocation> &changes);
    void launch_record(qint64 id, Application::launch_mode mode, quint64 generation,
                       const std::shared_ptr<std::atomic<quint64>> &token,
                       const std::function<void(const app_record &, Application::launch_mode)> &launch_action,
                       const QString &profile = {});
    void execute(quint64 id, const QString &operation, const std::function<void(Manager &)> &task,
                 operation_error_source source);
    void preview_files(const QStringList &paths, const std::shared_ptr<discovery_control> &cancelled);
    void review_inventory(const QVector<app_record> &records, const QVector<discovery_issue> &issues,
                          const std::shared_ptr<discovery_control> &cancelled);
    void remove_missing_applications(const QVector<app_record> &records,
                                     const std::shared_ptr<discovery_control> &cancelled);
    void apply_catalog_preview(const QVector<app_record> &applications, QVector<discovery_issue> issues,
                               const std::shared_ptr<discovery_control> &cancelled, import_conflict_policy policy,
                               const std::function<void(qsizetype, qsizetype)> &report_progress, bool quiet = false);
    void backup_profile(const QString &path, const QString &database_path, const QString &settings_path,
                        bool settings_ok);
    void restore_profile(const QString &path, const QString &database_path, const QString &settings_path,
                         const QByteArray &fingerprint, bool settings_ok, quint64 library_id, bool old_autostart);
    void export_diagnostics(const QString &path, const QByteArray &report);
    Manager() {};

    explicit Manager(const std::vector<Application> &applications) { add_applications(applications); }
    explicit Manager(const Application &application) { add_application(application); }

    bool app_exists(const Application &application) const;

    void add_application(const Application &application);
    void add_applications(const std::vector<Application> &applications);

    std::uint32_t launch(const std::string &title) const;
    Heartbeat heartbeat(const std::string &title, std::uint32_t process_id) const;

    std::vector<Application> get_apps() const;
    std::vector<std::string> get_names() const;
    std::vector<std::filesystem::path> get_paths() const;
};

#endif //XLIBRARY_MANAGER_H
