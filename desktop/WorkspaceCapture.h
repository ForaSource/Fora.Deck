#pragma once

#include "CatalogTypes.h"
#include "WorkspaceWindowBackend.h"

struct workspace_capture_candidate {
    workspace_window window;
    std::optional<workspace_item> item;
    QVector<workspace_item> launch_options;
    std::optional<workspace_placement> window_placement;
    QString issue;
};

QString workspace_executable(const app_record &app);
bool workspace_can_reuse(const app_record &app, const QString &profile);
QVector<workspace_window> workspace_matching_windows(const workspace_desktop &desktop, const QString &executable);
QVector<workspace_capture_candidate> workspace_capture(const workspace_desktop &desktop,
                                                       const QVector<app_record> &applications);
std::optional<workspace_placement> workspace_capture_placement(const workspace_window &window,
                                                               const QVector<workspace_monitor> &monitors);
const workspace_monitor *workspace_find_monitor(const QString &id, const QVector<workspace_monitor> &monitors);
QRect workspace_restore_frame(const workspace_placement &placement, const QRect &area);
