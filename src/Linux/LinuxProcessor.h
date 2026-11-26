#ifndef FORADECK_LINUX_PROCESSOR_H
#define FORADECK_LINUX_PROCESSOR_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace foradeck {

struct process_identity {
    std::uint32_t pid = 0;
    std::uint64_t start_time = 0;
};

std::optional<process_identity> read_process_identity(std::uint32_t process_id, std::error_code &error);
bool is_process_alive(const process_identity &identity, std::error_code &error);
bool script_process_matches(std::uint32_t process_id, const std::filesystem::path &expected_path);
bool process_matches(const process_identity &identity, const std::filesystem::path &executable_path,
                     std::error_code &error);
std::optional<process_identity> find_process(const std::filesystem::path &executable_path, std::error_code &error);
process_identity launch_process(const std::filesystem::path &executable_path, const std::wstring &arguments,
                                const std::filesystem::path &working_directory);

} // namespace foradeck

#endif // FORADECK_LINUX_PROCESSOR_H
