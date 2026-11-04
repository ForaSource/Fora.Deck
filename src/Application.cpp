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

bool Application::valid_launch_profile_id(const QString &profile) {
    if (profile.size() > catalog_limits::text_length) return false;
    for (const auto character : profile)
        if (character.isNull() || character.category() == QChar::Other_Control) return false;
    return profile.isEmpty() || profile == "configured" || profile == "normal" || profile == "private" ||
           (profile.startsWith("profile:") && profile.size() > 8);
}
