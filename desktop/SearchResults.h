#ifndef XLIBRARY_SEARCH_RESULTS_H
#define XLIBRARY_SEARCH_RESULTS_H

#include <algorithm>
#include <atomic>
#include <memory>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QListView>

#include "LibraryModel.h"

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
};

namespace search_results {
// Bound memory and metadata I/O per keystroke; a final action explains partial results.
inline constexpr int result_limit = 50;
inline constexpr int scan_limit = 20000;

inline QVector<search_entry> directory_entries(const QString &request,
                                               const std::shared_ptr<std::atomic<quint64>> &generation,
                                               const quint64 expected) {
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
                {search_entry::kind::message, QCoreApplication::translate("XLibrary", "Folder unavailable"), {}, 0}};
        prefix = target.fileName();
        path = target.absolutePath();
    }
    const QDir directory(path);
    if (!directory.exists() || !QFileInfo(path).isReadable())
        return {{search_entry::kind::message, QCoreApplication::translate("XLibrary", "Folder unavailable"), {}, 0}};
    const auto better = [](const search_entry &left, const search_entry &right) {
        if (left.type != right.type)
            return left.type == search_entry::kind::directory;
        const auto comparison = QString::compare(left.title, right.title, Qt::CaseInsensitive);
        return comparison == 0 ? left.title < right.title : comparison < 0;
    };
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
        if (!name.startsWith(prefix, Qt::CaseInsensitive))
            continue;
        const auto file = iterator.fileInfo();
        search_entry entry{file.isDir() ? search_entry::kind::directory : search_entry::kind::file, name,
                           file.absoluteFilePath(), 0};
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
    if (limited)
        entries.append({search_entry::kind::open_folder,
                        QCoreApplication::translate("XLibrary", "Results limited. Open folder"),
                        directory.absolutePath(), 0});
    return entries;
}
}

class SearchResults : public QListView {
    Q_OBJECT
    DesktopService &service;
    QLineEdit &input;
    bool compact;
    SearchResultModel *results;
    QFutureWatcher<QVector<search_entry>> path_watcher;
    std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(0);
    QString pending_path;
    QString active_path;
    quint64 active_generation = 0;
    bool special = false;
    bool files_enabled = false;
    int pending_file_queries = 0;
    QVector<search_entry> application_entries;
    void update_files();
    bool path_busy = false;
    quint64 pending_launch = 0;
    QMenu *action_menu = nullptr;
    void update_query();
    void update_applications();
    void read_path();
    void replace_entries(QVector<search_entry> entries);
    void activate_current();
    void show_actions();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

public:
    SearchResults(DesktopService &service, QLineEdit &input, bool compact, QWidget *parent = nullptr);
    ~SearchResults() override;
    void set_files_enabled(bool enabled);
    bool handles_query() const { return special; }
    void reset_launch() { pending_launch = 0; }

signals:
    void height_changed();
    void application_activated();
    void dismissed();
};

#endif
