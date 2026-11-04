#ifndef XLIBRARY_PROFILE_SCHEMA_H
#define XLIBRARY_PROFILE_SCHEMA_H

#include <filesystem>
#include <sqlite3.h>

namespace xlibrary {
inline constexpr int profile_schema_version = 8;
int read_schema_version(sqlite3 *connection);
void validate_profile(sqlite3 *connection);
void backup_database(sqlite3 *connection, const std::filesystem::path &destination);
void migrate_profile(sqlite3 *connection, const std::filesystem::path &path);
}

#endif
