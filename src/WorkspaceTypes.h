#pragma once

#include <optional>
#include <QMetaType>
#include <QRectF>
#include <QString>
#include <QVector>

struct sqlite3;

enum class workspace_item_kind { application, file, folder, url };

struct workspace_placement {
    QString monitor_id;
    QRectF geometry;
    bool maximized = false;
    bool operator==(const workspace_placement &) const = default;
};

struct workspace_item {
    qint64 id = 0;
    workspace_item_kind kind = workspace_item_kind::application;
    qint64 application_id = 0;
    QString target;
    QString display_name;
    QString launch_profile;
    bool enabled = true;
    bool reuse_existing = true;
    std::optional<workspace_placement> placement;
    bool operator==(const workspace_item &) const = default;
};

struct workspace_record {
    qint64 id = 0;
    QString name;
    QVector<workspace_item> items;
    bool operator==(const workspace_record &) const = default;
};

namespace xlibrary {
inline constexpr qsizetype workspace_item_limit = 32;
QString workspace_name_key(const QString &name);
QString workspace_item_issue(const workspace_item &item);
QString workspace_record_issue(const workspace_record &record);
QVector<workspace_record> read_workspaces(sqlite3 *connection);
}

Q_DECLARE_METATYPE(workspace_record)
