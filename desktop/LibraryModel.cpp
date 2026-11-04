#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>

#include "appearance.h"
#include "LibraryModel.h"
#include "localization.h"
#include "PathHelpers.h"
#include "PlatformIntegration.h"

namespace {
QString icon_cache_key(const app_record &record) {
    if (!record.icon_path.isEmpty() && QFileInfo(record.icon_path).isAbsolute())
        return "image:" + Application::target_kind(record) + ':' + record.icon_path;
    if (const auto path = Application::process_path(record); QFileInfo(path).isAbsolute())
        return "file:" + path;
    return "kind:" + Application::target_kind(record);
}
}

LibraryModel::LibraryModel(QObject *const parent) : QAbstractTableModel(parent) {
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

QIcon LibraryModel::fallback_icon(const app_record &record) const {
    const auto kind = Application::target_kind(record);
    const auto key = "kind:" + kind;
    if (const auto *icon = icons.object(key))
        return *icon;
    const auto icon = kind == "steam"        ? QIcon::fromTheme("steam", appearance::icon("library"))
                      : kind == "system_uri" ? QIcon::fromTheme("preferences-system", appearance::icon("settings"))
                      : kind == "aumid" || Application::file_target(record) ? appearance::icon("app")
                                                                            : appearance::icon("external");
    icons.insert(key, new QIcon(icon));
    return icon;
}

void LibraryModel::request_icon(const app_record &record) const {
    const auto key = icon_cache_key(record);
    if (key.startsWith("kind:") || pending_icons.contains(key) || pending_icons.size() >= 32) {
        return;
    }
    const auto image_file = key.startsWith("image:");
    const auto path = image_file ? record.icon_path : Application::process_path(record);
    const auto fallback = fallback_icon(record);
    pending_icons.insert(key);
    auto *self = const_cast<LibraryModel *>(this);
#ifdef Q_OS_LINUX
    // System icon engines use QPixmap and must run on the GUI thread.
    if (!image_file) {
        QTimer::singleShot(0, self, [self, key, path, fallback] {
            const auto image = desktop_platform::application_icon(path);
            self->pending_icons.remove(key);
            self->icons.insert(key, new QIcon(image.isNull() ? fallback : QIcon(QPixmap::fromImage(image))));
            for (int row = 0; row < self->records.size(); ++row) {
                if (icon_cache_key(self->records[row]) == key)
                    emit self->dataChanged(self->index(row, 0), self->index(row, 0), {Qt::DecorationRole});
            }
        });
        return;
    }
#endif
    auto *watcher = new QFutureWatcher<QImage>(self);
    connect(watcher, &QFutureWatcher<QImage>::finished, self, [self, watcher, key, fallback] {
        const auto image = watcher->result();
        self->pending_icons.remove(key);
        self->icons.insert(key, new QIcon(image.isNull() ? fallback : QIcon(QPixmap::fromImage(image))));
        watcher->deleteLater();
        for (int row = 0; row < self->records.size(); ++row) {
            if (icon_cache_key(self->records[row]) == key) {
                emit self->dataChanged(self->index(row, 0), self->index(row, 0), {Qt::DecorationRole});
            }
        }
    });
    watcher->setFuture(QtConcurrent::run(&self->icon_pool, [path, image_file] {
        return image_file ? QImage(path) : desktop_platform::application_icon(path);
    }));
}

QVariant LibraryModel::data(const QModelIndex &index, const int requested_role) const {
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
        return record.name + '\n' + QDir::toNativeSeparators(Application::application_path(record));
    case Qt::DecorationRole:
        if (index.column() == 0) {
            if (const auto *icon = icons.object(icon_cache_key(record)))
                return *icon;
            request_icon(record);
            return fallback_icon(record);
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

QVariant LibraryModel::headerData(const int section, const Qt::Orientation orientation,
                                  const int requested_role) const {
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
    searchable_records.clear();
    search_tags.clear();
    name_sort_keys.clear();
    tag_sort_keys.clear();
    searchable_records.reserve(records.size());
    search_tags.reserve(records.size());
    name_sort_keys.reserve(records.size());
    tag_sort_keys.reserve(records.size());
    const QCollator collator;
    for (const auto &record : records) {
        const auto local_path = Application::process_path(record);
        const auto file_name = local_path.isEmpty() ? record.path : QFileInfo(local_path).fileName();
        searchable_records.append(SearchQuery::prepare(record.name, file_name, record.tags, record.aliases));
        search_tags.append(record.tags.join(", ").toCaseFolded());
        name_sort_keys.append(collator.sortKey(searchable_records.back().names.front().text));
        tag_sort_keys.append(collator.sortKey(search_tags.back()));
    }
    endResetModel();
}

int LibraryModel::compare_names(const int left_row, const int right_row) const {
    return name_sort_keys[left_row].compare(name_sort_keys[right_row]);
}

int LibraryModel::compare_tags(const int left_row, const int right_row) const {
    return tag_sort_keys[left_row].compare(tag_sort_keys[right_row]);
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
    int first_changed = -1;
    int last_changed = -1;
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
            if (first_changed < 0)
                first_changed = row;
            last_changed = row;
        }
    }
    // Let the proxy update ordering once, after the entire state snapshot has been applied.
    if (first_changed >= 0)
        emit dataChanged(index(first_changed, 0), index(last_changed, 2),
                         {Qt::DisplayRole, running_role, available_role, record_role});
}

const app_record *LibraryModel::find(const qint64 id) const {
    for (const auto &record : records) {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

LibraryFilter::LibraryFilter(QObject *const parent) : QSortFilterProxyModel(parent) {
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

void LibraryFilter::setSourceModel(QAbstractItemModel *const model) {
    disconnect(reset_connection);
    disconnect(state_connection);
    query_ranks.clear();
    if (model) {
        reset_connection =
            connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { query_ranks.clear(); });
        state_connection =
            connect(model, &QAbstractItemModel::dataChanged, this,
                    [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
                        if (section == "running" && (roles.isEmpty() || roles.contains(LibraryModel::running_role))) {
                            query_ranks.clear();
                            invalidate();
                        }
                    });
    }
    QSortFilterProxyModel::setSourceModel(model);
}

int LibraryFilter::query_rank(const int row) const {
    const auto *model = static_cast<const LibraryModel *>(sourceModel());
    if (query_ranks.size() != model->rowCount()) {
        query_ranks.fill(-1, model->rowCount());
        auto best_rank = -1;
        for (int index = 0; index < model->rowCount(); ++index) {
            const auto &record = model->at(index);
            if ((section == "favorites" && !record.favorite) || (section == "running" && !record.running) ||
                (!selected_tag.isEmpty() && !has_tag(record.tags, selected_tag)))
                continue;
            const auto rank = query.rank(model->search_data(index));
            query_ranks[index] = rank;
            if (rank >= 0 && (best_rank < 0 || rank < best_rank))
                best_rank = rank;
        }
        for (auto &rank : query_ranks)
            if (!SearchQuery::accepts_rank(rank, best_rank))
                rank = -1;
    }
    return query_ranks[row];
}

void LibraryFilter::set_section(const QString &value, const QString &tag) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    section = value;
    selected_tag = tag;
    query_ranks.clear();
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
}

void LibraryFilter::set_sort_mode(const int value, const Qt::SortOrder order) {
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

void LibraryFilter::sort(const int column, const Qt::SortOrder order) {
    if (column >= 0 && column < 3)
        set_sort_mode(column == 1 ? 3 : column == 2 ? 4 : 0, order);
    else
        QSortFilterProxyModel::sort(column, order);
}

bool LibraryFilter::filterAcceptsRow(const int row, const QModelIndex &parent) const {
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
    // The proxy reverses comparisons for descending order; keep favorites pinned within each relevance rank.
    if (a.favorite != b.favorite)
        return sortOrder() == Qt::AscendingOrder ? a.favorite : b.favorite;
    if (sort_mode == 3) {
        const auto &a_tags = model->tag_names(left.row());
        const auto &b_tags = model->tag_names(right.row());
        if (a_tags.isEmpty() != b_tags.isEmpty())
            return !a_tags.isEmpty();
        const auto comparison = model->compare_tags(left.row(), right.row());
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
    const auto comparison = model->compare_names(left.row(), right.row());
    return comparison == 0 ? a.id < b.id : comparison < 0;
}
