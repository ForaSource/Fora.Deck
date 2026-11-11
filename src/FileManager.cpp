#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <algorithm>
#include <deque>
#include <functional>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>

#include "FileManager.h"
#include "SearchQuery.h"
#include "SqliteHelpers.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
using connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
connection open_index(const QString &path) {
    sqlite3 *raw = nullptr;
    const auto code = sqlite3_open_v2(path.toUtf8().constData(), &raw,
                                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    connection db(raw, sqlite3_close);
    if (code != SQLITE_OK)
        throw std::runtime_error(raw ? sqlite3_errmsg(raw) : "Cannot open file index");
    sqlite3_busy_timeout(raw, 3000);
    return db;
}
bool local_volume(const QStorageInfo &volume) {
    const auto type = volume.fileSystemType().toLower();
    static const QSet<QByteArray> excluded = {
        "proc", "sysfs", "tmpfs",      "devtmpfs",    "squashfs", "cgroup", "cgroup2", "autofs", "nfs",      "nfs4",
        "cifs", "smbfs", "fuse.sshfs", "fuse.rclone", "davfs",    "davfs2", "afs",     "ceph",   "glusterfs"};
    if (excluded.contains(type))
        return false;
#ifdef Q_OS_WIN
    const auto root = QDir::toNativeSeparators(volume.rootPath());
    const auto kind = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
    if (kind == DRIVE_REMOTE || kind == DRIVE_NO_ROOT_DIR || kind == DRIVE_UNKNOWN)
        return false;
#endif
    return true;
}
QString identity(const QString &path) {
    auto result = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
}
const QSet<QString> &extensions() {
    static const QSet<QString> allowed = [] {
        QSet<QString> result;
        for (const auto &value :
             QStringLiteral("pdf doc docx odt rtf txt md markdown tex epub mobi "
                            "xls xlsx xlsb ods csv tsv ppt pptx odp odg pages numbers key "
                            "png jpg jpeg jpe gif webp bmp tif tiff heic heif avif svg psd kra ora xcf "
                            "ai eps indd afdesign afphoto sketch fig dwg dxf blend stl obj 3mf step stp "
                            "mp3 wav flac ogg opus m4a aac mp4 mkv mov avi webm m4v srt vtt")
                 .split(' '))
            result.insert(value);
        return result;
    }();
    return allowed;
}
QString file_query_text(const QString &text, QString &extension) {
    auto query_text = text.simplified();
    const auto token = query_text.section(' ', -1).toLower();
    const auto alternate = search_query::switch_layout(token).toLower();
    for (auto candidate : {token, alternate}) {
        if (candidate.startsWith("*."))
            candidate.remove(0, 2);
        else if (candidate.startsWith('.'))
            candidate.remove(0, 1);
        if (extensions().contains(candidate)) {
            extension = candidate;
            query_text.chop(token.size());
            break;
        }
        const auto dot = candidate.lastIndexOf('.');
        if (dot > 0 && extensions().contains(candidate.mid(dot + 1))) {
            extension = candidate.mid(dot + 1);
            query_text.chop(candidate.size() - dot);
            break;
        }
    }
    return query_text.trimmed();
}
bool contains_path(const QString &root, const QString &path) {
    return root == path || path.startsWith(root.endsWith('/') ? root : root + '/');
}
bool excluded_path(const QStringList &exclusions, const QString &path) {
    return std::any_of(exclusions.cbegin(), exclusions.cend(), [&](const auto &root) {
        return contains_path(root, identity(path));
    });
}
bool traversable_directory(const QFileInfo &info) {
    if (!info.isDir() || info.isSymLink())
        return false;
#ifdef Q_OS_WIN
    const auto attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(info.absoluteFilePath().utf16()));
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
#endif
    return true;
}
}

bool FileManager::accepts_file(const QString &path) {
    const QFileInfo info(path);
    const auto name = info.fileName();
    if (!extensions().contains(info.suffix().toLower()) || name.startsWith('.') || name.startsWith('~') ||
        name.endsWith('~') || info.isHidden() || info.isSymLink() || !info.isFile() || !info.isReadable())
        return false;
    const auto lower = name.toLower();
    for (const auto &part : {".tmp.", ".temp.", ".bak.", ".backup.", ".lock.", ".part.", ".download."})
        if (lower.contains(QLatin1String(part)))
            return false;
#ifdef Q_OS_WIN
    const auto attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(info.absoluteFilePath().utf16()));
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY |
                                                      FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)))
        return false;
#endif
    QFile contents(path);
    if (!contents.open(QIODevice::ReadOnly))
        return false;
    const auto header = contents.read(512);
    if (header.startsWith("MZ") || header.startsWith(QByteArray::fromHex("7f454c46")) ||
        header.startsWith("SQLite format 3"))
        return false;
    static const QSet<QString> text_types = {"txt", "md", "markdown", "csv", "tsv", "tex", "srt", "vtt"};
    if (text_types.contains(info.suffix().toLower()) && header.contains('\0') &&
        !header.startsWith(QByteArray::fromHex("fffe")) && !header.startsWith(QByteArray::fromHex("feff")))
        return false;
    return true;
}

QStringList FileManager::volume_roots() {
    QStringList roots;
    for (const auto &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady() || !local_volume(volume))
            continue;
        roots.append(identity(volume.rootPath()));
    }
    if (roots.isEmpty())
        roots.append(QDir::rootPath());
    roots.removeDuplicates();
    return roots;
}

FileManager::FileManager(const QString &directory, const QStringList &saved_paths, QObject *parent) : QObject(parent) {
    database_path = QDir(directory).filePath("file-index.sqlite");
    scan_pool.setMaxThreadCount(1);
    query_pool.setMaxThreadCount(1);
    refresh.setInterval(15 * 60 * 1000);
    connect(&refresh, &QTimer::timeout, this, &FileManager::rescan);
    progress_timer.setInterval(1000);
    connect(&progress_timer, &QTimer::timeout, this, [this] {
        state = QCoreApplication::translate("XLibrary", "Indexing files: %1").arg(indexed->load());
        emit status_changed();
        emit index_updated();
    });
    // Create the small schema before workers are started; all enumeration and queries run off the GUI thread.
    try {
        QDir().mkpath(directory);
        auto db = open_index(database_path);
        xlibrary::sqlite_execute(db.get(), "PRAGMA journal_mode=WAL;"
                                           "CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY,name TEXT NOT "
                                           "NULL,scope TEXT NOT NULL,seen INTEGER NOT NULL);"
                                           "CREATE TABLE IF NOT EXISTS saved(path TEXT PRIMARY KEY);"
                                           "CREATE INDEX IF NOT EXISTS files_scope ON files(scope);");
        xlibrary::sqlite_execute(db.get(), "BEGIN; DELETE FROM saved;");
        for (const auto &path : saved_paths) {
            if (!QFileInfo(path).isAbsolute())
                continue;
            xlibrary::SqliteStatement insert(db.get(), "INSERT OR IGNORE INTO saved VALUES(?)");
            insert.bind(1, identity(path));
            insert.next();
        }
        xlibrary::sqlite_execute(db.get(), "COMMIT");
    } catch (const std::exception &e) {
        state = QString::fromUtf8(e.what());
    }
}
FileManager::~FileManager() {
    shutdown();
}
void FileManager::shutdown() {
    refresh.stop();
    progress_timer.stop();
    rescan_pending = false;
    ++*scan_generation;
    ++*query_generation;
    scan_pool.clear();
    scan_pool.waitForDone();
    // Mutation tasks must drain even when their GUI completion callbacks cannot run during shutdown.
    query_pool.waitForDone();
}
QStringList FileManager::persisted_saved_paths() const {
    auto db = open_index(database_path);
    xlibrary::SqliteStatement rows(db.get(), "SELECT path FROM saved ORDER BY path");
    QStringList paths;
    while (rows.next())
        paths.append(rows.qtext(0));
    return paths;
}
file_search_mode FileManager::mode_from_setting(const QString &value) {
    if (value == "basic")
        return file_search_mode::basic;
    if (value == "full")
        return file_search_mode::full;
    return file_search_mode::off;
}
void FileManager::configure(const file_search_mode selected_mode, const QStringList &roots,
                            const QStringList &exclusions) {
    mode = selected_mode;
    const auto normalized_paths = [](const QStringList &paths) {
        QStringList result;
        for (const auto &path : paths) {
            if (path.trimmed().isEmpty() || !QFileInfo(path).isAbsolute())
                continue;
            const auto canonical = QFileInfo(path).canonicalFilePath();
            result.append(identity(canonical.isEmpty() ? path : canonical));
        }
        result.removeDuplicates();
        result.sort();
        return result;
    };
    custom_roots = normalized_paths(roots);
    excluded_paths = normalized_paths(exclusions);
    ++*scan_generation;
    ++*query_generation;
    const auto scope_data = QString::number(static_cast<int>(mode)) + '\n' + custom_roots.join('\n') +
                            "\n--exclusions--\n" + excluded_paths.join('\n');
    scope = QString::fromLatin1(QCryptographicHash::hash(scope_data.toUtf8(), QCryptographicHash::Sha256).toHex());
    rescan_pending = is_enabled();
    if (is_enabled())
        refresh.start();
    else {
        refresh.stop();
        state = QCoreApplication::translate("XLibrary", "File search is off");
        emit status_changed();
    }
    emit configuration_changed();
    emit changed();
    if (rescan_pending && !scanning)
        start_scan();
}
void FileManager::rescan() {
    if (scanning || !is_enabled())
        return;
    start_scan();
}
void FileManager::start_scan() {
    rescan_pending = false;
    scanning = true;
    indexed->store(0);
    state = QCoreApplication::translate("XLibrary", "Indexing files: %1").arg(0);
    progress_timer.start();
    emit status_changed();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        state = watcher->result();
        watcher->deleteLater();
        scanning = false;
        progress_timer.stop();
        emit status_changed();
        emit changed();
        if (rescan_pending)
            start_scan();
    });
    const auto expected = scan_generation->load();
    const auto requested_roots = custom_roots;
    const auto selected_mode = mode;
    const auto exclusions = excluded_paths;
    watcher->setFuture(QtConcurrent::run(&scan_pool, [path = database_path, scope = scope, requested_roots,
                                                      selected_mode, exclusions,
                                                      generation = scan_generation, expected, count = indexed] {
        const auto roots = volume_roots();
        QStringList blocked_mounts;
        for (const auto &volume : QStorageInfo::mountedVolumes())
            if (!local_volume(volume))
                blocked_mounts.append(identity(volume.rootPath()));
        const auto cancelled = [&] { return generation->load() != expected; };
        try {
            auto db = open_index(path);
            const auto seen = QDateTime::currentMSecsSinceEpoch();
            std::deque<QPair<QString, int>> pending;
            QHash<QString, int> visited;
            quint64 skipped = 0;
            QStringList recursive_roots = requested_roots;
            if (selected_mode == file_search_mode::basic)
                for (const auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation,
                                            QStandardPaths::DownloadLocation}) {
                    const auto preferred = QStandardPaths::writableLocation(location);
                    if (!preferred.isEmpty())
                        recursive_roots.append(preferred);
                }
            else
                recursive_roots += roots;
            recursive_roots.removeDuplicates();
            const auto add_root = [&](const QString &root, const int depth) {
                const QFileInfo info(root);
                const auto canonical = info.canonicalFilePath();
                if (!canonical.isEmpty() && traversable_directory(info) && !excluded_path(exclusions, root) &&
                    !excluded_path(exclusions, canonical))
                    pending.emplace_back(canonical, depth);
                else
                    ++skipped;
            };
            for (const auto &root : recursive_roots)
                add_root(root, -1);
            if (selected_mode == file_search_mode::basic)
                for (const auto &root : roots)
                    add_root(root, 1);
            QVector<QPair<QString, QString>> batch;
            QElapsedTimer batch_age;
            batch_age.start();
            const auto flush = [&] {
                if (batch.isEmpty())
                    return;
                xlibrary::sqlite_execute(db.get(), "BEGIN");
                for (const auto &entry : batch) {
                    xlibrary::SqliteStatement insert(
                        db.get(), "INSERT INTO files VALUES(?,?,?,?) ON CONFLICT(path) "
                                  "DO UPDATE SET name=excluded.name,scope=excluded.scope,seen=excluded.seen");
                    insert.bind(1, entry.first);
                    insert.bind(2, entry.second);
                    insert.bind(3, scope);
                    insert.bind(4, seen);
                    insert.next();
                }
                xlibrary::sqlite_execute(db.get(), "COMMIT");
                count->fetch_add(batch.size());
                batch.clear();
                batch_age.restart();
            };
            while (!pending.empty() && !cancelled()) {
                const auto [directory, remaining_depth] = std::move(pending.front());
                pending.pop_front();
                if (std::any_of(blocked_mounts.cbegin(), blocked_mounts.cend(), [&](const auto &mount) {
                        return contains_path(mount, identity(directory));
                    }))
                    continue;
                if (batch_age.elapsed() > 100)
                    flush();
                auto key = identity(directory);
#ifndef Q_OS_WIN
                struct stat metadata {};
                if (::stat(QFile::encodeName(directory).constData(), &metadata) != 0) {
                    ++skipped;
                    continue;
                }
                key = QString::number(metadata.st_dev) + ':' + QString::number(metadata.st_ino);
#endif
                const auto previous_depth = visited.constFind(key);
                if (previous_depth != visited.cend() &&
                    (previous_depth.value() < 0 || (remaining_depth >= 0 && previous_depth.value() >= remaining_depth)))
                    continue;
                visited.insert(key, remaining_depth);
                if (!QFileInfo(directory).isReadable()) {
                    ++skipped;
                    continue;
                }
                QDirIterator entries(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
                while (entries.hasNext() && !cancelled()) {
                    entries.next();
                    const auto info = entries.fileInfo();
                    if (excluded_path(exclusions, info.absoluteFilePath()))
                        continue;
                    if (info.isDir()) {
                        if (remaining_depth != 0 && traversable_directory(info))
                            pending.emplace_back(info.absoluteFilePath(), remaining_depth < 0 ? -1 : remaining_depth - 1);
                    } else if (accepts_file(info.absoluteFilePath())) {
                        batch.append({identity(info.absoluteFilePath()), info.fileName()});
                        if (batch.size() >= 256 || batch_age.elapsed() > 100)
                            flush();
                    }
                }
            }
            flush();
            if (cancelled())
                return QCoreApplication::translate("XLibrary", "File indexing cancelled");
            xlibrary::SqliteStatement prune(db.get(), "DELETE FROM files WHERE (scope<>? OR seen<>?) AND NOT "
                                                      "EXISTS(SELECT 1 FROM saved s WHERE s.path=files.path)");
            prune.bind(1, scope);
            prune.bind(2, seen);
            prune.next();
            return QCoreApplication::translate("XLibrary", "Indexed %1 files; inaccessible folders: %2")
                .arg(count->load())
                .arg(skipped);
        } catch (const std::exception &e) {
            return QCoreApplication::translate("XLibrary", "File index error: %1").arg(QString::fromUtf8(e.what()));
        }
    }));
}

void FileManager::search(const QString &text, const quint64 request, QObject *receiver,
                         std::function<void(quint64, QVector<file_match>)> completed,
                         std::shared_ptr<std::atomic<quint64>> field_generation,
                         const QHash<QString, usage_record> &usage) {
    auto *watcher = new QFutureWatcher<QVector<file_match>>(receiver);
    connect(watcher, &QFutureWatcher<QVector<file_match>>::finished, receiver,
            [watcher, request, completed = std::move(completed)] {
                completed(request, watcher->result());
                watcher->deleteLater();
            });
    const auto expected = query_generation->load();
    const auto enabled = is_enabled();
    watcher->setFuture(QtConcurrent::run(&query_pool, [path = database_path, scope = scope, text, usage, enabled, exclusions = excluded_paths,
                                                       cache = search_cache, generation = query_generation, expected,
                                                       field_generation, request] {
        QVector<file_match> matches;
        const auto cancelled = [&] { return generation->load() != expected || field_generation->load() != request; };
        if (!enabled || text.trimmed().isEmpty() || cancelled())
            return matches;
        try {
            auto db = open_index(path);
            const auto pattern = search_query::file_pattern(text.trimmed());
            QString extension;
            const auto query_text = pattern ? text.trimmed() : file_query_text(text, extension);
            const SearchQuery query(query_text);
            const bool has_query = !query_text.isEmpty();
            QStringList preferred{QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
                                  QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)};
            for (auto &root : preferred)
                if (!root.isEmpty())
                    root = identity(root);
            const auto better = [](const file_match &a, const file_match &b) {
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
                return a.path < b.path;
            };
            // Direct queries never pay for edit distance across the full disk index.
            // Only a query with no direct matches needs the abbreviation/typo pass.
            QSet<QString> inspected;
            for (const bool suggestions : {false, true}) {
                if (suggestions && (!matches.isEmpty() || pattern || !has_query))
                    break;
                for (;;) {
                    const auto capacity = 30 - matches.size();
                    QVector<file_match> candidates;
                    xlibrary::SqliteStatement rows(
                        db.get(),
                        "SELECT f.path,f.name,EXISTS(SELECT 1 FROM saved s WHERE s.path=f.path) "
                        "FROM files f WHERE f.scope=? OR EXISTS(SELECT 1 FROM saved s WHERE s.path=f.path) "
                        "UNION ALL SELECT s.path,'',1 FROM saved s WHERE NOT EXISTS(SELECT 1 FROM files f WHERE "
                        "f.path=s.path)");
                    rows.bind(1, scope);
                    while (rows.next()) {
                        if (cancelled())
                            return QVector<file_match>{};
                        const auto file_path = rows.qtext(0);
                        if (excluded_path(exclusions, file_path) || inspected.contains(file_path))
                            continue;
                        const auto stored_name = rows.qtext(1);
                        const auto name = stored_name.isEmpty() ? QFileInfo(file_path).fileName() : stored_name;
                        if (!extension.isEmpty() &&
                            QFileInfo(name).suffix().compare(extension, Qt::CaseInsensitive) != 0)
                            continue;
                        int rank = 0;
                        if (pattern) {
                            if (!pattern->match(name.normalized(QString::NormalizationForm_C)).hasMatch())
                                continue;
                        } else if (has_query) {
                            auto *prepared = cache->object(name);
                            std::optional<SearchQuery::SearchableData> uncached;
                            if (!prepared) {
                                auto data = SearchQuery::prepare(name, {}, {});
                                // Retain the warm prefix instead of thrashing on indexes larger than the cache.
                                if (cache->totalCost() < cache->maxCost()) {
                                    prepared = new SearchQuery::SearchableData(std::move(data));
                                    cache->insert(name, prepared);
                                } else {
                                    uncached = std::move(data);
                                    prepared = &*uncached;
                                }
                            }
                            rank = query.rank(*prepared, suggestions);
                        }
                        if (rank < 0)
                            continue;
                        const auto history = usage.value(file_path);
                        file_match match{file_path,        name, rank, rows.number(2) != 0, true, history.count,
                                         history.last_used};
                        match.depth = static_cast<int>(file_path.count('/'));
                        for (int location = 0; location < preferred.size(); ++location)
                            if (!preferred[location].isEmpty() && file_path.startsWith(preferred[location] + '/')) {
                                match.location = location;
                                match.depth =
                                    static_cast<int>(file_path.mid(preferred[location].size() + 1).count('/'));
                                break;
                            }
                        auto position = std::lower_bound(candidates.begin(), candidates.end(), match, better);
                        if (position != candidates.end() || candidates.size() < capacity) {
                            // Content checks belong to indexing and the final bounded subset, not every competitive row.
                            const QFileInfo info(file_path);
                            if (!info.isFile() || !info.isReadable())
                                continue;
                            candidates.insert(position, std::move(match));
                            if (candidates.size() > capacity)
                                candidates.removeLast();
                        }
                    }
                    for (const auto &entry : candidates) {
                        if (cancelled())
                            return QVector<file_match>{};
                        inspected.insert(entry.path);
                        if (accepts_file(entry.path))
                            matches.append(entry);
                    }
                    // Refill after rejected candidates without reading every matching file's contents.
                    if (matches.size() == 30 || candidates.size() < capacity)
                        break;
                }
                std::sort(matches.begin(), matches.end(), better);
            }
            if (!matches.isEmpty()) {
                const auto best = matches.first().rank;
                matches.removeIf([best](const auto &entry) { return !SearchQuery::accepts_rank(entry.rank, best); });
            }
        } catch (const std::exception &) { /* Index status exposes persistent storage failures. */
        }
        return cancelled() ? QVector<file_match>{} : matches;
    }));
}
QVector<file_match> FileManager::filter_matches(const QString &text, QVector<file_match> matches) {
    if (text.trimmed().isEmpty())
        return {};
    const auto pattern = search_query::file_pattern(text.trimmed());
    QString extension;
    const auto query_text = pattern ? text.trimmed() : file_query_text(text, extension);
    const SearchQuery query(query_text);
    for (auto &match : matches) {
        if (!extension.isEmpty() && QFileInfo(match.name).suffix().compare(extension, Qt::CaseInsensitive) != 0)
            match.rank = -1;
        else if (pattern)
            match.rank = pattern->match(match.name.normalized(QString::NormalizationForm_C)).hasMatch() ? 0 : -1;
        else
            match.rank = query.rank(match.name, {}, {});
    }
    matches.removeIf([](const auto &match) { return match.rank < 0; });
    if (!matches.isEmpty()) {
        const auto best = std::min_element(matches.begin(), matches.end(), [](const auto &a, const auto &b) {
                              return a.rank < b.rank;
                          })->rank;
        matches.removeIf([best](const auto &match) { return !SearchQuery::accepts_rank(match.rank, best); });
    }
    return matches;
}

void FileManager::saved_files(QObject *receiver, std::function<void(QVector<file_match>)> completed) {
    auto *watcher = new QFutureWatcher<QVector<file_match>>(receiver);
    connect(watcher, &QFutureWatcher<QVector<file_match>>::finished, receiver,
            [watcher, completed = std::move(completed)] {
                completed(watcher->result());
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run(&query_pool, [path = database_path] {
        QVector<file_match> entries;
        try {
            auto db = open_index(path);
            xlibrary::SqliteStatement rows(db.get(), "SELECT path FROM saved ORDER BY path");
            while (rows.next()) {
                const auto file = rows.qtext(0);
                entries.append({file, QFileInfo(file).fileName(), 0, true, accepts_file(file)});
            }
        } catch (const std::exception &) {
        }
        return entries;
    }));
}
void FileManager::set_saved(const QString &path, const bool saved) {
    ++pending_saves;
    emit busy_changed();
    using save_result = QPair<QString, QStringList>;
    auto *watcher = new QFutureWatcher<save_result>(this);
    connect(watcher, &QFutureWatcher<save_result>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.first.isEmpty())
            emit error(result.first);
        else
            emit saved_paths_changed(result.second);
        --pending_saves;
        emit busy_changed();
        emit changed();
    });
    watcher->setFuture(
        QtConcurrent::run(&query_pool, [database = database_path, path = identity(path), saved]() -> save_result {
            try {
                if (saved) {
                    if (!accepts_file(path))
                        return {
                            QCoreApplication::translate("XLibrary", "The file is unavailable or its type is excluded."),
                            {}};

                }
                auto db = open_index(database);
                xlibrary::SqliteStatement statement(db.get(), saved ? "INSERT OR IGNORE INTO saved VALUES(?)"
                                                                    : "DELETE FROM saved WHERE path=?");
                statement.bind(1, path);
                statement.next();
                QStringList paths;
                xlibrary::SqliteStatement rows(db.get(), "SELECT path FROM saved ORDER BY path");
                while (rows.next())
                    paths.append(rows.qtext(0));
                return {{}, paths};
            } catch (const std::exception &e) {
                return {QString::fromUtf8(e.what()), {}};
            }
        }));
}
