#pragma once

#include <memory>
#include <QRect>

#include "WorkspaceTypes.h"

struct workspace_monitor {
    QString id;
    QRect work_area;
    bool primary = false;
};

struct workspace_window {
    quintptr handle = 0;
    qint64 pid = 0;
    quint64 process_birth = 0;
    QString executable;
    QString title;
    QRect frame;
    bool maximized = false;
    bool minimized = false;
};

struct workspace_desktop {
    bool can_capture = false;
    bool can_place = false;
    QString limitation;
    QVector<workspace_monitor> monitors;
    QVector<workspace_window> windows;
};

class WorkspaceWindowBackend {
public:
    virtual ~WorkspaceWindowBackend() = default;
    virtual workspace_desktop snapshot() = 0;
    virtual bool activate(const workspace_window &window) = 0;
    virtual bool place(const workspace_window &window, const QRect &frame, bool maximized) = 0;
};

// Called on the GUI thread with the actual Qt platform name. All backend
// methods are called serially from the workspace background executor.
std::unique_ptr<WorkspaceWindowBackend> make_workspace_window_backend(const QString &qt_platform);

bool same_workspace_window(const workspace_window &left, const workspace_window &right);
