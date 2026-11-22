#ifndef FORADECK_SEARCH_RESULTS_H
#define FORADECK_SEARCH_RESULTS_H

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <QCoreApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QTimer>
#include <QTreeView>

#include "FileManager.h"
#include "LibraryModel.h"
#include "PathHelpers.h"
#include "SearchQuery.h"

class DesktopService;
class SearchResultModel;
class QMenu;
struct search_entry {
    enum class kind { application, file, working_file, directory, calculation, message, open_folder };
    kind type = kind::file;
    QString title;
    QString path;
    qint64 application_id = 0;
    bool saved = false;
    int rank = 0;
    qint64 use_count = 0;
    qint64 last_used = 0;
    int location = 2;
    int depth = 0;
    QStringList tags{};
    bool favorite = false;
    qint64 modified = 0;
    qint64 size = -1;
    QString extension{};
    qint64 added = 0;
    bool available = true;
    QString volume_root{};
    QString volume_device{};
    bool operator==(const search_entry &) const = default;
};

namespace search_results {
// Bound memory and metadata I/O per keystroke; a final action explains partial results.
inline constexpr int result_limit = 50;
inline constexpr int scan_limit = 20000;

inline QVector<search_entry> directory_entries(
    const QString &request, const std::shared_ptr<std::atomic<quint64>> &generation, const quint64 expected,
    const QHash<QString, usage_record> &usage = {}, const QSet<QString> &favorites = {},
    const bool working_files_only = false, const QVariantMap &metadata = {}, const QStringList &required_tags = {},
    const bool favorites_only = false, const QHash<QString, qint64> &added_dates = {}, const int sort_mode = 0) {
    const auto cancelled = [&] { return generation->load() != expected; };
    if (cancelled())
        return {};
    auto path = request;
#ifdef Q_OS_WIN
    if (path.size() == 2 && path[1] == ':')
        path += '/';
#endif
    const QFileInfo target(path);
    QString prefix;
    if (!target.isDir()) {
        if (path.endsWith('/'))
            return {
                {search_entry::kind::message, QCoreApplication::translate("Fora.Deck", "Folder unavailable"), {}, 0}};
        prefix = target.fileName();
        path = target.absolutePath();
    }
    const QDir directory(path);
    if (!directory.exists() || !QFileInfo(path).isReadable())
        return {{search_entry::kind::message, QCoreApplication::translate("Fora.Deck", "Folder unavailable"), {}, 0}};
    const auto ordered = [working_files_only, sort_mode](const search_entry &left, const search_entry &right,
                                                         const bool rank_results) {
        if (rank_results && left.rank != right.rank)
            return left.rank < right.rank;
        if (working_files_only && left.type != right.type)
            return left.type == search_entry::kind::directory;
        if (working_files_only) {
            if (const auto comparison = FileManager::compare_file_order(left.extension, left.added, right.extension,
                                                                        right.added, sort_mode))
                return comparison < 0;
            if (sort_mode == 1) {
                const auto comparison = QString::compare(left.title, right.title, Qt::CaseInsensitive);
                if (comparison)
                    return comparison < 0;
            }
            if (sort_mode == 2 && left.last_used != right.last_used)
                return left.last_used > right.last_used;
            if (sort_mode == 4) {
                const auto comparison =
                    QString::compare(left.tags.join(','), right.tags.join(','), Qt::CaseInsensitive);
                if (comparison)
                    return comparison < 0;
            }
        }
        if (left.favorite != right.favorite)
            return left.favorite;
        if (left.saved != right.saved)
            return left.saved;
        if (left.use_count != right.use_count)
            return left.use_count > right.use_count;
        if (left.last_used != right.last_used)
            return left.last_used > right.last_used;
        if (left.type != right.type)
            return left.type == search_entry::kind::directory;
        const auto comparison = QString::compare(left.title, right.title, Qt::CaseInsensitive);
        return comparison == 0 ? left.title < right.title : comparison < 0;
    };
    const auto better = [&](const search_entry &left, const search_entry &right) { return ordered(left, right, true); };
    const auto pattern = search_query::file_pattern(prefix);
    QDirIterator iterator(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QVector<search_entry> entries;
    entries.reserve(result_limit + 1);
    int scanned = 0;
    bool limited = false;
    while (iterator.hasNext()) {
        if (cancelled())
            return {};
        if (scanned == scan_limit) {
            limited = true;
            break;
        }
        iterator.next();
        ++scanned;
        const auto name = iterator.fileName();
        if (pattern ? !pattern->match(name.normalized(QString::NormalizationForm_C)).hasMatch()
                    : !name.startsWith(prefix, Qt::CaseInsensitive))
            continue;
        const auto file = iterator.fileInfo();
        if (working_files_only && !file.isDir() && !FileManager::accepts_file(file.absoluteFilePath()))
            continue;
        search_entry entry{file.isDir() ? search_entry::kind::directory : search_entry::kind::file, name,
                           file.absoluteFilePath(), 0};
        const auto key = normalized_path(entry.path);
        entry.modified = file.lastModified().toMSecsSinceEpoch();
        entry.size = file.size();
        entry.extension = file.isDir() ? QString{} : file.suffix().toCaseFolded();
        entry.added = added_dates.value(key);
        const auto properties = metadata.value(key).toMap();
        entry.tags = properties.value("tags").toStringList();
        entry.favorite = properties.value("favorite").toBool();
        if (!file.isDir() && ((favorites_only && !entry.favorite) ||
                              !std::all_of(required_tags.cbegin(), required_tags.cend(),
                                           [&](const auto &tag) { return has_tag(entry.tags, tag); })))
            continue;
        const auto history = usage.value(key);
        entry.saved = favorites.contains(key);
        entry.use_count = history.count;
        entry.last_used = history.last_used;
        entry.rank = pattern || prefix.isEmpty() || name.compare(prefix, Qt::CaseInsensitive) == 0 ? 0 : 1;
        // Keep the best bounded subset, independent of enumeration order within the scanned portion.
        const auto position = std::lower_bound(entries.begin(), entries.end(), entry, better);
        entries.insert(position, std::move(entry));
        if (entries.size() > result_limit) {
            entries.removeLast();
            limited = true;
        }
    }
    if (cancelled())
        return {};
    if (working_files_only && (sort_mode == 0 || sort_mode == 5 || sort_mode == 6))
        std::sort(entries.begin(), entries.end(),
                  [&](const auto &left, const auto &right) { return ordered(left, right, false); });
    if (limited)
        entries.append({search_entry::kind::open_folder,
                        QCoreApplication::translate("Fora.Deck", "Results limited. Open folder"),
                        directory.absolutePath(), 0});
    return entries;
}
}

class SearchResults : public QTreeView {
    Q_OBJECT
    DesktopService &service;
    QLineEdit &input;
    bool compact;
    bool file_workspace;
    SearchResultModel *results;
    QFutureWatcher<QVector<search_entry>> path_watcher;
    std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(0);
    QString pending_path;
    QString active_path;
    quint64 active_generation = 0;
    bool special = false;
    bool files_enabled = false;
    int sort_mode = 0;
    QTimer file_delay;
    struct file_query {
        QString text;
        QStringList tags;
        bool favorites_only;
        int limit;
        int sort_mode;
        bool operator==(const file_query &) const = default;
    };
    std::optional<file_query> rendered_file_query;
    QVector<search_entry> application_entries;
    QVector<file_match> file_matches;
    file_query current_file_query() const;
    void update_files();
    void update_entries(bool refine_files = true);
    bool path_busy = false;
    quint64 pending_launch = 0;
    QMenu *action_menu = nullptr;
    void update_query(bool debounce_files = false);
    void update_applications();
    void sync_file_selection(const QModelIndex &index);
    void read_path();
    void replace_entries(QVector<search_entry> entries);
    QAbstractItemView *active_view();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

public:
    SearchResults(DesktopService &service, QLineEdit &input, bool compact, QWidget *parent = nullptr,
                  bool file_workspace = false);
    ~SearchResults() override;
    bool handles_query() const { return special; }
    void reset_launch() { pending_launch = 0; }
    void activate_current();
    void show_actions();
    void toggle_favorite();
    int result_count() const;
    void set_sort_mode(int mode);

signals:
    void sort_requested(int mode);
    void height_changed();
    void application_activated();
    void dismissed();
};

#endif
