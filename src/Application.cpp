#include <algorithm>
#include <iomanip>
#include <ostream>
#include <stdexcept>

#include "Application.h"

std::uint32_t Application::launch() {
    return this->launch({}, this->file_path.parent_path());
}

std::ostream &operator<<(std::ostream &stream, const Application &application) {
    const auto path_utf8 = application.get_file_path().u8string();
    const std::string path_string(path_utf8.begin(), path_utf8.end());

    return stream << "Application object: {\n"
                  << "\tPID: " << application.get_pid() << "\n"
                  << "\tTitle: " << application.get_file_title() << "\n"
                  << "\tPath: " << std::quoted(path_string) << " }";
}

void Application::set_executable(std::filesystem::path const &file_path) {
    if (file_path.empty()) {
        throw std::invalid_argument("Application executable path is empty");
    }
    std::filesystem::path executable_path = std::filesystem::absolute(file_path);

    const auto application_name = this->read_application_name(executable_path);

    if (this->file_path != executable_path) {
        this->process_handle.reset();
        this->pid = 0;
    }
    this->file_path = executable_path;
    this->file_title = application_name;
}

Application::Application() {
    this->set_empty();
}

Application::Application(const std::string &file_title, const std::filesystem::path &executable_path) : Application() {
    if (!executable_path.empty()) {
        this->set_executable(executable_path);
    }
    this->file_title = file_title;
}

Application::Application(const std::filesystem::path &executable_path) {
    this->set_executable(executable_path);
}

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <windows.h>

#include <QSettings>
#endif
#include "PathHelpers.h"

QString Application::target_kind(const app_record &record) {
    return record.target_kind.isEmpty()
               ? (QFileInfo(record.path).suffix() == "desktop" ? QString("desktop") : QString("executable"))
               : record.target_kind;
}

bool Application::file_target(const app_record &record) {
    const auto kind = target_kind(record);
    return kind == "executable" || kind == "desktop";
}

QString Application::process_path(const app_record &record) {
    if (file_target(record))
        return record.path;
    return target_kind(record) == "steam" && QFileInfo(record.executable_path).isAbsolute() ? record.executable_path
                                                                                            : QString{};
}

QString Application::application_path(const app_record &record) {
    if (target_kind(record) == "steam")
        return record.executable_path.isEmpty() ? record.installation_directory : record.executable_path;
    return record.path;
}

void Application::validate_launch_target(const app_record &record) {
    const auto kind = target_kind(record);
    if (record.path.isEmpty() || record.path.size() > catalog_limits::text_length ||
        record.path.contains(QChar::Null) || record.arguments.contains(QChar::Null) ||
        record.working_directory.contains(QChar::Null))
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Launch target is empty, too long, or contains a null character")
                .toStdString());
    // Metadata can travel between Windows and Linux profiles without becoming a launch target.
    static const QRegularExpression absolute_metadata_path(R"(^(/|[A-Za-z]:[/\\]|\\\\[^\\/]+[/\\][^\\/]+))");
    if (record.executable_path.size() > catalog_limits::text_length || record.executable_path.contains(QChar::Null) ||
        (!record.executable_path.isEmpty() && !absolute_metadata_path.match(record.executable_path).hasMatch()))
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Invalid executable metadata path").toStdString());
    if (file_target(record)) {
        if (!QFileInfo(record.path).isAbsolute())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Launch path must be absolute").toStdString());
        if ((kind == "desktop") != record.path.endsWith(".desktop"))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Desktop files require the desktop target type").toStdString());
        return;
    }
    if (!record.arguments.isEmpty() || !record.working_directory.isEmpty())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "This target type does not accept arguments or a working directory")
                .toStdString());
    if (kind == "steam") {
        static const QRegularExpression app_id("^[1-9][0-9]{0,9}$");
        bool numeric = false;
        const auto value = record.path.toULongLong(&numeric);
        if (!app_id.match(record.path).hasMatch() || !numeric || value > 4294967295ULL)
            throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Invalid Steam AppID").toStdString());
    } else if (kind == "system_uri") {
        static const QSet<QString> allowed{
            "ms-settings:",        "ms-settings:bluetooth",    "ms-settings:display",      "ms-settings:sound",
            "ms-settings:network", "ms-settings:appsfeatures", "ms-settings:windowsupdate"};
        if (!allowed.contains(record.path))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Unsupported Windows settings URI").toStdString());
    } else if (kind == "aumid") {
        static const QRegularExpression aumid("^[A-Za-z0-9][A-Za-z0-9._-]{0,199}![A-Za-z0-9][A-Za-z0-9._-]{0,199}$");
        if (!aumid.match(record.path).hasMatch())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Invalid application user model ID").toStdString());
    } else {
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Unsupported launch target type").toStdString());
    }
}

QString Application::launch_key(const app_record &record) {
    const auto kind = target_kind(record);
    return kind + ':' +
           (file_target(record) ? launch_configuration_key(record.path, record.arguments, record.working_directory)
                                : (kind == "aumid" ? record.path.toCaseFolded() : record.path));
}

QString Application::target_key(const app_record &record) {
    const auto kind = target_kind(record);
    return kind + ':' +
           (file_target(record) ? normalized_path(record.path)
            : kind == "aumid"   ? record.path.toCaseFolded()
                                : record.path);
}

bool Application::target_available(const app_record &record) {
    try {
        validate_launch_target(record);
    } catch (const std::exception &) {
        return false;
    }
    if (file_target(record))
        return QFileInfo(record.path).isFile();
    if (target_kind(record) == "steam")
        return !record.installation_directory.isEmpty() && QFileInfo(record.installation_directory).isDir();
#ifdef Q_OS_WIN
    if (target_kind(record) == "aumid") {
        // AppsFolder exposes only installed applications and does not activate them.
        return windows_aumid_available(record.path);
    }
    const QSettings handler("HKEY_CLASSES_ROOT\\ms-settings", QSettings::NativeFormat);
    return handler.contains("URL Protocol") && QFileInfo(qEnvironmentVariable("WINDIR") + "/explorer.exe").isFile();
#else
    return false;
#endif
}

bool Application::definitely_removed(const app_record &record, const QStringList &known_volume_roots,
                                     const QVariantMap &verified_locations) {
    // Shell activation failures and a missing desktop shortcut alone do not prove an uninstall.
    const auto kind = target_kind(record);
    if (kind != "executable" && kind != "steam" && kind != "desktop")
        return false;
    const auto absent_on_local_volume = [&](const QString &text) {
        if (!QFileInfo(text).isAbsolute())
            return false;
        const auto clean = normalized_path(text);
        for (const auto &root : known_volume_roots) {
            const auto prefix = normalized_path(root.endsWith('/') ? root : root + '/');
            if (clean == prefix || clean.startsWith(prefix.endsWith('/') ? prefix : prefix + '/')) {
                const QStorageInfo volume(root);
                if (!volume.isValid() || !volume.isReady() ||
                    normalized_path(volume.rootPath()) != normalized_path(root))
                    return false;
            }
        }
        const QStorageInfo volume(text);
        if (!volume.isValid() || !volume.isReady())
            return false;
        const auto verified = verified_locations.value(clean).toStringList();
        // The nearest surviving parent can belong to a different disk after an unmount.
        if (verified.size() != 2 || verified[0] != normalized_path(volume.rootPath()) ||
            verified[1] != QString::fromLatin1(volume.device().toHex()))
            return false;
#ifdef Q_OS_WIN
        if (GetDriveTypeW(QDir::toNativeSeparators(volume.rootPath()).toStdWString().c_str()) != DRIVE_FIXED)
            return false;
#else
        const auto type = volume.fileSystemType();
        if (type != "ext4" && type != "ext3" && type != "ext2" && type != "btrfs" && type != "xfs" && type != "zfs" &&
            type != "f2fs" && type != "ntfs3" && type != "overlay")
            return false;
#endif
        auto path = filesystem_path(text);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (status.type() != std::filesystem::file_type::not_found ||
            (error && error != std::errc::no_such_file_or_directory))
            return false;
        // A broken symlink, inaccessible ancestor or unavailable mount must be kept.
        bool readable_parent = false;
        for (path = path.parent_path(); !path.empty();) {
            error.clear();
            const auto parent_status = std::filesystem::symlink_status(path, error);
            if (std::filesystem::is_symlink(parent_status) || (error && error != std::errc::no_such_file_or_directory))
                return false;
            if (std::filesystem::exists(parent_status)) {
                if (!std::filesystem::is_directory(parent_status))
                    return false;
                if (!readable_parent) {
                    const std::filesystem::directory_iterator readable(path, error);
                    if (error)
                        return false;
                    readable_parent = true;
                }
            }
            const auto parent = path.parent_path();
            if (parent == path)
                break;
            path = parent;
        }
        return readable_parent;
    };
    if (kind == "steam") {
        const auto source = record.discovery_source;
        return QFileInfo(source).fileName() == "appmanifest_" + record.path + ".acf" &&
               absent_on_local_volume(source) && absent_on_local_volume(record.installation_directory);
    }
    if (kind == "desktop")
        return absent_on_local_volume(record.path) && absent_on_local_volume(record.executable_path);
    return absent_on_local_volume(record.path);
}

namespace {
// Valve KeyValues files contain escaped quoted strings, nested objects and // comments.
struct steam_node {
    QString key;
    QString value;
    QVector<steam_node> children;
};
class SteamVdfReader {
    QString text;
    qsizetype position = 0;
    const std::atomic_bool &cancelled;
    void whitespace() {
        for (;;) {
            if (cancelled.load())
                throw DiscoveryCancelled();
            while (position < text.size() && text[position].isSpace())
                ++position;
            if (text.mid(position, 2) != "//")
                return;
            while (position < text.size() && text[position] != '\n')
                ++position;
        }
    }
    QString token() {
        whitespace();
        if (position >= text.size())
            throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Unexpected end of VDF").toStdString());
        QString result;
        if (text[position] != '"') {
            while (position < text.size() && !text[position].isSpace() && text[position] != '{' &&
                   text[position] != '}')
                result += text[position++];
            if (result.isEmpty())
                throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Invalid VDF token").toStdString());
            return result;
        }
        ++position;
        while (position < text.size()) {
            auto character = text[position++];
            if (character == '"')
                return result;
            if (character == '\\' && position < text.size()) {
                const auto escaped = text[position++];
                if (escaped == '\\' || escaped == '"')
                    character = escaped;
                else {
                    result += character;
                    character = escaped;
                }
            }
            result += character;
        }
        throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Unterminated VDF string").toStdString());
    }
    QVector<steam_node> object(const int depth, const bool nested) {
        if (depth > 32)
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "VDF nesting limit exceeded").toStdString());
        QVector<steam_node> nodes;
        for (;;) {
            whitespace();
            if (position == text.size()) {
                if (nested)
                    throw std::invalid_argument(
                        QCoreApplication::translate("XLibrary", "Unterminated VDF object").toStdString());
                return nodes;
            }
            if (text[position] == '}') {
                if (!nested)
                    throw std::invalid_argument(
                        QCoreApplication::translate("XLibrary", "Unexpected VDF closing brace").toStdString());
                ++position;
                return nodes;
            }
            steam_node node;
            node.key = token();
            whitespace();
            if (position < text.size() && text[position] == '{') {
                ++position;
                node.children = object(depth + 1, true);
            } else
                node.value = token();
            nodes.append(std::move(node));
        }
    }

public:
    SteamVdfReader(QString contents, const std::atomic_bool &cancelled)
        : text(std::move(contents)), cancelled(cancelled) {
        if (text.startsWith(QChar(0xfeff)))
            text.remove(0, 1);
    }
    QVector<steam_node> read() { return object(0, false); }
};
QVector<steam_node> steam_read(const QString &path, const std::atomic_bool &cancelled) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(file.errorString().toStdString());
    const auto contents = file.read(8 * 1024 * 1024 + 1);
    if (contents.size() > 8 * 1024 * 1024)
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Steam metadata file exceeds 8 MiB").toStdString());
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error(file.errorString().toStdString());
    return SteamVdfReader(QString::fromUtf8(contents), cancelled).read();
}
QString steam_value(const QVector<steam_node> &nodes, const QString &key) {
    for (const auto &node : nodes)
        if (node.key.compare(key, Qt::CaseInsensitive) == 0)
            return node.value;
    return {};
}

const QVector<steam_node> &steam_children(const QVector<steam_node> &nodes, const QString &key) {
    for (const auto &node : nodes)
        if (node.key.compare(key, Qt::CaseInsensitive) == 0)
            return node.children;
    static const QVector<steam_node> empty;
    return empty;
}

// appinfo.vdf versions 39-41 use binary KeyValues; version 41 interns keys in a trailing string table.
class SteamBinaryReader {
    QByteArrayView data;
    const QVector<QString> *keys;
    const std::atomic_bool &cancelled;
    qsizetype position = 0;
    qsizetype node_count = 0;

public:
    SteamBinaryReader(const QByteArrayView data, const QVector<QString> *keys, const std::atomic_bool &cancelled)
        : data(data), keys(keys), cancelled(cancelled) {}

    void skip(const qsizetype size) {
        if (size < 0 || size > data.size() - position)
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Truncated Steam appinfo cache").toStdString());
        position += size;
    }

    template <typename T> T number() {
        const auto start = position;
        skip(sizeof(T));
        return qFromLittleEndian<T>(data.data() + start);
    }

    QString string() {
        const auto start = position;
        while (position < data.size() && data[position] != '\0') {
            if (position - start >= catalog_limits::text_length)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Steam appinfo string is too long").toStdString());
            ++position;
        }
        skip(1);
        return QString::fromUtf8(data.data() + start, position - start - 1);
    }

    qsizetype offset() const { return position; }

    QVector<steam_node> object(const int depth = 0) {
        if (depth > 32)
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Steam appinfo nesting limit exceeded").toStdString());
        QVector<steam_node> nodes;
        for (;;) {
            if (cancelled.load())
                throw DiscoveryCancelled();
            const auto type = number<quint8>();
            if (type == 8 || type == 11)
                return nodes;
            if (++node_count > 131072)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Steam appinfo node limit exceeded").toStdString());
            steam_node node;
            if (keys) {
                const auto index = number<quint32>();
                if (index >= static_cast<quint64>(keys->size()))
                    throw std::invalid_argument(
                        QCoreApplication::translate("XLibrary", "Invalid Steam appinfo string index").toStdString());
                node.key = (*keys)[index];
            } else
                node.key = string();
            switch (type) {
            case 0:
                node.children = object(depth + 1);
                break;
            case 1:
                node.value = string();
                break;
            case 2:
                node.value = QString::number(number<qint32>());
                break;
            case 3:
            case 4:
            case 6:
                skip(4);
                break;
            case 7:
            case 10:
                skip(8);
                break;
            default:
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Unsupported Steam appinfo value type").toStdString());
            }
            nodes.append(std::move(node));
        }
    }
};

QHash<QString, QVector<steam_node>> steam_appinfo(const QString &path, const QSet<QString> &app_ids,
                                                  const std::atomic_bool &cancelled) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(file.errorString().toStdString());
    // Read a bounded snapshot: Steam may be updating the cache concurrently.
    const auto data = file.read(128 * 1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error(file.errorString().toStdString());
    if (data.size() > 128 * 1024 * 1024)
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Steam appinfo cache exceeds 128 MiB").toStdString());
    SteamBinaryReader reader(data, nullptr, cancelled);
    const auto magic = reader.number<quint32>();
    if (magic < 0x07564427U || magic > 0x07564429U)
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Unsupported Steam appinfo cache version").toStdString());
    reader.skip(4); // Universe.
    QVector<QString> keys;
    auto entries_end = data.size();
    if (magic == 0x07564429U) {
        const auto table_offset = reader.number<quint64>();
        if (table_offset < 16 || table_offset > static_cast<quint64>(data.size()))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Invalid Steam appinfo string table offset").toStdString());
        entries_end = static_cast<qsizetype>(table_offset);
        SteamBinaryReader table(QByteArrayView(data).sliced(entries_end), nullptr, cancelled);
        const auto count = table.number<quint32>();
        if (count > 1048576 || count > static_cast<quint64>(data.size() - entries_end - 4))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Invalid Steam appinfo string table size").toStdString());
        keys.reserve(count);
        for (quint32 index = 0; index < count; ++index) {
            if (cancelled.load())
                throw DiscoveryCancelled();
            keys.append(table.string());
        }
    }
    QHash<QString, QVector<steam_node>> result;
    while (reader.offset() < entries_end) {
        if (cancelled.load())
            throw DiscoveryCancelled();
        const auto app_id = reader.number<quint32>();
        if (app_id == 0)
            return result;
        const auto size = reader.number<quint32>();
        const qsizetype header_size = magic >= 0x07564428U ? 60 : 40;
        if (size < header_size || size > static_cast<quint64>(entries_end - reader.offset()))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Invalid Steam appinfo entry size").toStdString());
        const auto id = QString::number(app_id);
        if (app_ids.contains(id)) {
            SteamBinaryReader entry(QByteArrayView(data).sliced(reader.offset() + header_size, size - header_size),
                                    magic == 0x07564429U ? &keys : nullptr, cancelled);
            result.insert(id, entry.object());
        }
        reader.skip(size);
    }
    throw std::invalid_argument(
        QCoreApplication::translate("XLibrary", "Steam appinfo cache has no footer").toStdString());
}

QString steam_local_executable(const app_record &record, const QString &value) {
    auto relative = value.trimmed();
    if (relative.startsWith('"') && relative.endsWith('"'))
        relative = relative.mid(1, relative.size() - 2);
    relative.replace('\\', '/');
    if (relative.isEmpty() || relative.contains(QChar::Null) || QDir::isAbsolutePath(relative) ||
        relative.contains(':'))
        return {};
    const auto root = QFileInfo(record.installation_directory).canonicalFilePath();
    const QFileInfo file(QDir(root).filePath(relative));
    const auto canonical = file.canonicalFilePath();
    if (root.isEmpty() || !file.isFile() || !canonical.startsWith(root + '/', path_case_sensitivity))
        return {};
#ifdef Q_OS_WIN
    if (file.suffix().compare("exe", Qt::CaseInsensitive) != 0)
        return {};
#else
    if (!file.isExecutable() && file.suffix().compare("exe", Qt::CaseInsensitive) != 0)
        return {};
#endif
    return canonical;
}

void steam_apply_metadata(app_record &record, const QVector<steam_node> &tree, const QStringList &roots) {
    const auto &app = steam_children(tree, "appinfo");
    const auto &launches = steam_children(steam_children(app, "config"), "launch");
    int best_score = -1;
    for (const auto &launch : launches) {
        const auto &config = steam_children(launch.children, "config");
        auto os_list = steam_value(config, "oslist").toLower().split(',', Qt::SkipEmptyParts);
        for (auto &os : os_list)
            os = os.trimmed();
        const auto type = steam_value(launch.children, "type");
        if (!type.isEmpty() && type.compare("default", Qt::CaseInsensitive) != 0)
            continue;
        int score = 0;
#ifdef Q_OS_WIN
        if (!os_list.isEmpty() && !os_list.contains("windows"))
            continue;
        const auto architecture = steam_value(config, "osarch");
        if (architecture == "64" && sizeof(void *) != 8)
            continue;
        score = architecture == "64" ? 1 : 0;
#else
        if (!os_list.isEmpty() && !os_list.contains("linux") && !os_list.contains("windows"))
            continue;
        // Prefer the native Linux entry, retaining Windows metadata for Proton installations.
        score = os_list.contains("linux") ? 2 : os_list.isEmpty() ? 1 : 0;
#endif
        const auto path = steam_local_executable(record, steam_value(launch.children, "executable"));
        if (!path.isEmpty() && score > best_score) {
            record.executable_path = path;
            best_score = score;
        }
    }
    const auto &common = steam_children(app, "common");
    for (const auto &root : roots) {
        // Steam's client icon is a square game icon, unlike library cover art.
        const auto hash = steam_value(common, "clienticon");
        static const QRegularExpression icon_hash("^[a-fA-F0-9]{40}$");
        if (icon_hash.match(hash).hasMatch()) {
            const auto path = QDir(root).filePath("steam/games/" + hash + ".ico");
            if (QFileInfo(path).isFile()) {
                record.icon_path = path;
                return;
            }
        }
    }
}
} // namespace

QVector<app_record> Application::discover_steam(QVector<discovery_issue> &issues, const std::atomic_bool &cancelled,
                                                const std::function<void(const discovery_progress &)> &progress) {
    QStringList roots;
#ifdef Q_OS_WIN
    roots = windows_steam_roots();
#else
    for (const auto &suffix : {"/.steam/steam", "/.local/share/Steam", "/.steam/root",
                               "/.var/app/com.valvesoftware.Steam/.local/share/Steam"})
        roots.append(QDir::homePath() + suffix);
#endif
    return discover_steam(roots, issues, cancelled, progress);
}

QVector<app_record> Application::discover_steam(const QStringList &roots, QVector<discovery_issue> &issues,
                                                const std::atomic_bool &cancelled,
                                                const std::function<void(const discovery_progress &)> &progress) {
    QStringList libraries;
    QSet<QString> library_keys;
    const auto add_library = [&](const QString &path) {
        const QFileInfo info(QDir::fromNativeSeparators(path));
        if (!info.isAbsolute() || !info.isDir())
            return;
        const auto key = normalized_path(info.canonicalFilePath());
        if (!library_keys.contains(key)) {
            library_keys.insert(key);
            libraries.append(info.absoluteFilePath());
        }
    };
    for (const auto &root : roots) {
        if (cancelled.load())
            return {};
        add_library(root);
        const auto folders = QDir(root).filePath("steamapps/libraryfolders.vdf");
        if (!QFileInfo::exists(folders))
            continue;
        try {
            const auto tree = steam_read(folders, cancelled);
            for (const auto &node : tree) {
                if (node.key.compare("libraryfolders", Qt::CaseInsensitive) != 0)
                    continue;
                for (const auto &entry : node.children) {
                    bool numeric = false;
                    entry.key.toUInt(&numeric);
                    if (!numeric)
                        continue;
                    add_library(entry.children.isEmpty() ? entry.value : steam_value(entry.children, "path"));
                }
            }
        } catch (const DiscoveryCancelled &) {
            return {};
        } catch (const std::exception &error) {
            issues.append({folders, QString::fromUtf8(error.what())});
        }
    }
    QStringList manifests;
    for (const auto &library : libraries) {
        const QDir directory(QDir(library).filePath("steamapps"));
        if (directory.exists() && !QFileInfo(directory.absolutePath()).isReadable())
            issues.append({directory.absolutePath(), "Steam library is not readable"});
        for (const auto &name : directory.entryList({"appmanifest_*.acf"}, QDir::Files, QDir::Name)) {
            if (cancelled.load())
                return {};
            manifests.append(directory.filePath(name));
        }
    }
    QVector<app_record> records;
    QSet<QString> app_ids;
    for (qsizetype index = 0; index < manifests.size(); ++index) {
        if (cancelled.load())
            return {};
        const auto &manifest = manifests[index];
        if (progress)
            progress({static_cast<int>(90.0 * index / manifests.size()), index, manifests.size(),
                      QCoreApplication::translate("XLibrary", "Checking Steam libraries")});
        try {
            const auto tree = steam_read(manifest, cancelled);
            const steam_node *state = nullptr;
            for (const auto &node : tree)
                if (node.key.compare("AppState", Qt::CaseInsensitive) == 0) {
                    state = &node;
                    break;
                }
            if (!state)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Steam manifest has no AppState").toStdString());
            app_record record;
            record.target_kind = "steam";
            record.path = steam_value(state->children, "appid");
            record.name = steam_value(state->children, "name").trimmed();
            validate_launch_target(record);
            const auto expected_file = "appmanifest_" + record.path + ".acf";
            if (QFileInfo(manifest).fileName() != expected_file)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Steam AppID does not match manifest filename")
                        .toStdString());
            const auto directory = steam_value(state->children, "installdir");
            if (record.name.isEmpty() || record.name.size() > 256 || record.name.contains(QChar::Null) ||
                directory.isEmpty() || directory == "." || directory == ".." || directory.contains('/') ||
                directory.contains('\\') || directory.contains(QChar::Null))
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Steam manifest has invalid name or installation directory")
                        .toStdString());
            bool state_numeric = false;
            const auto flags = steam_value(state->children, "StateFlags").toUInt(&state_numeric);
            if (!state_numeric || (flags & 4U) == 0) {
                issues.append({manifest, QCoreApplication::translate("XLibrary", "Steam game is not fully installed"),
                               discovery_issue_kind::skipped});
                continue;
            }
            record.installation_directory = QDir(QFileInfo(manifest).absolutePath()).filePath("common/" + directory);
            if (!QFileInfo(record.installation_directory).isDir()) {
                issues.append({manifest,
                               QCoreApplication::translate("XLibrary", "Steam installation directory is unavailable"),
                               discovery_issue_kind::skipped});
                continue;
            }
            if (app_ids.contains(record.path)) {
                issues.append({manifest, QCoreApplication::translate("XLibrary", "Duplicate Steam AppID"),
                               discovery_issue_kind::duplicate});
                continue;
            }
            app_ids.insert(record.path);
            record.discovery_source = manifest;
            // Cached cover art is optional and is never fetched or changed by discovery.
            for (const auto &root : roots) {
                const auto icon = QDir(root).filePath("appcache/librarycache/" + record.path + "_icon.jpg");
                if (QFileInfo(icon).isFile()) {
                    record.icon_path = icon;
                    break;
                }
            }
            records.append(std::move(record));
        } catch (const DiscoveryCancelled &) {
            return {};
        } catch (const std::exception &error) {
            issues.append({manifest, QString::fromUtf8(error.what())});
        }
    }
    if (!records.isEmpty()) {
        for (const auto &root : roots) {
            const auto path = QDir(root).filePath("appcache/appinfo.vdf");
            if (!QFileInfo::exists(path))
                continue;
            try {
                const auto metadata = steam_appinfo(path, app_ids, cancelled);
                for (auto &record : records) {
                    if (cancelled.load())
                        return {};
                    if (const auto entry = metadata.constFind(record.path); entry != metadata.cend())
                        steam_apply_metadata(record, *entry, roots);
                }
            } catch (const DiscoveryCancelled &) {
                return {};
            } catch (const std::exception &error) {
                issues.append({path, QString::fromUtf8(error.what())});
            }
        }
    }
    if (progress)
        progress(
            {90, manifests.size(), manifests.size(), QCoreApplication::translate("XLibrary", "Steam checks complete")});
    return cancelled.load() ? QVector<app_record>{} : records;
}

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace {
QString browser_family(const app_record &record) {
    if (!Application::file_target(record))
        return {};
    const auto filename = QFileInfo(record.path).completeBaseName().toCaseFolded();
    if (filename == "chrome" || filename.startsWith("google-chrome") || filename == "com.google.chrome")
        return "chrome";
    if (filename == "msedge" || filename.startsWith("microsoft-edge") || filename == "com.microsoft.edge")
        return "edge";
    if (filename == "brave" || filename.startsWith("brave-browser") || filename == "com.brave.browser")
        return "brave";
    if (filename.startsWith("firefox") || filename == "org.mozilla.firefox")
        return "firefox";
    return {};
}
QString browser_data_root(const QString &family) {
#ifdef Q_OS_WIN
    if (family == "firefox")
        return qEnvironmentVariable("APPDATA") + "/Mozilla/Firefox";
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    if (family == "chrome")
        return local + "/Google/Chrome/User Data";
    if (family == "edge")
        return local + "/Microsoft/Edge/User Data";
    if (family == "brave")
        return local + "/BraveSoftware/Brave-Browser/User Data";
#else
    if (family == "firefox")
        return QDir::homePath() + "/.mozilla/firefox";
    const auto config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (family == "chrome")
        return config + "/google-chrome";
    if (family == "edge")
        return config + "/microsoft-edge";
    if (family == "brave")
        return config + "/BraveSoftware/Brave-Browser";
#endif
    return {};
}
QString profile_argument(const QString &value) {
    // Both supported launch parsers accept double-quoted arguments with escaped backslashes and quotes.
    auto escaped = value;
#ifdef Q_OS_WIN
    escaped.replace('"', "\\\"");
#else
    escaped.replace('\\', "\\\\").replace('"', "\\\"");
#endif
    return '"' + escaped + '"';
}
}

QVector<launch_profile> Application::launch_modes(const app_record &record) {
    const auto family = browser_family(record);
    if (family.isEmpty())
        return {};
    return {{"normal", QCoreApplication::translate("XLibrary", "Normal window"), "normal", {}},
            {"private",
             family == "edge" ? QCoreApplication::translate("XLibrary", "InPrivate window")
                              : QCoreApplication::translate("XLibrary", "Private window"),
             "private",
             {}}};
}

QVector<launch_profile> Application::launch_profiles(const app_record &record) {
    auto profiles = launch_modes(record);
    if (profiles.isEmpty())
        return {};
    const auto family = browser_family(record);
    const auto root = browser_data_root(family);
    if (family == "firefox") {
        const auto metadata = root + "/profiles.ini";
        if (QFileInfo(metadata).size() > 8 * 1024 * 1024)
            return profiles;
        QSettings settings(metadata, QSettings::IniFormat);
        for (const auto &group : settings.childGroups()) {
            if (!group.startsWith("Profile"))
                continue;
            settings.beginGroup(group);
            const auto path = settings.value("Path").toString();
            const auto name = settings.value("Name").toString();
            const bool relative = settings.value("IsRelative", 1).toInt() != 0;
            settings.endGroup();
            const auto directory = relative ? QDir(root).absoluteFilePath(path) : path;
            if (path.isEmpty() || !QFileInfo(directory).isDir())
                continue;
            if (profiles.size() >= 128)
                break;
            profiles.append({"profile:" + directory, name.isEmpty() ? QFileInfo(directory).fileName() : name, "user",
                             record.icon_path});
        }
    } else {
        QFile state(root + "/Local State");
        if (state.open(QIODevice::ReadOnly) && state.size() <= 8 * 1024 * 1024) {
            const auto cache = QJsonDocument::fromJson(state.readAll())
                                   .object()
                                   .value("profile")
                                   .toObject()
                                   .value("info_cache")
                                   .toObject();
            for (auto item = cache.begin(); item != cache.end(); ++item) {
                const auto directory = item.key();
                // Chromium profile directories are single path components, never arbitrary paths.
                if (directory.isEmpty() || directory == "." || directory == ".." || directory.contains('/') ||
                    directory.contains('\\') || !QFileInfo(QDir(root).filePath(directory)).isDir())
                    continue;
                const auto info = item.value().toObject();
                const auto name = info.value("name").toString(directory);
                auto icon = QDir(root).filePath(directory + "/" + info.value("gaia_picture_file_name").toString());
                if (!QFileInfo(icon).isFile())
                    icon = record.icon_path;
                if (profiles.size() >= 128)
                    break;
                profiles.append({"profile:" + directory, name, "user", icon});
            }
        }
    }
    return profiles;
}

bool Application::requires_fresh_launch(const app_record &record) {
    if (browser_family(record).isEmpty())
        return false;
    static const QRegularExpression mode_argument(
        "(?:^|\\s)[\"']?(?:--(?:incognito|inprivate|private-window|profile-directory|user-data-dir)|-(?:private-window|"
        "P|profile|no-remote))(?=[=\\s\"']|$)");
    return mode_argument.match(configured_arguments(record)).hasMatch();
}

bool Application::valid_launch_profile_id(const QString &profile) {
    if (profile.size() > catalog_limits::text_length)
        return false;
    for (const auto character : profile)
        if (character.isNull() || character.category() == QChar::Other_Control)
            return false;
    return profile.isEmpty() || profile == "configured" || profile == "normal" || profile == "private" ||
           (profile.startsWith("profile:") && profile.size() > 8);
}

app_record Application::with_launch_profile(const app_record &record, const QString &profile) {
    auto result = record;
    if (profile.isEmpty() || profile == "configured")
        return result;
    const auto profiles = launch_profiles(record);
    const auto found = std::ranges::find(profiles, profile, &launch_profile::id);
    if (found == profiles.cend())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The selected browser profile is unavailable.").toStdString());
    const auto family = browser_family(record);
    // Existing custom mode/profile arguments remain intact unless the user chooses a conflicting mode.
    if (requires_fresh_launch(record))
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "This configuration already specifies a browser mode or profile. "
                                                    "Use its configured launch or edit its arguments.")
                .toStdString());
    QString arguments;
    if (profile == "private")
        arguments = family == "firefox" ? "-private-window" : family == "edge" ? "--inprivate" : "--incognito";
    else if (profile.startsWith("profile:")) {
        const auto directory = profile.mid(8);
        arguments = family == "firefox" ? "-no-remote -profile " + profile_argument(directory)
                                        : "--profile-directory=" + profile_argument(directory);
    } else
        arguments = family == "firefox" ? "-new-window about:blank" : "--new-window";
    result.arguments = arguments + (record.arguments.isEmpty() ? QString{} : ' ' + record.arguments);
    result.default_launch_profile.clear();
    return result;
}
