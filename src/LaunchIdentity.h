#ifndef XLIBRARY_LAUNCH_IDENTITY_H
#define XLIBRARY_LAUNCH_IDENTITY_H

#include <filesystem>
#include <sqlite3.h>
#include <string>
#include "CatalogTypes.h"

namespace xlibrary {
sqlite3_int64 ensure_launch_configuration(sqlite3 *connection, const app_record &record);
std::string path_utf8(const std::filesystem::path &path);
std::filesystem::path utf8_path(const std::string &path);
std::string path_identity(const std::filesystem::path &path, bool resolve_links = true);
sqlite3_int64 ensure_launch_configuration(sqlite3 *connection, const std::filesystem::path &path,
                                          const std::string &arguments = {},
                                          const std::filesystem::path &working_directory = {});
}

#endif
