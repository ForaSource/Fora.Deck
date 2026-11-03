#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <utility>

#include "Heartbeat.h"

struct Heartbeat::State {
    std::atomic<bool> alive{false};
    std::atomic<std::uint32_t> error_code{0};
};

Heartbeat::Heartbeat(const std::uint32_t process_id, const std::filesystem::path &executable_path)
    : state(std::make_shared<State>()) {
    auto check_process = make_process_checker(process_id, executable_path);

    const auto [alive, error_code] = check_process();
    if (error_code != 0) {
        throw std::system_error(error_code, std::system_category(), "Cannot check heartbeat process");
    }
    state->alive.store(alive);
    if (!alive) {
        return;
    }

    worker = std::jthread(
        [state = state, check_process = std::move(check_process)](const std::stop_token stop_token) mutable {
            std::mutex wait_mutex;
            std::condition_variable_any wake;
            std::unique_lock wait_lock(wait_mutex);
            while (!stop_token.stop_requested()) {
                wake.wait_for(wait_lock, stop_token, std::chrono::milliseconds(1500), [] { return false; });
                if (stop_token.stop_requested()) {
                    break;
                }
                const auto [alive, error_code] = check_process();
                state->error_code.store(error_code);
                state->alive.store(alive);
                if (!alive) {
                    break;
                }
            }
        });
}

bool Heartbeat::is_alive() const noexcept {
    return state && state->alive.load();
}

std::uint32_t Heartbeat::get_error_code() const noexcept {
    return state ? state->error_code.load() : 0;
}

void Heartbeat::request_stop() noexcept {
    worker.request_stop();
}
