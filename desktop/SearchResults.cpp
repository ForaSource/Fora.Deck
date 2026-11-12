#include <algorithm>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
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

class SearchResultModel : public QAbstractListModel {
public:
    QVector<search_entry> entries;
    explicit SearchResultModel(QObject *const parent) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(entries.size());
    }
    QVariant data(const QModelIndex &index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= entries.size())
            return {};
        const auto &entry = entries[index.row()];
        if (role == Qt::DisplayRole)
            return entry.title;
        if (role == Qt::ToolTipRole)
            return entry.path.isEmpty() ? entry.title : QDir::toNativeSeparators(entry.path);
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= entries.size() ||
            entries[index.row()].type == search_entry::kind::message)
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }
    void replace(QVector<search_entry> value) {
        beginResetModel();
        entries = std::move(value);
        endResetModel();
    }
};

namespace {
class SearchDelegate : public QStyledItemDelegate {
    DesktopService &service;

public:
    SearchDelegate(DesktopService &service, QObject *const parent) : QStyledItemDelegate(parent), service(service) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return {0, appearance::scaled(service.settings, 52)};
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
        }
        if (entry.type != search_entry::kind::message)
            icon.paint(painter, icon_rect);
        painter->setFont(option.font);
        painter->setPen(option.palette.color(entry.type == search_entry::kind::message ? QPalette::PlaceholderText
                                                                                       : QPalette::Text));
        const auto text_rect = area.adjusted(icon_size + 28, 0, -14, 0);
        painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft,
                          option.fontMetrics.elidedText(entry.title, Qt::ElideRight, text_rect.width()));
        painter->restore();
    }
};

}

SearchResults::SearchResults(DesktopService &service, QLineEdit &input, const bool compact, QWidget *const parent)
    : QListView(parent), service(service), input(input), compact(compact), results(new SearchResultModel(this)) {
    setObjectName("search_results");
    file_delay.setSingleShot(true);
    file_delay.setInterval(100);
    connect(&file_delay, &QTimer::timeout, this, &SearchResults::update_files);
    setAccessibleName(QCoreApplication::translate("XLibrary", "Search results"));
    setModel(results);
    setItemDelegate(new SearchDelegate(service, this));
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    input.installEventFilter(this);
    connect(&input, &QLineEdit::textChanged, this, &SearchResults::update_query);
    connect(this, &QListView::clicked, this, [this] { activate_current(); });
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
    connect(&service, &DesktopService::navigation_changed, this, [this] {
        if (!this->compact)
            update_query();
    });
    connect(&service, &DesktopService::profile_state_changed, this,
            [this] { setEnabled(!this->service.profile_busy() && !this->service.restart_required()); });
    connect(&service, &DesktopService::appearance_changed, this, &SearchResults::update_query);
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
            replace_entries(path_watcher.result());
        }
        if (!pending_path.isEmpty())
            read_path();
    });
    connect(service.files, &FileManager::changed, this, [this] {
        file_matches.clear();
        if (!action_menu)
            update_query();
    });
    const auto configure_files = [this] {
        set_files_enabled(this->compact ? this->service.searching_files() : this->service.file_search_active());
    };
    connect(service.files, &FileManager::configuration_changed, this, configure_files);
    connect(&service, &DesktopService::navigation_changed, this, configure_files);
    configure_files();
    update_query();
}

SearchResults::~SearchResults() {
    file_delay.stop();
    ++*generation;
}

void SearchResults::replace_entries(QVector<search_entry> entries) {
    if (entries.isEmpty() && files_enabled && input.text().trimmed().isEmpty())
        entries.append({search_entry::kind::message,
                        QCoreApplication::translate("XLibrary", "Enter a filename to search indexed files"),
                        {},
                        0});
    if (entries.isEmpty() && (special || files_enabled || (compact && !input.text().trimmed().isEmpty()))) {
        const auto hint = special ? QString{} : SearchQuery(input.text()).tag_hint(service.catalog().tags);
        entries.append({search_entry::kind::message,
                        hint.isEmpty() ? QCoreApplication::translate("XLibrary", "No results") : hint,
                        {},
                        0});
    }
    const bool preserve_position =
        property("rendered_query").isValid() && property("rendered_query").toString() == input.text();
    const auto top_index = indexAt(QPoint(0, 0));
    const auto top_entry = top_index.isValid() ? results->entries[top_index.row()] : search_entry{};
    const auto top_offset = top_index.isValid() ? visualRect(top_index).top() : 0;
    const auto scroll_position = verticalScrollBar()->value();
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
    const auto rows = qMin(results->rowCount(), 5);
    if (!compact && files_enabled) {
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setVisible(true);
    } else {
        setFixedHeight(rows * appearance::scaled(service.settings, 52) + (rows ? 4 : 0));
        setVisible(rows > 0);
    }
    if (preserve_position) {
        doItemsLayout();
        verticalScrollBar()->setValue(scroll_position);
        if (top_index.isValid())
            for (int row = 0; row < results->rowCount(); ++row)
                if (same_entry(results->entries[row], top_entry)) {
                    scrollTo(results->index(row, 0), QAbstractItemView::PositionAtTop);
                    verticalScrollBar()->setValue(verticalScrollBar()->value() - top_offset);
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
    // Keep only the five best matches in O(n), rather than sorting an entire second catalog on each key.
    for (int row = 0; row < service.model.rowCount(); ++row) {
        const auto &record = service.model.at(row);
        if (!compact && ((service.section() == "favorites" && !record.favorite) ||
                         (service.section() == "running" && !record.running) ||
                         (!service.tag().isEmpty() && !has_tag(record.tags, service.tag()))))
            continue;
        if (input.text().trimmed().isEmpty() && !record.favorite && !record.launch_count)
            continue;
        const auto rank = query.rank(service.model.search_data(row));
        if (rank < 0)
            continue;
        const QPair<int, int> candidate{row, rank};
        auto position = std::lower_bound(best.begin(), best.end(), candidate, better);
        if (position != best.end() || best.size() < 5) {
            best.insert(position, candidate);
            if (best.size() > 5)
                best.removeLast();
        }
    }
    QVector<search_entry> entries;
    for (const auto &candidate : best) {
        if (!SearchQuery::accepts_rank(candidate.second, best.front().second))
            break;
        const auto &record = service.model.at(candidate.first);
        auto title = record.name;
        if (!record.default_launch_profile.isEmpty()) {
            const auto mode = record.default_launch_profile == "private"
                                  ? QCoreApplication::translate("XLibrary", "Private mode")
                              : record.default_launch_profile == "normal"
                                  ? QCoreApplication::translate("XLibrary", "Normal mode")
                                  : QCoreApplication::translate("XLibrary", "Profile: %1")
                                        .arg(QFileInfo(record.default_launch_profile.mid(8)).fileName());
            title += " · " + mode;
        }
        entries.append({search_entry::kind::application, title, record.path, record.id, record.favorite,
                        candidate.second, record.launch_count, record.last_launch, 0, 0});
    }
    application_entries = entries;
}

void SearchResults::update_query() {
    if (action_menu)
        action_menu->close();
    pending_launch = 0;
    file_delay.stop();
    if (property("rendered_query").toString() != input.text())
        setCurrentIndex({});
    ++*generation;
    pending_path.clear();
    const auto text = input.text().trimmed();
    special = files_enabled && search_query::is_path(text);
    if (special) {
        file_matches.clear();
        pending_path = search_query::expanded_path(text);
        replace_entries({{search_entry::kind::message, QCoreApplication::translate("XLibrary", "…"), {}, 0}});
        if (!path_busy)
            read_path();
        return;
    }
    const auto calculation = search_query::calculate(text);
    // A numeric filename mask such as "2026*" is also an incomplete calculation.
    special = calculation.recognized &&
              !(files_enabled && !calculation.value && search_query::file_pattern(text).has_value());
    if (special) {
        file_matches.clear();
        if (calculation.value) {
            const auto number = QString::number(*calculation.value, 'g', 15);
            replace_entries({{search_entry::kind::calculation, "= " + number, number, 0}});
        } else
            replace_entries({{search_entry::kind::message, QCoreApplication::translate("XLibrary", "—"), {}, 0}});
    } else {
        if (!files_enabled || text.isEmpty())
            file_matches.clear();
        application_entries.clear();
        if (compact && !files_enabled)
            update_applications();
        update_entries();
        if (files_enabled && !text.isEmpty())
            file_delay.start();
    }
}

void SearchResults::set_files_enabled(const bool enabled) {
    if (files_enabled == enabled)
        return;
    files_enabled = enabled;
    file_matches.clear();
    update_query();
}
void SearchResults::update_files() {
    const auto expected = generation->load();
    service.files->search(
        input.text(), expected, this,
        [this](quint64 request, QVector<file_match> matches) {
            if (request != generation->load() || !files_enabled)
                return;
            file_matches = std::move(matches);
            update_entries();
        },
        generation, service.catalog().file_usage);
}

void SearchResults::update_entries() {
    if (!files_enabled) {
        replace_entries(compact ? application_entries : QVector<search_entry>{});
        return;
    }
    // Refine the last completed file search while its replacement is still pending.
    const auto matches = FileManager::filter_matches(input.text(), file_matches);
    if (matches.isEmpty()) {
        replace_entries({});
        return;
    }
    QVector<search_entry> entries;
    for (const auto &match : matches)
        entries.append({search_entry::kind::working_file,
                        (match.saved ? QStringLiteral("★ ") : QString{}) + match.name + QStringLiteral(" · ") +
                            QDir::toNativeSeparators(QFileInfo(match.path).absolutePath()),
                        match.path, 0, match.saved, match.rank, match.use_count, match.last_used, match.location,
                        match.depth});
    std::sort(entries.begin(), entries.end(), [](const search_entry &a, const search_entry &b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.saved != b.saved)
            return a.saved;
        if (a.use_count != b.use_count)
            return a.use_count > b.use_count;
        if (a.last_used != b.last_used)
            return a.last_used > b.last_used;
        if (a.location != b.location)
            return a.location < b.location;
        if (a.depth != b.depth)
            return a.depth < b.depth;
        const auto comparison = QString::compare(a.title, b.title, Qt::CaseInsensitive);
        if (comparison != 0)
            return comparison < 0;
        return a.path == b.path ? a.application_id < b.application_id : a.path < b.path;
    });
    if (!entries.isEmpty()) {
        const auto best = entries.front().rank;
        entries.removeIf([best](const auto &entry) { return !SearchQuery::accepts_rank(entry.rank, best); });
    }
    replace_entries(std::move(entries));
}

void SearchResults::read_path() {
    path_busy = true;
    active_path = std::move(pending_path);
    pending_path.clear();
    active_generation = generation->load();
    // Only one disk request is active per field; newer input replaces the pending request.
    auto usage = service.catalog().file_usage;
    QSet<QString> favorites;
    path_watcher.setFuture(QtConcurrent::run(search_results::directory_entries, active_path, generation,
                                             active_generation, usage, favorites, true));
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
            service.notify(QCoreApplication::translate("XLibrary", "Could not open the folder."));
    } else if (entry.type == search_entry::kind::directory) {
        input.setText(QDir::toNativeSeparators(entry.path + '/'));
        input.setFocus();
    } else if (entry.type == search_entry::kind::application) {
        pending_launch = service.request_launch(entry.application_id);
    } else if ((entry.type == search_entry::kind::file || entry.type == search_entry::kind::working_file)) {
        if (entry.type == search_entry::kind::working_file && !FileManager::accepts_file(entry.path)) {
            service.notify(QCoreApplication::translate("XLibrary", "The file is unavailable or its type is excluded."));
            update_query();
            return;
        }
        if (desktop_platform::open_url(QUrl::fromLocalFile(entry.path))) {
            service.record_file_use(entry.path);
            emit application_activated();
        } else
            service.notify(QCoreApplication::translate("XLibrary", "Could not open the file."));
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
        connect(menu->addAction(QCoreApplication::translate("XLibrary", "Open")), &QAction::triggered, this,
                [this, entry] {
                    if (entry.type == search_entry::kind::working_file && !FileManager::accepts_file(entry.path)) {
                        service.notify(QCoreApplication::translate("XLibrary",
                                                                   "The file is unavailable or its type is excluded."));
                        return;
                    }
                    if (desktop_platform::open_url(QUrl::fromLocalFile(entry.path))) {
                        service.record_file_use(entry.path);
                        emit application_activated();
                    } else
                        service.notify(QCoreApplication::translate("XLibrary", "Could not open the file."));
                });
        connect(menu->addAction(QCoreApplication::translate("XLibrary", "Open folder")), &QAction::triggered, this,
                [entry] { desktop_platform::open_url(QUrl::fromLocalFile(QFileInfo(entry.path).absolutePath())); });
        connect(menu->addAction(QCoreApplication::translate("XLibrary", "Copy path")), &QAction::triggered, this,
                [entry] { QApplication::clipboard()->setText(QDir::toNativeSeparators(entry.path)); });
        const bool saved = entry.saved;
        connect(menu->addAction(
                    QCoreApplication::translate("XLibrary", saved ? "Remove from My files" : "Save to My files")),
                &QAction::triggered, this, [this, entry, saved] { service.files->set_saved(entry.path, !saved); });
        connect(menu, &QMenu::aboutToHide, this, [this, menu] {
            if (action_menu == menu)
                action_menu = nullptr;
            menu->deleteLater();
            input.setFocus();
        });
        menu->popup(viewport()->mapToGlobal(visualRect(currentIndex()).bottomLeft()));
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
    menu->setTitle(QCoreApplication::translate("XLibrary", "Application actions"));
    menu->setAccessibleName(menu->title());
    const auto launch_action = [this, menu, id](const QString &title, const Application::launch_mode mode) {
        connect(menu->addAction(title), &QAction::triggered, this,
                [this, id, mode] { pending_launch = service.request_launch_mode(id, mode); });
    };
    launch_action(QCoreApplication::translate("XLibrary", "Launch"), Application::launch_mode::launch);
    if (record.new_instance_error.isEmpty() && Application::file_target(record))
        launch_action(QCoreApplication::translate("XLibrary", "New instance"), Application::launch_mode::new_instance);
    if (record.running && Application::file_target(record))
        launch_action(QCoreApplication::translate("XLibrary", "Switch to running window"),
                      Application::launch_mode::activate);
    const auto profiles = service.launch_profiles(record);
    if (!profiles.isEmpty()) {
        auto *profile_menu = menu->addMenu(QCoreApplication::translate("XLibrary", "Browser profile"));
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
        launch_action(QCoreApplication::translate("XLibrary", "Run as administrator"),
                      Application::launch_mode::elevated);
#endif
    menu->addSeparator();
    if (Application::file_target(record) || !record.installation_directory.isEmpty())
        connect(menu->addAction(QCoreApplication::translate("XLibrary", "Open folder")), &QAction::triggered, this,
                [this, id] { service.open_folder(id); });
    if (Application::file_target(record) || !record.installation_directory.isEmpty())
        connect(menu->addAction(QCoreApplication::translate("XLibrary", "Copy path")), &QAction::triggered, this,
                [record] {
                    QApplication::clipboard()->setText(QDir::toNativeSeparators(Application::application_path(record)));
                });
    connect(menu->addAction(record.favorite ? QCoreApplication::translate("XLibrary", "Remove from favorites")
                                            : QCoreApplication::translate("XLibrary", "Add to favorites")),
            &QAction::triggered, this,
            [this, id, favorite = record.favorite] { service.update_metadata({id}, {}, {}, favorite ? 0 : 1); });
    const auto edit_action = [this, menu, id](const QString &text, const QString &focus) {
        connect(menu->addAction(text), &QAction::triggered, this,
                [this, id, focus] { edit_application(service, id, window(), focus); });
    };
    edit_action(QCoreApplication::translate("XLibrary", "Edit tags"), "application_tags");
    edit_action(QCoreApplication::translate("XLibrary", "Edit application"), "application_name");
    edit_action(QCoreApplication::translate("XLibrary", "Assign hotkey"), "application_hotkey");
    menu->addSeparator();
    connect(menu->addAction(QCoreApplication::translate("XLibrary", "Remove from library")), &QAction::triggered, this,
            [this, id, name = record.name] {
                if (QMessageBox::question(
                        window(), QCoreApplication::translate("XLibrary", "Remove application"),
                        QCoreApplication::translate("XLibrary",
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
    if (object == &input && (compact || special || (files_enabled && results->rowCount() > 0))) {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
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
                        scrollTo(currentIndex());
                    }
                }
                event->accept();
                return true;
            }
        }
    }
    return QListView::eventFilter(object, event);
}
