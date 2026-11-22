#ifndef _WIN32
#include <sys/stat.h>
#include <sys/vfs.h>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>

#include "FileManager.h"
#include "PathHelpers.h"
#include "SearchQuery.h"
#include "SqliteHelpers.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int refresh_interval_ms = 6 * 60 * 60 * 1000;
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
QStringList saved_paths(sqlite3 *db) {
    xlibrary::SqliteStatement rows(db, "SELECT path FROM saved ORDER BY path");
    QStringList paths;
    while (rows.next())
        paths.append(rows.qtext(0));
    return paths;
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
QPair<QString, QString> split_file_query(const QString &text) {
    QStringList filename_terms;
    QStringList tag_terms;
    for (const auto &term : text.simplified().split(' ', Qt::SkipEmptyParts))
        (term.startsWith('#') || term.startsWith("-#") ? tag_terms : filename_terms).append(term);
    return {filename_terms.join(' '), tag_terms.join(' ')};
}
QString file_query_text(const QString &text, QString &extension) {
    const auto parts = split_file_query(text);
    auto query_text = parts.first;
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
    return (query_text.trimmed() + ' ' + parts.second).trimmed();
}
bool contains_path(const QString &root, const QString &path) {
    return root == path || path.startsWith(root.endsWith('/') ? root : root + '/');
}
bool excluded_path(const QStringList &exclusions, const QString &path) {
    return std::any_of(exclusions.cbegin(), exclusions.cend(),
                       [&](const auto &root) { return contains_path(root, identity(path)); });
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
QStringList volume_identity(const QString &path) {
    const QStorageInfo volume(path);
    if (!volume.isValid() || !volume.isReady() || !local_volume(volume))
        return {};
#ifdef Q_OS_WIN
    return {identity(volume.rootPath()), QString::fromLatin1(volume.device().toHex())};
#else
    // Unix device names can be reused for another drive. Prefer the filesystem UUID and subvolume.
    const auto device = QFileInfo(QString::fromUtf8(volume.device())).canonicalFilePath();
    const auto subvolume = QString::fromLatin1(volume.subvolume().toHex());
    if (!device.isEmpty())
        for (const auto &entry : QDir("/dev/disk/by-uuid").entryInfoList(QDir::Files | QDir::System))
            if (entry.canonicalFilePath() == device)
                return {identity(volume.rootPath()), "uuid:" + entry.fileName() + ':' + subvolume};
    // Filesystems without an exposed UUID remain usable, but never justify destructive cache pruning.
    struct statfs filesystem {};
    QFile boot_id("/proc/sys/kernel/random/boot_id");
    if (::statfs(QFile::encodeName(path).constData(), &filesystem) != 0 || !boot_id.open(QIODevice::ReadOnly))
        return {};
    const auto filesystem_id =
        QByteArray(reinterpret_cast<const char *>(&filesystem.f_fsid), sizeof(filesystem.f_fsid)).toHex();
    return {identity(volume.rootPath()), "session:" + QString::fromUtf8(boot_id.readAll().trimmed()) + ':' +
                                             QString::fromLatin1(volume.device().toHex()) + ':' +
                                             QString::fromLatin1(filesystem_id) + ':' + subvolume};
#endif
}
}

bool FileManager::supports_file_extension(const QString &extension) {
    return extensions().contains(extension.toLower());
}

bool FileManager::accepts_file(const QString &path) {
    const QFileInfo info(path);
    const auto name = info.fileName();
    if (!supports_file_extension(info.suffix()) || name.startsWith('.') || name.startsWith('~') || name.endsWith('~') ||
        info.isHidden() || info.isSymLink() || !info.isFile() || !info.isReadable())
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

bool FileManager::can_open(const QString &path, const QString &volume_root, const QString &volume_device) {
    return (volume_root.isEmpty() || volume_identity(path) == QStringList{volume_root, volume_device}) &&
           accepts_file(path);
}

FileManager::FileManager(const QString &directory, const QStringList &saved_paths, QObject *parent) : QObject(parent) {
    database_path = QDir(directory).filePath("file-index.sqlite");
    scan_pool.setMaxThreadCount(1);
    query_pool.setMaxThreadCount(1);
    refresh.setSingleShot(true);
    connect(&refresh, &QTimer::timeout, this, [this] {
        if (is_enabled() && !scanning)
            start_scan();
    });
    source_poll.setInterval(30000);
    connect(&source_poll, &QTimer::timeout, this, &FileManager::check_sources);
    progress_timer.setInterval(1000);
    connect(&progress_timer, &QTimer::timeout, this, [this] {
        state = QCoreApplication::translate("XLibrary", "Indexing files: %1").arg(indexed->load());
        emit status_changed();
    });
    // Create the small schema before workers are started; all enumeration and queries run off the GUI thread.
    try {
        QDir().mkpath(directory);
        auto db = open_index(database_path);
        xlibrary::sqlite_execute(
            db.get(),
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY,name TEXT NOT "
            "NULL,scope TEXT NOT NULL,seen INTEGER NOT NULL,added INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE IF NOT EXISTS saved(path TEXT PRIMARY KEY);"
            "CREATE INDEX IF NOT EXISTS files_scope ON files(scope);"
            "CREATE TABLE IF NOT EXISTS scan_state(scope TEXT PRIMARY KEY,completed INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS index_sources(scope TEXT NOT NULL,root TEXT NOT NULL,depth INTEGER NOT NULL,"
            "volume_root TEXT NOT NULL,volume_device TEXT NOT NULL,attempted INTEGER NOT NULL,"
            "completed INTEGER NOT NULL,state TEXT NOT NULL,issues TEXT NOT NULL,PRIMARY KEY(scope,root));");
        bool has_added = false;
        {
            xlibrary::SqliteStatement columns(db.get(), "PRAGMA table_info(files)");
            while (columns.next())
                has_added |= columns.qtext(1) == "added";
        }
        if (!has_added) {
            // Older indexes retained only their last scan time, the earliest available approximation.
            xlibrary::sqlite_execute(db.get(), "BEGIN IMMEDIATE;"
                                               "ALTER TABLE files ADD COLUMN added INTEGER NOT NULL DEFAULT 0;"
                                               "UPDATE files SET added=seen; COMMIT");
        }
        for (const auto *column : {"volume_root", "volume_device"}) {
            bool present = false;
            xlibrary::SqliteStatement columns(db.get(), "PRAGMA table_info(files)");
            while (columns.next())
                present |= columns.qtext(1) == QLatin1String(column);
            if (!present)
                xlibrary::sqlite_execute(
                    db.get(), (QStringLiteral("ALTER TABLE files ADD COLUMN ") + column + " TEXT NOT NULL DEFAULT ''")
                                  .toUtf8()
                                  .constData());
        }
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
    source_poll.stop();
    progress_timer.stop();
    rescan_pending = false;
    pending_volume_change = false;
    ++*scan_generation;
    ++*query_generation;
    scan_pool.clear();
    scan_pool.waitForDone();
    // Mutation tasks must drain even when their GUI completion callbacks cannot run during shutdown.
    query_pool.waitForDone();
}
QStringList FileManager::persisted_saved_paths() const {
    auto db = open_index(database_path);
    return saved_paths(db.get());
}
file_search_mode FileManager::mode_from_setting(const QString &value) {
    if (value == "basic")
        return file_search_mode::basic;
    if (value == "full")
        return file_search_mode::full;
    return file_search_mode::off;
}

void FileManager::load_source_states() {
    source_states.clear();
    auto db = open_index(database_path);
    xlibrary::SqliteStatement rows(db.get(),
                                   "SELECT root,depth,volume_root,volume_device,attempted,completed,state,issues "
                                   "FROM index_sources WHERE scope=? ORDER BY root");
    rows.bind(1, scope);
    while (rows.next()) {
        file_index_source source{rows.qtext(0),  static_cast<int>(rows.number(1)),
                                 rows.qtext(2),  rows.qtext(3),
                                 rows.number(4), rows.number(5),
                                 rows.qtext(6),  {}};
        for (const auto &entry : QJsonDocument::fromJson(rows.qtext(7).toUtf8()).array())
            source.unavailable_directories.append(entry.toString());
        source_states.append(std::move(source));
    }
}

QString FileManager::diagnostics_text() const {
    QStringList lines{status()};
    if (!is_enabled())
        return lines.join('\n');
    if (source_states.isEmpty())
        lines.append(QCoreApplication::translate("XLibrary", "No completed source scan. Refresh required."));
    const auto date = [](const qint64 value) {
        return value ? QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(value), QLocale::ShortFormat)
                     : QCoreApplication::translate("XLibrary", "Never");
    };
    for (const auto &source : source_states) {
        const bool stale =
            !source.completed || QDateTime::currentMSecsSinceEpoch() - source.completed >= refresh_interval_ms;
        const auto description =
            source.state == "unavailable"
                ? QCoreApplication::translate("XLibrary", "Source unavailable; cached results retained")
            : source.state != "complete" || stale
                ? QCoreApplication::translate("XLibrary", "Refresh required; search may be incomplete")
                : QCoreApplication::translate("XLibrary", "Last scan complete");
        lines.append('\n' + QDir::toNativeSeparators(source.root) + " — " + description);
        lines.append(QCoreApplication::translate("XLibrary", "Last attempt: %1. Last complete scan: %2.")
                         .arg(date(source.attempted), date(source.completed)));
        if (!source.unavailable_directories.isEmpty()) {
            lines.append(QCoreApplication::translate("XLibrary", "Unavailable folders: %1")
                             .arg(source.unavailable_directories.size()));
            for (const auto &directory : source.unavailable_directories.mid(0, 200))
                lines.append("  " + QDir::toNativeSeparators(directory));
            if (source.unavailable_directories.size() > 200)
                lines.append(QCoreApplication::translate("XLibrary", "Only the first 200 folders are shown."));
        }
    }
    return lines.join('\n');
}

void FileManager::configure(const file_search_mode selected_mode, const QStringList &roots,
                            const QStringList &exclusions) {
    mode = selected_mode;
    pending_volume_change = false;
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
    progress_timer.stop();
    const auto scope_data = QString::number(static_cast<int>(mode)) + '\n' + custom_roots.join('\n') +
                            "\n--exclusions--\n" + excluded_paths.join('\n');
    scope = QString::fromLatin1(QCryptographicHash::hash(scope_data.toUtf8(), QCryptographicHash::Sha256).toHex());
    refresh.stop();
    source_poll.stop();
    last_refresh = 0;
    source_states.clear();
    if (is_enabled()) {
        try {
            auto db = open_index(database_path);
            xlibrary::SqliteStatement completed(db.get(), "SELECT completed FROM scan_state WHERE scope=?");
            completed.bind(1, scope);
            if (completed.next())
                last_refresh = completed.number(0);
            load_source_states();
        } catch (const std::exception &e) {
            emit error(QString::fromUtf8(e.what()));
        }
    }
    rescan_pending = is_enabled() && (scanning || !last_refresh || source_states.isEmpty() ||
                                      std::any_of(source_states.cbegin(), source_states.cend(),
                                                  [](const auto &source) { return source.state != "complete"; }) ||
                                      QDateTime::currentMSecsSinceEpoch() - last_refresh >= refresh_interval_ms);
    if (!is_enabled()) {
        state = QCoreApplication::translate("XLibrary", "File search is off");
        emit status_changed();
    }
    emit configuration_changed();
    emit changed();
    if (is_enabled()) {
        source_poll.start();
        QTimer::singleShot(0, this, &FileManager::check_sources);
    }
    if (rescan_pending && !scanning)
        start_scan();
    else if (is_enabled() && !scanning) {
        state = QCoreApplication::translate("XLibrary", "File index is up to date");
        emit status_changed();
        schedule_refresh();
    }
}
void FileManager::schedule_refresh() {
    if (!is_enabled())
        return;
    const auto elapsed = QDateTime::currentMSecsSinceEpoch() - last_refresh;
    refresh.start(static_cast<int>(last_refresh
                                       ? std::clamp<qint64>(refresh_interval_ms - elapsed, 1, refresh_interval_ms)
                                       : refresh_interval_ms));
}
void FileManager::rescan() {
    if (!is_enabled())
        return;
    if (scanning) {
        rescan_pending = true;
        pending_volume_change = true;
        return;
    }
    start_scan({}, true);
}

void FileManager::check_sources() {
    if (!is_enabled() || scanning || checking_sources)
        return;
    checking_sources = true;
    struct probe_result {
        QVector<file_index_source> sources;
        QStringList returned;
        QString error;
    };
    const auto expected = scan_generation->load();
    auto *watcher = new QFutureWatcher<probe_result>(this);
    connect(watcher, &QFutureWatcher<probe_result>::finished, this, [this, watcher, expected] {
        const auto result = watcher->result();
        watcher->deleteLater();
        checking_sources = false;
        if (expected != scan_generation->load() || !is_enabled())
            return;
        if (!result.error.isEmpty()) {
            state = QCoreApplication::translate("XLibrary", "File index error: %1").arg(result.error);
            emit status_changed();
            return;
        }
        source_states = result.sources;
        emit status_changed();
        // A manual full scan or configuration change takes precedence over this probe.
        if (!scanning && !result.returned.isEmpty())
            start_scan(result.returned);
    });
    watcher->setFuture(QtConcurrent::run(
        &scan_pool,
        [path = database_path, scope = scope, sources = source_states, exclusions = excluded_paths,
         generation = scan_generation, expected]() mutable -> probe_result {
            probe_result result;
            try {
                auto db = open_index(path);
                for (auto &source : sources) {
                    if (generation->load() != expected)
                        return result;
                    const auto volume = volume_identity(source.root);
                    const bool available = volume.size() == 2 && traversable_directory(QFileInfo(source.root)) &&
                                           QFileInfo(source.root).isReadable() &&
                                           (source.volume_root.isEmpty() ||
                                            volume == QStringList{source.volume_root, source.volume_device});
                    if (available && source.state == "unavailable")
                        result.returned.append(source.root);
                    else if (!available)
                        source.state = "unavailable";
                    xlibrary::SqliteStatement update(db.get(),
                                                     "UPDATE index_sources SET state=? WHERE scope=? AND root=?");
                    update.bind(1, source.state);
                    update.bind(2, scope);
                    update.bind(3, source.root);
                    update.next();
                }
                // Newly mounted local volumes are also part of Basic/Full scope.
                for (const auto &root : volume_roots())
                    if (!excluded_path(exclusions, root) &&
                        std::none_of(sources.cbegin(), sources.cend(),
                                     [&](const auto &source) { return source.root == root; }))
                        result.returned.append(root);
                result.sources = std::move(sources);
            } catch (const std::exception &error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
}

void FileManager::start_scan(const QStringList &only_roots, const bool allow_volume_change) {
    refresh.stop();
    rescan_pending = false;
    scanning = true;
    indexed->store(0);
    state = QCoreApplication::translate("XLibrary", "Indexing files: %1").arg(0);
    progress_timer.start();
    emit status_changed();
    using scan_result = QPair<QString, qint64>;
    const auto expected = scan_generation->load();
    auto *watcher = new QFutureWatcher<scan_result>(this);
    connect(watcher, &QFutureWatcher<scan_result>::finished, this,
            [this, watcher, expected, partial = !only_roots.isEmpty()] {
                const auto result = watcher->result();
                watcher->deleteLater();
                scanning = false;
                progress_timer.stop();
                if (expected == scan_generation->load()) {
                    state = result.first;
                    if (result.second && !partial)
                        last_refresh = result.second;
                    try {
                        load_source_states();
                    } catch (const std::exception &error) {
                        state = QCoreApplication::translate("XLibrary", "File index error: %1")
                                    .arg(QString::fromUtf8(error.what()));
                    }
                    emit status_changed();
                }
                if (rescan_pending) {
                    const auto allow_change = pending_volume_change;
                    pending_volume_change = false;
                    start_scan({}, allow_change);
                } else if (expected == scan_generation->load() && !result.second && is_enabled())
                    refresh.start(refresh_interval_ms);
                else
                    schedule_refresh();
            });
    const auto requested_roots = custom_roots;
    const auto selected_mode = mode;
    const auto exclusions = excluded_paths;
    watcher->setFuture(QtConcurrent::run(
        &scan_pool,
        [path = database_path, scope = scope, requested_roots, selected_mode, exclusions, generation = scan_generation,
         expected, count = indexed, only_roots, allow_volume_change]() -> scan_result {
            const auto roots = volume_roots();
            QStringList blocked_mounts;
            for (const auto &volume : QStorageInfo::mountedVolumes())
                if (!local_volume(volume))
                    blocked_mounts.append(identity(volume.rootPath()));
#ifndef Q_OS_WIN
            // Qt may omit virtual filesystems from mountedVolumes().
            blocked_mounts += QStringList{"/proc", "/sys", "/dev"};
#endif
            const auto cancelled = [&] { return generation->load() != expected; };
            try {
                auto db = open_index(path);
                // Each batch rewrites scope ownership, so other scope timestamps become invalid immediately.
                xlibrary::SqliteStatement invalidate(db.get(), "DELETE FROM scan_state WHERE scope<>?");
                invalidate.bind(1, scope);
                invalidate.next();
                const auto seen = QDateTime::currentMSecsSinceEpoch();
                std::deque<QPair<QString, int>> pending;
                QHash<QString, int> visited;
                QHash<QString, QStringList> completed_directories;
                QSet<QString> unavailable_directories;
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
                QMap<QString, int> source_roots;
                for (const auto &root : recursive_roots)
                    source_roots.insert(identity(root), -1);
                if (selected_mode == file_search_mode::basic)
                    for (const auto &root : roots)
                        if (!source_roots.contains(root))
                            source_roots.insert(root, 1);
                QHash<QString, QStringList> known_volumes;
                // A scope edit must not discard offline volumes that still belong to Basic/Full mode.
                xlibrary::SqliteStatement previous_sources(
                    db.get(),
                    "SELECT root,depth,volume_root,volume_device,scope FROM index_sources ORDER BY attempted DESC");
                while (previous_sources.next()) {
                    const auto root = previous_sources.qtext(0);
                    const QStringList volume{previous_sources.qtext(2), previous_sources.qtext(3)};
                    if (!volume[0].isEmpty()) {
                        if (!known_volumes.contains(root))
                            known_volumes.insert(root, volume);
                        if (!known_volumes.contains(volume[0]))
                            known_volumes.insert(volume[0], volume);
                        if (!source_roots.contains(volume[0]))
                            source_roots.insert(volume[0], selected_mode == file_search_mode::basic ? 1 : -1);
                    }
                    if (previous_sources.qtext(4) == scope && !source_roots.contains(root))
                        source_roots.insert(root, static_cast<int>(previous_sources.number(1)));
                }
                xlibrary::SqliteStatement cached_volumes(
                    db.get(), "SELECT volume_root,volume_device FROM files "
                              "WHERE volume_root<>'' GROUP BY volume_root,volume_device ORDER BY MAX(seen) DESC");
                while (cached_volumes.next()) {
                    const auto root = cached_volumes.qtext(0);
                    if (!known_volumes.contains(root))
                        known_volumes.insert(root, QStringList{root, cached_volumes.qtext(1)});
                    if (!source_roots.contains(root))
                        source_roots.insert(root, selected_mode == file_search_mode::basic ? 1 : -1);
                }
                for (auto it = source_roots.begin(); it != source_roots.end();) {
                    if (excluded_path(exclusions, it.key()) ||
                        (!only_roots.isEmpty() && !only_roots.contains(it.key())))
                        it = source_roots.erase(it);
                    else
                        ++it;
                }
                const auto in_scope = [&](const QString &file_path) {
                    if (excluded_path(exclusions, file_path))
                        return false;
                    for (auto it = source_roots.cbegin(); it != source_roots.cend(); ++it) {
                        if (!contains_path(it.key(), file_path))
                            continue;
                        const auto relative = file_path.mid(it.key().size() + (it.key().endsWith('/') ? 0 : 1));
                        if (it.value() < 0 || relative.count('/') <= it.value())
                            return true;
                    }
                    return false;
                };
                if (only_roots.isEmpty()) {
                    QStringList retained;
                    QStringList excluded;
                    xlibrary::SqliteStatement old_files(db.get(), "SELECT path FROM files WHERE scope<>?");
                    old_files.bind(1, scope);
                    while (old_files.next() && !cancelled())
                        (in_scope(old_files.qtext(0)) ? retained : excluded).append(old_files.qtext(0));
                    xlibrary::sqlite_execute(db.get(), "BEGIN IMMEDIATE");
                    for (const auto &file_path : retained) {
                        xlibrary::SqliteStatement move(db.get(), "UPDATE files SET scope=? WHERE path=?");
                        move.bind(1, scope);
                        move.bind(2, file_path);
                        move.next();
                    }
                    for (const auto &file_path : excluded) {
                        xlibrary::SqliteStatement remove(db.get(),
                                                         "DELETE FROM files WHERE path=? AND NOT EXISTS(SELECT 1 FROM "
                                                         "saved WHERE saved.path=files.path)");
                        remove.bind(1, file_path);
                        remove.next();
                    }
                    xlibrary::sqlite_execute(db.get(), cancelled() ? "ROLLBACK" : "COMMIT");
                    if (cancelled())
                        return {QCoreApplication::translate("XLibrary", "File indexing cancelled"), 0};
                }
                QSet<QString> changed_volumes;
                if (!allow_volume_change)
                    for (auto it = known_volumes.cbegin(); it != known_volumes.cend(); ++it)
                        if (volume_identity(it.key()) != it.value())
                            changed_volumes.insert(it.key());
                for (auto it = source_roots.cbegin(); it != source_roots.cend(); ++it) {
                    xlibrary::SqliteStatement begin_source(
                        db.get(),
                        "INSERT INTO index_sources VALUES(?,?,?,?,?,?,0,'needs_refresh','[]') "
                        "ON CONFLICT(scope,root) DO UPDATE SET state='needs_refresh',attempted=excluded.attempted");
                    begin_source.bind(1, scope);
                    begin_source.bind(2, it.key());
                    begin_source.bind(3, it.value());
                    begin_source.bind(4, known_volumes.value(it.key()).value(0));
                    begin_source.bind(5, known_volumes.value(it.key()).value(1));
                    begin_source.bind(6, seen);
                    begin_source.next();
                }
                const auto add_root = [&](const QString &root, const int depth) {
                    const QFileInfo info(root);
                    const auto canonical = info.canonicalFilePath();
                    if (!canonical.isEmpty() && traversable_directory(info) && !excluded_path(exclusions, root) &&
                        !excluded_path(exclusions, canonical))
                        pending.emplace_back(canonical, depth);
                    else {
                        ++skipped;
                        unavailable_directories.insert(identity(root));
                    }
                };
                for (auto it = source_roots.cbegin(); it != source_roots.cend(); ++it)
                    add_root(it.key(), it.value());
                struct indexed_file {
                    QString path, name, volume_root, volume_device;
                };
                QVector<indexed_file> batch;
                QElapsedTimer batch_age;
                batch_age.start();
                const auto flush = [&] {
                    if (batch.isEmpty())
                        return;
                    xlibrary::sqlite_execute(db.get(), "BEGIN");
                    for (const auto &entry : batch) {
                        xlibrary::SqliteStatement insert(
                            db.get(), "INSERT INTO files(path,name,scope,seen,added,volume_root,volume_device) "
                                      "VALUES(?,?,?,?,?,?,?) ON CONFLICT(path) "
                                      "DO UPDATE SET name=excluded.name,scope=excluded.scope,seen=excluded.seen,"
                                      "volume_root=excluded.volume_root,volume_device=excluded.volume_device,"
                                      "added=CASE WHEN files.added=0 THEN excluded.added ELSE files.added END");
                        insert.bind(1, entry.path);
                        insert.bind(2, entry.name);
                        insert.bind(3, scope);
                        insert.bind(4, seen);
                        insert.bind(5, seen);
                        insert.bind(6, entry.volume_root);
                        insert.bind(7, entry.volume_device);
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
                    const auto directory_path = identity(directory);
                    if (std::any_of(changed_volumes.cbegin(), changed_volumes.cend(),
                                    [&](const auto &root) { return contains_path(root, directory_path); })) {
                        unavailable_directories.insert(directory_path);
                        ++skipped;
                        continue;
                    }
                    if (std::any_of(blocked_mounts.cbegin(), blocked_mounts.cend(), [&](const auto &mount) {
                            if (!contains_path(mount, directory_path))
                                return false;
                            // Local disks can be mounted below a virtual parent, e.g. /run/media.
                            return !std::any_of(roots.cbegin(), roots.cend(), [&](const auto &root) {
                                return root.size() > mount.size() && contains_path(mount, root) &&
                                       contains_path(root, directory_path);
                            });
                        })) {
                        continue;
                    }
                    if (batch_age.elapsed() > 100)
                        flush();
                    auto key = identity(directory);
#ifndef Q_OS_WIN
                    struct stat metadata {};
                    if (::stat(QFile::encodeName(directory).constData(), &metadata) != 0) {
                        ++skipped;
                        unavailable_directories.insert(directory_path);
                        continue;
                    }
                    key = QString::number(metadata.st_dev) + ':' + QString::number(metadata.st_ino);
#endif
                    const auto previous_depth = visited.constFind(key);
                    if (previous_depth != visited.cend() &&
                        (previous_depth.value() < 0 ||
                         (remaining_depth >= 0 && previous_depth.value() >= remaining_depth)))
                        continue;
                    visited.insert(key, remaining_depth);
                    if (!QFileInfo(directory).isReadable()) {
                        ++skipped;
                        unavailable_directories.insert(directory_path);
                        continue;
                    }
                    const auto directory_volume = volume_identity(directory);
                    std::error_code enumeration_error;
                    std::filesystem::directory_iterator entries(filesystem_path(directory), enumeration_error);
                    const std::filesystem::directory_iterator end;
                    for (; !enumeration_error && entries != end && !cancelled(); entries.increment(enumeration_error)) {
                        const QFileInfo info(path_text(entries->path()));
                        if (excluded_path(exclusions, info.absoluteFilePath()))
                            continue;
                        if (info.isDir()) {
                            if (remaining_depth != 0 && traversable_directory(info))
                                pending.emplace_back(info.absoluteFilePath(),
                                                     remaining_depth < 0 ? -1 : remaining_depth - 1);
                        } else if (accepts_file(info.absoluteFilePath())) {
                            if (directory_volume.size() != 2)
                                continue;
                            batch.append({identity(info.absoluteFilePath()), info.fileName(), directory_volume[0],
                                          directory_volume[1]});
                            if (batch.size() >= 256 || batch_age.elapsed() > 100)
                                flush();
                        }
                    }
                    if (!enumeration_error && directory_volume.size() == 2 &&
                        volume_identity(directory) == directory_volume)
                        completed_directories.insert(identity(directory), directory_volume);
                    else {
                        ++skipped;
                        unavailable_directories.insert(directory_path);
                    }
                }
                flush();
                if (cancelled())
                    return {QCoreApplication::translate("XLibrary", "File indexing cancelled"), 0};
                xlibrary::sqlite_execute(db.get(), "BEGIN IMMEDIATE");
                // Only a complete enumeration on the original volume proves that a cached file disappeared.
                QStringList obsolete;
                QHash<QString, QStringList> current_volumes;
                {
                    xlibrary::SqliteStatement stale(
                        db.get(),
                        "SELECT path,scope,volume_root,volume_device FROM files "
                        "WHERE (scope<>? OR seen<>?) AND NOT EXISTS(SELECT 1 FROM saved s WHERE s.path=files.path)");
                    stale.bind(1, scope);
                    stale.bind(2, seen);
                    while (stale.next() && !cancelled()) {
                        const auto file_path = stale.qtext(0);
                        const auto parent = identity(QFileInfo(file_path).absolutePath());
                        const QStringList previous_volume{stale.qtext(2), stale.qtext(3)};
                        std::error_code absence_error;
                        const auto file_status =
                            std::filesystem::symlink_status(filesystem_path(file_path), absence_error);
                        const bool absent = file_status.type() == std::filesystem::file_type::not_found &&
                                            (!absence_error || absence_error == std::errc::no_such_file_or_directory);
                        if (completed_directories.contains(parent) && !current_volumes.contains(parent))
                            current_volumes.insert(parent, volume_identity(parent));
                        const bool stable_volume =
#ifdef Q_OS_WIN
                            !previous_volume[1].isEmpty();
#else
                            previous_volume[1].startsWith("uuid:");
#endif
                        if (absent && stable_volume && !previous_volume[0].isEmpty() &&
                            completed_directories.value(parent) == previous_volume &&
                            current_volumes.value(parent) == previous_volume)
                            obsolete.append(file_path);
                    }
                }
                for (const auto &file_path : obsolete) {
                    xlibrary::SqliteStatement prune(db.get(), "DELETE FROM files WHERE path=?");
                    prune.bind(1, file_path);
                    prune.next();
                }
                xlibrary::SqliteStatement forget(db.get(), "DELETE FROM scan_state WHERE scope<>?");
                forget.bind(1, scope);
                forget.next();
                const auto completed = QDateTime::currentMSecsSinceEpoch();
                for (auto it = source_roots.cbegin(); it != source_roots.cend(); ++it) {
                    QJsonArray issues;
                    QStringList paths;
                    for (const auto &directory : unavailable_directories)
                        if (contains_path(it.key(), directory) &&
                            (it.value() < 0 ||
                             directory.mid(it.key().size() + (it.key().endsWith('/') ? 0 : 1)).count('/') <
                                 it.value() ||
                             directory == it.key()))
                            paths.append(directory);
                    paths.sort();
                    for (const auto &directory : paths)
                        issues.append(directory);
                    const auto volume = volume_identity(it.key());
                    const bool available = volume.size() == 2 && completed_directories.contains(it.key()) &&
                                           completed_directories.value(it.key()) == volume;
                    const bool complete = available && paths.isEmpty();
                    xlibrary::SqliteStatement finish_source(
                        db.get(),
                        "UPDATE index_sources SET "
                        "volume_root=CASE WHEN ? THEN ? ELSE volume_root END,"
                        "volume_device=CASE WHEN ? THEN ? ELSE volume_device END,"
                        "completed=CASE WHEN ? THEN ? ELSE completed END,state=?,issues=? WHERE scope=? AND root=?");
                    finish_source.bind(1, available);
                    finish_source.bind(2, volume.value(0));
                    finish_source.bind(3, available);
                    finish_source.bind(4, volume.value(1));
                    finish_source.bind(5, complete);
                    finish_source.bind(6, completed);
                    finish_source.bind(7, !available ? "unavailable" : complete ? "complete" : "needs_refresh");
                    finish_source.bind(8, QString::fromUtf8(QJsonDocument(issues).toJson(QJsonDocument::Compact)));
                    finish_source.bind(9, scope);
                    finish_source.bind(10, it.key());
                    finish_source.next();
                }
                xlibrary::SqliteStatement source_health(db.get(), "SELECT MIN(completed),SUM(state<>'complete') "
                                                                  "FROM index_sources WHERE scope=?");
                source_health.bind(1, scope);
                source_health.next();
                const auto full_completed = source_health.number(1) == 0 ? source_health.number(0) : 0;
                xlibrary::SqliteStatement remember(db.get(), "INSERT INTO scan_state VALUES(?,?) ON CONFLICT(scope) "
                                                             "DO UPDATE SET completed=excluded.completed");
                remember.bind(1, scope);
                remember.bind(2, full_completed);
                remember.next();
                if (cancelled()) {
                    xlibrary::sqlite_execute(db.get(), "ROLLBACK");
                    return {QCoreApplication::translate("XLibrary", "File indexing cancelled"), 0};
                }
                xlibrary::sqlite_execute(db.get(), "COMMIT");
                return {QCoreApplication::translate("XLibrary", "Indexed %1 files; inaccessible folders: %2")
                            .arg(count->load())
                            .arg(skipped),
                        completed};
            } catch (const std::exception &e) {
                return {
                    QCoreApplication::translate("XLibrary", "File index error: %1").arg(QString::fromUtf8(e.what())),
                    0};
            }
        }));
}

void FileManager::search(const QString &text, const quint64 request, QObject *receiver,
                         std::function<void(quint64, QVector<file_match>)> completed,
                         std::shared_ptr<std::atomic<quint64>> field_generation,
                         const QHash<QString, usage_record> &usage, const QStringList &tags, const bool favorites_only,
                         const int limit, const int sort_mode) {
    auto *watcher = new QFutureWatcher<QVector<file_match>>(receiver);
    connect(watcher, &QFutureWatcher<QVector<file_match>>::finished, receiver,
            [watcher, request, completed = std::move(completed)] {
                completed(request, watcher->result());
                watcher->deleteLater();
            });
    const auto expected = query_generation->load();
    const auto enabled = is_enabled();
    watcher->setFuture(QtConcurrent::run(&query_pool, [path = database_path, scope = scope, text, usage, enabled,
                                                       exclusions = excluded_paths, cache = search_cache,
                                                       generation = query_generation, expected, field_generation,
                                                       request, metadata = metadata, tags, favorites_only,
                                                       limit = qBound(1, limit, max_result_limit), sort_mode] {
        QVector<file_match> matches;
        const auto cancelled = [&] { return generation->load() != expected || field_generation->load() != request; };
        if (!enabled || cancelled())
            return matches;
        try {
            auto db = open_index(path);
            const auto parts = split_file_query(text);
            const auto pattern = search_query::file_pattern(parts.first);
            QString extension;
            const auto query_text = pattern ? parts.second : file_query_text(text, extension);
            const SearchQuery query(query_text);
            const bool has_query = !query_text.isEmpty();
            QStringList preferred{QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
                                  QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)};
            for (auto &root : preferred)
                if (!root.isEmpty())
                    root = identity(root);
            const auto ordered = [sort_mode](const file_match &a, const file_match &b, const bool rank_results) {
                if (rank_results && a.rank != b.rank)
                    return a.rank < b.rank;
                if (const auto comparison = compare_file_order(a.extension, a.added, b.extension, b.added, sort_mode))
                    return comparison < 0;
                if (sort_mode == 1) {
                    const auto comparison = QString::compare(a.name, b.name, Qt::CaseInsensitive);
                    if (comparison != 0)
                        return comparison < 0;
                }
                if (sort_mode == 2 && a.last_used != b.last_used)
                    return a.last_used > b.last_used;
                if (a.favorite != b.favorite)
                    return a.favorite;
                if (sort_mode == 4) {
                    const auto comparison = QString::compare(a.tags.join(','), b.tags.join(','), Qt::CaseInsensitive);
                    if (comparison != 0)
                        return comparison < 0;
                }
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
            const auto better = [&](const file_match &a, const file_match &b) { return ordered(a, b, true); };
            // Direct queries never pay for edit distance across the full disk index.
            // Only a query with no direct matches needs the abbreviation/typo pass.
            QSet<QString> inspected;
            for (const bool suggestions : {false, true}) {
                if (suggestions && (!matches.isEmpty() || pattern || !has_query))
                    break;
                for (;;) {
                    const auto capacity = limit - matches.size();
                    QVector<file_match> candidates;
                    candidates.reserve(capacity);
                    xlibrary::SqliteStatement rows(
                        db.get(), "SELECT f.path,f.name,EXISTS(SELECT 1 FROM saved s WHERE "
                                  "s.path=f.path),f.added,f.volume_root,f.volume_device "
                                  "FROM files f WHERE f.scope=? OR EXISTS(SELECT 1 FROM saved s WHERE s.path=f.path) "
                                  "UNION ALL SELECT s.path,'',1,0,'','' FROM saved s WHERE NOT EXISTS(SELECT 1 FROM "
                                  "files f WHERE "
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
                        const auto properties = metadata.value(file_path).toMap();
                        const auto file_tags = properties.value("tags").toStringList();
                        const bool favorite = properties.value("favorite").toBool();
                        if ((favorites_only && !favorite) ||
                            !std::all_of(tags.cbegin(), tags.cend(),
                                         [&](const auto &tag) { return has_tag(file_tags, tag); }))
                            continue;
                        if (!extension.isEmpty() &&
                            QFileInfo(name).suffix().compare(extension, Qt::CaseInsensitive) != 0)
                            continue;
                        int rank = 0;
                        if (pattern) {
                            if (!pattern->match(name.normalized(QString::NormalizationForm_C)).hasMatch())
                                continue;
                            rank = query.rank({}, {}, file_tags);
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
                            rank = file_tags.isEmpty()
                                       ? query.rank(*prepared, suggestions)
                                       : query.rank(SearchQuery::prepare(name, {}, file_tags), suggestions);
                        }
                        if (rank < 0)
                            continue;
                        const auto history = usage.value(file_path);
                        file_match match{file_path,        name, rank, rows.number(2) != 0, true, history.count,
                                         history.last_used};
                        match.tags = file_tags;
                        match.favorite = favorite;
                        match.extension = QFileInfo(name).suffix().toCaseFolded();
                        match.added = rows.number(3);
                        match.volume_root = rows.qtext(4);
                        match.volume_device = rows.qtext(5);
                        match.depth = static_cast<int>(file_path.count('/'));
                        for (int location = 0; location < preferred.size(); ++location)
                            if (!preferred[location].isEmpty() && file_path.startsWith(preferred[location] + '/')) {
                                match.location = location;
                                match.depth =
                                    static_cast<int>(file_path.mid(preferred[location].size() + 1).count('/'));
                                break;
                            }
                        // Keep the worst candidate at the top for logarithmic replacement.
                        if (candidates.size() == capacity) {
                            if (!better(match, candidates.front()))
                                continue;
                            std::pop_heap(candidates.begin(), candidates.end(), better);
                            candidates.removeLast();
                        }
                        candidates.append(std::move(match));
                        std::push_heap(candidates.begin(), candidates.end(), better);
                    }
                    // Only probe files that survived ranking. Availability does not affect their order.
                    for (auto &entry : candidates) {
                        if (cancelled())
                            return QVector<file_match>{};
                        inspected.insert(entry.path);
                        const QFileInfo info(entry.path);
                        entry.available =
                            info.isFile() && info.isReadable() &&
                            (entry.volume_root.isEmpty() ||
                             volume_identity(entry.path) == QStringList{entry.volume_root, entry.volume_device});
                        entry.modified = info.lastModified().toMSecsSinceEpoch();
                        entry.size = info.size();
                        if (!entry.available ||
                            (accepts_file(entry.path) && !excluded_path(exclusions, info.canonicalFilePath())))
                            matches.append(std::move(entry));
                    }
                    // Refill after rejected candidates without reading every matching file's contents.
                    if (matches.size() == limit || candidates.size() < capacity)
                        break;
                }
                std::sort(matches.begin(), matches.end(), better);
            }
            if (!matches.isEmpty()) {
                const auto best = matches.first().rank;
                matches.removeIf([best](const auto &entry) { return !SearchQuery::accepts_rank(entry.rank, best); });
            }
            // Matching chooses the bounded relevant subset; presentation groups extensions across match ranks.
            if (sort_mode == 0 || sort_mode == 5 || sort_mode == 6)
                std::sort(matches.begin(), matches.end(),
                          [&](const auto &a, const auto &b) { return ordered(a, b, false); });
        } catch (const std::exception &) { /* Index status exposes persistent storage failures. */
        }
        return cancelled() ? QVector<file_match>{} : matches;
    }));
}
int FileManager::compare_file_order(const QString &left_extension, const qint64 left_added,
                                    const QString &right_extension, const qint64 right_added, const int sort_mode) {
    if (sort_mode == 0 || sort_mode == 5)
        if (const auto comparison = QString::compare(left_extension, right_extension, Qt::CaseInsensitive))
            return comparison;
    if ((sort_mode == 0 || sort_mode == 5 || sort_mode == 6) && left_added != right_added)
        return left_added > right_added ? -1 : 1;
    return 0;
}

void FileManager::directory_dates(const QString &request, QObject *const receiver,
                                  std::function<void(QHash<QString, qint64>)> completed) {
    auto *watcher = new QFutureWatcher<QHash<QString, qint64>>(receiver);
    connect(watcher, &QFutureWatcher<QHash<QString, qint64>>::finished, receiver,
            [watcher, completed = std::move(completed)] {
                completed(watcher->result());
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run(&query_pool, [database = database_path, request] {
        QHash<QString, qint64> result;
        const QFileInfo target(request);
        auto root = identity(target.isDir() ? target.absoluteFilePath() : target.absolutePath());
        if (!root.endsWith('/'))
            root += '/';
        auto pattern = root;
        pattern.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
        pattern += '%';
        try {
            auto db = open_index(database);
            xlibrary::SqliteStatement rows(db.get(), "SELECT path,added FROM files WHERE path LIKE ? ESCAPE '\\' "
                                                     "AND instr(substr(path,?),'/')=0");
            rows.bind(1, pattern);
            rows.bind(2, root.size() + 1);
            while (rows.next())
                result.insert(rows.qtext(0), rows.number(1));
        } catch (const std::exception &) {
        }
        return result;
    }));
}

QVector<file_match> FileManager::filter_matches(const QString &text, QVector<file_match> matches) {
    if (text.trimmed().isEmpty())
        return matches;
    const auto parts = split_file_query(text);
    const auto pattern = search_query::file_pattern(parts.first);
    QString extension;
    const auto query_text = pattern ? parts.second : file_query_text(text, extension);
    const SearchQuery query(query_text);
    for (auto &match : matches) {
        if (!extension.isEmpty() && QFileInfo(match.name).suffix().compare(extension, Qt::CaseInsensitive) != 0)
            match.rank = -1;
        else if (pattern)
            match.rank = pattern->match(match.name.normalized(QString::NormalizationForm_C)).hasMatch()
                             ? query.rank({}, {}, match.tags)
                             : -1;
        else
            match.rank = query.rank(match.name, {}, match.tags);
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

void FileManager::restore_metadata(const QVariantMap &values) {
    metadata.clear();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!QFileInfo(it.key()).isAbsolute())
            continue;
        const auto properties = it.value().toMap();
        QStringList tags;
        for (const auto &tag : properties.value("tags").toStringList())
            if (valid_tag_name(tag) && !has_tag(tags, tag) && tags.size() < catalog_limits::tags_per_application)
                tags.append(tag);
        metadata.insert(identity(it.key()),
                        QVariantMap{{"tags", tags}, {"favorite", properties.value("favorite").toBool()}});
    }
}

QStringList FileManager::tags_for(const QString &path) const {
    return metadata.value(identity(path)).toMap().value("tags").toStringList();
}

bool FileManager::is_favorite(const QString &path) const {
    return metadata.value(identity(path)).toMap().value("favorite").toBool();
}

QStringList FileManager::tag_names() const {
    QStringList result;
    for (const auto &value : metadata)
        for (const auto &tag : value.toMap().value("tags").toStringList())
            if (!has_tag(result, tag))
                result.append(tag);
    result.sort(Qt::CaseInsensitive);
    return result;
}

void FileManager::rename_tag(const QString &old_name, const QString &new_name) {
    for (auto &value : metadata) {
        auto properties = value.toMap();
        auto tags = properties.value("tags").toStringList();
        if (!has_tag(tags, old_name))
            continue;
        tags.removeIf([&](const auto &tag) { return tag_name_key(tag) == tag_name_key(old_name); });
        if (!new_name.isEmpty() && !has_tag(tags, new_name))
            tags.append(new_name);
        properties.insert("tags", tags);
        value = properties;
    }
    emit metadata_changed();
    emit changed();
}

void FileManager::update_metadata(const QString &path, const QStringList &tags, const bool favorite) {
    if (!QFileInfo(path).isAbsolute() || tags.size() > catalog_limits::tags_per_application ||
        std::any_of(tags.cbegin(), tags.cend(), [](const auto &tag) { return !valid_tag_name(tag); }))
        return;
    const auto key = identity(path);
    if (tags.isEmpty() && !favorite)
        metadata.remove(key);
    else
        metadata.insert(key, QVariantMap{{"tags", tags}, {"favorite", favorite}});
    emit metadata_changed();
    emit changed();
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
QString FileManager::saved_path_identity(const QString &path) {
    return identity(path);
}

namespace {
void inspect_saved_path(const QString &path, const QSet<QString> &existing, file_save_result &result,
                        bool already_validated = false) {
    if (existing.contains(path))
        result.already_saved.append(path);
    else if (already_validated)
        result.added.append(path);
    else if (QFileInfo(path).isDir())
        result.rejected.insert(path, QCoreApplication::translate("XLibrary", "Choose a file inside the folder."));
    else if (!QFileInfo::exists(path))
        result.rejected.insert(
            path, QCoreApplication::translate("XLibrary", "The file no longer exists or is inaccessible."));
    else if (!FileManager::accepts_file(path))
        result.rejected.insert(
            path, QCoreApplication::translate("XLibrary", "The file is unavailable or its type is excluded."));
    else
        result.added.append(path);
}

void normalize_saved_paths(QStringList &paths) {
    paths.removeAll(QString{});
    for (auto &path : paths)
        path = identity(path);
    paths.removeDuplicates();
}
}

void FileManager::inspect_saved_files(QStringList paths, QObject *receiver,
                                      std::function<void(file_save_result)> completed) {
    auto *watcher = new QFutureWatcher<file_save_result>(receiver);
    connect(watcher, &QFutureWatcher<file_save_result>::finished, receiver,
            [watcher, completed = std::move(completed)] {
                watcher->deleteLater();
                completed(watcher->result());
            });
    watcher->setFuture(QtConcurrent::run(&query_pool, [database = database_path, paths = std::move(paths)]() mutable {
        file_save_result result;
        try {
            normalize_saved_paths(paths);
            const auto existing = saved_paths(open_index(database).get());
            const QSet<QString> keys(existing.cbegin(), existing.cend());
            for (const auto &path : paths)
                inspect_saved_path(path, keys, result);
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
}

void FileManager::set_saved(const QString &path, const bool saved) {
    update_saved_files({path}, saved, nullptr);
}

void FileManager::save_files(QStringList paths, QObject *receiver, std::function<void(file_save_result)> completed) {
    update_saved_files(std::move(paths), true, receiver, std::move(completed));
}

void FileManager::update_saved_files(QStringList paths, const bool saved, QObject *receiver,
                                     std::function<void(file_save_result)> completed) {
    const bool single = !completed;
    ++pending_saves;
    emit busy_changed();
    struct save_operation {
        file_save_result outcome;
        QStringList paths;
    };
    auto *watcher = new QFutureWatcher<save_operation>(this);
    connect(watcher, &QFutureWatcher<save_operation>::finished, this,
            [this, watcher, receiver = QPointer<QObject>(receiver), completed = std::move(completed)] {
                auto [outcome, paths] = watcher->result();
                watcher->deleteLater();
                if (outcome.error.isEmpty() && (completed || outcome.rejected.isEmpty()))
                    emit saved_paths_changed(paths);
                else if (!completed && !outcome.error.isEmpty())
                    emit error(outcome.error);
                else if (!completed && !outcome.rejected.isEmpty())
                    emit error(outcome.rejected.cbegin().value());
                --pending_saves;
                emit busy_changed();
                emit changed();
                if (receiver && completed)
                    completed(std::move(outcome));
            });
    watcher->setFuture(QtConcurrent::run(
        &query_pool, [database = database_path, paths = std::move(paths), saved, single]() mutable -> save_operation {
            file_save_result result;
            try {
                normalize_saved_paths(paths);
                if (single && saved && !accepts_file(paths.value(0))) {
                    result.rejected.insert(
                        paths.value(0),
                        QCoreApplication::translate("XLibrary", "The file is unavailable or its type is excluded."));
                    return {std::move(result), {}};
                }
                auto db = open_index(database);
                xlibrary::sqlite_execute(db.get(), "BEGIN IMMEDIATE");
                if (saved) {
                    const auto existing = saved_paths(db.get());
                    const QSet<QString> keys(existing.cbegin(), existing.cend());
                    const auto now = QDateTime::currentMSecsSinceEpoch();
                    for (const auto &path : paths) {
                        const auto added_before = result.added.size();
                        inspect_saved_path(path, keys, result, single);
                        if (result.added.size() == added_before)
                            continue;
                        xlibrary::SqliteStatement insert(db.get(), "INSERT OR IGNORE INTO saved VALUES(?)");
                        insert.bind(1, path);
                        insert.next();
                        xlibrary::SqliteStatement remember(db.get(),
                                                           "INSERT OR IGNORE INTO files(path,name,scope,seen,added) "
                                                           "VALUES(?,?,?,?,?)");
                        remember.bind(1, path);
                        remember.bind(2, QFileInfo(path).fileName());
                        // A saved-only file must not claim membership in the current indexed scope.
                        remember.bind(3, "");
                        remember.bind(4, now);
                        remember.bind(5, now);
                        remember.next();
                    }
                } else {
                    xlibrary::SqliteStatement remove(db.get(), "DELETE FROM saved WHERE path=?");
                    remove.bind(1, paths.value(0));
                    remove.next();
                }
                auto current = saved_paths(db.get());
                xlibrary::sqlite_execute(db.get(), "COMMIT");
                return {std::move(result), std::move(current)};
            } catch (const std::exception &error) {
                // Closing the SQLite connection rolls back an uncommitted transaction.
                result.added.clear();
                result.error = QString::fromUtf8(error.what());
                return {std::move(result), {}};
            }
        }));
}
