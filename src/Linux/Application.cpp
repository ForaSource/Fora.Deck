#include <memory>
#include <stdexcept>
#include <utility>

#include "Application.h"
#include "LaunchArguments.h"
#include "LinuxProcessor.h"
#include "PathHelpers.h"

std::uint32_t Application::get_pid() const noexcept {
    if (!this->process_handle) {
        return 0;
    }
    try {
        std::error_code error;
        const auto identity = std::static_pointer_cast<xlibrary::process_identity>(this->process_handle);
        return xlibrary::is_process_alive(*identity, error) ? this->pid : 0;
    } catch (...) {
        return 0;
    }
}

std::string Application::read_application_name(const std::filesystem::path &executable_path) {
    const auto filename_utf8 = executable_path.stem().u8string();
    const std::string fallback_name(filename_utf8.begin(), filename_utf8.end());

    return fallback_name;
}

std::uint32_t Application::launch(const std::wstring &arguments, const std::filesystem::path &working_directory) {
    const std::filesystem::path executable_path = this->get_file_path();
    if (executable_path.empty()) {
        throw std::invalid_argument("Application executable path is empty");
    }

    auto identity = std::make_shared<xlibrary::process_identity>();
    *identity = xlibrary::launch_process(executable_path, arguments, working_directory);
    this->pid = identity->pid;
    this->process_handle = std::move(identity);
    return this->pid;
}

#include <algorithm>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDirIterator>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include "ApplicationPaths.h"
#include "Heartbeat.h"

namespace {
QProcessEnvironment application_environment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!application_paths::bundled_runtime())
        return environment;
    // Only XLibrary needs bundled Qt; child applications use the original environment.
    for (const auto &name :
         {QString("LD_LIBRARY_PATH"), QString("QT_PLUGIN_PATH"), QString("QT_QPA_PLATFORM_PLUGIN_PATH")}) {
        const auto original = "XLIBRARY_ORIGINAL_" + name;
        if (environment.value(original + "_SET") == "x")
            environment.insert(name, environment.value(original));
        else
            environment.remove(name);
    }
    for (const auto &name : environment.keys()) {
        if (name.startsWith("XLIBRARY_"))
            environment.remove(name);
    }
    for (const auto &name : {"APPDIR", "APPIMAGE", "OWD", "ARGV0"})
        environment.remove(name);
    return environment;
}

struct desktop_entry {
    QString path;
    QHash<QString, QString> values;

    bool flag(const QString &key) const { return values.value(key) == "true"; }
};

QString absolute_path(const QString &path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString canonical_path(const QString &path) {
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? absolute_path(path) : canonical;
}

QString xdg_home(const char *const variable, const QString &fallback) {
    const auto value = qEnvironmentVariable(variable);
    return QFileInfo(value).isAbsolute() ? QDir::cleanPath(value) : QDir::homePath() + fallback;
}

QStringList application_directories() {
    QStringList result{xdg_home("XDG_DATA_HOME", "/.local/share") + "/applications"};
    auto directories = qEnvironmentVariable("XDG_DATA_DIRS");
    if (directories.isEmpty())
        directories = "/usr/local/share:/usr/share";
    for (const auto &directory : directories.split(':', Qt::SkipEmptyParts)) {
        if (QFileInfo(directory).isAbsolute())
            result.append(QDir::cleanPath(directory) + "/applications");
    }
    result.removeDuplicates();
    return result;
}

QString unescape_value(const QString &value) {
    QString result;
    result.reserve(value.size());
    for (qsizetype index = 0; index < value.size(); ++index) {
        const auto character = value[index];
        if (character != '\\' || index + 1 == value.size()) {
            result += character;
            continue;
        }
        const auto escaped = value[++index];
        if (escaped == 's')
            result += ' ';
        else if (escaped == 'n')
            result += '\n';
        else if (escaped == 't')
            result += '\t';
        else if (escaped == 'r')
            result += '\r';
        else if (escaped == '\\' || escaped == ';')
            result += escaped;
        else {
            // Exec has a second escaping layer that is parsed separately.
            result += '\\';
            result += escaped;
        }
    }
    return result;
}

desktop_entry read_entry(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        throw std::runtime_error(QCoreApplication::translate("XLibrary", "Could not read %1: %2")
                                     .arg(path, file.errorString())
                                     .toStdString());
    if (file.size() > 4 * 1024 * 1024)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "The .desktop file is too large.").toStdString());
    desktop_entry entry{absolute_path(path), {}};
    bool in_entry = false;
    const auto lines = QString::fromUtf8(file.readAll()).split('\n');
    for (auto line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.startsWith('[')) {
            in_entry = line == "[Desktop Entry]";
            continue;
        }
        if (!in_entry)
            continue;
        const auto separator = line.indexOf('=');
        if (separator > 0)
            entry.values.insert(line.left(separator).trimmed(), unescape_value(line.mid(separator + 1).trimmed()));
    }
    return entry;
}

QString localized_value(const desktop_entry &entry, const QString &key) {
    auto locale = qEnvironmentVariable("LC_ALL");
    if (locale.isEmpty())
        locale = qEnvironmentVariable("LC_MESSAGES");
    if (locale.isEmpty())
        locale = qEnvironmentVariable("LANG");
    if (locale.isEmpty())
        locale = QLocale::system().name();
    const auto modifier_index = locale.indexOf('@');
    const auto modifier = modifier_index >= 0 ? locale.mid(modifier_index) : QString{};
    if (modifier_index >= 0)
        locale.truncate(modifier_index);
    const auto encoding_index = locale.indexOf('.');
    if (encoding_index >= 0)
        locale.truncate(encoding_index);
    const auto language = locale.section('_', 0, 0);
    const QStringList candidates{locale + modifier, locale, language + modifier, language};
    for (const auto &candidate : candidates) {
        const auto localized_key = key + '[' + candidate + ']';
        if (entry.values.contains(localized_key))
            return entry.values.value(localized_key);
    }
    return entry.values.value(key);
}

QString executable_path(const QString &program) {
    if (program.isEmpty() || program.contains(QChar::Null))
        return {};
    if (!QFileInfo(program).isAbsolute()) {
        if (program.contains('/'))
            return {};
        return QStandardPaths::findExecutable(program);
    }
    const QFileInfo info(program);
    return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString{};
}

bool visible_entry(const desktop_entry &entry) {
    if (entry.values.value("Type") != "Application" || entry.flag("Hidden") || entry.flag("NoDisplay"))
        return false;
    const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':', Qt::SkipEmptyParts);
    const auto only_show = entry.values.value("OnlyShowIn").split(';', Qt::SkipEmptyParts);
    const auto not_show = entry.values.value("NotShowIn").split(';', Qt::SkipEmptyParts);
    for (const auto &desktop : desktops) {
        if (only_show.contains(desktop))
            return true;
        if (not_show.contains(desktop))
            return false;
    }
    return !entry.values.contains("OnlyShowIn");
}

// Exec из .desktop имеет собственные правила кавычек и field codes.
QStringList split_command(const QString &command) {
    QStringList result;
    QString current;
    bool quoted = false;
    bool started = false;
    for (qsizetype index = 0; index < command.size(); ++index) {
        const auto character = command[index];
        if (character == QChar::Null)
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The command contains a null character.").toStdString());
        if (character == '"') {
            quoted = !quoted;
            started = true;
        } else if (character == '\\' && index + 1 < command.size()) {
            const auto escaped = command[index + 1];
            if (!quoted || escaped == '"' || escaped == '`' || escaped == '$' || escaped == '\\') {
                current += escaped;
                ++index;
            } else {
                current += character;
            }
            started = true;
        } else if (!quoted && character.isSpace()) {
            if (started) {
                result.append(current);
                current.clear();
                started = false;
            }
        } else {
            current += character;
            started = true;
        }
    }
    if (quoted)
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The command has an unclosed double quote.").toStdString());
    if (started)
        result.append(current);
    return result;
}

QStringList user_arguments(const QString &arguments) {
    const auto utf8 = arguments.toUtf8();
    const auto parsed =
        xlibrary::split_launch_arguments(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    QStringList result;
    result.reserve(static_cast<qsizetype>(parsed.size()));
    for (const auto &argument : parsed)
        result.append(QString::fromUtf8(argument.data(), static_cast<qsizetype>(argument.size())));
    return result;
}

QStringList entry_command(const desktop_entry &entry) {
    const auto tokens = split_command(entry.values.value("Exec"));
    QStringList result;
    int file_codes = 0;
    for (const auto &token : tokens) {
        if (token == "%i") {
            const auto icon = localized_value(entry, "Icon");
            if (!icon.isEmpty())
                result.append({"--icon", icon});
            continue;
        }
        QString expanded;
        bool removed_code = false;
        for (qsizetype index = 0; index < token.size(); ++index) {
            if (token[index] != '%') {
                expanded += token[index];
                continue;
            }
            if (++index == token.size())
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Invalid Exec field code in .desktop.").toStdString());
            const auto code = token[index];
            if (QStringView(u"fFuU").contains(code) && ++file_codes > 1)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Exec may contain only one file or URL field code.")
                        .toStdString());
            if ((code == 'F' || code == 'U') && token.size() != 2)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "An Exec file list code must be a separate argument.")
                        .toStdString());
            if (code == '%')
                expanded += '%';
            else if (code == 'c')
                expanded += localized_value(entry, "Name");
            else if (code == 'k')
                expanded += entry.path;
            else if (QStringView(u"fFuUdDnNvm").contains(code))
                removed_code = true;
            else
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Unsupported Exec field code in .desktop.").toStdString());
        }
        // XLibrary launches applications without files, so %f/%F/%u/%U produce no arguments.
        if (!expanded.isEmpty() || !removed_code)
            result.append(expanded);
    }
    if (result.isEmpty() || result.front().isEmpty() || result.front().contains('='))
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The .desktop file has no valid Exec command.").toStdString());
    return result;
}

QString dbus_service(const desktop_entry &entry) {
    if (!entry.flag("DBusActivatable"))
        return {};
    const auto service = QFileInfo(entry.path).completeBaseName();
    static const QRegularExpression service_name("^[A-Za-z_-][A-Za-z0-9_-]*(?:\\.[A-Za-z_-][A-Za-z0-9_-]*)+$");
    if (!service_name.match(service).hasMatch())
        return {};
    return service;
}

void validate_entry(const desktop_entry &entry) {
    if (entry.values.value("Type") != "Application" || entry.flag("Hidden"))
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The .desktop file does not contain an available application.")
                .toStdString());
    if (localized_value(entry, "Name").isEmpty())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The .desktop file has no application name.").toStdString());
    const auto try_exec = entry.values.value("TryExec");
    if (!try_exec.isEmpty() && executable_path(try_exec).isEmpty())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The TryExec executable is unavailable.").toStdString());
    const auto directory = entry.values.value("Path");
    if (!directory.isEmpty() && !QFileInfo(directory).isAbsolute())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The .desktop working directory must be absolute.").toStdString());
    if (entry.flag("DBusActivatable")) {
        if (dbus_service(entry).isEmpty())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Invalid D-Bus service name in .desktop.").toStdString());
    } else {
        const auto command = entry_command(entry);
        if (executable_path(command.front()).isEmpty())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The executable specified by Exec is unavailable.")
                    .toStdString());
    }
}

bool activate_dbus(const desktop_entry &entry) {
    const auto service = dbus_service(entry);
    if (service.isEmpty())
        return false;
    auto object_path = '/' + service;
    object_path.replace('.', '/');
    object_path.replace('-', '_');
    auto message = QDBusMessage::createMethodCall(service, object_path, "org.freedesktop.Application", "Activate");
    message << QVariantMap{};
    const auto reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 5000);
    return reply.type() != QDBusMessage::ErrorMessage;
}

QStringList terminal_command(const QStringList &command) {
    const auto configured = qEnvironmentVariable("TERMINAL");
    if (!configured.isEmpty()) {
        auto terminal = split_command(configured);
        if (!terminal.isEmpty() && !executable_path(terminal.front()).isEmpty()) {
            terminal.append("-e");
            terminal.append(command);
            return terminal;
        }
    }
    const QList<QPair<QString, QString>> terminals{{"x-terminal-emulator", "-e"}, {"konsole", "-e"},
                                                   {"gnome-terminal", "--"},      {"kgx", "--"},
                                                   {"xfce4-terminal", "-x"},      {"xterm", "-e"}};
    for (const auto &[terminal, separator] : terminals) {
        const auto executable = executable_path(terminal);
        if (!executable.isEmpty()) {
            QStringList result{executable, separator};
            result.append(command);
            return result;
        }
    }
    throw std::runtime_error(
        QCoreApplication::translate("XLibrary",
                                    "This application requires a terminal. Install one or set the TERMINAL variable.")
            .toStdString());
}

struct process_identity {
    QString executable;
    QStringList arguments;
    QString flatpak_id;
    QString snap_name;
    quint32 dbus_pid = 0;
    bool script = false;
};

process_identity application_identity(const app_record &record) {
    process_identity result;
    const auto &path = record.path;
    auto directory = record.working_directory;
    QStringList command{path};
    if (QFileInfo(path).suffix() == "desktop") {
        const auto entry = read_entry(path);
        if (directory.isEmpty())
            directory = entry.values.value("Path");
        const auto service = dbus_service(entry);
        if (!service.isEmpty()) {
            if (auto *interface = QDBusConnection::sessionBus().interface()) {
                const auto pid = interface->servicePid(service);
                if (pid.isValid())
                    result.dbus_pid = pid.value();
            }
        }
        result.flatpak_id = entry.values.value("X-Flatpak");
        result.snap_name = entry.values.value("X-SnapInstanceName");
        if (entry.values.value("Exec").isEmpty())
            return result;
        try {
            command = entry_command(entry);
        } catch (const std::invalid_argument &) {
            if (!service.isEmpty())
                return result;
            throw;
        }
    } else if (directory.isEmpty()) {
        directory = QFileInfo(path).absolutePath();
    }
    command.append(user_arguments(record.arguments));
    if (QFileInfo(command.front()).fileName() == "env") {
        command.removeFirst();
        bool options = true;
        bool directory_changed = false;
        QString env_directory;
        while (!command.isEmpty()) {
            const auto argument = command.front();
            if (options && (argument == "--" || argument == "-")) {
                options = false;
                command.removeFirst();
            } else if (options && (argument == "-C" || argument == "--chdir")) {
                if (command.size() < 2)
                    return {};
                command.removeFirst();
                env_directory = command.takeFirst();
                directory_changed = true;
            } else if (options && (argument.startsWith("--chdir=") || argument.startsWith("-C"))) {
                env_directory = argument.mid(argument.startsWith("--chdir=") ? 8 : 2);
                directory_changed = true;
                command.removeFirst();
            } else if (options && (argument == "-u" || argument == "--unset")) {
                if (command.size() < 2)
                    return {};
                command.removeFirst();
                command.removeFirst();
            } else if (options && argument.startsWith('-')) {
                command.removeFirst();
            } else if (argument.contains('=')) {
                options = false;
                command.removeFirst();
            } else {
                break;
            }
        }
        if (directory_changed) {
            if (env_directory.isEmpty())
                return {};
            // GNU env applies the final -C once, relative to its launch directory.
            directory = QDir(directory.isEmpty() ? QDir::currentPath() : directory).absoluteFilePath(env_directory);
        }
    }
    if (command.isEmpty())
        return {};
    result.executable = executable_path(command.front());
    if (!result.executable.isEmpty()) {
        QFile executable(result.executable);
        result.script = executable.open(QIODevice::ReadOnly) && executable.read(2) == "#!";
        result.executable = canonical_path(result.executable);
    }
    result.arguments = command.mid(1);
    // Resolve file arguments in the same directory used at launch, not XLibrary's directory.
    const QDir working_directory(directory.isEmpty() ? QDir::currentPath() : directory);
    for (auto &argument : result.arguments) {
        if (argument.isEmpty() || argument.startsWith('-') || QFileInfo(argument).isAbsolute())
            continue;
        const QFileInfo candidate(working_directory.filePath(argument));
        if (candidate.isFile())
            argument = canonical_path(candidate.absoluteFilePath());
    }
    const auto program = QFileInfo(command.front()).fileName();
    if (program == "flatpak" && result.flatpak_id.isEmpty()) {
        const auto run = command.indexOf("run");
        for (auto index = run + 1; run >= 0 && index < command.size(); ++index) {
            if (!command[index].startsWith('-')) {
                result.flatpak_id = command[index];
                break;
            }
        }
    } else if (program == "snap" && result.snap_name.isEmpty()) {
        const auto run = command.indexOf("run");
        for (auto index = run + 1; run >= 0 && index < command.size(); ++index) {
            if (!command[index].startsWith('-')) {
                result.snap_name = command[index].section('.', 0, 0);
                break;
            }
        }
    } else if (command.front().startsWith("/snap/bin/") && result.snap_name.isEmpty()) {
        result.snap_name = program.section('.', 0, 0);
    }
    return result;
}

QString process_executable(const QString &process_directory) {
    auto path = QFileInfo(process_directory + "/exe").symLinkTarget();
    if (path.endsWith(" (deleted)"))
        path.chop(10);
    return path.isEmpty() ? QString{} : canonical_path(path);
}

QStringList process_arguments(const QString &process_directory) {
    QFile command_line(process_directory + "/cmdline");
    if (!command_line.open(QIODevice::ReadOnly))
        return {};
    const auto raw_arguments = command_line.read(1024 * 1024).split('\0');
    QStringList result;
    for (const auto &argument : raw_arguments)
        result.append(QString::fromLocal8Bit(argument));
    if (!result.isEmpty() && result.back().isEmpty())
        result.removeLast();
    return result;
}

bool matches_process(const process_identity &identity, const QString &process_directory, const QString &executable,
                     const QStringList &arguments) {
    if (identity.dbus_pid && QFileInfo(process_directory).fileName().toUInt() == identity.dbus_pid)
        return true;
    if (!identity.flatpak_id.isEmpty()) {
        QFile info(process_directory + "/root/.flatpak-info");
        if (info.open(QIODevice::ReadOnly)) {
            const auto contents = info.read(64 * 1024);
            return contents.split('\n').contains("name=" + identity.flatpak_id.toUtf8());
        }
        return false;
    }
    if (!identity.snap_name.isEmpty())
        return executable.startsWith("/snap/" + identity.snap_name + '/');
    if (identity.executable.isEmpty())
        return false;
    if (identity.executable == executable) {
        // Many applications share an interpreter; distinguish them by the script being launched.
        const auto program = QFileInfo(identity.executable).fileName();
        if (program.startsWith("python") || program == "perl" || program == "ruby" || program == "node" ||
            program == "java" || program == "bash" || program == "dash" || program == "zsh" || program == "sh") {
            if (arguments.size() <= identity.arguments.size())
                return false;
            for (qsizetype index = 0; index < identity.arguments.size(); ++index) {
                const auto &expected = identity.arguments[index];
                auto actual = arguments[index + 1];
                if (QFileInfo(expected).isAbsolute() && QFileInfo(expected).isFile()) {
                    if (QFileInfo(actual).isRelative())
                        actual = QFileInfo(process_directory + "/cwd").symLinkTarget() + '/' + actual;
                    if (canonical_path(actual) != canonical_path(expected))
                        return false;
                } else if (actual != expected) {
                    return false;
                }
            }
            return true;
        }
        return true;
    }
    // For an executable script, /proc/exe points to the interpreter; the script path remains in argv.
    if (identity.script)
        return xlibrary::script_process_matches(QFileInfo(process_directory).fileName().toUInt(),
                                                filesystem_path(identity.executable));
    return false;
}

QSet<quint32> matching_pids(const process_identity &identity) {
    QSet<quint32> result;
    const QDir proc("/proc");
    for (const auto &name : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const auto process_id = name.toUInt(&numeric);
        if (!numeric || process_id == 0)
            continue;
        const auto directory = proc.filePath(name);
        const auto executable = process_executable(directory);
        if (!executable.isEmpty() && matches_process(identity, directory, executable, process_arguments(directory)))
            result.insert(process_id);
    }
    return result;
}

QString desktop_escape(QString value) {
    value.replace('\\', "\\\\");
    value.replace('\n', "\\n");
    value.replace('\r', "\\r");
    value.replace('\t', "\\t");
    return value;
}

QString exec_quote(QString value) {
    value.replace('%', "%%");
    value.replace('\\', "\\\\");
    value.replace('"', "\\\"");
    value.replace('`', "\\`");
    value.replace('$', "\\$");
    return desktop_escape('"' + value + '"');
}

}

QSet<QString> Heartbeat::running_paths(const QVector<app_record> &applications) {
    if (!QFileInfo("/proc/self/exe").exists())
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "The /proc filesystem is unavailable: process state is unknown.")
                .toStdString());
    QSet<QString> result;
    QVector<QPair<QString, process_identity>> identities;
    for (const auto &application : applications) {
        if (!Application::file_target(application))
            continue;
        try {
            identities.append({absolute_path(application.path), application_identity(application)});
        } catch (const std::exception &) {
            // A deleted or corrupted entry must not prevent monitoring the other applications.
        }
    }
    const QDir proc("/proc");
    for (const auto &name : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        name.toUInt(&numeric);
        if (!numeric)
            continue;
        const auto directory = proc.filePath(name);
        const auto executable = process_executable(directory);
        if (executable.isEmpty())
            continue;
        const auto arguments = process_arguments(directory);
        for (const auto &[path, identity] : identities) {
            if (!result.contains(path) && matches_process(identity, directory, executable, arguments))
                result.insert(path);
        }
    }
    return result;
}

QString Application::new_instance_error(const app_record &record) {
    if (!file_target(record))
        return QCoreApplication::translate("XLibrary", "This target supports activation only.");
    if (QFileInfo(record.path).suffix() != "desktop")
        return {};
    try {
        const auto entry = read_entry(record.path);
        validate_entry(entry);
        if (entry.values.value("Exec").isEmpty())
            return QCoreApplication::translate("XLibrary",
                                               "Only activation is available: this desktop entry has no Exec command.");
        const auto command = entry_command(entry);
        if (executable_path(command.front()).isEmpty())
            return QCoreApplication::translate("XLibrary", "The executable specified by Exec is unavailable.");
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
    return {};
}

void Application::launch_application(const app_record &record, const launch_mode mode) {
    validate_launch_target(record);
    if (mode == launch_mode::elevated)
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "Elevation is only available on Windows").toStdString());
    if (!file_target(record)) {
        if (target_kind(record) != "steam")
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "This target is only supported on Windows").toStdString());
        if (mode == launch_mode::new_instance)
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Steam targets support activation only").toStdString());
        if (!target_available(record))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Steam installation directory is unavailable").toStdString());
        QProcess process;
        process.setProcessEnvironment(application_environment());
        process.setProgram("xdg-open");
        process.setArguments({"steam://run/" + record.path});
        if (!process.startDetached())
            throw std::runtime_error(process.errorString().toStdString());
        return;
    }
    QStringList command;
    QString directory = record.working_directory;
    if (QFileInfo(record.path).suffix() == "desktop") {
        const auto entry = read_entry(record.path);
        validate_entry(entry);
        if (mode != launch_mode::new_instance && !requires_fresh_launch(record) && record.arguments.isEmpty() &&
            record.working_directory.isEmpty() && activate_dbus(entry))
            return;
        command = entry_command(entry);
        command.append(user_arguments(record.arguments));
        if (directory.isEmpty())
            directory = entry.values.value("Path");
        if (entry.flag("Terminal"))
            command = terminal_command(command);
    } else {
        command.append(record.path);
        command.append(user_arguments(record.arguments));
        if (directory.isEmpty())
            directory = QFileInfo(record.path).absolutePath();
    }
    const auto executable = executable_path(command.takeFirst());
    if (executable.isEmpty())
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "The application executable is unavailable or is not executable.")
                .toStdString());
    if (!directory.isEmpty() && (!QFileInfo(directory).isAbsolute() || !QFileInfo(directory).isDir()))
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "The application working directory is unavailable.").toStdString());
    QProcess process;
    process.setProcessEnvironment(application_environment());
    process.setProgram(executable);
    process.setArguments(command);
    if (!directory.isEmpty())
        process.setWorkingDirectory(directory);
    if (!process.startDetached())
        throw std::runtime_error(QCoreApplication::translate("XLibrary", "Could not launch the application: %1")
                                     .arg(process.errorString())
                                     .toStdString());
}

QString Application::configured_arguments(const app_record &record) {
    if (target_kind(record) == "desktop")
        return read_entry(record.path).values.value("Exec") + ' ' + record.arguments;
    return record.arguments;
}

app_record Application::resolve_application(const QString &path) {
    app_record result;
    result.discovery_source = path;
    result.path = absolute_path(path);
    const QFileInfo info(result.path);
    if (!info.isFile())
        throw std::invalid_argument(
            QCoreApplication::translate("XLibrary", "The application file is unavailable.").toStdString());
    if (info.suffix() == "desktop") {
        const auto entry = read_entry(result.path);
        validate_entry(entry);
        result.name = localized_value(entry, "Name");
        result.working_directory = entry.values.value("Path");
    } else {
        if (!info.isExecutable())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary",
                                            "The file is not executable. Use a .desktop shortcut, executable file, "
                                            "AppImage or script with executable permission.")
                    .toStdString());
        result.name = info.completeBaseName();
    }
    return result;
}

QVector<app_record> Application::discover(const bool system_applications, const bool user_applications,
                                          QVector<discovery_issue> &issues, const std::atomic_bool &cancelled,
                                          const std::function<void(const discovery_progress &)> &progress) {
    if (progress)
        progress({-1, 0, 0, QCoreApplication::translate("XLibrary", "Preparing the XDG application list…")});
    QStringList entries;
    QSet<QString> desktop_ids;
    const auto directories = application_directories();
    for (qsizetype index = 0; index < directories.size(); ++index) {
        const auto &directory = directories[index];
        const QFileInfo directory_info(directory);
        if (directory_info.isDir() && !directory_info.isReadable()) {
            issues.append(discovery_issue{
                directory, QCoreApplication::translate("XLibrary", "Cannot access the application folder.")});
            continue;
        }
        QDirIterator files(directory, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            if (cancelled.load())
                return {};
            const auto path = files.next();
            auto desktop_id = QDir(directory).relativeFilePath(path);
            desktop_id.replace('/', '-');
            if (desktop_ids.contains(desktop_id)) {
                if ((index == 0 && user_applications) || (index > 0 && system_applications))
                    issues.append(
                        discovery_issue{path,
                                        QCoreApplication::translate(
                                            "XLibrary", "An entry from a higher-priority source overrides this entry."),
                                        discovery_issue_kind::duplicate});
                continue;
            }
            desktop_ids.insert(desktop_id);
            // Even a disabled source retains user override priority.
            if ((index == 0 && user_applications) || (index > 0 && system_applications))
                entries.append(path);
        }
    }
    QVector<app_record> result;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        if (cancelled.load())
            return {};
        if (progress)
            progress({static_cast<int>(90.0 * index / entries.size()), index, entries.size(),
                      QCoreApplication::translate("XLibrary", "Checking .desktop entries")});
        try {
            const auto entry = read_entry(entries[index]);
            if (!visible_entry(entry)) {
                issues.append(discovery_issue{
                    entry.path,
                    QCoreApplication::translate(
                        "XLibrary",
                        "The entry is hidden, is not an application, or does not match the current desktop."),
                    discovery_issue_kind::skipped});
                continue;
            }
            result.append(resolve_application(entry.path));
        } catch (const std::exception &error) {
            issues.append(discovery_issue{entries[index], QString::fromUtf8(error.what())});
        }
    }
    if (progress)
        progress({90, entries.size(), entries.size(),
                  QCoreApplication::translate("XLibrary", "Application checks complete")});
    if (system_applications || user_applications)
        result += discover_steam(issues, cancelled, progress);
    return cancelled.load() ? QVector<app_record>{} : result;
}

QVector<app_record> Application::discover_system(QVector<discovery_issue> &, const std::atomic_bool &,
                                                 const std::function<void(const discovery_progress &)> &) {
    return {};
}

bool Application::set_autostart(const bool enabled, QString &error) {
    QString executable;
    if (enabled) {
        try {
            executable = application_paths::appimage_file();
        } catch (const std::exception &failure) {
            error = QString::fromUtf8(failure.what());
            return false;
        }
        if (executable.isEmpty())
            executable = QCoreApplication::applicationFilePath();
    }
    const auto directory = xdg_home("XDG_CONFIG_HOME", "/.config") + "/autostart";
    if (!QDir().mkpath(directory)) {
        error = QCoreApplication::translate("XLibrary", "Could not create the XDG autostart folder.");
        return false;
    }
    QSaveFile file(directory + "/io.github.LapplandJulia.XLibrary.desktop");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = QCoreApplication::translate("XLibrary", "Could not open the autostart file: ") + file.errorString();
        return false;
    }
    const auto content = enabled ? "[Desktop Entry]\nType=Application\nName=XLibrary\nExec=" + exec_quote(executable) +
                                       " --background\nTerminal=false\n"
                                 : QString("[Desktop Entry]\nType=Application\nName=XLibrary\nHidden=true\n");
    const auto bytes = content.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        error = QCoreApplication::translate("XLibrary", "Could not save XDG autostart: ") + file.errorString();
        return false;
    }
    return true;
}

QProcessEnvironment Application::environment() {
    return application_environment();
}
QString Application::icon_name(const QString &path) {
    return localized_value(read_entry(path), "Icon");
}
QSet<quint32> Application::matching_processes(const app_record &record) {
    return matching_pids(application_identity(record));
}
