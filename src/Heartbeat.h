#ifndef XLIBRARY_HEARTBEAT_H
#define XLIBRARY_HEARTBEAT_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <QSet>
#include <thread>
#include <utility>

#include "CatalogTypes.h"

// Monitors one process or resolves the running paths of the complete catalog.
class Heartbeat {
private:
    static std::move_only_function<std::pair<bool, std::uint32_t>()>
    make_process_checker(std::uint32_t process_id, const std::filesystem::path &executable_path);

    struct State;
    std::shared_ptr<State> state;
    std::jthread worker;

public:
    static QSet<QString> running_paths(const QVector<app_record> &applications);
    Heartbeat(std::uint32_t process_id, const std::filesystem::path &executable_path);
    Heartbeat(const Heartbeat &) = delete;
    Heartbeat &operator=(const Heartbeat &) = delete;
    Heartbeat(Heartbeat &&) noexcept = default;
    Heartbeat &operator=(Heartbeat &&) noexcept = default;

    Heartbeat() {};

    bool is_alive() const noexcept;
    std::uint32_t get_error_code() const noexcept;
    void request_stop() noexcept;
};

#ifdef _WIN32
namespace xlibrary {
QString windows_process_path(quint32 process_id);
}
#endif

#endif //XLIBRARY_HEARTBEAT_H
