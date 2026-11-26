#ifndef FORADECK_HEARTBEAT_H
#define FORADECK_HEARTBEAT_H

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
    struct process_sample {
        quint32 pid = 0;
        quint64 start_time = 0;
        double cpu_seconds = -1;
        qint64 memory_bytes = -1;
    };
    using process_snapshot = QHash<QString, QVector<process_sample>>;
    static process_snapshot sample_processes(const QVector<app_record> &applications, bool memory, bool cpu);
    static void terminate_application(const app_record &application);
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
namespace foradeck {
QString windows_process_path(quint32 process_id);
}
#endif

#endif //FORADECK_HEARTBEAT_H
