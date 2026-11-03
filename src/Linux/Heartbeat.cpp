#include <stdexcept>
#include <system_error>

#include "Heartbeat.h"
#include "LinuxProcessor.h"

std::move_only_function<std::pair<bool, std::uint32_t>()>
Heartbeat::make_process_checker(const std::uint32_t process_id, const std::filesystem::path &executable_path) {
    if (process_id == 0 || executable_path.filename().empty()) {
        throw std::invalid_argument("Heartbeat requires a nonzero PID and an executable name");
    }
    const auto expected_path = std::filesystem::weakly_canonical(std::filesystem::absolute(executable_path));
    std::error_code initial_error;
    auto identity = xlibrary::read_process_identity(process_id, initial_error);
    if (initial_error) {
        throw std::system_error(initial_error, "Cannot open heartbeat process");
    }
    auto check_process = [identity, expected_path]() mutable -> std::pair<bool, std::uint32_t> {
        std::error_code error;
        if (identity) {
            if (xlibrary::process_matches(*identity, expected_path, error)) {
                return {true, 0};
            }
            if (error) {
                return {false, static_cast<std::uint32_t>(error.value())};
            }
            identity.reset();
        }
        identity = xlibrary::find_process(expected_path, error);
        return {identity.has_value(), static_cast<std::uint32_t>(error.value())};
    };

    return check_process;
}
