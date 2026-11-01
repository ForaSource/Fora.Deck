#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>

#include "library_model.h"
#include "localization.h"
#include "path_helpers.h"
#include "platform_integration.h"

LibraryModel::LibraryModel(QObject *parent) : QAbstractTableModel(parent) {
    icon_pool.setMaxThreadCount(2);
    icon_pool.setExpiryTimeout(5000);
}

LibraryModel::~LibraryModel() {
    icon_pool.clear();
    icon_pool.waitForDone();
}

int LibraryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(records.size());
}

int LibraryModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : 3;
}

void LibraryModel::request_icon(const QString &path) const {
    if (pending_icons.contains(path) || pending_icons.size() >= 32 || path.isEmpty()) {
        return;
    }
    pending_icons.insert(path);
    auto *self = const_cast<LibraryModel *>(this);
#ifdef Q_OS_LINUX
    // System icon engines use QPixmap and must run on the GUI thread.
    QTimer::singleShot(0, self, [self, path] {
        const auto image = desktop_platform::application_icon(path);
        self->pending_icons.remove(path);
        self->icons.insert(path, new QIcon(image.isNull() ? QIcon{} : QIcon(QPixmap::fromImage(image))));
        for (int row = 0; row < self->records.size(); ++row) {
            if (self->records[row].path == path)
                emit self->dataChanged(self->index(row, 0), self->index(row, 0), {Qt::DecorationRole});
        }
    });
#else
    auto *watcher = new QFutureWatcher<QImage>(self);
    connect(watcher, &QFutureWatcher<QImage>::finished, self, [self, watcher, path] {
        const auto image = watcher->result();
        self->pending_icons.remove(path);
        self->icons.insert(path, new QIcon(image.isNull() ? QIcon{} : QIcon(QPixmap::fromImage(image))));
        watcher->deleteLater();
        for (int row = 0; row < self->records.size(); ++row) {
            if (self->records[row].path == path) {
                emit self->dataChanged(self->index(row, 0), self->index(row, 0), {Qt::DecorationRole});
            }
        }
    });
    watcher->setFuture(
        QtConcurrent::run(&self->icon_pool, [path] { return desktop_platform::application_icon(path); }));
#endif
}

QVariant LibraryModel::data(const QModelIndex &index, int requested_role) const {
    if (!index.isValid() || index.row() >= records.size() || index.row() < 0) {
        return {};
    }
    const auto &record = records[index.row()];
    switch (requested_role) {
    case Qt::DisplayRole:
        if (index.column() == 0)
            return record.name;
        if (index.column() == 1)
            return record.tags.join(", ");
        return record.running              ? QCoreApplication::translate("XLibrary", "Running")
               : record.available          ? QCoreApplication::translate("XLibrary", "Not running")
               : record.volume_unavailable ? QCoreApplication::translate("XLibrary", "Drive unavailable")
                                           : QCoreApplication::translate("XLibrary", "Unavailable");
    case Qt::ToolTipRole:
        return record.name + '\n' + QDir::toNativeSeparators(record.path);
    case Qt::DecorationRole:
        if (index.column() == 0) {
            if (const auto *icon = icons.object(record.path))
                return *icon;
            request_icon(record.path);
        }
        return {};
    case id_role:
        return record.id;
    case record_role:
        return QVariant::fromValue(record);
    case tags_role:
        return record.tags;
    case running_role:
        return record.running;
    case favorite_role:
        return record.favorite;
    case available_role:
        return record.available;
    default:
        return {};
    }
}

QVariant LibraryModel::headerData(int section, Qt::Orientation orientation, int requested_role) const {
    if (orientation == Qt::Horizontal && requested_role == Qt::DisplayRole) {
        return QStringList{QCoreApplication::translate("XLibrary", "Application"),
                           QCoreApplication::translate("XLibrary", "Tags"),
                           QCoreApplication::translate("XLibrary", "Status")}
            .value(section);
    }
    if (orientation == Qt::Horizontal && requested_role == Qt::ToolTipRole) {
        return QCoreApplication::translate("XLibrary", "Click to sort. Click again to reverse the order.");
    }
    return {};
}

void LibraryModel::replace(QVector<app_record> applications) {
    beginResetModel();
    records = std::move(applications);
    search_names.clear();
    search_paths.clear();
    search_tags.clear();
    search_names.reserve(records.size());
    search_paths.reserve(records.size());
    search_tags.reserve(records.size());
    for (const auto &record : records) {
        search_names.append(record.name.toCaseFolded());
        search_paths.append(QFileInfo(record.path).fileName().toCaseFolded());
        search_tags.append(record.tags.join(", ").toCaseFolded());
    }
    endResetModel();
}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    auto roles = QAbstractTableModel::roleNames();
    roles.insert(id_role, "application_id");
    roles.insert(record_role, "application_record");
    roles.insert(tags_role, "tags");
    roles.insert(running_role, "running");
    roles.insert(favorite_role, "favorite");
    roles.insert(available_role, "available");
    return roles;
}

void LibraryModel::update_state(const QVector<app_record> &applications) {
    QHash<qint64, const app_record *> states;
    for (const auto &record : applications)
        states.insert(record.id, &record);
    for (int row = 0; row < records.size(); ++row) {
        auto &record = records[row];
        const auto *state = states.value(record.id);
        if (state && (record.running != state->running || record.available != state->available ||
                      record.volume_unavailable != state->volume_unavailable ||
                      record.new_instance_error != state->new_instance_error)) {
            record.running = state->running;
            record.available = state->available;
            record.volume_unavailable = state->volume_unavailable;
            record.new_instance_error = state->new_instance_error;
            emit dataChanged(index(row, 0), index(row, 2),
                             {Qt::DisplayRole, running_role, available_role, record_role});
        }
    }
}

const app_record *LibraryModel::find(qint64 id) const {
    for (const auto &record : records) {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

LibraryFilter::LibraryFilter(QObject *parent) : QSortFilterProxyModel(parent) {
    setSortCaseSensitivity(Qt::CaseInsensitive);
    setDynamicSortFilter(true);
    sort(0);
}

void LibraryFilter::set_query(const QString &query) {
    if (query_text == query) {
        emit query_changed(query_text);
        return;
    }
    query_text = query;
    this->query = SearchQuery(query);
    query_ranks.clear();
    // Relevance can change even when the set of accepted rows stays the same.
    invalidate();
    emit query_changed(query_text);
}

void LibraryFilter::setSourceModel(QAbstractItemModel *model) {
    disconnect(reset_connection);
    query_ranks.clear();
    if (model)
        reset_connection =
            connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { query_ranks.clear(); });
    QSortFilterProxyModel::setSourceModel(model);
}

int LibraryFilter::query_rank(int row) const {
    const auto *model = static_cast<const LibraryModel *>(sourceModel());
    if (query_ranks.size() != model->rowCount())
        query_ranks.fill(-2, model->rowCount());
    auto &rank = query_ranks[row];
    if (rank == -2)
        rank =
            query.rank(model->search_name(row), model->search_path(row), model->at(row).tags, model->at(row).aliases);
    return rank;
}

void LibraryFilter::set_section(const QString &value, const QString &tag) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    section = value;
    selected_tag = tag;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
}

void LibraryFilter::set_sort_mode(int value, Qt::SortOrder order) {
    const auto mode = qBound(0, value, 4);
    const bool changed = sort_mode != mode;
    sort_mode = mode;
    const auto column = sort_mode == 3 ? 1 : sort_mode == 4 ? 2 : 0;
    if (sortColumn() != column || sortOrder() != order)
        QSortFilterProxyModel::sort(column, order);
    else if (changed)
        invalidate();
    emit sort_changed(sort_mode, order);
}

void LibraryFilter::sort(int column, Qt::SortOrder order) {
    if (column >= 0 && column < 3)
        set_sort_mode(column == 1 ? 3 : column == 2 ? 4 : 0, order);
    else
        QSortFilterProxyModel::sort(column, order);
}

bool LibraryFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    if (parent.isValid())
        return false;
    const auto *model = static_cast<const LibraryModel *>(sourceModel());
    const auto &record = model->at(row);
    if (section == "favorites" && !record.favorite)
        return false;
    if (section == "running" && !record.running)
        return false;
    if (!selected_tag.isEmpty() && !has_tag(record.tags, selected_tag))
        return false;
    return query_rank(row) >= 0;
}

bool LibraryFilter::lessThan(const QModelIndex &left, const QModelIndex &right) const {
    const auto *model = static_cast<const LibraryModel *>(sourceModel());
    const auto &a = model->at(left.row());
    const auto &b = model->at(right.row());
    if (query.has_text()) {
        const auto a_rank = query_rank(left.row());
        const auto b_rank = query_rank(right.row());
        if (a_rank != b_rank)
            return sortOrder() == Qt::AscendingOrder ? a_rank < b_rank : a_rank > b_rank;
    }
    if (sort_mode == 3) {
        const auto &a_tags = model->tag_names(left.row());
        const auto &b_tags = model->tag_names(right.row());
        if (a_tags.isEmpty() != b_tags.isEmpty())
            return !a_tags.isEmpty();
        const auto comparison = QString::localeAwareCompare(a_tags, b_tags);
        if (comparison != 0)
            return comparison < 0;
    }
    if (sort_mode == 4) {
        const auto a_status = a.running ? 0 : a.available ? 1 : 2;
        const auto b_status = b.running ? 0 : b.available ? 1 : 2;
        if (a_status != b_status)
            return a_status < b_status;
    }
    if (sort_mode == 1 && a.last_launch != b.last_launch)
        return a.last_launch > b.last_launch;
    if (sort_mode == 2 && a.favorite != b.favorite)
        return a.favorite;
    const auto comparison =
        QString::localeAwareCompare(model->search_name(left.row()), model->search_name(right.row()));
    return comparison == 0 ? a.id < b.id : comparison < 0;
}
