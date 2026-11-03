#ifndef XLIBRARY_DESKTOP_SERVICE_H
#define XLIBRARY_DESKTOP_SERVICE_H

#include <atomic>
#include <functional>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include "library_model.h"
#include "operation_types.h"
#include "profile_backup.h"
#include "update_checker.h"

class Manager;

class DesktopService : public QObject {
    Q_OBJECT
    Q_PROPERTY(qint64 selection READ selection NOTIFY selection_changed)
    Q_PROPERTY(QString section READ section NOTIFY navigation_changed)
    Q_PROPERTY(bool ready READ is_ready NOTIFY catalog_changed)
private:
    std::shared_ptr<std::atomic<quint64>> request_sequence = std::make_shared<std::atomic<quint64>>(0);
    QThread worker_thread;
    Manager *worker;
    QThreadPool discovery_pool;
    QTimer scenario_timer;
    QVector<scenario_step> pending_steps;
    QString scenario_name;
    qsizetype step_index = 0;
    operation_state scenario_status = operation_state::ready;
    quint64 scenario_request_id = 0;
    std::shared_ptr<std::atomic<quint64>> scenario_generation = std::make_shared<std::atomic<quint64>>(0);
    QSet<qint64> launching;
    operation_state discovery_state = operation_state::ready;
    quint64 discovery_request_id = 0;
    discovery_progress scan_progress;
    QString scan_report;
    QVector<app_record> preview_records;
    QVector<discovery_issue> preview_issues;
    QString preview_operation;
    bool reviewing = false;
    bool undo_available = false;
    QVector<qint64> selected_ids;
    std::shared_ptr<discovery_control> discovery_cancelled = std::make_shared<discovery_control>();
    operation_state library_status = operation_state::loading;
    quint64 library_request_id = 0;
    operation_error opening_error;
    operation_error last_failure;
    operation_state profile_status = operation_state::ready;
    quint64 profile_request_id = 0;
    quint64 update_request_id = 0;
    quint64 update_source_request_id = 0;
    int current_schema_version = 0;
    bool restart_pending = false;
    QString library_file;
    qint64 selected_id = 0;
    QString current_section = "library";
    QString current_tag;
    catalog_snapshot snapshot;
    quint64 begin_operation(const QString &operation);
    void publish_result(const operation_result &result);
    quint64 enqueue(const QString &operation, std::function<void(Manager &)> task,
                    operation_error_source source = operation_error_source::sqlite, quint64 request_id = 0);
    void finish_scenario(operation_state state, const operation_error &error = {});
    void next_scenario_step();
    bool begin_catalog_preview(const QString &operation);
    void present_catalog_preview(QVector<app_record> records, QVector<discovery_issue> issues = {});

public:
    LibraryModel model;
    LibraryFilter filter;
    QSettings settings;
    UpdateChecker updates;
    explicit DesktopService(QObject *parent = nullptr, const QString &profile_path = {},
                            const QString &database_path = {});
    ~DesktopService() override;
    operation_state library_state() const { return library_status; }
    const operation_error &last_error() const { return last_failure; }
    bool is_ready() const { return library_status == operation_state::ready; }
    bool is_opening() const { return library_status == operation_state::loading; }
    QString library_error() const { return opening_error.user_message; }
    QString library_path() const { return library_file; }
    int schema_version() const { return current_schema_version; }
    bool profile_busy() const { return profile_status == operation_state::loading; }
    bool restart_required() const { return restart_pending; }
    bool safe_mode() const;
    QString diagnostics_text() const;
    quint64 backup_profile(const QString &path);
    quint64 preview_profile(const QString &path);
    quint64 restore_profile(const QString &path, const QByteArray &fingerprint);
    quint64 export_diagnostics(const QString &path);
    quint64 preview_paths(const QString &old_root, const QString &new_root);
    quint64 recover_paths(const QVector<path_relocation> &changes);
    void restart_application();
    void retry_library();
    void open_profile_folder();
    void restore_library(const QString &path);
    bool is_discovering() const { return discovery_state == operation_state::loading; }
    bool is_cancelling_discovery() const {
        return is_discovering() && discovery_cancelled->phase.load() == discovery_phase::cancelling;
    }
    bool can_cancel_discovery() const {
        return is_discovering() && discovery_cancelled->phase.load() == discovery_phase::running;
    }
    const discovery_progress &discovery_status() const { return scan_progress; }
    QString discovery_report() const { return scan_report; }
    bool is_scenario_active() const { return scenario_status == operation_state::loading; }
    qint64 selection() const { return selected_id; }
    const app_record *selected() const { return model.find(selected_id); }
    const catalog_snapshot &catalog() const { return snapshot; }
    const QVector<qint64> &selections() const { return selected_ids; }
    void select_applications(const QVector<qint64> &ids);
    bool can_undo_removal() const { return undo_available; }
    void remove_applications(const QVector<qint64> &ids);
    void undo_removal();
    quint64 update_metadata(const QVector<qint64> &ids, const QStringList &add_tags, const QStringList &remove_tags,
                            int favorite);
    bool is_reviewing() const { return reviewing; }
    bool is_json_preview() const { return preview_operation == "import"; }
    bool is_scan_preview() const { return preview_operation == "discover"; }
    const QVector<app_record> &catalog_preview() const { return preview_records; }
    QString preview_details() const;
    void apply_catalog_preview(const QVector<int> &rows, bool remember_exclusions,
                               import_conflict_policy policy = import_conflict_policy::skip);
    void preview_files(const QStringList &paths);
    void clear_discovery_exclusions();
    QString section() const { return current_section; }
    QString tag() const { return current_tag; }
    Q_INVOKABLE void select(qint64 id);
    Q_INVOKABLE void navigate(const QString &section, const QString &tag = {});
    Q_INVOKABLE quint64 save_application(app_record record);
    Q_INVOKABLE quint64 resolve_application(const QString &path);
    Q_INVOKABLE void remove_application(qint64 id);
    Q_INVOKABLE quint64 save_tag(const QString &old_name, const QString &new_name);
    Q_INVOKABLE void remove_tag(const QString &name);
    Q_INVOKABLE quint64 save_scenario(scenario_record record);
    Q_INVOKABLE void remove_scenario(qint64 id);
    Q_INVOKABLE void run_scenario(qint64 id);
    Q_INVOKABLE void cancel_scenario();
    Q_INVOKABLE void discover();
    Q_INVOKABLE void cancel_discovery();
    Q_INVOKABLE void export_catalog(const QString &path);
    Q_INVOKABLE void import_catalog(const QString &path);
    Q_INVOKABLE void launch(qint64 id, bool new_instance = false, bool scenario = false);
    quint64 request_launch(qint64 id, bool new_instance = false, bool scenario = false);
    Q_INVOKABLE void open_folder(qint64 id);
    quint64 save_filter(saved_filter record);
    void remove_filter(qint64 id);
    void apply_filter(const saved_filter &record);
    void clear_filters();
    void clear_history();
    void notify(const QString &message);

signals:
    void library_open_requested(quint64 request_id);
    void library_restore_requested(quint64 request_id, const QString &path);
    void catalog_changed();
    void selection_changed();
    void selections_changed();
    void undo_removal_changed();
    void catalog_preview_ready();
    void navigation_changed();
    void message(const QString &text);
    void operation_changed(const operation_result &result);
    void operation_finished(const QString &operation, bool success);
    void application_saved(quint64 request_id, qint64 id);
    void application_resolved(quint64 request_id, const QString &requested_path, const app_record &record);
    void scenario_saved(quint64 request_id, qint64 id);
    void discovery_changed();
    void discovery_progress_changed();
    void discovery_report_changed();
    void launch_succeeded();
    void application_launch_finished(qint64 id, const operation_result &result);
    void scenario_changed(const QString &status);
    void appearance_changed();
    void profile_state_changed();
    void profile_preview_ready(quint64 request_id, const QString &path, const profile_manifest &manifest);
    void paths_preview_ready(quint64 request_id, const QVector<path_relocation> &changes);
};

#endif
