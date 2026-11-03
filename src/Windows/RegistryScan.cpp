#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "RegistryScan.h"

namespace xlibrary {

namespace detail {

struct discovered_application {
    std::filesystem::path executable;
};

template <class T> struct scan_result {
    std::vector<T> entries;
    std::vector<scan_issue> issues;
};

constexpr wchar_t app_paths_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths";
constexpr wchar_t uninstall_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

struct RegistryKey {
    HKEY handle = nullptr;
    RegistryKey() = default;
    RegistryKey(const RegistryKey &) = delete;
    RegistryKey &operator=(const RegistryKey &) = delete;
    ~RegistryKey() {
        if (handle)
            RegCloseKey(handle);
    }
};

bool absent(const LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

std::wstring read_string(const HKEY key, const wchar_t *const name, const registry_source &source,
                         std::vector<scan_issue> &issues) {
    // RegGetValue automatically expands variables in REG_EXPAND_SZ.
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    DWORD bytes = 0;
    auto status = RegGetValueW(key, nullptr, name, flags, nullptr, nullptr, &bytes);
    if (absent(status))
        return {};
    if (status != ERROR_SUCCESS) {
        issues.push_back({source, name ? name : L"(Default)", status});
        return {};
    }
    // The installer may resize the value between the two reads.
    for (int attempt = 0; attempt != 4; ++attempt) {
        constexpr DWORD max_bytes = 64 * 1024;
        if (bytes > max_bytes) {
            issues.push_back({source, L"Registry string exceeds 64 KiB", ERROR_MORE_DATA});
            return {};
        }
        if (bytes % sizeof(wchar_t) != 0) {
            issues.push_back({source, L"Invalid UTF-16 byte count", ERROR_INVALID_DATA});
            return {};
        }
        // Read directly into the string to avoid a separate buffer and a copy.
        std::wstring value(std::max<std::size_t>(1, bytes / sizeof(wchar_t)), L'\0');
        DWORD capacity = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        status = RegGetValueW(key, nullptr, name, flags, nullptr, value.data(), &capacity);
        if (status == ERROR_SUCCESS) {
            const auto end = value.find(L'\0');
            if (end == std::wstring::npos) {
                status = ERROR_INVALID_DATA;
                break;
            }
            value.resize(end);
            return value;
        }
        if (absent(status))
            return {};
        if (status != ERROR_MORE_DATA)
            break;
        bytes = capacity;
    }
    issues.push_back({source, name ? name : L"(Default)", status});
    return {};
}

template <class Visitor>
void visit_subkeys(const registry_scope scope, const wchar_t *const branch, std::vector<scan_issue> &issues,
                   const Visitor &visitor) {
    const HKEY root = scope == registry_scope::current_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    // These flags are ignored on 32-bit Windows, so duplicate entries are possible.
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        registry_source source{scope, view, branch};
        RegistryKey parent;
        auto status = RegOpenKeyExW(root, branch, 0, KEY_READ | view, &parent.handle);
        if (absent(status))
            continue;
        if (status != ERROR_SUCCESS) {
            issues.push_back({source, L"RegOpenKeyExW", status});
            continue;
        }
        // A subkey name fits in 256 characters including the null terminator.
        for (DWORD index = 0;; ++index) {
            wchar_t name[256]{};
            DWORD length = static_cast<DWORD>(std::size(name));
            status = RegEnumKeyExW(parent.handle, index, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS) {
                issues.push_back({source, L"RegEnumKeyExW", status});
                break;
            }
            registry_source child_source{scope, view, std::wstring(branch) + L"\\" + std::wstring(name, length)};
            RegistryKey child;
            status = RegOpenKeyExW(parent.handle, name, 0, KEY_READ | view, &child.handle);
            if (absent(status))
                continue; // The subkey was deleted during enumeration.
            if (status != ERROR_SUCCESS) {
                issues.push_back({child_source, L"RegOpenKeyExW", status});
                continue;
            }
            visitor(child.handle, std::wstring(name, length), child_source);
        }
    }
}

bool equal_ignore_case(const std::wstring &lhs, const std::wstring &rhs) {
    return CompareStringOrdinal(lhs.c_str(), -1, rhs.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring trim_path(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    value.erase(last + 1);
    value.erase(0, first);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value.pop_back();
        value.erase(0, 1);
    }
    return value;
}

void append_unique(std::vector<discovered_application> &target, discovered_application entry) {
    const auto duplicate = std::find_if(target.begin(), target.end(), [&](const discovered_application &existing) {
        return equal_ignore_case(existing.executable.native(), entry.executable.native());
    });
    if (duplicate == target.end())
        target.push_back(std::move(entry));
}

void scan_app_paths(const registry_scope scope, scan_result<discovered_application> &result) {
    visit_subkeys(scope, app_paths_key, result.issues,
                  [&](const HKEY key, const std::wstring &, const registry_source &source) {
                      auto value = trim_path(read_string(key, nullptr, source, result.issues));
                      if (value.empty())
                          return;
                      std::filesystem::path path(value);
                      // Windows дополняет отсутствующее расширение App Paths суффиксом .exe.
                      if (path.extension().empty())
                          path += L".exe";
                      if (!path.is_absolute() || !equal_ignore_case(path.extension().native(), L".exe")) {
                          result.issues.push_back({source, L"Invalid App Paths EXE", ERROR_INVALID_DATA});
                          return;
                      }
                      path = path.lexically_normal();
                      const DWORD attributes = GetFileAttributesW(path.c_str());
                      if (attributes == INVALID_FILE_ATTRIBUTES) {
                          const auto error = static_cast<LSTATUS>(GetLastError());
                          result.issues.push_back({source, L"GetFileAttributesW", error});
                          return;
                      }
                      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                          return;
                      append_unique(result.entries, {std::move(path)});
                  });
}

scan_result<uninstall_entry> scan_uninstall(const registry_scope scope) {
    scan_result<uninstall_entry> result;
    visit_subkeys(
        scope, uninstall_key, result.issues, [&](const HKEY key, const std::wstring &, const registry_source &source) {
            const auto read = [&](const wchar_t *const field) {
                return read_string(key, field, source, result.issues);
            };
            uninstall_entry entry{read(L"DisplayName"),
                                  read(L"DisplayVersion"),
                                  read(L"Publisher"),
                                  read(L"InstallLocation"),
                                  read(L"DisplayIcon"),
                                  read(L"UninstallString"),
                                  read(L"QuietUninstallString"),
                                  source};
            // Keep unnamed entries too; the UI filters them separately.
            // The shared subkey may appear in both registry views.
            // Merge only identical data from the same subkey.
            const auto duplicate =
                std::find_if(result.entries.begin(), result.entries.end(), [&](const uninstall_entry &old) {
                    return old.source.key == source.key && old.display_name == entry.display_name &&
                           old.display_version == entry.display_version && old.publisher == entry.publisher &&
                           old.install_location == entry.install_location && old.display_icon == entry.display_icon &&
                           old.uninstall_command == entry.uninstall_command &&
                           old.quiet_uninstall_command == entry.quiet_uninstall_command;
                });
            if (duplicate == result.entries.end())
                result.entries.push_back(std::move(entry));
        });
    return result;
}

template <class T> void append_issues(scan_result<T> &scan, std::vector<scan_issue> *const issues) {
    if (issues) {
        issues->insert(issues->end(), std::make_move_iterator(scan.issues.begin()),
                       std::make_move_iterator(scan.issues.end()));
    }
}

std::vector<::Application> make_applications(scan_result<discovered_application> scan,
                                             std::vector<scan_issue> *const issues,
                                             const registry_progress &progress = {}) {
    append_issues(scan, issues);
    std::vector<::Application> applications;
    applications.reserve(scan.entries.size());
    if (progress && !progress(0, scan.entries.size()))
        return {};
    for (const auto &entry : scan.entries) {
        // The Application constructor reads the EXE name itself.
        applications.emplace_back(entry.executable);
        if (progress && !progress(applications.size(), scan.entries.size()))
            return {};
    }
    return applications;
}

} // namespace detail

std::vector<::Application> scan_app_paths_current_user(std::vector<scan_issue> *const issues) {
    detail::scan_result<detail::discovered_application> scan;
    detail::scan_app_paths(registry_scope::current_user, scan);
    return detail::make_applications(std::move(scan), issues);
}

std::vector<::Application> scan_app_paths_local_machine(std::vector<scan_issue> *const issues) {
    detail::scan_result<detail::discovered_application> scan;
    detail::scan_app_paths(registry_scope::local_machine, scan);
    return detail::make_applications(std::move(scan), issues);
}

std::vector<uninstall_entry> scan_uninstall_current_user(std::vector<scan_issue> *const issues) {
    auto scan = detail::scan_uninstall(registry_scope::current_user);
    detail::append_issues(scan, issues);
    return std::move(scan.entries);
}

std::vector<uninstall_entry> scan_uninstall_local_machine(std::vector<scan_issue> *const issues) {
    auto scan = detail::scan_uninstall(registry_scope::local_machine);
    detail::append_issues(scan, issues);
    return std::move(scan.entries);
}

std::vector<::Application> scan_registry_applications(std::vector<scan_issue> *const issues,
                                                      const registry_progress &progress) {
    detail::scan_result<detail::discovered_application> scan;
    detail::scan_app_paths(registry_scope::current_user, scan);
    detail::scan_app_paths(registry_scope::local_machine, scan);
    // Deduplicate paths before creating Application objects.
    return detail::make_applications(std::move(scan), issues, progress);
}

} // namespace xlibrary
