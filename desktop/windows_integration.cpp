#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <dwmapi.h>
#include <memory>
#include <QApplication>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>
#include <QWidget>
#include <shlobj.h>
#include <stdexcept>

#include "application_paths.h"
#include "localization.h"
#include "platform_integration.h"
#include "Windows/RegistryScan.h"

namespace {
using unique_handle = std::unique_ptr<void, decltype(&CloseHandle)>;

bool is_system_application(const QString &path) {
    const auto normalized_path = QDir::cleanPath(QDir::fromNativeSeparators(path));
    wchar_t windows_directory[32768]{};
    const auto length = GetWindowsDirectoryW(windows_directory, static_cast<UINT>(std::size(windows_directory)));
    if (length > 0 && length < std::size(windows_directory)) {
        const auto prefix = QDir::fromNativeSeparators(QString::fromWCharArray(windows_directory, length)) + '/';
        if (normalized_path.startsWith(prefix, Qt::CaseInsensitive))
            return true;
    }

    // WindowsApps also contains regular Microsoft applications; the publisher alone does not imply an OS component.
    for (const auto *folder :
         {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramFilesX64, &FOLDERID_LocalAppData}) {
        PWSTR raw_path = nullptr;
        if (FAILED(SHGetKnownFolderPath(*folder, 0, nullptr, &raw_path)))
            continue;
        const auto root = QDir::fromNativeSeparators(QString::fromWCharArray(raw_path));
        CoTaskMemFree(raw_path);
        const bool local = IsEqualGUID(*folder, FOLDERID_LocalAppData);
        if (!local) {
            // Windows components may also reside outside the OS directory.
            for (const auto *directory : {"Internet Explorer/", "Windows Mail/", "Windows Media Player/",
                                          "Windows Photo Viewer/", "Windows Defender/", "Windows NT/", "WSL/"}) {
                if (normalized_path.startsWith(root + '/' + directory, Qt::CaseInsensitive))
                    return true;
            }
        }
        const auto prefix = root + (local ? "/Microsoft/WindowsApps/" : "/WindowsApps/");
        if (!normalized_path.startsWith(prefix, Qt::CaseInsensitive))
            continue;
        const auto relative_path = normalized_path.mid(prefix.size());
        if (relative_path.startsWith("Microsoft.Windows", Qt::CaseInsensitive) ||
            relative_path.startsWith("MicrosoftWindows.", Qt::CaseInsensitive) ||
            relative_path.startsWith("Microsoft.DesktopAppInstaller_", Qt::CaseInsensitive) ||
            relative_path.startsWith("MicrosoftCorporationII.WindowsSubsystemForLinux_", Qt::CaseInsensitive) ||
            (local && relative_path.compare("winget.exe", Qt::CaseInsensitive) == 0))
            return true;
    }
    for (const auto *folder :
         {&FOLDERID_ProgramFilesCommon, &FOLDERID_ProgramFilesCommonX86, &FOLDERID_ProgramFilesCommonX64}) {
        PWSTR raw_path = nullptr;
        if (FAILED(SHGetKnownFolderPath(*folder, 0, nullptr, &raw_path)))
            continue;
        const auto prefix = QDir::fromNativeSeparators(QString::fromWCharArray(raw_path)) + "/microsoft shared/ink/";
        CoTaskMemFree(raw_path);
        if (normalized_path.startsWith(prefix, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

bool should_import_application(const app_record &record) {
    if (is_system_application(record.path))
        return false;
    // Shortcut names may be localized, and uninstall commands may run the main EXE with an argument.
    static const QRegularExpression uninstaller_name("^(?:uninstall|uninst|unins[0-9]*)\\.exe$",
                                                     QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression uninstall_argument(
        "(?:^|\\s)(?:--start-uninstaller|[-/]uninstall(?:remove)?|[-/]removeonly)(?=\\s|$)",
        QRegularExpression::CaseInsensitiveOption);
    return !uninstaller_name.match(QFileInfo(record.path).fileName()).hasMatch() &&
           !uninstall_argument.match(record.arguments).hasMatch();
}

QString process_path(DWORD process_id) {
    const unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id), &CloseHandle);
    if (!process) {
        return {};
    }
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    return QueryFullProcessImageNameW(process.get(), 0, path, &length) ? QString::fromWCharArray(path, length)
                                                                       : QString{};
}

struct window_search {
    QString path;
    HWND window = nullptr;
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || GetWindowTextLengthW(window) == 0) {
        return TRUE;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    auto &search = *reinterpret_cast<window_search *>(parameter);
    if (process_path(process_id).compare(search.path, Qt::CaseInsensitive) == 0) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

class ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

public:
    ~ComScope() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};
}

QString desktop_platform::default_global_shortcut() {
    return "Ctrl+Alt+Shift+Space";
}

QString desktop_platform::default_overlay_shortcut() {
    return "Ctrl+Alt+Space";
}

QSet<QString> desktop_platform::running_paths(const QVector<app_record> &applications) {
    QSet<QString> names;
    QSet<QString> paths;
    for (const auto &application : applications) {
        names.insert(QFileInfo(application.path).fileName().toCaseFolded());
    }
    if (names.isEmpty()) {
        return paths;
    }
    const auto raw_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw_snapshot == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Could not retrieve the Windows process list.").toStdString());
    }
    const unique_handle snapshot(raw_snapshot, &CloseHandle);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (names.contains(QString::fromWCharArray(entry.szExeFile).toCaseFolded())) {
                const auto path = process_path(entry.th32ProcessID);
                if (!path.isEmpty()) {
                    paths.insert(QDir::fromNativeSeparators(path).toCaseFolded());
                }
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }
    return paths;
}

bool desktop_platform::activate_window(const app_record &record) {
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

QString desktop_platform::new_instance_error(const app_record &) {
    return {};
}

void desktop_platform::launch_application(const app_record &record, launch_mode mode) {
    if (mode == launch_mode::activate && activate_window(record))
        return;
    const ComScope com;
    Application application(record.name.toStdString(), std::filesystem::path(record.path.toStdWString()));
    const auto directory =
        record.working_directory.isEmpty() ? QFileInfo(record.path).absolutePath() : record.working_directory;
    application.launch(record.arguments.toStdWString(), std::filesystem::path(directory.toStdWString()));
}

void desktop_platform::open_folder(const QString &path) {
    const ComScope com;
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
        error = QCoreApplication::translate("XLibrary", "This copy of XLibrary was not installed by the installer.");
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

QImage desktop_platform::application_icon(const QString &path) {
    HICON large = nullptr;
    const auto native_path = QDir::toNativeSeparators(path).toStdWString();
    ExtractIconExW(native_path.c_str(), 0, &large, nullptr, 1);
    if (!large) {
        return {};
    }
    const auto result = QImage::fromHICON(large);
    DestroyIcon(large);
    return result;
}

app_record desktop_platform::resolve_application(const QString &path) {
    const ComScope com;
    app_record result;
    result.discovery_source = path;
    result.path = QFileInfo(path).absoluteFilePath();
    if (QFileInfo(path).suffix().compare("lnk", Qt::CaseInsensitive) == 0) {
        IShellLinkW *raw_link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                    reinterpret_cast<void **>(&raw_link)))) {
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not read the shortcut.").toStdString());
        }
        const auto release_link = [](IShellLinkW *value) { value->Release(); };
        const std::unique_ptr<IShellLinkW, decltype(release_link)> link(raw_link, release_link);
        IPersistFile *raw_file = nullptr;
        if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&raw_file)))) {
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not open the shortcut.").toStdString());
        }
        const auto release_file = [](IPersistFile *value) { value->Release(); };
        const std::unique_ptr<IPersistFile, decltype(release_file)> file(raw_file, release_file);
        if (FAILED(file->Load(path.toStdWString().c_str(), STGM_READ))) {
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not load the shortcut.").toStdString());
        }
        wchar_t buffer[32768]{};
        if (FAILED(link->GetPath(buffer, static_cast<int>(std::size(buffer)), nullptr, 0)) || buffer[0] == L'\0') {
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "The shortcut has no path to a regular application.")
                    .toStdString());
        }
        result.path = QString::fromWCharArray(buffer);
        buffer[0] = L'\0';
        if (FAILED(link->GetArguments(buffer, static_cast<int>(std::size(buffer)))))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not read the shortcut arguments.").toStdString());
        result.arguments = QString::fromWCharArray(buffer);
        buffer[0] = L'\0';
        if (FAILED(link->GetWorkingDirectory(buffer, static_cast<int>(std::size(buffer)))))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not read the shortcut working directory.")
                    .toStdString());
        result.working_directory = QString::fromWCharArray(buffer);
        // Skip unusable shortcuts during discovery, before the shared import transaction.
        if (!result.working_directory.isEmpty() && !QFileInfo(result.working_directory).isAbsolute()) {
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The working directory must be an absolute path.")
                    .toStdString());
        }
    }
    const Application application(std::filesystem::path(result.path.toStdWString()));
    result.name = QFileInfo(path).suffix().compare("lnk", Qt::CaseInsensitive) == 0
                      ? QFileInfo(path).completeBaseName()
                      : QString::fromUtf8(application.get_file_title());
    result.path = QDir::fromNativeSeparators(result.path);
    result.available = QFileInfo(result.path).isFile();
    return result;
}

QVector<app_record> desktop_platform::discover(bool registry, bool start_menu, QVector<discovery_issue> &issues,
                                               const std::atomic_bool &cancelled,
                                               const std::function<void(const discovery_progress &)> &progress) {
    QVector<app_record> result;
    QStringList shortcuts;
    // Build the queue first so its size stays fixed while reading shortcuts and EXE metadata.
    if (progress)
        progress({-1, 0, 0, QCoreApplication::translate("XLibrary", "Preparing the application list…")});
    if (start_menu) {
        for (const auto *folder : {&FOLDERID_Programs, &FOLDERID_CommonPrograms}) {
            if (cancelled.load())
                return {};
            PWSTR raw_path = nullptr;
            if (FAILED(SHGetKnownFolderPath(*folder, 0, nullptr, &raw_path))) {
                issues.append(discovery_issue{
                    "Start Menu", QCoreApplication::translate("XLibrary", "The Start menu folder is unavailable.")});
                continue;
            }
            const QString directory = QString::fromWCharArray(raw_path);
            CoTaskMemFree(raw_path);
            QDirIterator files(directory, {"*.lnk"}, QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                if (cancelled.load())
                    return {};
                shortcuts.append(files.next());
            }
        }
    }
    qsizetype registry_count = 0;
    const auto report = [&](qsizetype processed, qsizetype total, const QString &stage) {
        if (progress)
            progress({total > 0 ? static_cast<int>(90.0 * processed / total) : 90, processed, total, stage});
    };
    if (registry && !cancelled.load()) {
        std::vector<xlibrary::scan_issue> registry_issues;
        const auto applications =
            xlibrary::scan_registry_applications(&registry_issues, [&](std::size_t processed, std::size_t total) {
                registry_count = static_cast<qsizetype>(total);
                report(static_cast<qsizetype>(processed), registry_count + shortcuts.size(),
                       QCoreApplication::translate("XLibrary", "Checking the Windows registry"));
                return !cancelled.load();
            });
        for (const auto &application : applications) {
            if (cancelled.load())
                return {};
            app_record record;
            record.name = QString::fromUtf8(application.get_file_title());
            record.path = QDir::fromNativeSeparators(QString::fromStdWString(application.get_file_path().wstring()));
            if (should_import_application(record))
                result.append(std::move(record));
            else
                issues.append(discovery_issue{
                    record.path, QCoreApplication::translate("XLibrary", "System component or uninstaller."),
                    discovery_issue_kind::skipped});
        }
        for (const auto &issue : registry_issues) {
            const auto root = issue.source.scope == xlibrary::registry_scope::current_user ? "HKCU" : "HKLM";
            issues.append(discovery_issue{
                QString("%1\\%2 [%3]")
                    .arg(root, QString::fromStdWString(issue.source.key))
                    .arg(issue.source.view == KEY_WOW64_32KEY ? 32 : 64),
                QString("%1: Win32 %2").arg(QString::fromStdWString(issue.operation)).arg(issue.error)});
        }
    }
    const auto total = registry_count + shortcuts.size();
    for (qsizetype index = 0; index < shortcuts.size(); ++index) {
        if (cancelled.load())
            return {};
        report(registry_count + index, total, QCoreApplication::translate("XLibrary", "Checking Start menu shortcuts"));
        const auto &path = shortcuts[index];
        try {
            const auto record = resolve_application(path);
            if (record.available && QFileInfo(record.path).suffix().compare("exe", Qt::CaseInsensitive) == 0 &&
                should_import_application(record))
                result.append(record);
            else
                issues.append(discovery_issue{
                    path,
                    !record.available ? QCoreApplication::translate("XLibrary", "The target file is missing.")
                    : QFileInfo(record.path).suffix().compare("exe", Qt::CaseInsensitive) != 0
                        ? QCoreApplication::translate("XLibrary", "The shortcut target is not an EXE.")
                        : QCoreApplication::translate("XLibrary", "System component or uninstaller."),
                    discovery_issue_kind::skipped});
        } catch (const std::exception &error) {
            issues.append(discovery_issue{path, QString::fromUtf8(error.what())});
        }
    }
    if (cancelled.load())
        return {};
    report(total, total, QCoreApplication::translate("XLibrary", "Application checks complete"));
    return result;
}

bool desktop_platform::set_autostart(bool enabled, QString &error) {
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

bool desktop_platform::animations_enabled() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled;
}

void desktop_platform::apply_backdrop(QWidget *window, bool dark, bool glass) {
    const auto handle = reinterpret_cast<HWND>(window->winId());
    const BOOL dark_value = dark;
    DwmSetWindowAttribute(handle, 20, &dark_value, sizeof(dark_value));
    const int backdrop = glass ? 2 : 1;
    DwmSetWindowAttribute(handle, 38, &backdrop, sizeof(backdrop));
}

GlobalHotkey::GlobalHotkey(QObject *parent) : QObject(parent) {
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
        error = QCoreApplication::translate("XLibrary", "Restart XLibrary before changing the shortcut again.");
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

bool GlobalHotkey::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
    const auto *event = static_cast<MSG *>(message);
    if (event->message == WM_HOTKEY && event->wParam == static_cast<WPARAM>(current_id) && current_id) {
        *result = 0;
        emit activated();
        return true;
    }
    return false;
}
