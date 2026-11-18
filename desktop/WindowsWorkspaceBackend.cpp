#include <windows.h>

#include <array>
#include <bit>
#include <dwmapi.h>
#include <limits>
#include <QCoreApplication>
#include <QFileInfo>
#include <string_view>

#include "WorkspaceWindowBackend.h"

namespace {

using process_handle = std::unique_ptr<void, decltype(&CloseHandle)>;

class NativeDpiScope {
    using set_dpi_context = decltype(&SetThreadDpiAwarenessContext);

    // Early Windows 10 builds lack this API. Keep the thread's existing context on those builds.
    static set_dpi_context setter() {
        static const auto function = std::bit_cast<set_dpi_context>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
        return function;
    }

    DPI_AWARENESS_CONTEXT previous_ = nullptr;

public:
    NativeDpiScope() {
        if (const auto set_context = setter()) {
            previous_ = set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            if (!previous_)
                previous_ = set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        }
    }

    NativeDpiScope(const NativeDpiScope &) = delete;
    NativeDpiScope &operator=(const NativeDpiScope &) = delete;

    ~NativeDpiScope() {
        if (previous_)
            setter()(previous_);
    }
};

struct window_owner {
    DWORD thread = 0;
    DWORD pid = 0;
    bool operator==(const window_owner &) const = default;
};

window_owner owner_of(HWND window) {
    window_owner owner;
    owner.thread = GetWindowThreadProcessId(window, &owner.pid);
    return owner;
}

bool eligible_window(HWND window, window_owner owner) {
    if (!owner.thread || !owner.pid || owner.pid == GetCurrentProcessId() || window == GetShellWindow() ||
        window == GetDesktopWindow() || !IsWindowVisible(window) || !IsWindowEnabled(window) || IsIconic(window) ||
        GetAncestor(window, GA_ROOT) != window || GetWindow(window, GW_OWNER)) {
        return false;
    }

    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (extended_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)))
        return false;

    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked)
        return false;

    std::array<wchar_t, 256> class_name{};
    const int length = GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size()));
    if (!length)
        return false;
    const std::wstring_view name(class_name.data(), static_cast<std::size_t>(length));
    // Explorer's actual application windows remain eligible. Hosted views lack a reliable executable match.
    return name != L"Progman" && name != L"WorkerW" && name != L"Shell_TrayWnd" && name != L"Shell_SecondaryTrayWnd" &&
           name != L"TaskSwitcherWnd" && name != L"MultitaskingViewFrame" && name != L"XamlExplorerHostIslandWindow" &&
           name != L"ApplicationFrameWindow" && name != L"Windows.UI.Core.CoreWindow";
}

workspace_window window_identity(HWND window, DWORD pid, HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    BOOL critical = FALSE;
    if (!process || !IsProcessCritical(process, &critical) || critical ||
        !GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        return {};
    }

    std::array<wchar_t, 32768> path;
    auto length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length))
        return {};
    const auto executable = QFileInfo(QString::fromWCharArray(path.data(), length)).canonicalFilePath();
    if (executable.isEmpty())
        return {};

    workspace_window result;
    result.handle = reinterpret_cast<quintptr>(window);
    result.pid = pid;
    result.process_birth = (static_cast<quint64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    result.executable = executable;
    return result;
}

QRect to_frame(const RECT &rect) {
    const auto width = static_cast<qint64>(rect.right) - rect.left;
    const auto height = static_cast<qint64>(rect.bottom) - rect.top;
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max()) {
        return {};
    }
    return {rect.left, rect.top, static_cast<int>(width), static_cast<int>(height)};
}

BOOL CALLBACK collect_windows(HWND window, LPARAM context) {
    const auto owner = owner_of(window);
    if (!eligible_window(window, owner))
        return TRUE;

    std::array<wchar_t, 1024> title{};
    const auto length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (!length)
        return TRUE;

    const process_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner.pid), CloseHandle);
    auto result = window_identity(window, owner.pid, process.get());
    if (!result.process_birth)
        return TRUE;

    RECT rect{};
    if (!GetWindowRect(window, &rect) || owner_of(window) != owner || IsIconic(window))
        return TRUE;
    result.frame = to_frame(rect);
    if (!result.frame.isValid())
        return TRUE;

    result.title = QString::fromWCharArray(title.data(), length);
    result.maximized = IsZoomed(window);
    auto &windows = *reinterpret_cast<QVector<workspace_window> *>(context);
    windows.append(std::move(result));
    return TRUE;
}

BOOL CALLBACK collect_monitors(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        const auto work_area = to_frame(info.rcWork);
        auto &monitors = *reinterpret_cast<QVector<workspace_monitor> *>(context);
        if (work_area.isValid()) {
            monitors.append(
                {QString::fromWCharArray(info.szDevice), work_area, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        }
    }
    return TRUE;
}

process_handle verified_process(const workspace_window &expected, window_owner owner) {
    const auto window = reinterpret_cast<HWND>(expected.handle);
    process_handle process(nullptr, CloseHandle);
    if (owner.pid != expected.pid || !eligible_window(window, owner) || !GetWindowTextLengthW(window))
        return process;

    process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner.pid));
    if (!same_workspace_window(expected, window_identity(window, owner.pid, process.get())))
        process.reset();
    return process;
}

class WindowsWorkspaceBackend final : public WorkspaceWindowBackend {
    bool native_desktop_;

public:
    explicit WindowsWorkspaceBackend(bool native_desktop) : native_desktop_(native_desktop) {}

    workspace_desktop snapshot() override {
        workspace_desktop desktop;
        if (!native_desktop_) {
            desktop.limitation =
                QCoreApplication::translate("Workspaces", "Window capture and placement require the Windows desktop.");
            return desktop;
        }

        const NativeDpiScope native_dpi;
        desktop.can_capture = EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&desktop.windows));
        desktop.can_place =
            EnumDisplayMonitors(nullptr, nullptr, collect_monitors, reinterpret_cast<LPARAM>(&desktop.monitors)) &&
            !desktop.monitors.isEmpty();
        if (!desktop.can_capture || !desktop.can_place)
            desktop.limitation =
                QCoreApplication::translate("Workspaces", "Windows could not read the desktop layout.");
        return desktop;
    }

    bool activate(const workspace_window &expected) override {
        if (!native_desktop_)
            return false;
        const auto window = reinterpret_cast<HWND>(expected.handle);
        const auto owner = owner_of(window);
        const auto process = verified_process(expected, owner);
        return process && owner_of(window) == owner && eligible_window(window, owner) &&
               (GetForegroundWindow() == window || SetForegroundWindow(window));
    }

    bool place(const workspace_window &expected, const QRect &frame, bool maximized) override {
        if (!native_desktop_ || !frame.isValid() || frame.right() == std::numeric_limits<LONG>::max() ||
            frame.bottom() == std::numeric_limits<LONG>::max()) {
            return false;
        }

        const NativeDpiScope native_dpi;
        RECT rect{frame.left(), frame.top(), frame.right() + 1L, frame.bottom() + 1L};
        const auto monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info) || !to_frame(info.rcWork).contains(frame))
            return false;

        const auto window = reinterpret_cast<HWND>(expected.handle);
        const auto owner = owner_of(window);
        const auto process = verified_process(expected, owner);
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!process || !GetWindowPlacement(window, &placement) || owner_of(window) != owner ||
            !eligible_window(window, owner)) {
            return false;
        }

        const auto current_monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        if (maximized && placement.showCmd == SW_SHOWMAXIMIZED && current_monitor == monitor)
            return true;

        // WINDOWPLACEMENT uses workspace coordinates for the unowned, non-tool windows we manage.
        OffsetRect(&rect, info.rcMonitor.left - info.rcWork.left, info.rcMonitor.top - info.rcWork.top);
        placement.rcNormalPosition = rect;
        if (current_monitor != monitor)
            placement.ptMaxPosition = {-1, -1};
        placement.flags = WPF_ASYNCWINDOWPLACEMENT;
        placement.showCmd = placement.showCmd == SW_SHOWMAXIMIZED ? SW_RESTORE : SW_SHOWNOACTIVATE;
        if (!SetWindowPlacement(window, &placement))
            return false;

        // Queue the move before maximizing so Windows chooses the target monitor for the maximized window.
        if (maximized) {
            placement.showCmd = SW_SHOWMAXIMIZED;
            if (owner_of(window) != owner || !eligible_window(window, owner) || !SetWindowPlacement(window, &placement))
                return false;
        }

        return true;
    }
};

}

std::unique_ptr<WorkspaceWindowBackend> make_workspace_window_backend(const QString &qt_platform) {
    return std::make_unique<WindowsWorkspaceBackend>(qt_platform == QStringLiteral("windows"));
}
