#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <dwmapi.h>
#include <memory>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDirIterator>
#include <QEvent>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QUrl>
#include <QWidget>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdexcept>
#include <vector>

#include "ApplicationPaths.h"
#include "Heartbeat.h"
#include "localization.h"
#include "PlatformIntegration.h"
#include "WindowManager.h"
#include "Windows/RegistryScan.h"

namespace {

struct window_search {
    QString path;
    HWND window = nullptr;
};

BOOL CALLBACK find_window(const HWND window, const LPARAM parameter) {
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || GetWindowTextLengthW(window) == 0) {
        return TRUE;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    auto &search = *reinterpret_cast<window_search *>(parameter);
    if (xlibrary::windows_process_path(process_id).compare(search.path, Qt::CaseInsensitive) == 0) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

void apply_native_frame(QWidget *const window, const bool dark, const bool glass) {
    if (!window->isWindow() || (window->windowFlags() & Qt::FramelessWindowHint) || !window->internalWinId() ||
        QGuiApplication::platformName() != "windows")
        return;
    const auto handle = reinterpret_cast<HWND>(window->internalWinId());
    // Windows 11 attributes are numeric for compatibility with older MinGW headers.
    constexpr DWORD immersive_dark_mode_attribute = 20;
    constexpr DWORD corner_preference_attribute = 33;
    constexpr DWORD border_color_attribute = 34;
    constexpr DWORD caption_color_attribute = 35;
    constexpr DWORD text_color_attribute = 36;
    const BOOL dark_value = dark;
    DwmSetWindowAttribute(handle, immersive_dark_mode_attribute, &dark_value, sizeof(dark_value));
    const auto palette = window->palette();
    const auto native_color = [](const QColor &color) { return RGB(color.red(), color.green(), color.blue()); };
    const auto surface_role = qobject_cast<QDialog *>(window) ? QPalette::Base : QPalette::Window;
    const COLORREF caption_color = native_color(palette.color(surface_role));
    const COLORREF text_color = native_color(palette.color(QPalette::WindowText));
    // Older Windows versions retain their native frame when an attribute is unsupported.
    DwmSetWindowAttribute(handle, caption_color_attribute, &caption_color, sizeof(caption_color));
    DwmSetWindowAttribute(handle, text_color_attribute, &text_color, sizeof(text_color));
    DwmSetWindowAttribute(handle, border_color_attribute, &caption_color, sizeof(caption_color));
    const int round_corners = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(handle, corner_preference_attribute, &round_corners, sizeof(round_corners));
    const int backdrop = glass ? 2 : 1;
    DwmSetWindowAttribute(handle, 38, &backdrop, sizeof(backdrop));
}

class WindowAppearanceFilter final : public QObject {
    bool dark = false;
    bool glass = false;

public:
    using QObject::QObject;

    void refresh(const bool dark_value, const bool glass_value) {
        dark = dark_value;
        glass = glass_value;
        for (auto *window : QApplication::topLevelWidgets())
            if (window->isVisible())
                apply(window);
    }

    void apply(QWidget *const window) const { apply_native_frame(window, dark, glass); }

protected:
    bool eventFilter(QObject *const object, QEvent *const event) override {
        if (event->type() == QEvent::Show || event->type() == QEvent::WinIdChange ||
            event->type() == QEvent::PaletteChange) {
            if (auto *window = qobject_cast<QWidget *>(object); window && window->isWindow())
                apply(window);
        }
        return QObject::eventFilter(object, event);
    }
};

}

QString desktop_platform::default_global_shortcut() {
    return "Ctrl+Alt+Shift+Space";
}

QString desktop_platform::default_overlay_shortcut() {
    return "Ctrl+Alt+Space";
}

void desktop_platform::activate_overlay(QWidget *const window) {
    window->raise();
    window->activateWindow();
}

bool WindowManager::activate_window(const app_record &record) {
    window_search search{QDir::toNativeSeparators(record.path), nullptr};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    if (!search.window) {
        return false;
    }
    if (IsIconic(search.window)) {
        ShowWindowAsync(search.window, SW_RESTORE);
    }
    if (!SetForegroundWindow(search.window)) {
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary",
                                        "Windows could not switch windows. Select the application on the taskbar.")
                .toStdString());
    }
    return true;
}

void desktop_platform::open_folder(const QString &path) {
    const xlibrary::ComScope com;
    const auto directory = QDir::toNativeSeparators(QFileInfo(path).absolutePath()).toStdWString();
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"explore", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Could not open the application folder.").toStdString());
}

bool desktop_platform::open_url(const QUrl &url) {
    return QDesktopServices::openUrl(url);
}

bool desktop_platform::installed_for_updates() {
    if (application_paths::portable_mode())
        return false;
    const QSettings uninstall("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                              "{2B7F5CF1-ABF4-4992-899D-B0A17F8DA51E}_is1",
                              QSettings::Registry64Format);
    const auto installed_path = QFileInfo(uninstall.value("InstallLocation").toString()).canonicalFilePath();
    const auto current_path = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
    return !installed_path.isEmpty() && !current_path.isEmpty() &&
           installed_path.compare(current_path, Qt::CaseInsensitive) == 0;
}

bool desktop_platform::start_update_installer(const QString &path, QString &error) {
    if (!installed_for_updates()) {
        error = QCoreApplication::translate("XLibrary", "This copy of Fora.Deck was not installed by the installer.");
        return false;
    }
    const auto native_path = QDir::toNativeSeparators(path).toStdWString();
    const auto parameters =
        ("/SP- /NORESTART /DIR=\"" + QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + '"')
            .toStdWString();
    SHELLEXECUTEINFOW execution{};
    execution.cbSize = sizeof(execution);
    execution.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execution.lpVerb = L"open";
    execution.lpFile = native_path.c_str();
    execution.lpParameters = parameters.c_str();
    execution.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execution)) {
        error = QCoreApplication::translate("XLibrary", "Could not start the installer (Windows error %1).")
                    .arg(GetLastError());
        return false;
    }
    if (execution.hProcess)
        CloseHandle(execution.hProcess);
    return true;
}

desktop_platform::file_association desktop_platform::default_file_application(const QString &path) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto cleanup_com = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return {};
    const auto suffix = QFileInfo(path).suffix();
    if (suffix.isEmpty())
        return {};
    const auto association = ('.' + suffix).toStdWString();
    const auto query = [&](const ASSOCSTR kind) {
        DWORD length = 0;
        AssocQueryStringW(ASSOCF_NONE, kind, association.c_str(), L"open", nullptr, &length);
        if (!length || length > 32768)
            return QString{};
        std::vector<wchar_t> buffer(length);
        return SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, kind, association.c_str(), L"open", buffer.data(), &length))
                   ? QString::fromWCharArray(buffer.data())
                   : QString{};
    };
    file_association result;
    result.name = query(ASSOCSTR_FRIENDLYAPPNAME);
    const auto executable = query(ASSOCSTR_EXECUTABLE);
    if (result.name.isEmpty() && !executable.isEmpty())
        result.name = QFileInfo(executable).completeBaseName();
    if (!executable.isEmpty()) {
        HICON icon = nullptr;
        const auto cleanup_icon = qScopeGuard([&icon] {
            if (icon)
                DestroyIcon(icon);
        });
        ExtractIconExW(executable.toStdWString().c_str(), 0, &icon, nullptr, 1);
        if (icon)
            result.icon = QImage::fromHICON(icon);
    }
    return result;
}

QImage desktop_platform::application_icon(const QString &path) {
    HICON large = nullptr;
    const auto cleanup_large = qScopeGuard([&large] {
        if (large)
            DestroyIcon(large);
    });
    const auto native_path = QDir::toNativeSeparators(path).toStdWString();
    ExtractIconExW(native_path.c_str(), 0, &large, nullptr, 1);
    if (large) {
        return QImage::fromHICON(large);
    }
    // Shell providers and file associations are resolved on LibraryModel's icon worker.
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto cleanup_com = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return {};
    if (QMimeDatabase{}.mimeTypeForFile(path, QMimeDatabase::MatchExtension).name().startsWith("image/")) {
        IShellItemImageFactory *raw_factory = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(native_path.c_str(), nullptr, IID_PPV_ARGS(&raw_factory)))) {
            const auto release_factory = [](IShellItemImageFactory *factory) { factory->Release(); };
            const std::unique_ptr<IShellItemImageFactory, decltype(release_factory)> factory(raw_factory,
                                                                                             release_factory);
            HBITMAP bitmap = nullptr;
            const auto cleanup_bitmap = qScopeGuard([&bitmap] {
                if (bitmap)
                    DeleteObject(bitmap);
            });
            if (SUCCEEDED(factory->GetImage({128, 128}, SIIGBF_THUMBNAILONLY, &bitmap)) && bitmap) {
                const auto image = QImage::fromHBITMAP(bitmap);
                if (!image.isNull())
                    return image;
            }
        }
    }
    DWORD length = 0;
    const auto association = ('.' + QFileInfo(path).suffix()).toStdWString();
    AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, association.c_str(), L"open", nullptr, &length);
    if (length > 0 && length <= 32768) {
        std::vector<wchar_t> executable(length);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, association.c_str(), L"open",
                                        executable.data(), &length))) {
            ExtractIconExW(executable.data(), 0, &large, nullptr, 1);
            if (large) {
                return QImage::fromHICON(large);
            }
        }
    }
    // Packaged applications may not expose an executable; their Shell file icon still follows the association.
    SHFILEINFOW info{};
    const auto cleanup_icon = qScopeGuard([&info] {
        if (info.hIcon)
            DestroyIcon(info.hIcon);
    });
    if (SHGetFileInfoW(native_path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon)
        return QImage::fromHICON(info.hIcon);
    return {};
}

bool desktop_platform::set_autostart(const bool enabled, QString &error) {
    QSettings startup("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) {
        startup.setValue("XLibrary",
                         '"' + QDir::toNativeSeparators(QApplication::applicationFilePath()) + "\" --background");
    } else {
        startup.remove("XLibrary");
    }
    startup.sync();
    if (startup.status() != QSettings::NoError) {
        error = QCoreApplication::translate("XLibrary", "Could not change autostart in the Windows registry.");
        return false;
    }
    return true;
}

bool desktop_platform::translucent_windows_supported() {
    // DWM compositing is always enabled on the supported Windows 10+ versions.
    return true;
}

bool desktop_platform::animations_enabled() {
    if (qApp->property("xlibrary_disable_animations").toBool())
        return false;
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled;
}

void desktop_platform::apply_backdrop(QWidget *const window, const bool dark, const bool glass) {
    static auto *const filter = [] {
        auto *result = new WindowAppearanceFilter(qApp);
        qApp->installEventFilter(result);
        return result;
    }();
    filter->refresh(dark, glass);
    window->winId();
    filter->apply(window);
}

GlobalHotkey::GlobalHotkey(QObject *const parent) : QObject(parent) {
    qApp->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey() {
    qApp->removeNativeEventFilter(this);
    if (current_id) {
        UnregisterHotKey(nullptr, current_id);
    }
}

bool GlobalHotkey::set_sequence(const QKeySequence &value, QString &error) {
    if (sequence == value) {
        return true;
    }
    if (value.isEmpty()) {
        if (current_id) {
            UnregisterHotKey(nullptr, current_id);
        }
        current_id = 0;
        sequence = value;
        return true;
    }
    if (value.count() != 1) {
        error = QCoreApplication::translate("XLibrary", "Use a single key combination.");
        return false;
    }
    const auto combination = value[0];
    const auto key = combination.key();
    UINT virtual_key = 0;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        virtual_key = static_cast<UINT>(key);
    } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        virtual_key = static_cast<UINT>(key);
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        virtual_key = VK_F1 + key - Qt::Key_F1;
    } else if (key == Qt::Key_Space) {
        virtual_key = VK_SPACE;
    } else {
        error = QCoreApplication::translate("XLibrary", "Use a letter, digit, F1–F24, or Space.");
        return false;
    }
    const auto modifiers = combination.keyboardModifiers();
    if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        error = QCoreApplication::translate("XLibrary", "Add Ctrl, Alt, or Win to the global shortcut.");
        return false;
    }
    if (modifiers & ~(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier)) {
        error = QCoreApplication::translate("XLibrary", "The shortcut contains an unsupported modifier.");
        return false;
    }
    UINT flags = MOD_NOREPEAT;
    if (modifiers & Qt::ControlModifier)
        flags |= MOD_CONTROL;
    if (modifiers & Qt::AltModifier)
        flags |= MOD_ALT;
    if (modifiers & Qt::ShiftModifier)
        flags |= MOD_SHIFT;
    if (modifiers & Qt::MetaModifier)
        flags |= MOD_WIN;
    if (next_id > 0xbfff) {
        error = QCoreApplication::translate("XLibrary", "Restart Fora.Deck before changing the shortcut again.");
        return false;
    }
    const auto new_id = next_id++;
    if (!RegisterHotKey(nullptr, new_id, flags, virtual_key)) {
        error = QCoreApplication::translate("XLibrary", "The shortcut is in use by Windows or another application.");
        return false;
    }
    if (current_id) {
        UnregisterHotKey(nullptr, current_id);
    }
    current_id = new_id;
    sequence = value;
    return true;
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &, void *const message, qintptr *const result) {
    const auto *event = static_cast<MSG *>(message);
    if (event->message == WM_HOTKEY && event->wParam == static_cast<WPARAM>(current_id) && current_id) {
        // Qt passes a null result for messages handled by the thread's event loop.
        if (result)
            *result = 0;
        emit activated();
        return true;
    }
    return false;
}

qint64 desktop_platform::launch_application(const app_record &record, const launch_mode mode) {
    if (mode == launch_mode::activate && WindowManager::activate_window(record))
        return 0;
    return Application::launch_application(record, mode);
}
