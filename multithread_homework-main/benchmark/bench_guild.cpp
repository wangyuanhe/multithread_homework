/**
 * @file bench_guild.cpp
 * @brief 公会管理系统性能基准测试
 *
 * 对比不同线程数下的吞吐量，直观感受并发收益。
 */

#include "guild_registry.h"
#include "dispatch_pool.h"
#include <iostream>
#include <windows.h>
#include <chrono>
#include <vector>
#include <thread>
#include <atomic>
#include <iomanip>

using namespace guild;
using namespace std::chrono;

// ============================================================
// 工具函数
// ============================================================

struct BenchResult {
    std::string name;
    int threads;
    double ops_per_sec;
    double latency_us;
};

void print_result(const BenchResult& r) {
    std::cout << std::left << std::setw(30) << r.name
              << " threads=" << std::setw(3) << r.threads
              << " throughput=" << std::setw(10) << static_cast<int>(r.ops_per_sec) << " ops/s"
              << " latency=" << std::fixed << std::setprecision(2) << r.latency_us << " us\n";
}

// ============================================================
// 基准1：档案馆读吞吐量（读多写少场景）
// ============================================================

BenchResult bench_read_throughput(int num_threads, int duration_ms = 1000) {
    GuildRegistry reg(16);
    // 预填充数据
    for (int i = 0; i < 1000; ++i) {
        reg.register_adventurer("hero_" + std::to_string(i), Attribute{int64_t(i)});
    }

    std::atomic<int64_t> ops{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            int key_idx = t;
            while (!stop.load()) {
                reg.query("hero_" + std::to_string(key_idx % 1000));
                ++key_idx;
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(milliseconds(duration_ms));
    stop = true;
    for (auto& t : threads) t.join();

    double ops_per_sec = ops.load() * 1000.0 / duration_ms;
    return {"Registry Read", num_threads, ops_per_sec, 1e6 / ops_per_sec * num_threads};
}

// ============================================================
// 基准2：线程池任务吞吐量
// ============================================================

BenchResult bench_threadpool(int num_workers, int num_tasks = 100000) {
    DispatchPool pool(num_workers);
    std::atomic<int> done{0};

    auto start = steady_clock::now();
    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.dispatch([&] {
            done.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1us);
        }));
    }
    for (auto& f : futures) f.get();
    auto elapsed = steady_clock::now() - start;

    double ms = duration<double, std::milli>(elapsed).count();
    double ops_per_sec = num_tasks * 1000.0 / ms;
    return {"ThreadPool Tasks", num_workers, ops_per_sec, ms * 1000.0 / num_tasks};
}

// ============================================================
// 基准3：混合读写（模拟真实场景）
// ============================================================

BenchResult bench_mixed_rw(int num_threads, int duration_ms = 1000) {
    GuildRegistry reg(16);
    std::atomic<int64_t> ops{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            int counter = t;
            while (!stop.load()) {
                std::string key = "hero_" + std::to_string(counter % 100);
                if (counter % 10 == 0) {
                    // 10% 写操作
                    reg.register_adventurer(key, Attribute{int64_t(counter)});
                } else {
                    // 90% 读操作
                    reg.query(key);
                }
                ++counter;
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(milliseconds(duration_ms));
    stop = true;
    for (auto& t : threads) t.join();

    double ops_per_sec = ops.load() * 1000.0 / duration_ms;
    return {"Mixed R/W (90/10)", num_threads, ops_per_sec, 1e6 / ops_per_sec * num_threads};
}

// ============================================================
// main
// ============================================================

int main() {
    SetConsoleOutputCP(65001);
    std::cout << "=== 异世界冒险者公会管理系统 — 性能基准 ===\n\n";

    std::vector<BenchResult> results;

    // 档案馆读吞吐量
    std::cout << "--- 档案馆读吞吐量 ---\n";
    for (int t : {1, 2, 4, 8}) {
        auto r = bench_read_throughput(t);
        print_result(r);
        results.push_back(r);
    }

    std::cout << "\n--- 线程池任务吞吐量 ---\n";
    for (int t : {1, 2, 4, 8}) {
        auto r = bench_threadpool(t);
        print_result(r);
        results.push_back(r);
    }

    std::cout << "\n--- 混合读写（90%读/10%写）---\n";
    for (int t : {1, 2, 4, 8}) {
        auto r = bench_mixed_rw(t);
        print_result(r);
        results.push_back(r);
    }

    std::cout << "\n=== 基准测试完成 ===\n";
    std::cout << "提示：对比 1线程 vs 多线程 的吞吐量，观察并发收益。\n";
    std::cout << "      读多写少场景下，shared_mutex 的优势最为明显。\n";
    std::cout << "      删除基准2中87行的睡眠，重新运行测试，比较差异。\n";

    return 0;
}
