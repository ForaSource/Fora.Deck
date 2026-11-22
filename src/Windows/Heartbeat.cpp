#include <windows.h>
#include <tlhelp32.h>

#include <iterator>
#include <psapi.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "Application.h"
#include "Heartbeat.h"
#include "PathHelpers.h"

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

Heartbeat::process_snapshot Heartbeat::sample_processes(const QVector<app_record> &applications, const bool memory,
                                                        const bool cpu) {
    QSet<QString> names;
    QSet<QString> expected_paths;
    process_snapshot paths;
    for (const auto &application : applications) {
        const auto path = Application::process_path(application);
        if (path.isEmpty())
            continue;
        names.insert(QFileInfo(path).fileName().toCaseFolded());
        expected_paths.insert(normalized_path(path));
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
                const unique_handle process(
                    OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID),
                    &CloseHandle);
                if (!process)
                    continue;
                wchar_t path_buffer[32768]{};
                DWORD length = static_cast<DWORD>(std::size(path_buffer));
                if (!QueryFullProcessImageNameW(process.get(), 0, path_buffer, &length) ||
                    WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
                    continue;
                const auto path = normalized_path(QString::fromWCharArray(path_buffer, length));
                if (!expected_paths.contains(path))
                    continue;
                process_sample sample;
                sample.pid = entry.th32ProcessID;
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
                    const auto ticks = [](const FILETIME &time) {
                        return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                    };
                    sample.start_time = ticks(created);
                    if (cpu)
                        sample.cpu_seconds = (double(ticks(kernel)) + double(ticks(user))) / 10000000.0;
                }
                if (memory) {
                    PROCESS_MEMORY_COUNTERS counters{};
                    counters.cb = sizeof(counters);
                    if (K32GetProcessMemoryInfo(process.get(), &counters, sizeof(counters)))
                        sample.memory_bytes = static_cast<qint64>(counters.WorkingSetSize);
                }
                paths[path].append(sample);
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }
    return paths;
}

void Heartbeat::terminate_application(const app_record &application) {
    const auto processes = sample_processes({application}, false, false);
    QStringList errors;
    for (auto it = processes.cbegin(); it != processes.cend(); ++it) {
        for (const auto &sample : it.value()) {
            const auto fail = [&](const DWORD error) {
                errors.append(
                    QCoreApplication::translate("XLibrary", "PID %1: %2")
                        .arg(sample.pid)
                        .arg(QString::fromStdString(std::system_category().message(static_cast<int>(error)))));
            };
            if (sample.pid == GetCurrentProcessId()) {
                errors.append(QCoreApplication::translate("XLibrary", "Fora.Deck cannot terminate itself here."));
                continue;
            }
            const unique_handle process(
                OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, sample.pid),
                &CloseHandle);
            if (!process) {
                const auto error = GetLastError();
                if (error != ERROR_INVALID_PARAMETER)
                    fail(error);
                continue;
            }
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
                continue;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
                fail(GetLastError());
                continue;
            }
            const auto start_time = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (!sample.start_time || start_time != sample.start_time)
                continue;
            const auto [alive, error] =
                check_process_handle(process.get(), QDir::toNativeSeparators(it.key()).toStdWString());
            if (error)
                fail(error);
            else if (alive && !TerminateProcess(process.get(), 1))
                fail(GetLastError());
        }
    }
    if (!errors.isEmpty())
        throw std::runtime_error(errors.join('\n').toStdString());
}
