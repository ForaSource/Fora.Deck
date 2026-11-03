#ifndef XLIBRARY_REGISTRY_SCAN_H
#define XLIBRARY_REGISTRY_SCAN_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Application.h"

namespace xlibrary {

class ComScope {
    std::int32_t result;

public:
    ComScope();
    ~ComScope();
};

enum class registry_scope { current_user, local_machine };

struct registry_source {
    registry_scope scope;
    std::uint32_t view; // KEY_WOW64_32KEY or KEY_WOW64_64KEY flag.
    std::wstring key;
};

struct scan_issue {
    registry_source source;
    std::wstring operation;
    std::int32_t error; // Win32 error code.
};

// Installation records are not launchable Application objects.
struct uninstall_entry {
    std::wstring display_name;
    std::wstring display_version;
    std::wstring publisher;
    std::wstring install_location;
    std::wstring display_icon;
    std::wstring uninstall_command;
    std::wstring quiet_uninstall_command;
    registry_source source;
};

// If issues != nullptr, append errors to the supplied vector.
// Do not suppress Application constructor exceptions.
std::vector<::Application> scan_app_paths_current_user(std::vector<scan_issue> *issues = nullptr);
std::vector<::Application> scan_app_paths_local_machine(std::vector<scan_issue> *issues = nullptr);
// Count unique existing EXEs; returning false from the callback cancels metadata reads.
using registry_progress = std::function<bool(std::size_t, std::size_t)>;
std::vector<::Application> scan_registry_applications(std::vector<scan_issue> *issues = nullptr,
                                                      const registry_progress &progress = {});

// Keep these lists separate; never merge them with the Application list.
std::vector<uninstall_entry> scan_uninstall_current_user(std::vector<scan_issue> *issues = nullptr);
std::vector<uninstall_entry> scan_uninstall_local_machine(std::vector<scan_issue> *issues = nullptr);

} // namespace xlibrary

#endif // XLIBRARY_REGISTRY_SCAN_H
