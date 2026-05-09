#include "dispatch_pool.h"

namespace guild {
    DispatchPool::DispatchPool(size_t num_windows) {
        for (size_t i = 0; i < num_windows; ++i) {
            workers_.emplace_back(&DispatchPool::worker_loop, this);
        }
    }

    void DispatchPool::worker_loop() {
        while (true) {
            auto task = board_.wait_and_take();
            if (!task.has_value()) break;
            (*task)();
        }
    }

    void DispatchPool::shutdown() {
        if (shutdown_.exchange(true)) {
            return;
        }
        board_.close_board();
        for (auto &t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    DispatchPool::~DispatchPool() {
        shutdown();
    }
} // namespace guild
