#include <cstdlib>
#include <stdexcept>

#include "Database.h"

std::filesystem::path Database::get_database_path() {
    const auto *local_app_data = _wgetenv(L"LOCALAPPDATA");
    if (local_app_data == nullptr || local_app_data[0] == L'\0') {
        throw std::runtime_error("LOCALAPPDATA is not set");
    }

    const std::filesystem::path local_app_data_path(local_app_data);
    if (!local_app_data_path.is_absolute()) {
        throw std::runtime_error("LOCALAPPDATA must be an absolute path");
    }

    return local_app_data_path / L"Fora.Deck" / L"foradeck.db";
}
