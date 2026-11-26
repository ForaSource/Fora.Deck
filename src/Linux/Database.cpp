#include <cstdlib>
#include <stdexcept>

#include "Database.h"

std::filesystem::path Database::get_database_path() {
    const auto *data_home = std::getenv("XDG_DATA_HOME");
    if (data_home != nullptr && data_home[0] != '\0') {
        const std::filesystem::path data_path(data_home);
        if (data_path.is_absolute()) {
            return data_path / "Fora.Deck" / "foradeck.db";
        }
    }
    // XDG does not allow relative values for this variable.
    const auto *home = std::getenv("HOME");
    if (home == nullptr || !std::filesystem::path(home).is_absolute()) {
        throw std::runtime_error("An absolute HOME or XDG_DATA_HOME is required");
    }
    return std::filesystem::path(home) / ".local" / "share" / "Fora.Deck" / "foradeck.db";
}
