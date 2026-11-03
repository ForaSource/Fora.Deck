#include <windows.h>
#include <tlhelp32.h>

#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "Heartbeat.h"

namespace {
using unique_handle = std::unique_ptr<void, decltype(&CloseHandle)>;

std::pair<bool, DWORD> check_process_handle(const HANDLE process_handle, const std::wstring &expected_path) {
    const auto wait_result = WaitForSingleObject(process_handle, 0);
    if (wait_result == WAIT_OBJECT_0) {
        return {false, 0};
    }
    if (wait_result == WAIT_FAILED) {
        return {false, GetLastError()};
    }

    wchar_t process_path[32768]{};
    DWORD path_length = static_cast<DWORD>(std::size(process_path));
    if (!QueryFullProcessImageNameW(process_handle, 0, process_path, &path_length)) {
        const auto error_code = GetLastError();
        if (WaitForSingleObject(process_handle, 0) == WAIT_OBJECT_0) {
            return {false, 0};
        }
        return {false, error_code};
    }

    const int comparison = CompareStringOrdinal(process_path, -1, expected_path.c_str(), -1, TRUE);
    if (comparison == 0) {
        return {false, GetLastError()};
    }
    // The process may have exited while its path was being read.
    const auto final_wait = WaitForSingleObject(process_handle, 0);
    if (final_wait == WAIT_FAILED) {
        return {false, GetLastError()};
    }
    return {comparison == CSTR_EQUAL && final_wait == WAIT_TIMEOUT, 0};
}
}

std::move_only_function<std::pair<bool, std::uint32_t>()>
Heartbeat::make_process_checker(const std::uint32_t process_id, const std::filesystem::path &executable_path) {
    const auto expected_name = executable_path.filename().wstring();
    if (process_id == 0 || expected_name.empty()) {
        throw std::invalid_argument("Heartbeat requires a nonzero PID and an executable name");
    }
    const auto expected_path = std::filesystem::absolute(executable_path).lexically_normal().wstring();

    unique_handle process_handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id),
                                 &CloseHandle);
    if (!process_handle) {
        const auto error_code = GetLastError();
        if (error_code != ERROR_INVALID_PARAMETER) {
            throw std::system_error(error_code, std::system_category(), "Cannot open heartbeat process");
        }
    }

    auto check_process = [process_handle = std::move(process_handle), expected_name, expected_path]() mutable {
        if (process_handle) {
            const auto result = check_process_handle(process_handle.get(), expected_path);
            if (result.first || result.second != 0) {
                return result;
            }
            process_handle.reset();
        }

        // Firefox and other applications may hand off the launch to an existing instance.
        const auto snapshot_handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot_handle == INVALID_HANDLE_VALUE) {
            return std::pair<bool, DWORD>{false, GetLastError()};
        }
        const unique_handle snapshot(snapshot_handle, &CloseHandle);
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.get(), &entry)) {
            const auto error_code = GetLastError();
            return std::pair<bool, DWORD>{false, error_code == ERROR_NO_MORE_FILES ? 0 : error_code};
        }

        DWORD check_error = 0;
        do {
            const int comparison = CompareStringOrdinal(entry.szExeFile, -1, expected_name.c_str(), -1, TRUE);
            if (comparison == 0) {
                return std::pair<bool, DWORD>{false, GetLastError()};
            }
            if (comparison != CSTR_EQUAL) {
                continue;
            }

            unique_handle candidate(
                OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID), &CloseHandle);
            if (!candidate) {
                const auto error_code = GetLastError();
                if (error_code != ERROR_INVALID_PARAMETER) {
                    check_error = error_code;
                }
                continue;
            }

            const auto [alive, error_code] = check_process_handle(candidate.get(), expected_path);
            if (alive) {
                // Keep the verified handle so PID reuse cannot substitute a different process.
                process_handle = std::move(candidate);
                return std::pair<bool, DWORD>{true, 0};
            }
            if (error_code != 0) {
                check_error = error_code;
            }
        } while (Process32NextW(snapshot.get(), &entry));

        const auto enumeration_error = GetLastError();
        if (enumeration_error != ERROR_NO_MORE_FILES) {
            return std::pair<bool, DWORD>{false, enumeration_error};
        }
        return std::pair<bool, DWORD>{false, check_error};
    };

    return check_process;
}

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
QString xlibrary::windows_process_path(const quint32 process_id) {
    const unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id), &CloseHandle);
    if (!process) {
        return {};
    }
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    return QueryFullProcessImageNameW(process.get(), 0, path, &length) ? QString::fromWCharArray(path, length)
                                                                       : QString{};
}

QSet<QString> Heartbeat::running_paths(const QVector<app_record> &applications) {
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
                const auto path = xlibrary::windows_process_path(entry.th32ProcessID);
                if (!path.isEmpty()) {
                    paths.insert(QDir::fromNativeSeparators(path).toCaseFolded());
                }
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }
    return paths;
}
