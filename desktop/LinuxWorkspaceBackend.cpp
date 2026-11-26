#include <QCoreApplication>

#include "WorkspaceWindowBackend.h"

#ifdef FORADECK_HAVE_WORKSPACE_X11
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/res.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits.h>
#include <limits>
#include <QFile>
#include <QFileInfo>
#include <QMargins>
#include <QThread>
#endif

namespace {

class LaunchOnlyWorkspaceBackend final : public WorkspaceWindowBackend {
public:
    explicit LaunchOnlyWorkspaceBackend(QString reason) : limitation(std::move(reason)) {}

    workspace_desktop snapshot() override {
        workspace_desktop desktop;
        desktop.limitation = limitation;
        return desktop;
    }

    bool activate(const workspace_window &) override { return false; }
    bool place(const workspace_window &, const QRect &, bool) override { return false; }

private:
    QString limitation;
};

QString wayland_limitation() {
    return QCoreApplication::translate(
        "Workspaces", "Wayland workspaces can launch resources only. Window capture and placement are unavailable.");
}

#ifdef FORADECK_HAVE_WORKSPACE_X11

template <typename T> using x11_reply = std::unique_ptr<T, decltype(&std::free)>;

template <typename T> x11_reply<T> take_reply(T *reply) {
    return x11_reply<T>(reply, &std::free);
}

struct x11_atoms {
    xcb_atom_t supported = XCB_NONE;
    xcb_atom_t supporting_wm_check = XCB_NONE;
    xcb_atom_t client_list = XCB_NONE;
    xcb_atom_t client_list_stacking = XCB_NONE;
    xcb_atom_t active_window = XCB_NONE;
    xcb_atom_t moveresize_window = XCB_NONE;
    xcb_atom_t current_desktop = XCB_NONE;
    xcb_atom_t work_area = XCB_NONE;
    xcb_atom_t desktop = XCB_NONE;
    xcb_atom_t strut = XCB_NONE;
    xcb_atom_t strut_partial = XCB_NONE;
    xcb_atom_t window_type = XCB_NONE;
    xcb_atom_t normal = XCB_NONE;
    xcb_atom_t dock = XCB_NONE;
    xcb_atom_t state = XCB_NONE;
    xcb_atom_t hidden = XCB_NONE;
    xcb_atom_t fullscreen = XCB_NONE;
    xcb_atom_t skip_taskbar = XCB_NONE;
    xcb_atom_t skip_pager = XCB_NONE;
    xcb_atom_t maximized_horz = XCB_NONE;
    xcb_atom_t maximized_vert = XCB_NONE;
    xcb_atom_t frame_extents = XCB_NONE;
    xcb_atom_t wm_pid = XCB_NONE;
    xcb_atom_t wm_name = XCB_NONE;
    xcb_atom_t utf8_string = XCB_NONE;
    xcb_atom_t wm_protocols = XCB_NONE;
    xcb_atom_t wm_take_focus = XCB_NONE;
    xcb_atom_t allowed_actions = XCB_NONE;
    xcb_atom_t action_move = XCB_NONE;
    xcb_atom_t action_resize = XCB_NONE;
    xcb_atom_t action_maximize_horz = XCB_NONE;
    xcb_atom_t action_maximize_vert = XCB_NONE;
};

struct x11_desktop {
    QVector<xcb_atom_t> supported;
    QVector<xcb_window_t> windows;
};

struct x11_window {
    workspace_window window;
    QMargins decorations;
    bool maximized_horz = false;
    bool maximized_vert = false;
};

quint64 process_birth(const QByteArray &proc) {
    QFile stat_file(QFile::decodeName(proc + "/stat"));
    if (!stat_file.open(QIODevice::ReadOnly))
        return 0;
    const auto data = stat_file.read(4096);
    // The comm field can contain spaces and parentheses. Field 22 follows its closing parenthesis.
    const auto end_of_comm = data.lastIndexOf(')');
    if (end_of_comm < 0 || end_of_comm + 2 >= data.size())
        return 0;
    const auto fields = data.sliced(end_of_comm + 2).split(' ');
    return fields.size() > 19 ? fields.at(19).toULongLong() : 0;
}

bool read_process(quint32 pid, workspace_window &window) {
    if (!pid || pid == QCoreApplication::applicationPid() || pid > std::numeric_limits<pid_t>::max())
        return false;
    const auto proc = QByteArray("/proc/") + QByteArray::number(pid);
    const auto proc_exe = proc + "/exe";

    struct stat proc_details {};
    if (::stat(proc.constData(), &proc_details) != 0 || proc_details.st_uid != geteuid())
        return false;
    const auto birth = process_birth(proc);
    if (!birth)
        return false;

    std::array<char, PATH_MAX> target{};
    const auto length = readlink(proc_exe.constData(), target.data(), target.size());
    if (length <= 0 || static_cast<size_t>(length) == target.size() || target.front() != '/')
        return false;
    const QByteArray raw_executable(target.data(), length);
    if (raw_executable.endsWith(" (deleted)"))
        return false;
    const auto executable = QFileInfo(QFile::decodeName(raw_executable)).canonicalFilePath();
    if (executable.isEmpty())
        return false;

    const auto executable_name = QFile::encodeName(executable);
    struct stat process_file {};
    struct stat local_file {};
    // The executable path may name a different file inside a mount namespace.
    if (::stat(proc_exe.constData(), &process_file) != 0 || ::stat(executable_name.constData(), &local_file) != 0 ||
        !S_ISREG(local_file.st_mode) || process_file.st_dev != local_file.st_dev ||
        process_file.st_ino != local_file.st_ino || process_birth(proc) != birth) {
        return false;
    }
    window.pid = pid;
    window.process_birth = birth;
    window.executable = executable;
    return true;
}

bool local_x11_socket(xcb_connection_t *connection) {
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    return getpeername(xcb_get_file_descriptor(connection), reinterpret_cast<sockaddr *>(&address), &size) == 0 &&
           address.ss_family == AF_UNIX;
}

class X11WorkspaceBackend final : public WorkspaceWindowBackend {
public:
    workspace_desktop snapshot() override {
        workspace_desktop desktop;
        const auto state = desktop_state();
        if (!state) {
            if (xwayland) {
                desktop.limitation = wayland_limitation();
            } else if (connection && root != XCB_NONE && !has_local_pid) {
                desktop.limitation = QCoreApplication::translate(
                    "Workspaces",
                    "The X11 server cannot verify window ownership. Workspaces can launch resources only.");
            } else {
                desktop.limitation = QCoreApplication::translate(
                    "Workspaces",
                    "Window capture and placement require a local X11 session with a compatible window manager.");
            }
            return desktop;
        }

        desktop.can_capture = true;
        desktop.monitors = monitors(state->windows);
        desktop.can_place = !desktop.monitors.isEmpty() && supports_placement(*state);
        if (!desktop.can_place) {
            desktop.limitation = QCoreApplication::translate(
                "Workspaces", "Window placement is unavailable with this X11 window manager.");
        }
        for (const auto id : state->windows)
            if (auto window = read_window(id))
                desktop.windows.append(std::move(window->window));
        return desktop;
    }

    bool activate(const workspace_window &window) override {
        const auto state = desktop_state();
        if (!state || !state->supported.contains(atoms.active_window))
            return false;
        const auto active = property32(root, atoms.active_window, XCB_ATOM_WINDOW);
        return verified_window(window, *state) &&
               send_message(static_cast<xcb_window_t>(window.handle), atoms.active_window,
                            {2, XCB_CURRENT_TIME, active.value(0)});
    }

    bool place(const workspace_window &window, const QRect &frame, bool maximized) override {
        const auto state = desktop_state();
        if (!frame.isValid() || !state || !supports_placement(*state))
            return false;
        const auto visible_monitors = monitors(state->windows);
        if (std::ranges::none_of(visible_monitors,
                                 [&frame](const auto &monitor) { return monitor.work_area.contains(frame); }))
            return false;
        auto current = verified_window(window, *state);
        if (!current)
            return false;
        if (current->window.maximized == maximized && current->maximized_horz == current->maximized_vert &&
            current->window.frame == frame)
            return true;

        const auto id = static_cast<xcb_window_t>(window.handle);
        if (!can_move(id, maximized))
            return false;
        if (current->maximized_horz || current->maximized_vert) {
            if (!send_message(id, atoms.state, {0, atoms.maximized_horz, atoms.maximized_vert, 2}))
                return false;
            // The normal window can have different frame extents after the WM removes maximization.
            for (int attempt = 0; attempt < 20; ++attempt) {
                current = verified_window(window, *state);
                if (!current || (!current->maximized_horz && !current->maximized_vert))
                    break;
                QThread::msleep(10);
            }
            if (!current || current->maximized_horz || current->maximized_vert)
                return false;
        }

        const auto client = frame.marginsRemoved(current->decorations);
        if (!client.isValid() || client.width() > std::numeric_limits<uint16_t>::max() ||
            client.height() > std::numeric_limits<uint16_t>::max())
            return false;
        // Static gravity specifies the client origin. The caller uses the decorated frame.
        constexpr uint32_t flags = XCB_GRAVITY_STATIC | (0xf << 8) | (2 << 12);
        if (!send_message(id, atoms.moveresize_window,
                          {flags, static_cast<uint32_t>(client.x()), static_cast<uint32_t>(client.y()),
                           static_cast<uint32_t>(client.width()), static_cast<uint32_t>(client.height())}))
            return false;
        return !maximized || (verified_window(window, *state) &&
                              send_message(id, atoms.state, {1, atoms.maximized_horz, atoms.maximized_vert, 2}));
    }

private:
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection{nullptr, &xcb_disconnect};
    xcb_window_t root = XCB_NONE;
    int screen_number = 0;
    bool xwayland = false;
    bool has_local_pid = false;
    bool has_randr_monitors = false;
    x11_atoms atoms;

    xcb_atom_t atom(const char *name, bool only_if_exists = false) const {
        const auto length = QByteArrayView(name).size();
        const auto cookie = xcb_intern_atom(connection.get(), only_if_exists, static_cast<uint16_t>(length), name);
        const auto reply = take_reply(xcb_intern_atom_reply(connection.get(), cookie, nullptr));
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_NONE);
    }

    void load_atoms() {
        atoms = {.supported = atom("_NET_SUPPORTED"),
                 .supporting_wm_check = atom("_NET_SUPPORTING_WM_CHECK"),
                 .client_list = atom("_NET_CLIENT_LIST"),
                 .client_list_stacking = atom("_NET_CLIENT_LIST_STACKING"),
                 .active_window = atom("_NET_ACTIVE_WINDOW"),
                 .moveresize_window = atom("_NET_MOVERESIZE_WINDOW"),
                 .current_desktop = atom("_NET_CURRENT_DESKTOP"),
                 .work_area = atom("_NET_WORKAREA"),
                 .desktop = atom("_NET_WM_DESKTOP"),
                 .strut = atom("_NET_WM_STRUT"),
                 .strut_partial = atom("_NET_WM_STRUT_PARTIAL"),
                 .window_type = atom("_NET_WM_WINDOW_TYPE"),
                 .normal = atom("_NET_WM_WINDOW_TYPE_NORMAL"),
                 .dock = atom("_NET_WM_WINDOW_TYPE_DOCK"),
                 .state = atom("_NET_WM_STATE"),
                 .hidden = atom("_NET_WM_STATE_HIDDEN"),
                 .fullscreen = atom("_NET_WM_STATE_FULLSCREEN"),
                 .skip_taskbar = atom("_NET_WM_STATE_SKIP_TASKBAR"),
                 .skip_pager = atom("_NET_WM_STATE_SKIP_PAGER"),
                 .maximized_horz = atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
                 .maximized_vert = atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                 .frame_extents = atom("_NET_FRAME_EXTENTS"),
                 .wm_pid = atom("_NET_WM_PID"),
                 .wm_name = atom("_NET_WM_NAME"),
                 .utf8_string = atom("UTF8_STRING"),
                 .wm_protocols = atom("WM_PROTOCOLS"),
                 .wm_take_focus = atom("WM_TAKE_FOCUS"),
                 .allowed_actions = atom("_NET_WM_ALLOWED_ACTIONS"),
                 .action_move = atom("_NET_WM_ACTION_MOVE"),
                 .action_resize = atom("_NET_WM_ACTION_RESIZE"),
                 .action_maximize_horz = atom("_NET_WM_ACTION_MAXIMIZE_HORZ"),
                 .action_maximize_vert = atom("_NET_WM_ACTION_MAXIMIZE_VERT")};
    }

    bool has_xwayland_outputs() const {
        const auto cookie = xcb_randr_get_screen_resources_current(connection.get(), root);
        const auto resources =
            take_reply(xcb_randr_get_screen_resources_current_reply(connection.get(), cookie, nullptr));
        if (!resources)
            return false;
        const auto *outputs = xcb_randr_get_screen_resources_current_outputs(resources.get());
        for (int index = 0; index < xcb_randr_get_screen_resources_current_outputs_length(resources.get()); ++index) {
            const auto output_cookie =
                xcb_randr_get_output_info(connection.get(), outputs[index], resources->config_timestamp);
            const auto output = take_reply(xcb_randr_get_output_info_reply(connection.get(), output_cookie, nullptr));
            if (!output)
                continue;
            const QByteArrayView name(reinterpret_cast<const char *>(xcb_randr_get_output_info_name(output.get())),
                                      xcb_randr_get_output_info_name_length(output.get()));
            constexpr QByteArrayView prefix("XWAYLAND");
            if (name.startsWith(prefix) && name.size() > prefix.size() &&
                std::ranges::all_of(name.sliced(prefix.size()), [](char c) { return c >= '0' && c <= '9'; }))
                return true;
        }
        return false;
    }

    bool connect() {
        if (connection && !xcb_connection_has_error(connection.get()))
            return root != XCB_NONE && !xwayland && has_local_pid;

        root = XCB_NONE;
        xwayland = has_local_pid = has_randr_monitors = false;
        const auto display = qgetenv("DISPLAY");
        // Limit ownership verification to local Unix-domain connections.
        if (!display.startsWith(':') && !display.startsWith("unix:") && !display.startsWith("unix/:"))
            return false;
        connection.reset(xcb_connect(display.constData(), &screen_number));
        if (!connection || xcb_connection_has_error(connection.get()) || !local_x11_socket(connection.get())) {
            connection.reset();
            return false;
        }
        auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection.get()));
        for (int index = 0; index < screen_number && screen.rem; ++index)
            xcb_screen_next(&screen);
        if (!screen.rem)
            return false;
        root = screen.data->root;

        constexpr QByteArrayView xwayland_extension("XWAYLAND");
        const auto extension = take_reply(xcb_query_extension_reply(
            connection.get(),
            xcb_query_extension(connection.get(), xwayland_extension.size(), xwayland_extension.data()), nullptr));
        const auto scale = property(root, atom("_XWAYLAND_GLOBAL_OUTPUT_SCALE", true), XCB_GET_PROPERTY_TYPE_ANY);
        if ((extension && extension->present) || (scale && scale->type != XCB_NONE)) {
            xwayland = true;
            return false;
        }

        const auto *randr = xcb_get_extension_data(connection.get(), &xcb_randr_id);
        if (randr && randr->present) {
            const auto version = take_reply(xcb_randr_query_version_reply(
                connection.get(), xcb_randr_query_version(connection.get(), 1, 5), nullptr));
            has_randr_monitors =
                version && (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 5));
            if (version &&
                (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 3)) &&
                has_xwayland_outputs()) {
                xwayland = true;
                return false;
            }
        }
        const auto *res = xcb_get_extension_data(connection.get(), &xcb_res_id);
        if (res && res->present) {
            const auto version = take_reply(
                xcb_res_query_version_reply(connection.get(), xcb_res_query_version(connection.get(), 1, 2), nullptr));
            has_local_pid =
                version && (version->server_major > 1 || (version->server_major == 1 && version->server_minor >= 2));
        }
        if (!has_local_pid)
            return false;
        load_atoms();
        return true;
    }

    x11_reply<xcb_get_property_reply_t> property(xcb_window_t id, xcb_atom_t name, xcb_atom_t type) const {
        if (name == XCB_NONE)
            return take_reply<xcb_get_property_reply_t>(nullptr);
        const auto cookie = xcb_get_property(connection.get(), false, id, name, type, 0, 16384);
        return take_reply(xcb_get_property_reply(connection.get(), cookie, nullptr));
    }

    QVector<uint32_t> property32(xcb_window_t id, xcb_atom_t name, xcb_atom_t type) const {
        const auto reply = property(id, name, type);
        if (!reply || reply->type != type || reply->format != 32 || reply->bytes_after || !reply->value_len)
            return {};
        const auto *begin = static_cast<const uint32_t *>(xcb_get_property_value(reply.get()));
        return {begin, begin + reply->value_len};
    }

    QByteArray property8(xcb_window_t id, xcb_atom_t name, xcb_atom_t type) const {
        const auto reply = property(id, name, type);
        if (!reply || reply->type != type || reply->format != 8 || reply->bytes_after)
            return {};
        return {static_cast<const char *>(xcb_get_property_value(reply.get())), reply->value_len};
    }

    std::optional<x11_desktop> desktop_state() {
        if (!connect())
            return std::nullopt;
        const auto check = property32(root, atoms.supporting_wm_check, XCB_ATOM_WINDOW);
        if (check.size() != 1 || !check.front() ||
            property32(check.front(), atoms.supporting_wm_check, XCB_ATOM_WINDOW) != check)
            return std::nullopt;
        auto supported = property32(root, atoms.supported, XCB_ATOM_ATOM);
        if (!supported.contains(atoms.client_list) && !supported.contains(atoms.client_list_stacking))
            return std::nullopt;
        QVector<xcb_window_t> windows;
        if (supported.contains(atoms.client_list_stacking))
            windows = property32(root, atoms.client_list_stacking, XCB_ATOM_WINDOW);
        if (windows.isEmpty() && supported.contains(atoms.client_list))
            windows = property32(root, atoms.client_list, XCB_ATOM_WINDOW);
        return x11_desktop{std::move(supported), std::move(windows)};
    }

    bool supports_placement(const x11_desktop &state) const {
        return state.supported.contains(atoms.moveresize_window) && state.supported.contains(atoms.state) &&
               state.supported.contains(atoms.maximized_horz) && state.supported.contains(atoms.maximized_vert);
    }

    bool can_move(xcb_window_t id, bool maximized) const {
        const auto allowed = property32(id, atoms.allowed_actions, XCB_ATOM_ATOM);
        return allowed.isEmpty() || (allowed.contains(atoms.action_move) && allowed.contains(atoms.action_resize) &&
                                     (!maximized || (allowed.contains(atoms.action_maximize_horz) &&
                                                     allowed.contains(atoms.action_maximize_vert))));
    }

    quint32 local_pid(xcb_window_t id) const {
        const xcb_res_client_id_spec_t spec{id, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
        const auto cookie = xcb_res_query_client_ids(connection.get(), 1, &spec);
        const auto reply = take_reply(xcb_res_query_client_ids_reply(connection.get(), cookie, nullptr));
        if (!reply)
            return 0;
        for (auto pid = xcb_res_query_client_ids_ids_iterator(reply.get()); pid.rem; xcb_res_client_id_value_next(&pid))
            if (pid.data->spec.mask == spec.mask && xcb_res_client_id_value_value_length(pid.data) == 1)
                return *xcb_res_client_id_value_value(pid.data);
        return 0;
    }

    std::optional<x11_window> read_window(xcb_window_t id) const {
        const auto attr = take_reply(xcb_get_window_attributes_reply(
            connection.get(), xcb_get_window_attributes(connection.get(), id), nullptr));
        if (!attr || attr->override_redirect || attr->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT ||
            attr->map_state != XCB_MAP_STATE_VIEWABLE)
            return std::nullopt;
        const auto types = property32(id, atoms.window_type, XCB_ATOM_ATOM);
        if ((!types.isEmpty() && types.front() != atoms.normal) ||
            !property32(id, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW).isEmpty())
            return std::nullopt;
        const auto state = property32(id, atoms.state, XCB_ATOM_ATOM);
        if (state.contains(atoms.hidden) || state.contains(atoms.fullscreen) || state.contains(atoms.skip_taskbar) ||
            state.contains(atoms.skip_pager))
            return std::nullopt;
        const auto hints = property32(id, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS);
        constexpr uint32_t input_hint = 1;
        if (hints.size() >= 2 && (hints[0] & input_hint) && !hints[1] &&
            !property32(id, atoms.wm_protocols, XCB_ATOM_ATOM).contains(atoms.wm_take_focus))
            return std::nullopt;

        x11_window result;
        result.window.handle = id;
        const auto advertised_pid = property32(id, atoms.wm_pid, XCB_ATOM_CARDINAL);
        const auto pid = local_pid(id);
        // The client property corroborates the server PID, which might refer to a different PID namespace.
        if (advertised_pid.size() != 1 || advertised_pid.front() != pid || !read_process(pid, result.window))
            return std::nullopt;
        const auto title = property8(id, atoms.wm_name, atoms.utf8_string);
        result.window.title = title.isEmpty() ? QString::fromLatin1(property8(id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING))
                                              : QString::fromUtf8(title);
        result.window.title = result.window.title.left(512).trimmed();
        result.window.title.removeIf([](QChar character) { return character.category() == QChar::Other_Control; });

        const auto geometry_cookie = xcb_get_geometry(connection.get(), id);
        const auto translate_cookie = xcb_translate_coordinates(connection.get(), id, root, 0, 0);
        const auto geometry = take_reply(xcb_get_geometry_reply(connection.get(), geometry_cookie, nullptr));
        const auto position = take_reply(xcb_translate_coordinates_reply(connection.get(), translate_cookie, nullptr));
        if (!geometry || !geometry->width || !geometry->height || !position || !position->same_screen)
            return std::nullopt;
        const auto extents = property32(id, atoms.frame_extents, XCB_ATOM_CARDINAL);
        if (extents.size() == 4 && std::ranges::all_of(extents, [](uint32_t extent) {
                return extent <= std::numeric_limits<uint16_t>::max();
            })) {
            result.decorations = {int(extents[0]), int(extents[2]), int(extents[1]), int(extents[3])};
        } else {
            const int border = geometry->border_width;
            result.decorations = {border, border, border, border};
        }
        result.window.frame =
            QRect(position->dst_x, position->dst_y, geometry->width, geometry->height).marginsAdded(result.decorations);
        result.maximized_horz = state.contains(atoms.maximized_horz);
        result.maximized_vert = state.contains(atoms.maximized_vert);
        result.window.maximized = result.maximized_horz && result.maximized_vert;
        return result;
    }

    std::optional<x11_window> verified_window(const workspace_window &window, const x11_desktop &state) const {
        if (!window.handle || window.handle > std::numeric_limits<xcb_window_t>::max())
            return std::nullopt;
        const auto id = static_cast<xcb_window_t>(window.handle);
        if (!state.windows.contains(id))
            return std::nullopt;
        auto current = read_window(id);
        return current && same_workspace_window(window, current->window) ? std::move(current) : std::nullopt;
    }

    bool send_message(xcb_window_t window, xcb_atom_t type, const std::array<uint32_t, 5> &data) const {
        xcb_client_message_event_t event{};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = window;
        event.type = type;
        std::ranges::copy(data, event.data.data32);
        constexpr auto mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        const auto cookie =
            xcb_send_event_checked(connection.get(), false, root, mask, reinterpret_cast<const char *>(&event));
        const auto error = take_reply(xcb_request_check(connection.get(), cookie));
        return !error && !xcb_connection_has_error(connection.get());
    }

    QString monitor_name(xcb_atom_t id) const {
        const auto reply =
            take_reply(xcb_get_atom_name_reply(connection.get(), xcb_get_atom_name(connection.get(), id), nullptr));
        if (!reply)
            return {};
        auto name = QString::fromUtf8(xcb_get_atom_name_name(reply.get()), xcb_get_atom_name_name_length(reply.get()));
        name = name.left(252);
        name.removeIf([](QChar character) { return character.category() == QChar::Other_Control; });
        return name;
    }

    QVector<workspace_monitor> monitors(const QVector<xcb_window_t> &windows) const {
        const auto geometry =
            take_reply(xcb_get_geometry_reply(connection.get(), xcb_get_geometry(connection.get(), root), nullptr));
        if (!geometry || !geometry->width || !geometry->height)
            return {};
        const QRect screen(0, 0, geometry->width, geometry->height);
        QVector<workspace_monitor> result;
        if (has_randr_monitors) {
            const auto reply = take_reply(xcb_randr_get_monitors_reply(
                connection.get(), xcb_randr_get_monitors(connection.get(), root, true), nullptr));
            if (reply) {
                for (auto it = xcb_randr_get_monitors_monitors_iterator(reply.get()); it.rem;
                     xcb_randr_monitor_info_next(&it)) {
                    const auto &monitor = *it.data;
                    const auto name = monitor_name(monitor.name);
                    const auto area = QRect(monitor.x, monitor.y, monitor.width, monitor.height).intersected(screen);
                    if (!name.isEmpty() && area.isValid())
                        result.append({QStringLiteral("x11:") + name, area, monitor.primary != 0});
                }
            }
        }
        if (result.isEmpty())
            result.append({QStringLiteral("x11:screen:%1").arg(screen_number), screen, true});
        apply_work_areas(result, screen, windows);
        if (!result.isEmpty() && std::ranges::none_of(result, &workspace_monitor::primary))
            result.front().primary = true;
        return result;
    }

    void apply_work_areas(QVector<workspace_monitor> &monitors, const QRect &screen,
                          const QVector<xcb_window_t> &windows) const {
        const auto desktop = property32(root, atoms.current_desktop, XCB_ATOM_CARDINAL).value(0);
        QVector<QVector<uint32_t>> struts;
        for (const auto id : windows) {
            if (!property32(id, atoms.window_type, XCB_ATOM_ATOM).contains(atoms.dock))
                continue;
            const auto dock_desktop = property32(id, atoms.desktop, XCB_ATOM_CARDINAL).value(0, desktop);
            if (dock_desktop != desktop && dock_desktop != std::numeric_limits<uint32_t>::max())
                continue;
            auto strut = property32(id, atoms.strut_partial, XCB_ATOM_CARDINAL);
            if (strut.size() != 12) {
                strut = property32(id, atoms.strut, XCB_ATOM_CARDINAL);
                if (strut.size() != 4)
                    continue;
                strut.append({0, uint32_t(screen.bottom()), 0, uint32_t(screen.bottom()), 0, uint32_t(screen.right()),
                              0, uint32_t(screen.right())});
            }
            if (strut[0] <= uint32_t(screen.width()) && strut[1] <= uint32_t(screen.width()) &&
                strut[2] <= uint32_t(screen.height()) && strut[3] <= uint32_t(screen.height())) {
                struts.append(std::move(strut));
            }
        }
        if (struts.isEmpty()) {
            const auto areas = property32(root, atoms.work_area, XCB_ATOM_CARDINAL);
            if (desktop < uint32_t(areas.size() / 4)) {
                const auto offset = desktop * 4;
                if (areas[offset] < uint32_t(screen.width()) && areas[offset + 1] < uint32_t(screen.height()) &&
                    areas[offset + 2] <= uint32_t(screen.width()) && areas[offset + 3] <= uint32_t(screen.height())) {
                    const QRect work_area{int(areas[offset]), int(areas[offset + 1]), int(areas[offset + 2]),
                                          int(areas[offset + 3])};
                    for (auto &monitor : monitors) {
                        const auto clipped = monitor.work_area.intersected(work_area);
                        if (clipped.isValid())
                            monitor.work_area = clipped;
                    }
                }
            }
            return;
        }

        // _NET_WORKAREA is a single rectangle per virtual desktop. Partial struts reserve space
        // only on the monitors they intersect, including panels on an edge inside the X11 screen.
        for (auto &monitor : monitors) {
            const auto geometry = monitor.work_area;
            auto area = geometry;
            for (const auto &strut : struts) {
                const auto overlaps = [&](int first, int last, int start_index) {
                    const auto start = strut[start_index];
                    const auto end = strut[start_index + 1];
                    return start <= end && start <= uint32_t(last) && end >= uint32_t(first);
                };
                if (overlaps(geometry.top(), geometry.bottom(), 4))
                    area.setLeft(qMax(area.left(), int(strut[0])));
                if (overlaps(geometry.top(), geometry.bottom(), 6))
                    area.setRight(qMin(area.right(), screen.right() - int(strut[1])));
                if (overlaps(geometry.left(), geometry.right(), 8))
                    area.setTop(qMax(area.top(), int(strut[2])));
                if (overlaps(geometry.left(), geometry.right(), 10))
                    area.setBottom(qMin(area.bottom(), screen.bottom() - int(strut[3])));
            }
            monitor.work_area = area;
        }
        monitors.removeIf([](const auto &monitor) { return !monitor.work_area.isValid(); });
    }
};

#endif
}

std::unique_ptr<WorkspaceWindowBackend> make_workspace_window_backend(const QString &qt_platform) {
    if (qt_platform.startsWith(QStringLiteral("wayland")) ||
        qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0 ||
        !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || !qEnvironmentVariableIsEmpty("WAYLAND_SOCKET")) {
        return std::make_unique<LaunchOnlyWorkspaceBackend>(wayland_limitation());
    }
    if (qt_platform != QStringLiteral("xcb")) {
        return std::make_unique<LaunchOnlyWorkspaceBackend>(QCoreApplication::translate(
            "Workspaces",
            "Window capture and placement are unavailable on this display. Workspaces can launch resources only."));
    }
#ifdef FORADECK_HAVE_WORKSPACE_X11
    return std::make_unique<X11WorkspaceBackend>();
#else
    return std::make_unique<LaunchOnlyWorkspaceBackend>(QCoreApplication::translate(
        "Workspaces", "This build lacks X11 window management support. Workspaces can launch resources only."));
#endif
}
