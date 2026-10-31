#include "LaunchArguments.h"
#include "Linux/LinuxProcessor.h"
#include "localization.h"
#ifdef XLIBRARY_HAVE_X11
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>

namespace {
constexpr int x11_key_press = KeyPress;
constexpr int x11_key_release = KeyRelease;
constexpr int x11_success = Success;
}
// Xlib defines macros with the same names as Qt enums and types.
#undef None
#undef Bool
#undef Status
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef FontChange
#undef Expose
#undef CursorShape
#undef Unsorted
#undef Success
#undef GrayScale
#undef Always
#undef Above
#undef Below
#undef Complex
#endif

#include <algorithm>
#include <memory>
#include <mutex>
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDirIterator>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStyle>
#include <QUrl>
#include <QWidget>
#include <stdexcept>

#include "application_paths.h"
#include "path_helpers.h"
#include "platform_integration.h"

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

QString xdg_home(const char *variable, const QString &fallback) {
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

process_identity application_identity(const QString &path) {
    process_identity result;
    QStringList command{path};
    if (QFileInfo(path).suffix() == "desktop") {
        const auto entry = read_entry(path);
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
    }
    if (QFileInfo(command.front()).fileName() == "env") {
        command.removeFirst();
        while (!command.isEmpty()) {
            const auto argument = command.front();
            if (argument == "-u" || argument == "--unset") {
                if (command.size() < 2)
                    return {};
                command.removeFirst();
                command.removeFirst();
            } else if (argument.startsWith('-') || argument.contains('=')) {
                command.removeFirst();
            } else {
                break;
            }
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

#ifdef XLIBRARY_HAVE_X11
QSet<quint32> matching_processes(const process_identity &identity) {
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
#endif

bool wayland_session() {
    return QGuiApplication::platformName().startsWith("wayland") ||
           qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland";
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

#ifdef XLIBRARY_HAVE_X11
std::mutex x11_mutex;
int x11_error_code = 0;

int record_x11_error(Display *, XErrorEvent *event) {
    x11_error_code = event->error_code;
    return 0;
}

class X11ErrorScope {
    Display *display;
    XErrorHandler previous;

public:
    explicit X11ErrorScope(Display *value) : display(value), previous(XSetErrorHandler(record_x11_error)) {
        XSync(display, False);
        x11_error_code = 0;
    }
    ~X11ErrorScope() {
        XSync(display, False);
        XSetErrorHandler(previous);
    }
    bool succeeded() {
        XSync(display, False);
        return x11_error_code == 0;
    }
};

using x11_display = std::unique_ptr<Display, decltype(&XCloseDisplay)>;

QVector<unsigned long> window_property(Display *display, Window window, Atom property, Atom expected_type) {
    Atom actual_type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char *raw_value = nullptr;
    const auto status = XGetWindowProperty(display, window, property, 0, 65536, False, expected_type, &actual_type,
                                           &format, &count, &remaining, &raw_value);
    const std::unique_ptr<unsigned char, decltype(&XFree)> value(raw_value, &XFree);
    if (status != x11_success || actual_type != expected_type || format != 32 || !raw_value)
        return {};
    const auto *data = reinterpret_cast<const unsigned long *>(raw_value);
    return QVector<unsigned long>(data, data + count);
}

unsigned int modifier_mask(Display *display, KeySym symbol) {
    const auto keycode = XKeysymToKeycode(display, symbol);
    const std::unique_ptr<XModifierKeymap, decltype(&XFreeModifiermap)> mapping(XGetModifierMapping(display),
                                                                                &XFreeModifiermap);
    if (!mapping || !keycode)
        return 0;
    for (int modifier = 0; modifier < 8; ++modifier) {
        for (int slot = 0; slot < mapping->max_keypermod; ++slot) {
            if (mapping->modifiermap[modifier * mapping->max_keypermod + slot] == keycode)
                return 1U << modifier;
        }
    }
    return 0;
}

KeySym hotkey_symbol(Qt::Key key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return XK_a + key - Qt::Key_A;
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return XK_0 + key - Qt::Key_0;
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return XK_F1 + key - Qt::Key_F1;
    return key == Qt::Key_Space ? XK_space : NoSymbol;
}
#endif
}

class LinuxHotkeyState {
public:
#ifdef XLIBRARY_HAVE_X11
    x11_display display{nullptr, &XCloseDisplay};
    std::unique_ptr<QSocketNotifier> notifier;
    int keycode = 0;
    unsigned int modifiers = 0;
    unsigned int ignored_modifiers = LockMask;
    QVector<unsigned int> lock_combinations;
    bool pressed = false;

    ~LinuxHotkeyState() {
        notifier.reset();
        if (display) {
            const std::lock_guard lock(x11_mutex);
            {
                const X11ErrorScope errors(display.get());
                if (keycode) {
                    for (const auto locks : lock_combinations)
                        XUngrabKey(display.get(), keycode, modifiers | locks, DefaultRootWindow(display.get()));
                }
            }
            display.reset();
        }
    }
#endif
};

bool desktop_platform::activate_window(const app_record &record) {
    const auto &path = record.path;
    if (wayland_session())
        return false;
#ifdef XLIBRARY_HAVE_X11
    const auto processes = matching_processes(application_identity(path));
    if (processes.isEmpty())
        return false;
    const std::lock_guard lock(x11_mutex);
    const x11_display display(XOpenDisplay(nullptr), &XCloseDisplay);
    if (!display)
        return false;
    X11ErrorScope errors(display.get());
    const auto root = DefaultRootWindow(display.get());
    auto windows =
        window_property(display.get(), root, XInternAtom(display.get(), "_NET_CLIENT_LIST_STACKING", False), XA_WINDOW);
    if (windows.isEmpty())
        windows =
            window_property(display.get(), root, XInternAtom(display.get(), "_NET_CLIENT_LIST", False), XA_WINDOW);
    const auto process_atom = XInternAtom(display.get(), "_NET_WM_PID", False);
    const auto active_atom = XInternAtom(display.get(), "_NET_ACTIVE_WINDOW", False);
    const auto current_active = window_property(display.get(), root, active_atom, XA_WINDOW);
    for (auto iterator = windows.crbegin(); iterator != windows.crend(); ++iterator) {
        const auto window = *iterator;
        const auto process_id = window_property(display.get(), window, process_atom, XA_CARDINAL);
        if (process_id.isEmpty() || !processes.contains(static_cast<quint32>(process_id.front())))
            continue;
        // EWMH delegates focus decisions to the window manager, including restoring a minimized window.
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = active_atom;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 2;
        event.xclient.data.l[1] = CurrentTime;
        event.xclient.data.l[2] = current_active.isEmpty() ? 0 : static_cast<long>(current_active.front());
        if (XSendEvent(display.get(), root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event) == 0 ||
            !errors.succeeded())
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary",
                                            "Could not send the window activation request to the X11 window manager.")
                    .toStdString());
        return true;
    }
#endif
    return false;
}

GlobalHotkey::GlobalHotkey(QObject *parent) : QObject(parent) {
}

GlobalHotkey::~GlobalHotkey() = default;

bool GlobalHotkey::set_sequence(const QKeySequence &value, QString &error) {
    if (sequence == value)
        return true;
    if (value.isEmpty()) {
        linux_state.reset();
        sequence = value;
        return true;
    }
    if (value.count() != 1) {
        error = QCoreApplication::translate("XLibrary", "Use a single key combination.");
        return false;
    }
    if (wayland_session()) {
        error = QCoreApplication::translate(
            "XLibrary", "Wayland manages global shortcuts. Assign XLibrary in the system keyboard settings.");
        return false;
    }
#ifdef XLIBRARY_HAVE_X11
    const auto combination = value[0];
    const auto symbol = hotkey_symbol(combination.key());
    if (symbol == NoSymbol) {
        error = QCoreApplication::translate("XLibrary", "Use a letter, digit, F1–F24, or Space.");
        return false;
    }
    const auto qt_modifiers = combination.keyboardModifiers();
    if (!(qt_modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        error = QCoreApplication::translate("XLibrary", "Add Ctrl, Alt, or Super to the global shortcut.");
        return false;
    }
    if (qt_modifiers & ~(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier)) {
        error = QCoreApplication::translate("XLibrary", "The shortcut contains an unsupported modifier.");
        return false;
    }
    if (!linux_state) {
        auto state = std::make_unique<LinuxHotkeyState>();
        {
            const std::lock_guard lock(x11_mutex);
            state->display.reset(XOpenDisplay(nullptr));
            if (!state->display) {
                error = QCoreApplication::translate("XLibrary",
                                                    "Could not connect to the X11 server for the global shortcut.");
                return false;
            }
            int detectable = False;
            XkbSetDetectableAutoRepeat(state->display.get(), True, &detectable);
            state->ignored_modifiers |=
                modifier_mask(state->display.get(), XK_Num_Lock) | modifier_mask(state->display.get(), XK_Scroll_Lock);
        }
        state->lock_combinations.append(0);
        for (unsigned int bit = 1; bit <= Mod5Mask; bit <<= 1) {
            if (!(state->ignored_modifiers & bit))
                continue;
            const auto size = state->lock_combinations.size();
            for (qsizetype index = 0; index < size; ++index)
                state->lock_combinations.append(state->lock_combinations[index] | bit);
        }
        state->notifier =
            std::make_unique<QSocketNotifier>(ConnectionNumber(state->display.get()), QSocketNotifier::Read);
        connect(state->notifier.get(), &QSocketNotifier::activated, this, [this] {
            bool activate = false;
            {
                const std::lock_guard lock(x11_mutex);
                while (XPending(linux_state->display.get())) {
                    XEvent event{};
                    XNextEvent(linux_state->display.get(), &event);
                    if (event.type == x11_key_release && static_cast<int>(event.xkey.keycode) == linux_state->keycode) {
                        if (XPending(linux_state->display.get())) {
                            XEvent next{};
                            XPeekEvent(linux_state->display.get(), &next);
                            if (next.type == x11_key_press && next.xkey.keycode == event.xkey.keycode &&
                                next.xkey.time == event.xkey.time)
                                continue;
                        }
                        linux_state->pressed = false;
                    } else if (event.type == x11_key_press &&
                               static_cast<int>(event.xkey.keycode) == linux_state->keycode &&
                               (event.xkey.state & 0xffU & ~linux_state->ignored_modifiers) == linux_state->modifiers) {
                        if (!linux_state->pressed)
                            activate = true;
                        linux_state->pressed = true;
                    }
                }
            }
            if (activate)
                emit activated();
        });
        linux_state = std::move(state);
    }
    const std::lock_guard lock(x11_mutex);
    auto *display = linux_state->display.get();
    const auto keycode = XKeysymToKeycode(display, symbol);
    unsigned int modifiers = 0;
    if (qt_modifiers & Qt::ControlModifier)
        modifiers |= ControlMask;
    if (qt_modifiers & Qt::ShiftModifier)
        modifiers |= ShiftMask;
    if (qt_modifiers & Qt::AltModifier) {
        const auto alt = modifier_mask(display, XK_Alt_L);
        if (!alt) {
            error = QCoreApplication::translate("XLibrary", "The X11 keymap has no Alt modifier.");
            return false;
        }
        modifiers |= alt;
    }
    if (qt_modifiers & Qt::MetaModifier) {
        const auto super = modifier_mask(display, XK_Super_L);
        if (!super) {
            error = QCoreApplication::translate("XLibrary", "The X11 keymap has no Super modifier.");
            return false;
        }
        modifiers |= super;
    }
    if (!keycode) {
        error = QCoreApplication::translate("XLibrary", "This key is unavailable in the current X11 keymap.");
        return false;
    }
    if (keycode == linux_state->keycode && modifiers == linux_state->modifiers) {
        sequence = value;
        return true;
    }
    X11ErrorScope errors(display);
    for (const auto locks : linux_state->lock_combinations)
        XGrabKey(display, keycode, modifiers | locks, DefaultRootWindow(display), False, GrabModeAsync, GrabModeAsync);
    if (!errors.succeeded()) {
        for (const auto locks : linux_state->lock_combinations)
            XUngrabKey(display, keycode, modifiers | locks, DefaultRootWindow(display));
        error = QCoreApplication::translate("XLibrary",
                                            "The shortcut is in use by the window manager or another application.");
        return false;
    }
    if (linux_state->keycode) {
        for (const auto locks : linux_state->lock_combinations)
            XUngrabKey(display, linux_state->keycode, linux_state->modifiers | locks, DefaultRootWindow(display));
    }
    linux_state->keycode = keycode;
    linux_state->modifiers = modifiers;
    linux_state->pressed = false;
    sequence = value;
    return true;
#else
    error = QCoreApplication::translate(
        "XLibrary", "The global shortcut requires an X11-enabled build. Assign XLibrary using system settings.");
    return false;
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &, void *, qintptr *) {
    return false;
}

QSet<QString> desktop_platform::running_paths(const QVector<app_record> &applications) {
    if (!QFileInfo("/proc/self/exe").exists())
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "The /proc filesystem is unavailable: process state is unknown.")
                .toStdString());
    QSet<QString> result;
    QVector<QPair<QString, process_identity>> identities;
    for (const auto &application : applications) {
        try {
            identities.append({absolute_path(application.path), application_identity(application.path)});
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

QString desktop_platform::new_instance_error(const app_record &record) {
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

void desktop_platform::launch_application(const app_record &record, launch_mode mode) {
    if (mode == launch_mode::activate && activate_window(record))
        return;
    QStringList command;
    QString directory = record.working_directory;
    if (QFileInfo(record.path).suffix() == "desktop") {
        const auto entry = read_entry(record.path);
        validate_entry(entry);
        if (mode != launch_mode::new_instance && record.arguments.isEmpty() && record.working_directory.isEmpty() &&
            activate_dbus(entry))
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

void desktop_platform::open_folder(const QString &path) {
    if (!open_url(QUrl::fromLocalFile(QFileInfo(path).absolutePath())))
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary",
                                        "Could not open the application folder. Check that xdg-open is available.")
                .toStdString());
}

bool desktop_platform::open_url(const QUrl &url) {
    const auto executable = executable_path("xdg-open");
    if (executable.isEmpty())
        return false;
    QProcess process;
    try {
        process.setProcessEnvironment(application_environment());
    } catch (const std::exception &) {
        return false;
    }
    process.setProgram(executable);
    process.setArguments({url.toString(QUrl::FullyEncoded)});
    return process.startDetached();
}

bool desktop_platform::installed_for_updates() {
    return false;
}

bool desktop_platform::start_update_installer(const QString &, QString &error) {
    error = QCoreApplication::translate("XLibrary", "Use the GitHub release to update this installation.");
    return false;
}

QImage desktop_platform::application_icon(const QString &path) {
    QString icon_name;
    if (QFileInfo(path).suffix() == "desktop") {
        try {
            icon_name = localized_value(read_entry(path), "Icon");
        } catch (const std::exception &) {
            return {};
        }
        if (QFileInfo(icon_name).isAbsolute()) {
            const QImage image(icon_name);
            if (!image.isNull())
                return image;
        }
    }
    // LibraryModel calls the Linux icon engine on the GUI thread: QPixmap cannot move to QtConcurrent.
    QIcon icon;
    if (!icon_name.isEmpty() && !QFileInfo(icon_name).isAbsolute())
        icon = QIcon::fromTheme(icon_name);
    if (icon.isNull())
        icon = QIcon::fromTheme("application-x-executable", QFileIconProvider{}.icon(QFileInfo(path)));
    return icon.pixmap(64, 64).toImage();
}

app_record desktop_platform::resolve_application(const QString &path) {
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
                QCoreApplication::translate("XLibrary", "The file is not executable.").toStdString());
        result.name = info.completeBaseName();
    }
    return result;
}

QVector<app_record> desktop_platform::discover(bool system_applications, bool user_applications,
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
    return cancelled.load() ? QVector<app_record>{} : result;
}

bool desktop_platform::set_autostart(bool enabled, QString &error) {
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

bool desktop_platform::animations_enabled() {
    return qApp->style()->styleHint(QStyle::SH_Widget_Animate) != 0;
}

QString desktop_platform::default_global_shortcut() {
    return default_overlay_shortcut().isEmpty() ? QString{} : QStringLiteral("Ctrl+Alt+Shift+Space");
}

QString desktop_platform::default_overlay_shortcut() {
#ifdef XLIBRARY_HAVE_X11
    if (!wayland_session() && QGuiApplication::platformName() == "xcb")
        return "Ctrl+Alt+Space";
#endif
    return {};
}

void desktop_platform::apply_backdrop(QWidget *, bool, bool) {
    // Qt draws a fully opaque version; Linux has no generally available DWM backdrop.
}
