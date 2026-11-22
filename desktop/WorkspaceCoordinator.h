#pragma once

#include <functional>
#include <QElapsedTimer>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include "OperationTypes.h"
#include "WorkspaceCapture.h"

enum class workspace_step_state {
    pending,
    disabled,
    checking,
    launching,
    waiting,
    positioning,
    opened,
    ready,
    reused,
    positioned,
    unverified,
    skipped,
    failed,
    cancelled
};

enum class workspace_outcome { none, completed, partial, failed, cancelled };

struct workspace_step_report {
    QString label;
    workspace_step_state state = workspace_step_state::pending;
    QString detail;
    bool reused = false;
    bool opened = false;
    bool placement_requested = false;
    bool placement_attempted = false;
    bool placement_verified = false;
    qint64 duration_ms = 0;
};

class WorkspaceCoordinator : public QObject {
    Q_OBJECT
public:
    using launch_token = std::shared_ptr<std::atomic<quint64>>;
    using app_launcher =
        std::function<quint64(const app_record &, const QString &, bool, quint64, const launch_token &)>;
    using resource_launcher = std::function<bool(const QUrl &)>;

    WorkspaceCoordinator(std::unique_ptr<WorkspaceWindowBackend> backend, app_launcher launch_app,
                         resource_launcher open_resource, int timeout_ms = 12000, QObject *parent = nullptr);
    ~WorkspaceCoordinator() override;

    void check_capabilities();
    quint64 capture(QVector<app_record> applications);
    quint64 prepare(workspace_record workspace, QVector<app_record> applications);
    bool start(quint64 id);
    void cancel();
    void launch_finished(const operation_result &result, qint64 process_id = 0);

    bool active() const { return phase != Phase::idle; }
    bool capturing() const { return capture_id != 0; }
    bool awaiting_confirmation() const { return phase == Phase::confirm; }
    quint64 run_id() const { return generation; }
    const QString &name() const { return run_name; }
    const auto &report() const { return steps; }
    const auto &desktop() const { return capabilities; }
    workspace_outcome outcome() const { return final_outcome; }
    qint64 duration_ms() const { return total_duration; }

signals:
    void capabilities_changed();
    void capture_ready(quint64 id, const QVector<workspace_capture_candidate> &candidates);
    void preflight_ready(quint64 id, const QStringList &warnings);
    void changed();

private:
    enum class Phase { idle, preflight, confirm, running };
    struct Entry {
        workspace_item item;
        std::optional<app_record> app;
        QString executable;
        QString issue;
        bool can_reuse = false;
        bool ambiguous_configuration = false;
    };

    std::shared_ptr<WorkspaceWindowBackend> backend;
    app_launcher launch_app;
    resource_launcher open_resource;
    QThreadPool executor;
    QTimer launch_timer;
    QElapsedTimer step_clock;
    QElapsedTimer run_clock;
    qint64 total_duration = 0;
    workspace_desktop capabilities;
    Phase phase = Phase::idle;
    workspace_outcome final_outcome = workspace_outcome::none;
    QString run_name;
    quint64 generation = 0;
    quint64 capture_id = 0;
    quint64 next_capture = 0;
    quint64 launch_request = 0;
    qint64 launched_pid = 0;
    const int timeout_ms;
    qsizetype index = 0;
    QVector<Entry> entries;
    QVector<workspace_step_report> steps;
    QVector<workspace_window> previous_windows;
    QRect target_frame;
    workspace_window target_window;
    QString placement_monitor;
    QString placement_note;
    qint64 placement_started = 0;
    qint64 readiness_started = 0;
    launch_token run_token;
    launch_token request_token;
    bool probing = false;

    template <typename Work, typename Finish> void background(Work work, Finish finish);
    void set_capabilities(workspace_desktop desktop);
    static Entry preflight(workspace_item item, const QVector<app_record> &applications);
    void next();
    void resolve_app(workspace_desktop desktop);
    void wait_for_window();
    void use_window(const workspace_window &window, const workspace_desktop &desktop, bool reuse);
    void verify_placement();
    void complete_step(workspace_step_state state, const QString &detail);
    void finish(workspace_outcome outcome);
    void update_step(workspace_step_state state, const QString &detail);
    bool current(quint64 id, qsizetype step) const;
};

QString workspace_step_text(workspace_step_state state);
QString workspace_outcome_text(workspace_outcome outcome);
QString workspace_item_label(const workspace_item &item, const QVector<app_record> &applications);
