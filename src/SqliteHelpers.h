#ifndef XLIBRARY_SQLITE_HELPERS_H
#define XLIBRARY_SQLITE_HELPERS_H

#include <memory>
#include <QString>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xlibrary {
inline void sqlite_execute(sqlite3 *const connection, const char *const sql) {
    if (sqlite3_exec(connection, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(connection));
}

class SqliteStatement {
    sqlite3 *connection;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{nullptr, &sqlite3_finalize};

public:
    SqliteStatement(sqlite3 *const connection, const char *const sql) : connection(connection) {
        sqlite3_stmt *raw = nullptr;
        const auto status = sqlite3_prepare_v2(connection, sql, -1, &raw, nullptr);
        statement.reset(raw);
        if (status != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(connection));
    }
    void bind(const int index, const std::string_view value) {
        if (sqlite3_bind_text64(statement.get(), index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8) !=
            SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(connection));
    }
    void bind(const int index, const sqlite3_int64 value) {
        if (sqlite3_bind_int64(statement.get(), index, value) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(connection));
    }
    void bind(const int index, const char *const value) { bind(index, std::string_view(value)); }
    void bind(const int index, const QString &value) {
        const auto bytes = value.toUtf8();
        bind(index, std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    }
    QString qtext(const int column) const {
        const auto *value = sqlite3_column_text(statement.get(), column);
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value),
                                         sqlite3_column_bytes(statement.get(), column))
                     : QString{};
    }
    bool next() {
        const auto status = sqlite3_step(statement.get());
        if (status != SQLITE_ROW && status != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(connection));
        return status == SQLITE_ROW;
    }
    sqlite3_int64 number(const int column) const { return sqlite3_column_int64(statement.get(), column); }
    std::string text(const int column) const {
        const auto *value = sqlite3_column_text(statement.get(), column);
        return value ? std::string(reinterpret_cast<const char *>(value), sqlite3_column_bytes(statement.get(), column))
                     : std::string{};
    }
};
}

#endif
