#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <QUrl>

#include "Application.h"
#include "SqliteHelpers.h"
#include "WorkspaceTypes.h"

namespace {
bool valid_text(const QString &text, const qsizetype limit) {
    return text.size() <= limit && std::none_of(text.cbegin(), text.cend(), [](const QChar character) {
               return character.category() == QChar::Other_Control;
           });
}

bool valid_geometry(const QRectF &rect) {
    return rect.isValid() && rect.left() >= 0 && rect.top() >= 0 && rect.right() <= 1.000001 &&
           rect.bottom() <= 1.000001;
}
}

QString xlibrary::workspace_name_key(const QString &name) {
    return name.trimmed().normalized(QString::NormalizationForm_C).toCaseFolded();
}

QString xlibrary::workspace_item_issue(const workspace_item &item) {
    if (item.id < 0 || !valid_text(item.display_name, 256) || !valid_text(item.target, 4096) ||
        !valid_text(item.launch_profile, 1024) || !Application::valid_launch_profile_id(item.launch_profile))
        return QCoreApplication::translate("Workspaces", "Invalid resource details.");
    if (item.kind == workspace_item_kind::application) {
        if (item.application_id < 0 || !item.target.isEmpty())
            return QCoreApplication::translate("Workspaces", "Choose an application from the library.");
    } else if (item.application_id || item.placement || !item.launch_profile.isEmpty()) {
        return QCoreApplication::translate("Workspaces", "Application options require a library application.");
    }
    if (item.placement && (!valid_text(item.placement->monitor_id, 256) || !valid_geometry(item.placement->geometry)))
        return QCoreApplication::translate("Workspaces", "Invalid window placement.");

    switch (item.kind) {
    case workspace_item_kind::application:
        return {};
    case workspace_item_kind::file:
    case workspace_item_kind::folder:
        if (!QDir::isAbsolutePath(item.target) || item.target.startsWith(':'))
            return QCoreApplication::translate("Workspaces", "Enter an absolute file or folder path.");
        return {};
    case workspace_item_kind::url: {
        const QUrl url(item.target, QUrl::StrictMode);
        if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty() ||
            !url.userInfo().isEmpty())
            return QCoreApplication::translate("Workspaces", "Enter an HTTP or HTTPS URL without credentials.");
        return {};
    }
    }
    return QCoreApplication::translate("Workspaces", "Unknown resource type.");
}

QString xlibrary::workspace_record_issue(const workspace_record &record) {
    if (record.id < 0 || record.name.trimmed().isEmpty() || !valid_text(record.name, 128))
        return QCoreApplication::translate("Workspaces", "Enter a workspace name of up to 128 characters.");
    if (record.items.isEmpty() || record.items.size() > workspace_item_limit)
        return QCoreApplication::translate("Workspaces", "Add between 1 and %1 resources.").arg(workspace_item_limit);
    QSet<qint64> ids;
    for (const auto &item : record.items) {
        if (item.id && ids.contains(item.id))
            return QCoreApplication::translate("Workspaces", "A resource ID occurs more than once.");
        if (item.id)
            ids.insert(item.id);
        if (const auto issue = workspace_item_issue(item); !issue.isEmpty())
            return issue;
    }
    return {};
}

QVector<workspace_record> xlibrary::read_workspaces(sqlite3 *const connection) {
    QVector<workspace_record> records;
    SqliteStatement rows(connection, "SELECT id,name,name_key,created_at,updated_at FROM workspace ORDER BY name_key;");
    SqliteStatement columns(connection, R"(
        SELECT i.id,i.position,i.kind,i.application_id,i.target,i.display_name,i.launch_profile,i.enabled,i.reuse_existing,
               w.item_id,w.monitor_id,w.x,w.y,w.width,w.height,w.maximized
        FROM workspace_item i LEFT JOIN workspace_window w ON w.item_id=i.id LIMIT 0;
    )");
    columns.next();
    while (rows.next()) {
        workspace_record record{rows.number(0), rows.qtext(1), {}};
        SqliteStatement items(connection, R"(
            SELECT i.id,i.position,i.kind,i.application_id,i.target,i.display_name,i.launch_profile,i.enabled,i.reuse_existing,
                   w.item_id,w.monitor_id,w.x,w.y,w.width,w.height,w.maximized
            FROM workspace_item i LEFT JOIN workspace_window w ON w.item_id=i.id
            WHERE i.workspace_id=? ORDER BY i.position;
        )");
        items.bind(1, record.id);
        while (items.next()) {
            if (items.number(0) <= 0 || items.number(1) != record.items.size() ||
                record.items.size() >= workspace_item_limit || items.number(2) < 0 || items.number(2) > 3 ||
                (items.number(7) != 0 && items.number(7) != 1) || (items.number(8) != 0 && items.number(8) != 1) ||
                (items.number(15) != 0 && items.number(15) != 1))
                throw std::runtime_error("Profile contains an invalid workspace item");
            workspace_item item{items.number(0),
                                static_cast<workspace_item_kind>(items.number(2)),
                                items.number(3),
                                items.qtext(4),
                                items.qtext(5),
                                items.qtext(6),
                                items.number(7) != 0,
                                items.number(8) != 0,
                                {}};
            if (items.number(9))
                item.placement = workspace_placement{items.qtext(10),
                                                     {items.real(11), items.real(12), items.real(13), items.real(14)},
                                                     items.number(15) != 0};
            record.items.append(std::move(item));
        }
        if (record.id <= 0 || rows.qtext(2) != workspace_name_key(record.name) ||
            !workspace_record_issue(record).isEmpty())
            throw std::runtime_error("Profile contains an invalid workspace");
        records.append(std::move(record));
    }
    return records;
}
