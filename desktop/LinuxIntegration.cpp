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
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
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
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStyle>
#include <QtGui/qguiapplication_platform.h>
#include <QUrl>
#include <QUuid>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <stdexcept>

#include "ApplicationPaths.h"
#include "PathHelpers.h"
#include "PlatformIntegration.h"
#include "WindowManager.h"

namespace {
bool wayland_session() {
    return QGuiApplication::platformName().startsWith("wayland") ||
           qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland";
}

#ifdef XLIBRARY_HAVE_X11
std::mutex x11_mutex;
int x11_error_code = 0;
Time hotkey_activation_time = CurrentTime;

int record_x11_error(Display *, XErrorEvent *const event) {
    x11_error_code = event->error_code;
    return 0;
}

class X11ErrorScope {
    Display *display;
    XErrorHandler previous;

public:
    explicit X11ErrorScope(Display *const value) : display(value), previous(XSetErrorHandler(record_x11_error)) {
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

QVector<unsigned long> window_property(Display *const display, const Window window, const Atom property,
                                       const Atom expected_type) {
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

unsigned int modifier_mask(Display *const display, const KeySym symbol) {
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

KeySym hotkey_symbol(const Qt::Key key) {
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

struct PortalShortcut {
    QString id;
    QVariantMap properties;
};
using PortalShortcuts = QList<PortalShortcut>;
Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(PortalShortcuts)

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}

class LinuxHotkeyState : public QObject {
    Q_OBJECT
    GlobalHotkey *owner;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QString session_path;
    QString request_path;
    QString action_id;
    QString trigger;
    bool binding = false;
    bool bound = false;
    bool failed = false;
    QTimer timeout;
    static constexpr auto service = "org.freedesktop.portal.Desktop";
    static constexpr auto portal_path = "/org/freedesktop/portal/desktop";
    static constexpr auto shortcut_interface = "org.freedesktop.portal.GlobalShortcuts";
    static constexpr auto request_interface = "org.freedesktop.portal.Request";
    static constexpr auto session_interface = "org.freedesktop.portal.Session";

    static QString token() {
        return QStringLiteral("xlibrary_") + QUuid::createUuid().toString(QUuid::Id128);
    }
    QString sender_path() const {
        auto sender = bus.baseService();
        sender.remove(0, 1);
        sender.replace('.', '_');
        return sender;
    }
    void close_path(const QString &path, const char *interface) {
        if (!path.isEmpty())
            bus.asyncCall(QDBusMessage::createMethodCall(service, path, interface, "Close"));
    }
    void close_portal() {
        timeout.stop();
        if (!request_path.isEmpty())
            bus.disconnect(service, request_path, request_interface, "Response", this,
                           SLOT(response(uint,QVariantMap)));
        close_path(request_path, request_interface);
        close_path(session_path, session_interface);
        request_path.clear();
        session_path.clear();
        bound = false;
    }
    void fail() {
        if (failed)
            return;
        failed = true;
        close_portal();
        emit owner->registration_failed(QCoreApplication::translate(
            "XLibrary", "The GlobalShortcuts portal did not register the shortcut. Use system keyboard settings: "
                        "KDE: Keyboard → Shortcuts; GNOME: Keyboard → Custom Shortcuts; "
                        "Xfce: Keyboard → Application Shortcuts. Command: XLibrary --overlay "
                        "(quick search) or XLibrary (library). For applications/scenarios use the command in their editor."));
    }
    void request(const QString &method, const QVariantList &arguments, QVariantMap options) {
        const auto handle_token = token();
        options.insert("handle_token", handle_token);
        request_path = "/org/freedesktop/portal/desktop/request/" + sender_path() + '/' + handle_token;
        if (!bus.connect(service, request_path, request_interface, "Response", this,
                         SLOT(response(uint,QVariantMap)))) {
            fail();
            return;
        }
        auto message = QDBusMessage::createMethodCall(service, portal_path, shortcut_interface, method);
        auto parameters = arguments;
        parameters.append(options);
        message.setArguments(parameters);
        const auto expected_path = request_path;
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, expected_path, watcher] {
            const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();
            // A fast Response can arrive before the method reply and start the next request.
            if (request_path != expected_path)
                return;
            if (reply.isError() || reply.value().path() != expected_path) {
                if (!reply.isError())
                    close_path(reply.value().path(), request_interface);
                fail();
            }
        });
        timeout.start();
    }

private slots:
    void response(uint code, const QVariantMap &results) {
        bus.disconnect(service, request_path, request_interface, "Response", this,
                       SLOT(response(uint,QVariantMap)));
        request_path.clear();
        timeout.stop();
        if (code != 0) {
            fail();
            return;
        }
        if (!binding) {
            // The portal historically returns this object path as a string.
            const auto handle = results.value("session_handle").toString();
            if (handle != session_path) {
                fail();
                return;
            }
            binding = true;
            request("BindShortcuts", {QVariant::fromValue(QDBusObjectPath(session_path)),
                                     QVariant::fromValue(PortalShortcuts{{action_id,
                                         {{"description", QCoreApplication::translate("XLibrary", "XLibrary shortcut") +
                                                               " (" + trigger + ')'},
                                          {"preferred_trigger", trigger}}}}), QString{}}, {});
        } else {
            const auto shortcuts = qdbus_cast<PortalShortcuts>(results.value("shortcuts"));
            bound = std::ranges::any_of(shortcuts, [this](const auto &shortcut) { return shortcut.id == action_id; });
            if (!bound)
                fail();
        }
    }
    void portal_activated(const QDBusObjectPath &session, const QString &id, qulonglong,
                          const QVariantMap &options) {
        if (!bound || session.path() != session_path || id != action_id)
            return;
        const auto activation_token = options.value("activation_token").toString().toUtf8();
        if (QGuiApplication::platformName().startsWith("wayland")) {
            // Qt's xdg-shell requestActivate consumes this token (Qt 6.5+).
            qunsetenv("XDG_ACTIVATION_TOKEN");
            if (!activation_token.isEmpty() && activation_token.size() <= 4096 && !activation_token.contains('\0'))
                qputenv("XDG_ACTIVATION_TOKEN", activation_token);
        }
        emit owner->activated();
        if (QGuiApplication::platformName().startsWith("wayland"))
            qunsetenv("XDG_ACTIVATION_TOKEN");
    }
    void session_closed(const QVariantMap &) {
        if (!session_path.isEmpty())
            fail();
    }

public:
    explicit LinuxHotkeyState(GlobalHotkey *value) : owner(value) {
        timeout.setSingleShot(true);
        timeout.setInterval(120000); // A portal may wait for the user's permission dialog.
        connect(&timeout, &QTimer::timeout, this, &LinuxHotkeyState::fail);
    }
    void start_portal(const QKeySequence &value, const int id) {
        qDBusRegisterMetaType<PortalShortcut>();
        qDBusRegisterMetaType<PortalShortcuts>();
        action_id = "xlibrary_action_" + QString::number(id);
        const auto combination = value[0];
        const auto modifiers = combination.keyboardModifiers();
        if (modifiers & Qt::ControlModifier)
            trigger += "CTRL+";
        if (modifiers & Qt::AltModifier)
            trigger += "ALT+";
        if (modifiers & Qt::ShiftModifier)
            trigger += "SHIFT+";
        if (modifiers & Qt::MetaModifier)
            trigger += "LOGO+";
        const auto key = combination.key();
        trigger += key == Qt::Key_Space ? QStringLiteral("space")
                   : key >= Qt::Key_A && key <= Qt::Key_Z ? QString(QChar('a' + key - Qt::Key_A))
                   : QKeySequence(key).toString(QKeySequence::PortableText);
        auto *service_watcher = new QDBusServiceWatcher(service, bus,
                                                       QDBusServiceWatcher::WatchForUnregistration, this);
        connect(service_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { fail(); });
        auto message = QDBusMessage::createMethodCall(service, portal_path, "org.freedesktop.DBus.Properties", "Get");
        message.setArguments({shortcut_interface, "version"});
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            const QDBusPendingReply<QDBusVariant> reply = *watcher;
            watcher->deleteLater();
            if (failed)
                return;
            if (reply.isError() || reply.value().variant().toUInt() < 1) {
                fail();
                return;
            }
            const auto session_token = token();
            session_path = "/org/freedesktop/portal/desktop/session/" + sender_path() + '/' + session_token;
            if (!bus.connect(service, portal_path, shortcut_interface, "Activated", this,
                             SLOT(portal_activated(QDBusObjectPath,QString,qulonglong,QVariantMap))) ||
                !bus.connect(service, session_path, session_interface, "Closed", this,
                             SLOT(session_closed(QVariantMap)))) {
                fail();
                return;
            }
            request("CreateSession", {}, {{"session_handle_token", session_token}});
        });
    }
#ifdef XLIBRARY_HAVE_X11
    x11_display display{nullptr, &XCloseDisplay};
    std::unique_ptr<QSocketNotifier> notifier;
    int keycode = 0;
    unsigned int modifiers = 0;
    unsigned int ignored_modifiers = LockMask;
    QVector<unsigned int> lock_combinations;
    bool pressed = false;
#endif

    ~LinuxHotkeyState() override {
        close_portal();
#ifdef XLIBRARY_HAVE_X11
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
#endif
    }
};

bool WindowManager::activate_window(const app_record &record) {
    if (wayland_session())
        return false;
#ifdef XLIBRARY_HAVE_X11
    const auto processes = Application::matching_processes(record);
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

GlobalHotkey::GlobalHotkey(QObject *const parent) : QObject(parent) {
    connect(this, &GlobalHotkey::registration_failed, this, [this] { sequence = {}; });
}

GlobalHotkey::~GlobalHotkey() = default;

void desktop_platform::activate_overlay(QWidget *const window) {
    window->raise();
#if defined(XLIBRARY_HAVE_X11) && QT_CONFIG(xcb)
    if (!wayland_session() && hotkey_activation_time != CurrentTime) {
        if (const auto *native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
            const std::lock_guard lock(x11_mutex);
            auto *display = native->display();
            X11ErrorScope errors(display);
            const auto native_window = static_cast<Window>(window->winId());
            const auto root = DefaultRootWindow(display);
            const auto user_time_atom = XInternAtom(display, "_NET_WM_USER_TIME", False);
            const unsigned long timestamp = hotkey_activation_time;
            XChangeProperty(display, native_window, user_time_atom, XA_CARDINAL, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(&timestamp), 1);
            XEvent event{};
            event.xclient.type = ClientMessage;
            event.xclient.window = native_window;
            event.xclient.message_type = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
            event.xclient.format = 32;
            event.xclient.data.l[0] = 1;
            event.xclient.data.l[1] = static_cast<long>(timestamp);
            if (XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event) &&
                errors.succeeded())
                return;
        }
    }
#endif
    // Qt requests xdg-activation on Wayland; the compositor decides whether to grant it.
    if (auto *handle = window->windowHandle())
        handle->requestActivate();
}

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
        const auto combination = value[0];
        const auto key = combination.key();
        const auto modifiers = combination.keyboardModifiers();
        if (!((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9) ||
              (key >= Qt::Key_F1 && key <= Qt::Key_F24) || key == Qt::Key_Space) ||
            !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) ||
            (modifiers & ~(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier))) {
            error = QCoreApplication::translate("XLibrary", "Use Ctrl, Alt, or Super with a letter, digit, F1–F24, or Space.");
            return false;
        }
        linux_state = std::make_unique<LinuxHotkeyState>(this);
        if (!current_id)
            current_id = next_id++;
        sequence = value;
        linux_state->start_portal(value, current_id);
        return true; // Submission only; registration_failed reports asynchronous refusal.
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
        auto state = std::make_unique<LinuxHotkeyState>(this);
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
            Time activation_time = CurrentTime;
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
                        if (linux_state->pressed) {
                            activate = true;
                            activation_time = event.xkey.time;
                        }
                        linux_state->pressed = false;
                    } else if (event.type == x11_key_press &&
                               static_cast<int>(event.xkey.keycode) == linux_state->keycode &&
                               (event.xkey.state & 0xffU & ~linux_state->ignored_modifiers) == linux_state->modifiers) {
                        linux_state->pressed = true;
                    }
                }
            }
            if (activate) {
                // The passive keyboard grab ends on release. Focus requests on press race that grab,
                // and Qt cannot infer this user timestamp from our separate X11 connection.
                const QScopedValueRollback activation_scope(hotkey_activation_time, activation_time);
                emit activated();
            }
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

void desktop_platform::open_folder(const QString &path) {
    if (!open_url(QUrl::fromLocalFile(QFileInfo(path).absolutePath())))
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary",
                                        "Could not open the application folder. Check that xdg-open is available.")
                .toStdString());
}

bool desktop_platform::open_url(const QUrl &url) {
    const auto executable = QStandardPaths::findExecutable("xdg-open");
    if (executable.isEmpty())
        return false;
    QProcess process;
    try {
        process.setProcessEnvironment(Application::environment());
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
            icon_name = Application::icon_name(path);
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

void desktop_platform::launch_application(const app_record &record, const launch_mode mode) {
    if (mode == launch_mode::activate && WindowManager::activate_window(record))
        return;
    Application::launch_application(record, mode);
}

bool desktop_platform::set_autostart(const bool enabled, QString &error) {
    return Application::set_autostart(enabled, error);
}

#include "LinuxIntegration.moc"
