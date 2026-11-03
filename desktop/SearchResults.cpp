#include <algorithm>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QtConcurrent/QtConcurrentRun>
#include <QUrl>

#include "Appearance.h"
#include "DesktopService.h"
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
    QIcon app_icon;
    QIcon file_icon;
    QIcon folder_icon;
    QIcon copy_icon;

public:
    SearchDelegate(DesktopService &service, QObject *const parent)
        : QStyledItemDelegate(parent), service(service), app_icon(appearance::icon("app")),
          file_icon(appearance::icon("file")), folder_icon(appearance::icon("folder")),
          copy_icon(appearance::icon("copy")) {}
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
            appearance::paint_selection(*painter, area, option.palette, 12);
        const auto icon_size = appearance::scaled(service.settings, 24);
        const QRect icon_rect(area.left() + 14, area.center().y() - icon_size / 2, icon_size, icon_size);
        auto icon = entry.type == search_entry::kind::application   ? app_icon
                    : entry.type == search_entry::kind::directory   ? folder_icon
                    : entry.type == search_entry::kind::calculation ? copy_icon
                                                                    : file_icon;
        if (entry.type == search_entry::kind::application) {
            // Reuse the model's asynchronous icon cache; painting never touches the disk.
            for (int row = 0; row < service.model.rowCount(); ++row)
                if (service.model.at(row).id == entry.application_id) {
                    const auto cached =
                        service.model.data(service.model.index(row, 0), Qt::DecorationRole).value<QIcon>();
                    if (!cached.isNull())
                        icon = cached;
                    break;
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

QVector<search_entry> directory_entries(const QString &request, const std::shared_ptr<std::atomic<quint64>> &generation,
                                        const quint64 expected) {
    if (generation->load() != expected)
        return {};
    auto path = request;
#ifdef Q_OS_WIN
    if (path.size() == 2 && path[1] == ':')
        path += '/';
#endif
    QFileInfo target(path);
    QString prefix;
    if (!target.isDir()) {
        if (path.endsWith('/'))
            return {
                {search_entry::kind::message, QCoreApplication::translate("XLibrary", "Folder unavailable"), {}, 0}};
        prefix = target.fileName();
        path = target.absolutePath();
    }
    const QDir directory(path);
    if (!directory.exists() || !QFileInfo(path).isReadable())
        return {{search_entry::kind::message, QCoreApplication::translate("XLibrary", "Folder unavailable"), {}, 0}};
    const auto files = directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                               QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    QVector<search_entry> entries;
    entries.reserve(files.size());
    for (const auto &file : files) {
        if (generation->load() != expected)
            return {};
        if (!file.fileName().startsWith(prefix, Qt::CaseInsensitive))
            continue;
        entries.append({file.isDir() ? search_entry::kind::directory : search_entry::kind::file, file.fileName(),
                        file.absoluteFilePath(), 0});
    }
    return entries;
}
}

SearchResults::SearchResults(DesktopService &service, QLineEdit &input, const bool compact, QWidget *const parent)
    : QListView(parent), service(service), input(input), compact(compact), results(new SearchResultModel(this)) {
    setObjectName("search_results");
    setAccessibleName(QCoreApplication::translate("XLibrary", "Search results"));
    setModel(results);
    setItemDelegate(new SearchDelegate(service, this));
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    input.installEventFilter(this);
    connect(&input, &QLineEdit::textChanged, this, &SearchResults::update_query);
    connect(this, &QListView::clicked, this, [this] { activate_current(); });
    connect(&service.model, &QAbstractItemModel::dataChanged, viewport(), qOverload<>(&QWidget::update));
    connect(&service, &DesktopService::catalog_changed, this, &SearchResults::update_query);
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
            auto entries = path_watcher.result();
            // Resolve library identities in memory on the GUI thread, preserving launch configurations.
            QHash<QString, qint64> known;
            for (const auto &record : this->service.model.applications()) {
                const auto path = normalized_path(record.path);
                // A file path cannot identify which arguments or working directory the user wants.
                if (known.contains(path))
                    known[path] = 0;
                else
                    known.insert(path, record.id);
            }
            for (auto &entry : entries)
                if (entry.type == search_entry::kind::file) {
                    entry.application_id = known.value(normalized_path(entry.path));
                    if (entry.application_id)
                        entry.type = search_entry::kind::application;
                }
            replace_entries(std::move(entries));
        }
        if (!pending_path.isEmpty())
            read_path();
    });
    update_query();
}

SearchResults::~SearchResults() {
    ++*generation;
}

void SearchResults::replace_entries(QVector<search_entry> entries) {
    if (entries.isEmpty() && (special || (compact && !input.text().trimmed().isEmpty()))) {
        const auto hint = special ? QString{} : SearchQuery(input.text()).tag_hint(service.catalog().tags);
        entries.append({search_entry::kind::message,
                        hint.isEmpty() ? QCoreApplication::translate("XLibrary", "No results") : hint,
                        {},
                        0});
    }
    results->replace(std::move(entries));
    setCurrentIndex(results->index(0, 0));
    const auto rows = qMin(results->rowCount(), 5);
    setFixedHeight(rows * appearance::scaled(service.settings, 52) + (rows ? 4 : 0));
    setVisible(rows > 0);
    emit height_changed();
}

void SearchResults::update_applications() {
    const SearchQuery query(input.text());
    QVector<QPair<int, int>> best;
    const auto better = [this](const auto &left, const auto &right) {
        if (left.second != right.second)
            return left.second < right.second;
        const auto comparison =
            QString::localeAwareCompare(service.model.search_name(left.first), service.model.search_name(right.first));
        return comparison == 0 ? service.model.at(left.first).id < service.model.at(right.first).id : comparison < 0;
    };
    // Keep only the five best matches in O(n), rather than sorting an entire second catalog on each key.
    for (int row = 0; row < service.model.rowCount(); ++row) {
        const auto &record = service.model.at(row);
        const auto rank =
            query.rank(service.model.search_name(row), service.model.search_path(row), record.tags, record.aliases);
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
        const auto &record = service.model.at(candidate.first);
        entries.append({search_entry::kind::application, record.name, record.path, record.id});
    }
    replace_entries(std::move(entries));
}

void SearchResults::update_query() {
    pending_launch = 0;
    ++*generation;
    pending_path.clear();
    const auto text = input.text().trimmed();
    special = search_query::is_path(text);
    if (special) {
        pending_path = search_query::expanded_path(text);
        replace_entries({{search_entry::kind::message, QCoreApplication::translate("XLibrary", "…"), {}, 0}});
        if (!path_busy)
            read_path();
        return;
    }
    const auto calculation = search_query::calculate(text);
    special = calculation.recognized;
    if (special) {
        if (calculation.value) {
            const auto number = QString::number(*calculation.value, 'g', 15);
            replace_entries({{search_entry::kind::calculation, "= " + number, number, 0}});
        } else
            replace_entries({{search_entry::kind::message, QCoreApplication::translate("XLibrary", "—"), {}, 0}});
    } else if (compact && !text.isEmpty())
        update_applications();
    else
        replace_entries({});
}

void SearchResults::read_path() {
    path_busy = true;
    active_path = std::move(pending_path);
    pending_path.clear();
    active_generation = generation->load();
    // Only one disk request is active per field; newer input replaces the pending request.
    path_watcher.setFuture(QtConcurrent::run(directory_entries, active_path, generation, active_generation));
}

void SearchResults::activate_current() {
    if (!isEnabled() || service.profile_busy() || service.restart_required())
        return;
    const auto row = currentIndex().row();
    if (row < 0 || row >= results->entries.size())
        return;
    const auto entry = results->entries[row];
    if (entry.type == search_entry::kind::directory) {
        input.setText(QDir::toNativeSeparators(entry.path + '/'));
        input.setFocus();
    } else if (entry.type == search_entry::kind::application) {
        pending_launch = service.request_launch(entry.application_id);
    } else if (entry.type == search_entry::kind::file) {
        if (desktop_platform::open_url(QUrl::fromLocalFile(entry.path)))
            emit application_activated();
        else
            service.notify(QCoreApplication::translate("XLibrary", "Could not open the file."));
    } else if (entry.type == search_entry::kind::calculation)
        QApplication::clipboard()->setText(entry.path);
}

bool SearchResults::eventFilter(QObject *const object, QEvent *const event) {
    if (object == &input && (compact || special)) {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            const bool plain = !(key->modifiers() & ~Qt::KeypadModifier);
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
