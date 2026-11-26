#include <algorithm>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QtConcurrent/QtConcurrentRun>
#include <QUrl>

#include "appearance.h"
#include "DesktopService.h"
#include "editors.h"
#include "PathHelpers.h"
#include "PlatformIntegration.h"
#include "SearchQuery.h"
#include "SearchResults.h"

class SearchResultModel : public QAbstractTableModel {
    bool compact;
    LibraryModel &library;

public:
    QVector<search_entry> entries;
    std::function<void(const search_entry &, bool)> favorite_changed;
    explicit SearchResultModel(const bool compact, LibraryModel &library, QObject *const parent)
        : QAbstractTableModel(parent), compact(compact), library(library) {
        connect(&library, &LibraryModel::icon_ready, this, [this] {
            // Repaint visible rows, including icon requests deferred by the shared queue limit.
            if (!entries.isEmpty())
                emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DecorationRole});
        });
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : compact ? 1 : 5; }
    QVariant headerData(const int section, const Qt::Orientation orientation, const int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        return section == 0   ? QCoreApplication::translate("Fora.Deck", "File")
               : section == 1 ? QCoreApplication::translate("Fora.Deck", "Tags")
               : section == 2 ? QCoreApplication::translate("Fora.Deck", "Extension")
               : section == 3 ? QCoreApplication::translate("Fora.Deck", "Date added")
                              : QCoreApplication::translate("Fora.Deck", "Folder");
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(entries.size());
    }
    QVariant data(const QModelIndex &index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= entries.size())
            return {};
        const auto &entry = entries[index.row()];
        if (role == Qt::DisplayRole) {
            if (index.column() == 1)
                return entry.tags.join(", ");
            if (index.column() == 2)
                return entry.extension;
            if (index.column() == 3)
                return entry.added > 0
                           ? QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(entry.added), QLocale::ShortFormat)
                           : QString::fromUtf8("—");
            if (index.column() == 4)
                return QDir::toNativeSeparators(QFileInfo(entry.path).absolutePath());
            return entry.available
                       ? entry.title
                       : entry.title + " · " + QCoreApplication::translate("Fora.Deck", "Source or file unavailable");
        }
        if (role == LibraryModel::tags_role)
            return entry.tags;
        if (role == LibraryModel::favorite_role)
            return entry.favorite;
        if (role == LibraryModel::available_role)
            return entry.available && entry.type != search_entry::kind::message;
        if (role == Qt::DecorationRole && index.column() == 0) {
            if (!entry.available)
                return appearance::icon("file");
            if (entry.type == search_entry::kind::file || entry.type == search_entry::kind::working_file)
                return library.file_icon(entry.path, entry.modified, entry.size);
            return appearance::icon(entry.type == search_entry::kind::directory ? "folder" : "file");
        }
        if (role == Qt::ToolTipRole)
            return (entry.path.isEmpty() ? entry.title : QDir::toNativeSeparators(entry.path)) +
                   (entry.available ? QString{}
                                    : '\n' + QCoreApplication::translate("Fora.Deck", "Source or file unavailable"));
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= entries.size() ||
            entries[index.row()].type == search_entry::kind::message)
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }
    bool setData(const QModelIndex &index, const QVariant &value, const int role) override {
        if (role != LibraryModel::favorite_role || !index.isValid() || index.row() < 0 ||
            index.row() >= entries.size() || !favorite_changed)
            return false;
        favorite_changed(entries[index.row()], value.toBool());
        return true;
    }
    void replace(QVector<search_entry> value) {
        library.retry_file_icons();
        beginResetModel();
        entries = std::move(value);
        endResetModel();
    }
};

namespace {
class SearchDelegate : public QStyledItemDelegate {
    DesktopService &service;
    bool compact;
    mutable QFont badge_font;
    mutable int badge_text_width = 0;

    int shortcut_width(const QFont &font) const {
        if (badge_text_width == 0 || badge_font != font) {
            badge_font = font;
            badge_text_width = 0;
            const QFontMetrics metrics(font);
            for (int digit = 0; digit < 10; ++digit)
                badge_text_width =
                    qMax(badge_text_width,
                         metrics.horizontalAdvance(
                             QKeySequence(Qt::ALT | (Qt::Key_0 + digit)).toString(QKeySequence::NativeText)));
        }
        return badge_text_width;
    }

public:
    SearchDelegate(DesktopService &service, const bool compact, QObject *const parent)
        : QStyledItemDelegate(parent), service(service), compact(compact) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return {0, appearance::scaled(service.settings, 60)};
    }
    void paint(QPainter *const painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto *model = static_cast<const SearchResultModel *>(index.model());
        const auto &entry = model->entries[index.row()];
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const auto area = option.rect.adjusted(2, 2, -2, -2);
        if (option.state & QStyle::State_Selected)
            appearance::paint_selection(*painter, area, option.palette, appearance::scaled(service.settings, 8),
                                        option.state & QStyle::State_HasFocus);
        else if (option.state & QStyle::State_MouseOver) {
            painter->setBrush(option.palette.alternateBase());
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(area, appearance::scaled(service.settings, 8),
                                     appearance::scaled(service.settings, 8));
        }
        const auto icon_size = appearance::scaled(service.settings, 32);
        const QRect icon_rect(area.left() + 14, area.center().y() - icon_size / 2, icon_size, icon_size);
        const auto *icon_name =
            entry.type == search_entry::kind::application                                                    ? "app"
            : (entry.type == search_entry::kind::directory || entry.type == search_entry::kind::open_folder) ? "folder"
            : entry.type == search_entry::kind::calculation ? "calculator"
                                                            : "file";
        auto icon = appearance::icon(icon_name, option.palette.color(QPalette::PlaceholderText));
        if (entry.type == search_entry::kind::application) {
            // Reuse the model's asynchronous icon cache; painting never touches the disk.
            const auto row = service.model.row_for_id(entry.application_id);
            if (row >= 0) {
                const auto cached = service.model.data(service.model.index(row, 0), Qt::DecorationRole).value<QIcon>();
                if (!cached.isNull())
                    icon = cached;
            }
        } else if (entry.type == search_entry::kind::file || entry.type == search_entry::kind::working_file) {
            icon = index.data(Qt::DecorationRole).value<QIcon>();
        }
        if (entry.type != search_entry::kind::message)
            icon.paint(painter, icon_rect);
        painter->setFont(option.font);
        painter->setPen(option.palette.color(entry.type == search_entry::kind::message ? QPalette::PlaceholderText
                                                                                       : QPalette::Text));
        auto text_rect = area.adjusted(icon_size + 28, 0, -14, 0);
        if (compact && index.row() < 10 && entry.type != search_entry::kind::message) {
            const auto shortcut = QKeySequence(Qt::ALT | (index.row() == 9 ? Qt::Key_0 : Qt::Key_1 + index.row()))
                                      .toString(QKeySequence::NativeText);
            auto shortcut_font = option.font;
            shortcut_font.setPixelSize(appearance::scaled(service.settings, 12));
            const QFontMetrics metrics(shortcut_font);
            const auto padding = appearance::scaled(service.settings, 7);
            const auto height = metrics.height() + padding;
            const auto width = shortcut_width(shortcut_font) + 2 * padding;
            const QRect shortcut_rect(text_rect.right() - width + 1, text_rect.center().y() - height / 2, width,
                                      height);
            const auto badge_palette = QApplication::palette();
            painter->setPen(Qt::NoPen);
            painter->setBrush(badge_palette.button());
            painter->drawRoundedRect(shortcut_rect, 5, 5);
            painter->setFont(shortcut_font);
            painter->setPen(badge_palette.color(QPalette::PlaceholderText));
            painter->drawText(shortcut_rect, Qt::AlignCenter, shortcut);
            text_rect.setRight(shortcut_rect.left() - appearance::scaled(service.settings, 16));
            painter->setFont(option.font);
            painter->setPen(option.palette.color(QPalette::Text));
        }
        const auto path = QDir::toNativeSeparators(entry.path);
        if (!compact && !path.isEmpty() && path != entry.title) {
            const auto title_rect = text_rect.adjusted(0, 0, 0, -text_rect.height() / 2);
            painter->drawText(
                title_rect, Qt::AlignBottom | Qt::AlignLeft,
                option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, title_rect.width()));
            auto detail_font = option.font;
            detail_font.setPixelSize(appearance::scaled(service.settings, 12));
            painter->setFont(detail_font);
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            const auto path_rect = text_rect.adjusted(0, text_rect.height() / 2 + 2, 0, 0);
            painter->drawText(path_rect, Qt::AlignTop | Qt::AlignLeft,
                              QFontMetrics(detail_font).elidedText(path, Qt::ElideMiddle, path_rect.width()));
        } else {
            painter->drawText(
                text_rect, Qt::AlignVCenter | Qt::AlignLeft,
                option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, text_rect.width()));
        }
        painter->restore();
    }
};

}

SearchResults::SearchResults(DesktopService &service, QLineEdit &input, const bool compact, QWidget *const parent,
                             const bool file_workspace)
    : QTreeView(parent), service(service), input(input), compact(compact), file_workspace(file_workspace),
      results(new SearchResultModel(!file_workspace, service.model, this)) {
    setObjectName("search_results");
    file_delay.setSingleShot(true);
    file_delay.setInterval(100);
    connect(&file_delay, &QTimer::timeout, this, &SearchResults::update_files);
    setAccessibleName(QCoreApplication::translate("Fora.Deck", "Search results"));
    setModel(results);
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, &SearchResults::sync_file_selection);
    results->favorite_changed = [this](const search_entry &entry, const bool favorite) {
        if (!isEnabled() || this->service.profile_busy() || this->service.restart_required() ||
            (entry.type != search_entry::kind::file && entry.type != search_entry::kind::working_file))
            return;
        this->service.files->update_metadata(entry.path, this->service.files->tags_for(entry.path), favorite);
    };
    setItemDelegate(new SearchDelegate(service, compact, this));
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setAllColumnsShowFocus(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setHeaderHidden(!file_workspace);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    if (file_workspace) {
        for (int column = 1; column < 5; ++column)
            header()->setSectionResizeMode(column, QHeaderView::Interactive);
        header()->resizeSection(1, appearance::scaled(service.settings, 220));
        header()->resizeSection(2, appearance::scaled(service.settings, 260));
        header()->setSectionsClickable(true);
        connect(header(), &QHeaderView::sectionClicked, this, [this](const int column) {
            if (column < 4)
                emit sort_requested(column == 0 ? 1 : column == 1 ? 4 : column == 2 ? 5 : 6);
        });
    }
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(compact ? Qt::NoFocus : Qt::StrongFocus);
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    input.installEventFilter(this);
    connect(&input, &QLineEdit::textChanged, this, [this] { update_query(true); });
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (this->compact)
            activate_current();
        else if (index.column() == 0 &&
                 viewport()->mapFromGlobal(QCursor::pos()).x() < appearance::scaled(this->service.settings, 36))
            toggle_favorite();
    });
    connect(this, &QTreeView::activated, this, [this] {
        if (!this->compact)
            activate_current();
    });
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        const auto index = indexAt(position);
        if (index.isValid()) {
            setCurrentIndex(index);
            show_actions();
        }
    });
    connect(&service.model, &QAbstractItemModel::dataChanged, viewport(), qOverload<>(&QWidget::update));
    connect(&service, &DesktopService::catalog_changed, this, [this] {
        if (files_enabled)
            return;
        if (special) {
            update_query();
            return;
        }
        application_entries.clear();
        if (this->compact)
            update_applications();
        update_entries();
    });
    connect(&service, &DesktopService::profile_state_changed, this,
            [this] { setEnabled(!this->service.profile_busy() && !this->service.restart_required()); });
    connect(&service, &DesktopService::appearance_changed, this, [this] { update_query(); });
    connect(&service, &DesktopService::application_launch_finished, this,
            [this](qint64, const operation_result &result) {
                if (pending_launch && pending_launch == result.id) {
                    pending_launch = 0;
                    if (result.state == operation_state::completed)
                        emit application_activated();
                }
            });
    connect(&path_watcher, &QFutureWatcher<QVector<search_entry>>::finished, this, [this] {
        path_busy = false;
        if (active_generation == generation->load() && special && search_query::is_path(this->input.text())) {
            auto entries = path_watcher.result();
            for (auto &entry : entries) {
                entry.tags = this->service.files->tags_for(entry.path);
                entry.favorite = this->service.files->is_favorite(entry.path);
            }
            replace_entries(std::move(entries));
        }
        if (!pending_path.isEmpty())
            read_path();
    });
    connect(service.files, &FileManager::changed, this, [this] {
        ++*generation;
        file_matches.clear();
        rendered_file_query.reset();
        QTimer::singleShot(0, this, [this] { update_query(); });
    });
    const auto configure_files = [this] {
        const bool enabled = this->compact ? this->service.searching_files()
                                           : this->file_workspace && this->service.file_search_active();
        if (this->compact && files_enabled == enabled)
            return;
        files_enabled = enabled;
        update_query();
    };
    connect(&service, &DesktopService::navigation_changed, this, configure_files);
    files_enabled = compact ? service.searching_files() : file_workspace && service.file_search_active();
    update_query();
}

SearchResults::~SearchResults() {
    file_delay.stop();
    ++*generation;
}

void SearchResults::sync_file_selection(const QModelIndex &index) {
    if (!file_workspace || !service.file_search_active())
        return;
    const auto row = index.row();
    const auto *entry = row >= 0 && row < results->entries.size() ? &results->entries[row] : nullptr;
    service.select_file(
        entry && (entry->type == search_entry::kind::file || entry->type == search_entry::kind::working_file)
            ? entry->path
            : QString{},
        entry ? entry->volume_root : QString{}, entry ? entry->volume_device : QString{});
}

void SearchResults::replace_entries(QVector<search_entry> entries) {
    if (entries.isEmpty() && (special || files_enabled || (compact && !input.text().trimmed().isEmpty()))) {
        const auto hint = special ? QString{} : SearchQuery(input.text()).tag_hint(service.catalog().tags);
        entries.append({search_entry::kind::message,
                        hint.isEmpty() ? QCoreApplication::translate("Fora.Deck", "No results") : hint,
                        {},
                        0});
    }
    const bool preserve_position =
        property("rendered_query").isValid() && property("rendered_query").toString() == input.text();
    auto *visible_view = active_view();
    const auto top_index = visible_view->indexAt(QPoint(0, 0));
    const auto top_entry = top_index.isValid() ? results->entries[top_index.row()] : search_entry{};
    const auto top_offset = top_index.isValid() ? visible_view->visualRect(top_index).top() : 0;
    const auto scroll_position = visible_view->verticalScrollBar()->value();
    const auto previous = currentIndex().isValid() ? results->entries[currentIndex().row()] : search_entry{};
    const auto same_entry = [](const search_entry &a, const search_entry &b) {
        return a.type == b.type && a.path == b.path && a.application_id == b.application_id;
    };
    results->replace(std::move(entries));
    int selected = 0;
    if (preserve_position && !previous.path.isEmpty())
        for (qsizetype i = 0; i < results->entries.size(); ++i)
            if (same_entry(results->entries[i], previous)) {
                selected = static_cast<int>(i);
                break;
            }
    setCurrentIndex(results->index(selected, 0));
    const int visible_rows = compact ? qBound(3, service.settings.value("search/overlay_rows", 5).toInt(), 10) : 5;
    const auto rows = qMin(results->rowCount(), visible_rows);
    if (!compact && files_enabled) {
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setVisible(true);
    } else {
        const auto preferred_height = rows * appearance::scaled(service.settings, 60) + (rows ? 4 : 0);
        setProperty("preferred_height", preferred_height);
        setFixedHeight(preferred_height);
        setVisible(rows > 0);
    }
    if (preserve_position) {
        visible_view->doItemsLayout();
        visible_view->verticalScrollBar()->setValue(scroll_position);
        if (top_index.isValid())
            for (int row = 0; row < results->rowCount(); ++row)
                if (same_entry(results->entries[row], top_entry)) {
                    visible_view->scrollTo(results->index(row, 0), QAbstractItemView::PositionAtTop);
                    visible_view->verticalScrollBar()->setValue(visible_view->verticalScrollBar()->value() -
                                                                top_offset);
                    break;
                }
    }
    setProperty("rendered_query", input.text());
    emit height_changed();
}

void SearchResults::update_applications() {
    const SearchQuery query(input.text());
    QVector<QPair<int, int>> best;
    const auto better = [this](const auto &left, const auto &right) {
        if (left.second != right.second)
            return left.second < right.second;
        const auto &a = service.model.at(left.first);
        const auto &b = service.model.at(right.first);
        if (a.favorite != b.favorite)
            return a.favorite;
        if (a.launch_count != b.launch_count)
            return a.launch_count > b.launch_count;
        if (a.last_launch != b.last_launch)
            return a.last_launch > b.last_launch;
        const auto comparison =
            QString::localeAwareCompare(service.model.search_name(left.first), service.model.search_name(right.first));
        return comparison == 0 ? service.model.at(left.first).id < service.model.at(right.first).id : comparison < 0;
    };
    // Retain every match so results beyond the visible rows remain scrollable.
    for (int row = 0; row < service.model.rowCount(); ++row) {
        const auto &record = service.model.at(row);
        if (!compact && ((service.section() == "favorites" && !record.favorite) ||
                         (service.section() == "running" && !record.running) ||
                         !std::all_of(service.tags().cbegin(), service.tags().cend(),
                                      [&](const QString &tag) { return has_tag(record.tags, tag); })))
            continue;
        if (input.text().trimmed().isEmpty() && !record.favorite && !record.launch_count)
            continue;
        const auto rank = query.rank(service.model.search_data(row));
        if (rank < 0)
            continue;
        best.append({row, rank});
    }
    std::sort(best.begin(), best.end(), better);
    QVector<search_entry> entries;
    for (const auto &candidate : best) {
        if (!SearchQuery::accepts_rank(candidate.second, best.front().second))
            break;
        const auto &record = service.model.at(candidate.first);
        auto title = record.name;
        if (!record.default_launch_profile.isEmpty()) {
            const auto mode = record.default_launch_profile == "private"
                                  ? QCoreApplication::translate("Fora.Deck", "Private mode")
                              : record.default_launch_profile == "normal"
                                  ? QCoreApplication::translate("Fora.Deck", "Normal mode")
                                  : QCoreApplication::translate("Fora.Deck", "Profile: %1")
                                        .arg(QFileInfo(record.default_launch_profile.mid(8)).fileName());
            title += " · " + mode;
        }
        entries.append({search_entry::kind::application, title, record.path, record.id, record.favorite,
                        candidate.second, record.launch_count, record.last_launch, 0, 0});
    }
    application_entries = entries;
}

void SearchResults::update_query(const bool debounce_files) {
    if (action_menu)
        action_menu->close();
    pending_launch = 0;
    file_delay.stop();
    if (property("rendered_query").toString() != input.text())
        setCurrentIndex({});
    ++*generation;
    pending_path.clear();
    if (!compact && ((file_workspace && !files_enabled) || (!file_workspace && service.file_search_active()))) {
        special = false;
        if (file_workspace)
            setCurrentIndex({});
        else
            replace_entries({});
        return;
    }
    const auto text = input.text().trimmed();
    special = files_enabled && search_query::is_path(text);
    if (special) {
        rendered_file_query.reset();
        file_matches.clear();
        pending_path = search_query::expanded_path(text);
        replace_entries({{search_entry::kind::message, QCoreApplication::translate("Fora.Deck", "…"), {}, 0}});
        if (!path_busy)
            read_path();
        return;
    }
    const auto calculation = search_query::calculate(text);
    // A numeric filename mask such as "2026*" is also an incomplete calculation.
    special = calculation.recognized &&
              !(files_enabled && !calculation.value && search_query::file_pattern(text).has_value());
    if (special) {
        rendered_file_query.reset();
        file_matches.clear();
        if (calculation.value) {
            const auto number = QString::number(*calculation.value, 'g', 15);
            replace_entries({{search_entry::kind::calculation, "= " + number, number, 0}});
        } else
            replace_entries({{search_entry::kind::message, QCoreApplication::translate("Fora.Deck", "—"), {}, 0}});
    } else {
        if (!files_enabled)
            file_matches.clear();
        application_entries.clear();
        if (compact && !files_enabled)
            update_applications();
        if (file_workspace && rendered_file_query == current_file_query()) {
            if (!currentIndex().isValid())
                setCurrentIndex(results->index(0, 0));
            else
                sync_file_selection(currentIndex());
        } else {
            rendered_file_query.reset();
            if (file_workspace && !debounce_files) {
                file_matches.clear();
                replace_entries({{search_entry::kind::message, QCoreApplication::translate("Fora.Deck", "…"), {}, 0}});
            } else {
                update_entries();
            }
        }
        if (files_enabled && debounce_files)
            file_delay.start();
        else if (files_enabled)
            update_files();
    }
}

SearchResults::file_query SearchResults::current_file_query() const {
    const int limit =
        compact ? 30
                : qBound(1, service.settings.value("files/result_limit", FileManager::default_result_limit).toInt(),
                         FileManager::max_result_limit);
    return {input.text(), compact ? QStringList{} : service.tags(), !compact && service.section() == "favorites", limit,
            sort_mode};
}

void SearchResults::update_files() {
    const auto expected = generation->load();
    const auto query = current_file_query();
    service.files->search(
        query.text, expected, this,
        [this, query](quint64 request, QVector<file_match> matches) {
            if (request != generation->load() || !files_enabled)
                return;
            if (file_workspace)
                rendered_file_query = query;
            file_matches = std::move(matches);
            update_entries(false);
        },
        generation, service.catalog().file_usage, query.tags, query.favorites_only, query.limit, query.sort_mode);
}

void SearchResults::update_entries(const bool refine_files) {
    if (!files_enabled) {
        replace_entries(compact ? application_entries : QVector<search_entry>{});
        return;
    }
    // Refine the last completed file search while its replacement is still pending.
    const auto matches = refine_files ? FileManager::filter_matches(input.text(), file_matches) : file_matches;
    if (matches.isEmpty()) {
        replace_entries({});
        return;
    }
    QVector<search_entry> entries;
    entries.reserve(matches.size());
    for (const auto &match : matches) {
        if (refine_files && !compact &&
            ((service.section() == "favorites" && !match.favorite) ||
             !std::all_of(service.tags().cbegin(), service.tags().cend(),
                          [&](const auto &tag) { return has_tag(match.tags, tag); })))
            continue;
        entries.append({search_entry::kind::working_file, match.name, match.path, 0, match.saved, match.rank,
                        match.use_count, match.last_used, match.location, match.depth});
        entries.last().tags = match.tags;
        entries.last().favorite = match.favorite;
        entries.last().modified = match.modified;
        entries.last().size = match.size;
        entries.last().extension = match.extension;
        entries.last().added = match.added;
        entries.last().available = match.available;
        entries.last().volume_root = match.volume_root;
        entries.last().volume_device = match.volume_device;
    }
    if (!file_workspace || entries != results->entries)
        replace_entries(std::move(entries));
}

void SearchResults::read_path() {
    path_busy = true;
    active_path = std::move(pending_path);
    pending_path.clear();
    active_generation = generation->load();
    // Only one disk request is active per field; newer input replaces the pending request.
    service.files->directory_dates(
        active_path, this, [this, expected = active_generation](QHash<QString, qint64> dates) {
            if (expected != generation->load()) {
                path_busy = false;
                if (!pending_path.isEmpty())
                    read_path();
                return;
            }
            path_watcher.setFuture(QtConcurrent::run(search_results::directory_entries, active_path, generation,
                                                     active_generation, service.catalog().file_usage, QSet<QString>{},
                                                     true, service.files->metadata_values(),
                                                     compact ? QStringList{} : service.tags(),
                                                     !compact && service.section() == "favorites", dates, sort_mode));
        });
}

void SearchResults::activate_current() {
    if (!isEnabled() || service.profile_busy() || service.restart_required())
        return;
    const auto row = currentIndex().row();
    if (row < 0 || row >= results->entries.size())
        return;
    const auto entry = results->entries[row];
    if (entry.type == search_entry::kind::open_folder) {
        if (desktop_platform::open_url(QUrl::fromLocalFile(entry.path))) {
            emit application_activated();
        } else
            service.notify(QCoreApplication::translate("Fora.Deck", "Could not open the folder."));
    } else if (entry.type == search_entry::kind::directory) {
        input.setText(QDir::toNativeSeparators(entry.path + '/'));
        input.setFocus();
    } else if (entry.type == search_entry::kind::application) {
        pending_launch = service.request_launch(entry.application_id);
    } else if ((entry.type == search_entry::kind::file || entry.type == search_entry::kind::working_file)) {
        if (!entry.available || (entry.type == search_entry::kind::working_file &&
                                 !FileManager::can_open(entry.path, entry.volume_root, entry.volume_device))) {
            service.notify(
                QCoreApplication::translate("Fora.Deck", "The file is unavailable or its type is excluded."));
            update_query();
            return;
        }
        if (desktop_platform::open_url(QUrl::fromLocalFile(entry.path))) {
            service.record_file_use(entry.path);
            emit application_activated();
        } else
            service.notify(QCoreApplication::translate("Fora.Deck", "Could not open the file."));
    } else if (entry.type == search_entry::kind::calculation)
        QApplication::clipboard()->setText(entry.path);
}

void SearchResults::show_actions() {
    const auto row = currentIndex().row();
    if (row >= 0 && row < results->entries.size() &&
        (results->entries[row].type == search_entry::kind::file ||
         results->entries[row].type == search_entry::kind::working_file) &&
        isEnabled() && !service.profile_busy() && !service.restart_required()) {
        const auto entry = results->entries[row];
        if (action_menu)
            action_menu->close();
        auto *menu = new QMenu(this);
        action_menu = menu;
        auto *open = menu->addAction(QCoreApplication::translate("Fora.Deck", "Open"));
        open->setEnabled(entry.available);
        connect(open, &QAction::triggered, this, [this, entry] {
            if (!entry.available || (entry.type == search_entry::kind::working_file &&
                                     !FileManager::can_open(entry.path, entry.volume_root, entry.volume_device))) {
                service.notify(
                    QCoreApplication::translate("Fora.Deck", "The file is unavailable or its type is excluded."));
                return;
            }
            if (desktop_platform::open_url(QUrl::fromLocalFile(entry.path))) {
                service.record_file_use(entry.path);
                emit application_activated();
            } else
                service.notify(QCoreApplication::translate("Fora.Deck", "Could not open the file."));
        });
        connect(menu->addAction(QCoreApplication::translate("Fora.Deck", "Open folder")), &QAction::triggered, this,
                [entry] { desktop_platform::open_url(QUrl::fromLocalFile(QFileInfo(entry.path).absolutePath())); });
        connect(menu->addAction(QCoreApplication::translate("Fora.Deck", "Copy path")), &QAction::triggered, this,
                [entry] { QApplication::clipboard()->setText(QDir::toNativeSeparators(entry.path)); });
        const bool favorite = service.files->is_favorite(entry.path);
        connect(menu->addAction(
                    QCoreApplication::translate("Fora.Deck", favorite ? "Remove from favorites" : "Add to favorites")),
                &QAction::triggered, this, [this, entry, favorite] {
                    service.files->update_metadata(entry.path, service.files->tags_for(entry.path), !favorite);
                });
        connect(menu->addAction(QCoreApplication::translate("Fora.Deck", "Edit tags")), &QAction::triggered, this,
                [this, entry] {
                    bool accepted = false;
                    const auto text = QInputDialog::getText(
                        this, QCoreApplication::translate("Fora.Deck", "Edit tags"),
                        QCoreApplication::translate("Fora.Deck", "Tags separated by commas"), QLineEdit::Normal,
                        service.files->tags_for(entry.path).join(", "), &accepted);
                    if (!accepted || service.profile_busy() || service.restart_required())
                        return;
                    QStringList tags;
                    for (auto tag : text.split(',', Qt::SkipEmptyParts)) {
                        tag = tag.trimmed();
                        if (tag.startsWith('#'))
                            tag.remove(0, 1);
                        if (!valid_tag_name(tag) || tags.size() >= catalog_limits::tags_per_application) {
                            service.notify(QCoreApplication::translate("Fora.Deck", "Invalid tag name."));
                            return;
                        }
                        if (!has_tag(tags, tag))
                            tags.append(tag);
                    }
                    service.files->update_metadata(entry.path, tags, service.files->is_favorite(entry.path));
                });
        const bool saved = entry.saved;
        connect(menu->addAction(
                    QCoreApplication::translate("Fora.Deck", saved ? "Remove from My files" : "Save to My files")),
                &QAction::triggered, this, [this, entry, saved] { service.files->set_saved(entry.path, !saved); });
        connect(menu, &QMenu::aboutToHide, this, [this, menu] {
            if (action_menu == menu)
                action_menu = nullptr;
            menu->deleteLater();
            input.setFocus();
        });
        auto *visible_view = active_view();
        menu->popup(visible_view->viewport()->mapToGlobal(visible_view->visualRect(currentIndex()).bottomLeft()));
        menu->setActiveAction(menu->actions().first());
        return;
    }
    if (!isEnabled() || service.profile_busy() || service.restart_required() || row < 0 ||
        row >= results->entries.size() || results->entries[row].type != search_entry::kind::application)
        return;
    const auto id = results->entries[row].application_id;
    const auto *current = service.model.find(id);
    if (!current)
        return;
    const auto record = *current;
    if (action_menu)
        action_menu->close();
    auto *menu = new QMenu(this);
    action_menu = menu;
    menu->setObjectName("quick_actions");
    menu->setTitle(QCoreApplication::translate("Fora.Deck", "Application actions"));
    menu->setAccessibleName(menu->title());
    const auto launch_action = [this, menu, id](const QString &title, const Application::launch_mode mode) {
        connect(menu->addAction(title), &QAction::triggered, this,
                [this, id, mode] { pending_launch = service.request_launch_mode(id, mode); });
    };
    launch_action(QCoreApplication::translate("Fora.Deck", "Launch"), Application::launch_mode::launch);
    if (record.new_instance_error.isEmpty() && Application::file_target(record))
        launch_action(QCoreApplication::translate("Fora.Deck", "New instance"), Application::launch_mode::new_instance);
    if (record.running && Application::file_target(record))
        launch_action(QCoreApplication::translate("Fora.Deck", "Switch to running window"),
                      Application::launch_mode::activate);
    if (record.running)
        connect(menu->addAction(appearance::icon("stop"), QCoreApplication::translate("Fora.Deck", "Terminate")),
                &QAction::triggered, this, [this, id] { confirm_application_termination(service, id, window()); });
    const auto profiles = service.launch_profiles(record);
    if (!profiles.isEmpty()) {
        auto *profile_menu = menu->addMenu(QCoreApplication::translate("Fora.Deck", "Browser profile"));
        for (const auto &profile : profiles) {
            auto *action =
                profile_menu->addAction(appearance::icon(profile.kind == "private" ? "lock" : "app"), profile.name);
            action->setCheckable(true);
            action->setChecked(record.default_launch_profile == profile.id);
            connect(action, &QAction::triggered, this, [this, id, profile_id = profile.id] {
                pending_launch = service.request_launch_profile(id, profile_id);
            });
        }
    }
#ifdef Q_OS_WIN
    if (Application::target_kind(record) == "executable" &&
        QFileInfo(record.path).suffix().compare("exe", Qt::CaseInsensitive) == 0)
        launch_action(QCoreApplication::translate("Fora.Deck", "Run as administrator"),
                      Application::launch_mode::elevated);
#endif
    menu->addSeparator();
    if (Application::file_target(record) || !record.installation_directory.isEmpty())
        connect(menu->addAction(QCoreApplication::translate("Fora.Deck", "Open folder")), &QAction::triggered, this,
                [this, id] { service.open_folder(id); });
    if (Application::file_target(record) || !record.installation_directory.isEmpty())
        connect(menu->addAction(QCoreApplication::translate("Fora.Deck", "Copy path")), &QAction::triggered, this,
                [record] {
                    QApplication::clipboard()->setText(QDir::toNativeSeparators(Application::application_path(record)));
                });
    connect(menu->addAction(record.favorite ? QCoreApplication::translate("Fora.Deck", "Remove from favorites")
                                            : QCoreApplication::translate("Fora.Deck", "Add to favorites")),
            &QAction::triggered, this,
            [this, id, favorite = record.favorite] { service.update_metadata({id}, {}, {}, favorite ? 0 : 1); });
    const auto edit_action = [this, menu, id](const QString &text, const QString &focus) {
        connect(menu->addAction(text), &QAction::triggered, this,
                [this, id, focus] { edit_application(service, id, window(), focus); });
    };
    edit_action(QCoreApplication::translate("Fora.Deck", "Edit tags"), "application_tags");
    edit_action(QCoreApplication::translate("Fora.Deck", "Edit application"), "application_name");
    edit_action(QCoreApplication::translate("Fora.Deck", "Assign hotkey"), "application_hotkey");
    menu->addSeparator();
    connect(menu->addAction(QCoreApplication::translate("Fora.Deck", "Remove from library")), &QAction::triggered, this,
            [this, id, name = record.name] {
                if (QMessageBox::question(
                        window(), QCoreApplication::translate("Fora.Deck", "Remove application"),
                        QCoreApplication::translate("Fora.Deck",
                                                    "Remove “%1” from the library? The application files will be kept.")
                            .arg(name)) == QMessageBox::Yes)
                    service.remove_application(id);
            });
    connect(menu, &QMenu::aboutToHide, this, [this, menu] {
        if (action_menu == menu)
            action_menu = nullptr;
        input.setFocus();
        menu->deleteLater();
    });
    menu->popup(viewport()->mapToGlobal(visualRect(currentIndex()).bottomLeft()));
    if (!menu->actions().isEmpty())
        menu->setActiveAction(menu->actions().first());
}

bool SearchResults::eventFilter(QObject *const object, QEvent *const event) {
    if (object == &input && (compact || isVisible() || (file_workspace && files_enabled)) &&
        (compact || special || (files_enabled && results->rowCount() > 0))) {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (compact && (key->modifiers() & ~Qt::KeypadModifier) == Qt::AltModifier && key->key() >= Qt::Key_0 &&
                key->key() <= Qt::Key_9) {
                const int row = key->key() == Qt::Key_0 ? 9 : key->key() - Qt::Key_1;
                if (row < results->rowCount() && results->entries[row].type != search_entry::kind::message) {
                    if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
                        setCurrentIndex(results->index(row, 0));
                        scrollTo(currentIndex());
                        activate_current();
                    }
                    event->accept();
                    return true;
                }
            }
            const bool plain = !(key->modifiers() & ~Qt::KeypadModifier);
            const bool actions_key =
                (compact || files_enabled) && ((plain && key->key() == Qt::Key_Tab) ||
                                               (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier));
            if (actions_key && currentIndex().isValid() &&
                (results->entries[currentIndex().row()].type == search_entry::kind::application ||
                 results->entries[currentIndex().row()].type == search_entry::kind::file ||
                 results->entries[currentIndex().row()].type == search_entry::kind::working_file)) {
                if (event->type() == QEvent::KeyPress)
                    show_actions();
                event->accept();
                return true;
            }
            if (plain && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Up ||
                          key->key() == Qt::Key_Down || key->key() == Qt::Key_Escape)) {
                if (event->type() == QEvent::KeyPress) {
                    if (key->key() == Qt::Key_Escape) {
                        if (compact)
                            emit dismissed();
                        else
                            input.clear();
                    } else if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                        activate_current();
                    else {
                        const auto row = qBound(0, currentIndex().row() + (key->key() == Qt::Key_Down ? 1 : -1),
                                                qMax(0, results->rowCount() - 1));
                        setCurrentIndex(results->index(row, 0));
                        active_view()->scrollTo(currentIndex());
                    }
                }
                event->accept();
                return true;
            }
        }
    }
    return QTreeView::eventFilter(object, event);
}

int SearchResults::result_count() const {
    return static_cast<int>(std::count_if(results->entries.cbegin(), results->entries.cend(),
                                          [](const auto &entry) { return entry.type != search_entry::kind::message; }));
}

QAbstractItemView *SearchResults::active_view() {
    if (file_workspace)
        if (auto *cards = window()->findChild<QAbstractItemView *>("file_cards"); cards && cards->isVisible())
            return cards;
    return this;
}

void SearchResults::resizeEvent(QResizeEvent *const event) {
    QTreeView::resizeEvent(event);
    if (!file_workspace)
        return;
    const int unit = qMax(1, fontMetrics().height());
    const int available = viewport()->width();
    setColumnHidden(1, available < unit * 34);
    setColumnHidden(2, available < unit * 30);
    setColumnHidden(3, available < unit * 42);
    setColumnHidden(4, available < unit * 58);
    header()->resizeSection(1, qBound(unit * 7, available / 6, unit * 12));
    header()->resizeSection(2, unit * 7);
    header()->resizeSection(3, unit * 11);
    header()->resizeSection(4, qBound(unit * 8, available / 5, unit * 15));
}

void SearchResults::set_sort_mode(const int mode) {
    sort_mode = mode;
    if (file_workspace) {
        const int column = mode == 1 ? 0 : mode == 4 ? 1 : mode == 0 || mode == 5 ? 2 : mode == 6 ? 3 : -1;
        header()->setSortIndicatorShown(column >= 0);
        header()->setSortIndicator(column, mode == 6 ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
    update_query();
}

void SearchResults::toggle_favorite() {
    const auto row = currentIndex().row();
    if (!isEnabled() || service.profile_busy() || service.restart_required() || row < 0 ||
        row >= results->entries.size())
        return;
    const auto entry = results->entries[row];
    if (entry.type == search_entry::kind::file || entry.type == search_entry::kind::working_file)
        service.files->update_metadata(entry.path, service.files->tags_for(entry.path),
                                       !service.files->is_favorite(entry.path));
}
