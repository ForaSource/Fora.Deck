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
#include <QRegularExpression>
#ifdef Q_OS_WIN
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

void Application::validate_launch_target(const app_record &record) {
    const auto kind = target_kind(record);
    if (record.path.isEmpty() || record.path.size() > catalog_limits::text_length ||
        record.path.contains(QChar::Null) || record.arguments.contains(QChar::Null) ||
        record.working_directory.contains(QChar::Null))
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Launch target is empty, too long, or contains a null character")
                .toStdString());
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

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace {
QString browser_family(const app_record &record) {
    if (!Application::file_target(record)) return {};
    const auto filename = QFileInfo(record.path).completeBaseName().toCaseFolded();
    if (filename == "chrome" || filename.startsWith("google-chrome") || filename == "com.google.chrome") return "chrome";
    if (filename == "msedge" || filename.startsWith("microsoft-edge") || filename == "com.microsoft.edge") return "edge";
    if (filename == "brave" || filename.startsWith("brave-browser") || filename == "com.brave.browser") return "brave";
    if (filename.startsWith("firefox") || filename == "org.mozilla.firefox") return "firefox";
    return {};
}
QString browser_data_root(const QString &family) {
#ifdef Q_OS_WIN
    if (family == "firefox") return qEnvironmentVariable("APPDATA") + "/Mozilla/Firefox";
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    if (family == "chrome") return local + "/Google/Chrome/User Data";
    if (family == "edge") return local + "/Microsoft/Edge/User Data";
    if (family == "brave") return local + "/BraveSoftware/Brave-Browser/User Data";
#else
    if (family == "firefox") return QDir::homePath() + "/.mozilla/firefox";
    const auto config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (family == "chrome") return config + "/google-chrome";
    if (family == "edge") return config + "/microsoft-edge";
    if (family == "brave") return config + "/BraveSoftware/Brave-Browser";
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
    if (family.isEmpty()) return {};
    return {{"normal", QCoreApplication::translate("XLibrary", "Normal window"), "normal", {}},
            {"private", family == "edge" ? QCoreApplication::translate("XLibrary", "InPrivate window")
                                         : QCoreApplication::translate("XLibrary", "Private window"), "private", {}}};
}

QVector<launch_profile> Application::launch_profiles(const app_record &record) {
    auto profiles = launch_modes(record);
    if (profiles.isEmpty()) return {};
    const auto family = browser_family(record);
    const auto root = browser_data_root(family);
    if (family == "firefox") {
        const auto metadata = root + "/profiles.ini";
        if (QFileInfo(metadata).size() > 8 * 1024 * 1024) return profiles;
        QSettings settings(metadata, QSettings::IniFormat);
        for (const auto &group : settings.childGroups()) {
            if (!group.startsWith("Profile")) continue;
            settings.beginGroup(group);
            const auto path = settings.value("Path").toString();
            const auto name = settings.value("Name").toString();
            const bool relative = settings.value("IsRelative", 1).toInt() != 0;
            settings.endGroup();
            const auto directory = relative ? QDir(root).absoluteFilePath(path) : path;
            if (path.isEmpty() || !QFileInfo(directory).isDir()) continue;
            if (profiles.size() >= 128) break;
            profiles.append({"profile:" + directory, name.isEmpty() ? QFileInfo(directory).fileName() : name,
                             "user", record.icon_path});
        }
    } else {
        QFile state(root + "/Local State");
        if (state.open(QIODevice::ReadOnly) && state.size() <= 8 * 1024 * 1024) {
            const auto cache = QJsonDocument::fromJson(state.readAll()).object().value("profile").toObject()
                                   .value("info_cache").toObject();
            for (auto item = cache.begin(); item != cache.end(); ++item) {
                const auto directory = item.key();
                // Chromium profile directories are single path components, never arbitrary paths.
                if (directory.isEmpty() || directory == "." || directory == ".." || directory.contains('/') ||
                    directory.contains('\\') || !QFileInfo(QDir(root).filePath(directory)).isDir()) continue;
                const auto info = item.value().toObject();
                const auto name = info.value("name").toString(directory);
                auto icon = QDir(root).filePath(directory + "/" + info.value("gaia_picture_file_name").toString());
                if (!QFileInfo(icon).isFile()) icon = record.icon_path;
                if (profiles.size() >= 128) break;
                profiles.append({"profile:" + directory, name, "user", icon});
            }
        }
    }
    return profiles;
}

bool Application::requires_fresh_launch(const app_record &record) {
    if (browser_family(record).isEmpty()) return false;
    static const QRegularExpression mode_argument(
        "(?:^|\\s)[\"']?(?:--(?:incognito|inprivate|private-window|profile-directory|user-data-dir)|-(?:private-window|P|profile|no-remote))(?=[=\\s\"']|$)");
    return mode_argument.match(configured_arguments(record)).hasMatch();
}

bool Application::valid_launch_profile_id(const QString &profile) {
    if (profile.size() > catalog_limits::text_length) return false;
    for (const auto character : profile)
        if (character.isNull() || character.category() == QChar::Other_Control) return false;
    return profile.isEmpty() || profile == "configured" || profile == "normal" || profile == "private" ||
           (profile.startsWith("profile:") && profile.size() > 8);
}

app_record Application::with_launch_profile(const app_record &record, const QString &profile) {
    auto result = record;
    if (profile.isEmpty() || profile == "configured") return result;
    const auto profiles = launch_profiles(record);
    const auto found = std::ranges::find(profiles, profile, &launch_profile::id);
    if (found == profiles.cend())
        throw std::invalid_argument(QCoreApplication::translate("XLibrary", "The selected browser profile is unavailable.").toStdString());
    const auto family = browser_family(record);
    // Existing custom mode/profile arguments remain intact unless the user chooses a conflicting mode.
    if (requires_fresh_launch(record))
        throw std::invalid_argument(QCoreApplication::translate("XLibrary", "This configuration already specifies a browser mode or profile. Use its configured launch or edit its arguments.").toStdString());
    QString arguments;
    if (profile == "private")
        arguments = family == "firefox" ? "-private-window" : family == "edge" ? "--inprivate" : "--incognito";
    else if (profile.startsWith("profile:")) {
        const auto directory = profile.mid(8);
        arguments = family == "firefox" ? "-no-remote -profile " + profile_argument(directory)
                                         : "--profile-directory=" + profile_argument(directory);
    } else arguments = family == "firefox" ? "-new-window about:blank" : "--new-window";
    result.arguments = arguments + (record.arguments.isEmpty() ? QString{} : ' ' + record.arguments);
    result.default_launch_profile.clear();
    return result;
}
