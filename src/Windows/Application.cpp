#include <windows.h>
#include <shellapi.h>
#include <winver.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "Application.h"

std::uint32_t Application::get_pid() const noexcept {
    if (!this->process_handle || WaitForSingleObject(this->process_handle.get(), 0) != WAIT_TIMEOUT) {
        return 0;
    }
    return this->pid;
}

std::string Application::read_application_name(const std::filesystem::path &executable_path) {
    const auto filename_utf8 = executable_path.stem().u8string();
    const std::string fallback_name(filename_utf8.begin(), filename_utf8.end());

    auto extension = executable_path.extension().wstring();
    std::ranges::transform(extension, extension.begin(),
                           [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    if (extension.empty()) {
        throw std::invalid_argument("Invalid app path");
    }
    if (extension != L".exe" || !std::filesystem::is_regular_file(executable_path)) {
        return fallback_name;
    }

    DWORD ignored_handle{};
    const DWORD version_size = GetFileVersionInfoSizeW(executable_path.c_str(), &ignored_handle);
    if (version_size == 0) {
        return fallback_name;
    }

    std::vector<std::byte> version_info(version_size);
    if (!GetFileVersionInfoW(executable_path.c_str(), 0, version_size, version_info.data())) {
        return fallback_name;
    }

    struct language_and_code_page {
        WORD language;
        WORD code_page;
    };

    void *translation_data{};
    UINT translation_size{};
    if (!VerQueryValueW(version_info.data(), L"\\VarFileInfo\\Translation", &translation_data, &translation_size) ||
        translation_data == nullptr || translation_size == 0 ||
        translation_size % sizeof(language_and_code_page) != 0) {
        return fallback_name;
    }

    const auto translation_count = translation_size / sizeof(language_and_code_page);
    const wchar_t *field_names[] = {L"FileDescription", L"ProductName"};

    // Look for a description in every language before trying the product name.
    for (const auto *field_name : field_names) {
        for (std::size_t index = 0; index < translation_count; ++index) {
            language_and_code_page translation{};
            std::memcpy(&translation,
                        static_cast<const unsigned char *>(translation_data) + index * sizeof(translation),
                        sizeof(translation));

            wchar_t query[64]{};
            const int query_length = std::swprintf(query, std::size(query), L"\\StringFileInfo\\%04x%04x\\%ls",
                                                   static_cast<unsigned int>(translation.language),
                                                   static_cast<unsigned int>(translation.code_page), field_name);
            if (query_length <= 0) {
                continue;
            }

            void *name_data{};
            UINT name_length{};
            if (!VerQueryValueW(version_info.data(), query, &name_data, &name_length) || name_data == nullptr ||
                name_length == 0) {
                continue;
            }

            // The string belongs to version_info; copy it before releasing the buffer.
            std::wstring_view name(static_cast<const wchar_t *>(name_data), name_length);
            name = name.substr(0, name.find(L'\0'));
            if (name.empty() || !std::in_range<int>(name.size())) {
                continue;
            }

            const int character_count = static_cast<int>(name.size());
            const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), character_count,
                                                      nullptr, 0, nullptr, nullptr);
            if (utf8_size == 0) {
                continue;
            }

            std::string application_name(static_cast<std::size_t>(utf8_size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), character_count,
                                    application_name.data(), utf8_size, nullptr, nullptr) == utf8_size) {
                return application_name;
            }
        }
    }

    return fallback_name;
}

std::uint32_t Application::launch(const std::wstring &arguments, const std::filesystem::path &working_directory) {
    const std::filesystem::path executable_path = this->get_file_path();
    if (executable_path.empty()) {
        throw std::invalid_argument("Application executable path is empty");
    }

    SHELLEXECUTEINFOW launch_info{};
    launch_info.cbSize = sizeof(launch_info);
    launch_info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch_info.lpVerb = L"open";
    launch_info.lpFile = executable_path.c_str();
    launch_info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    launch_info.lpDirectory = working_directory.c_str();
    launch_info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&launch_info)) {
        const auto error_code = GetLastError();
        throw std::runtime_error("Application launch failed: " + std::to_string(error_code));
    }

    std::unique_ptr<void, decltype(&CloseHandle)> process_handle(launch_info.hProcess, &CloseHandle);
    if (!process_handle) {
        // Shell may hand off the request to an existing application without creating a process.
        return 0;
    }

    const auto process_id = GetProcessId(process_handle.get());
    if (process_id == 0) {
        return 0;
    }

    this->process_handle = std::move(process_handle);
    this->pid = process_id;
    return process_id;
}

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <shlobj.h>
#include <shobjidl.h>

#include "PathHelpers.h"
#include "Windows/RegistryScan.h"
namespace {
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

}

QString Application::new_instance_error(const app_record &record) {
    return file_target(record) ? QString{}
                               : QCoreApplication::translate("XLibrary", "This target supports activation only.");
}

QString Application::configured_arguments(const app_record &record) {
    return record.arguments;
}

void Application::launch_application(const app_record &record, const launch_mode mode) {
    validate_launch_target(record);
    const xlibrary::ComScope com;
    const auto kind = target_kind(record);
    if (file_target(record)) {
        const auto suffix = QFileInfo(record.path).suffix().toLower();
        if (kind != "executable" || (suffix != "exe" && suffix != "bat" && suffix != "cmd"))
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Unsupported Windows executable target").toStdString());
    }
    if (!file_target(record)) {
        if (mode == launch_mode::new_instance || mode == launch_mode::elevated)
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "This target supports activation only").toStdString());
        if (!target_available(record))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Launch target is unavailable").toStdString());
        if (kind == "aumid") {
            IApplicationActivationManager *raw_manager = nullptr;
            const auto created = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
                                                  IID_PPV_ARGS(&raw_manager));
            if (FAILED(created))
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Cannot create application activation manager")
                        .toStdString());
            const auto release = [](IApplicationActivationManager *value) { value->Release(); };
            const std::unique_ptr<IApplicationActivationManager, decltype(release)> manager(raw_manager, release);
            DWORD process_id = 0;
            const auto activated =
                manager->ActivateApplication(record.path.toStdWString().c_str(), nullptr, AO_NONE, &process_id);
            if (FAILED(activated))
                throw std::runtime_error(
                    QCoreApplication::translate("XLibrary", "Application activation failed (Windows error %1).")
                        .arg(activated)
                        .toStdString());
            return;
        }
        const auto target = (kind == "steam" ? "steam://run/" + record.path : record.path).toStdWString();
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = target.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Target activation failed (Windows error %1).")
                    .arg(GetLastError())
                    .toStdString());
        return;
    }
    if (mode == launch_mode::elevated) {
        if (kind != "executable" || QFileInfo(record.path).suffix().compare("exe", Qt::CaseInsensitive) != 0 ||
            !QFileInfo(record.path).isAbsolute() || !QFileInfo(record.path).isFile())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Elevation requires an available Windows executable")
                    .toStdString());
        const auto path = QDir::toNativeSeparators(record.path).toStdWString();
        const auto arguments = record.arguments.toStdWString();
        const auto directory =
            QDir::toNativeSeparators(effective_working_directory(record.path, record.working_directory)).toStdWString();
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        info.lpDirectory = directory.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Elevated launch failed (Windows error %1).")
                    .arg(GetLastError())
                    .toStdString());
        return;
    }
    Application application(record.name.toStdString(), std::filesystem::path(record.path.toStdWString()));
    const auto directory = effective_working_directory(record.path, record.working_directory);
    application.launch(record.arguments.toStdWString(), std::filesystem::path(directory.toStdWString()));
}

app_record Application::resolve_application(const QString &path) {
    const auto supported_target = [](const QString &value) {
        const auto suffix = QFileInfo(value).suffix().toLower();
        if (suffix != "exe" && suffix != "bat" && suffix != "cmd" && suffix != "lnk")
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "Supported files: .exe, .lnk, .bat, .cmd.").toStdString());
        if (!QFileInfo(value).isFile())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The application file is unavailable.").toStdString());
    };
    supported_target(path);
    const xlibrary::ComScope com;
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
        const auto release_link = [](IShellLinkW *const value) { value->Release(); };
        const std::unique_ptr<IShellLinkW, decltype(release_link)> link(raw_link, release_link);
        IPersistFile *raw_file = nullptr;
        if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&raw_file)))) {
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not open the shortcut.").toStdString());
        }
        const auto release_file = [](IPersistFile *const value) { value->Release(); };
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
        if (!result.working_directory.isEmpty())
            result.working_directory = effective_working_directory(result.path, result.working_directory);
    }
    supported_target(result.path);
    const Application application(std::filesystem::path(result.path.toStdWString()));
    result.name = QFileInfo(path).suffix().compare("lnk", Qt::CaseInsensitive) == 0
                      ? QFileInfo(path).completeBaseName()
                      : QString::fromUtf8(application.get_file_title());
    result.path = QDir::fromNativeSeparators(result.path);
    result.available = QFileInfo(result.path).isFile();
    return result;
}

QVector<app_record> Application::discover(const bool registry, const bool start_menu, QVector<discovery_issue> &issues,
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
    const auto report = [&](const qsizetype processed, const qsizetype total, const QString &stage) {
        if (progress)
            progress({total > 0 ? static_cast<int>(90.0 * processed / total) : 90, processed, total, stage});
    };
    if (registry && !cancelled.load()) {
        std::vector<xlibrary::scan_issue> registry_issues;
        const auto applications = xlibrary::scan_registry_applications(
            &registry_issues, [&](const std::size_t processed, const std::size_t total) {
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
    QSet<QString> shortcut_applications;
    for (const auto &record : std::as_const(result)) {
        if (!record.discovery_source.isEmpty())
            shortcut_applications.insert(normalized_path(record.path) + QChar::Null +
                                         record.name.normalized(QString::NormalizationForm_C).toCaseFolded());
    }
    QHash<QString, qsizetype> configurations;
    QVector<app_record> unique;
    unique.reserve(result.size());
    for (auto &record : result) {
        const auto name_key = normalized_path(record.path) + QChar::Null +
                              record.name.normalized(QString::NormalizationForm_C).toCaseFolded();
        const auto key = launch_configuration_key(record.path, record.arguments, record.working_directory);
        const auto existing = configurations.constFind(key);
        // App Paths has no launch parameters; the named shortcut is authoritative.
        const bool duplicate = (record.discovery_source.isEmpty() && shortcut_applications.contains(name_key)) ||
                               existing != configurations.cend();
        if (duplicate) {
            issues.append(
                {record.discovery_source.isEmpty() ? record.path : record.discovery_source,
                 QCoreApplication::translate("XLibrary", "This launch configuration is already in the library."),
                 discovery_issue_kind::duplicate});
            if (existing != configurations.cend() && !record.discovery_source.isEmpty())
                unique[*existing].discovery_sources.append(record.discovery_source);
        } else {
            configurations.insert(key, unique.size());
            unique.append(std::move(record));
        }
    }
    if (registry)
        unique += discover_system(issues, cancelled, progress);
    if (registry || start_menu)
        unique += discover_steam(issues, cancelled, progress);
    QVector<app_record> combined;
    QSet<QString> combined_keys;
    for (auto &record : unique) {
        if (cancelled.load())
            return {};
        const auto key = launch_key(record);
        if (!combined_keys.contains(key)) {
            combined_keys.insert(key);
            combined.append(std::move(record));
        }
    }
    report(total, total, QCoreApplication::translate("XLibrary", "Application checks complete"));
    return combined;
}

xlibrary::ComScope::ComScope() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
}
xlibrary::ComScope::~ComScope() {
    if (SUCCEEDED(result))
        CoUninitialize();
}

bool Application::windows_aumid_available(const QString &aumid) {
    const xlibrary::ComScope com;
    IShellItem *item = nullptr;
    const auto result =
        SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, aumid.toStdWString().c_str(), IID_PPV_ARGS(&item));
    if (item)
        item->Release();
    return SUCCEEDED(result);
}

QStringList Application::windows_steam_roots() {
    QStringList roots;
    for (const auto &key :
         {QString("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QString("HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam"),
          QString("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam")}) {
        const QSettings registry(key, QSettings::NativeFormat);
        for (const auto &value : {"SteamPath", "InstallPath"}) {
            const auto path = registry.value(value).toString();
            if (!path.isEmpty())
                roots.append(QDir::fromNativeSeparators(path));
        }
    }
    for (const auto &variable : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const auto directory = qEnvironmentVariable(variable);
        if (!directory.isEmpty())
            roots.append(directory + "/Steam");
    }
    roots.removeDuplicates();
    return roots;
}

QVector<app_record> Application::discover_system(QVector<discovery_issue> &issues, const std::atomic_bool &cancelled,
                                                 const std::function<void(const discovery_progress &)> &progress) {
    QVector<app_record> result;
    const auto add = [&](const QString &name, const QString &kind, const QString &value) {
        if (cancelled.load())
            return;
        app_record record;
        record.name = QCoreApplication::translate("XLibrary", name.toUtf8().constData());
        record.target_kind = kind;
        record.path = value;
        record.discovery_source =
            "windows-system:" + (kind == "executable" ? QFileInfo(value).fileName().toLower() : value);
        if (target_available(record))
            result.append(std::move(record));
        else
            issues.append({record.discovery_source,
                           QCoreApplication::translate("XLibrary", "The target is not installed or is unavailable."),
                           discovery_issue_kind::skipped});
    };
    if (progress)
        progress({-1, 0, 0, QCoreApplication::translate("XLibrary", "Checking Windows system applications")});
    const auto windows = qEnvironmentVariable("WINDIR");
    if (!windows.isEmpty()) {
        add(QT_TRANSLATE_NOOP("XLibrary", "File Explorer"), "executable", windows + "/explorer.exe");
        add(QT_TRANSLATE_NOOP("XLibrary", "Task Manager"), "executable", windows + "/System32/Taskmgr.exe");
        add(QT_TRANSLATE_NOOP("XLibrary", "Control Panel"), "executable", windows + "/System32/control.exe");
    }
    add(QT_TRANSLATE_NOOP("XLibrary", "Settings"), "system_uri", "ms-settings:");
    add(QT_TRANSLATE_NOOP("XLibrary", "Bluetooth"), "system_uri", "ms-settings:bluetooth");
    add(QT_TRANSLATE_NOOP("XLibrary", "Display"), "system_uri", "ms-settings:display");
    add(QT_TRANSLATE_NOOP("XLibrary", "Sound"), "system_uri", "ms-settings:sound");
    add(QT_TRANSLATE_NOOP("XLibrary", "Network"), "system_uri", "ms-settings:network");
    add(QT_TRANSLATE_NOOP("XLibrary", "Installed apps"), "system_uri", "ms-settings:appsfeatures");
    add(QT_TRANSLATE_NOOP("XLibrary", "Windows Update"), "system_uri", "ms-settings:windowsupdate");
    add(QT_TRANSLATE_NOOP("XLibrary", "Calculator"), "aumid", "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
    add(QT_TRANSLATE_NOOP("XLibrary", "Microsoft Store"), "aumid", "Microsoft.WindowsStore_8wekyb3d8bbwe!App");
    add(QT_TRANSLATE_NOOP("XLibrary", "Windows Terminal"), "aumid", "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App");
    if (progress)
        progress({90, result.size(), result.size(),
                  QCoreApplication::translate("XLibrary", "System application checks complete")});
    return cancelled.load() ? QVector<app_record>{} : result;
}
