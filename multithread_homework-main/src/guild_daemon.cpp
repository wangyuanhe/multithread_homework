#include "guild_daemon.h"

namespace guild {
    // ============================================================
    // ExpirySpirit
    // ============================================================

    ExpirySpirit::ExpirySpirit(GuildRegistry &registry, std::chrono::milliseconds patrol_interval)
        : registry_(registry), patrol_interval_(patrol_interval) {
    }

    void ExpirySpirit::start() {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) {
            return;
        }
        stop_flag_.store(false);
        thread_ = std::thread(&ExpirySpirit::patrol_loop, this);
    }

    void ExpirySpirit::stop() {
        if (!stop_flag_.exchange(true)) {
            cv_.notify_one();
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

    ExpirySpirit::~ExpirySpirit() {
        stop();
    }

    void ExpirySpirit::patrol_loop() {
        std::unique_lock lock(mutex_);
        while (!stop_flag_.load()) {
            cv_.wait_for(lock, patrol_interval_, [this] { return stop_flag_.load(); });
            if (stop_flag_.load()) break;

            registry_.evict_all_expired();
        }
    }

    // ============================================================
    // StatsDaemon
    // ============================================================

    StatsDaemon::StatsDaemon(const GuildRegistry &registry, std::chrono::milliseconds report_interval)
        : registry_(registry), report_interval_(report_interval) {
    }

    void StatsDaemon::start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) {
            return;
        }
        stop_flag_.store(false);
        thread_ = std::thread(&StatsDaemon::report_loop, this);
    }

    void StatsDaemon::stop() {
        if (!stop_flag_.exchange(true)) {
            cv_.notify_one();
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

    StatsDaemon::~StatsDaemon() {
        stop();
    }

    void StatsDaemon::report_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_flag_.load()) {
            cv_.wait_for(lock, report_interval_, [this] { return stop_flag_.load(); });
            if (stop_flag_.load()) break;

            registry_.stats().print();
        }
    }
} // namespace guild
