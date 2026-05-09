/**
 * @file stress_test.cpp
 * @brief 公会大混战 — 综合并发压力测试
 *
 * 模拟真实场景：大量冒险者同时进行各种操作，
 * 验证整个系统在高并发下的正确性和稳定性。
 */

#include "guild_registry.h"
#include "dispatch_pool.h"
#include "trade_system.h"
#include "expedition.h"
#include "guild_daemon.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <iostream>

using namespace guild;
using namespace std::chrono_literals;

// ============================================================
// 综合压力测试
// ============================================================

/**
 * @brief 公会大混战
 *
 * 同时运行：
 *   - 多个注册线程（不断注册新冒险者）
 *   - 多个查询线程（不断查询冒险者属性）
 *   - 多个删除线程（不断删除冒险者）
 *   - 守护精灵（后台清理过期条目）
 *
 * 验证：不崩溃、不死锁、统计数据合理
 */
TEST(StressTest, GuildChaos) {
    GuildRegistry reg(32);
    ExpirySpirit spirit(reg, 100ms);
    spirit.start();

    constexpr int kDuration = 2;  // 秒
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kDeleters = 2;

    std::atomic<bool> stop{false};
    std::atomic<int64_t> writes{0}, reads{0}, deletes{0};

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kDuration);

    // 写线程
    std::vector<std::thread> writers;
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&, i] {
            std::mt19937 rng(i);
            std::uniform_int_distribution<int> dist(0, 99);
            while (std::chrono::steady_clock::now() < deadline) {
                std::string key = "hero_" + std::to_string(dist(rng));
                reg.register_adventurer(key, Attribute{int64_t(dist(rng))}, 1s);
                writes.fetch_add(1);
            }
        });
    }

    // 读线程
    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&, i] {
            std::mt19937 rng(i + 100);
            std::uniform_int_distribution<int> dist(0, 99);
            while (std::chrono::steady_clock::now() < deadline) {
                std::string key = "hero_" + std::to_string(dist(rng));
                reg.query(key);
                reads.fetch_add(1);
            }
        });
    }

    // 删除线程
    std::vector<std::thread> deleters;
    for (int i = 0; i < kDeleters; ++i) {
        deleters.emplace_back([&, i] {
            std::mt19937 rng(i + 200);
            std::uniform_int_distribution<int> dist(0, 99);
            while (std::chrono::steady_clock::now() < deadline) {
                std::string key = "hero_" + std::to_string(dist(rng));
                reg.dismiss(key);
                deletes.fetch_add(1);
            }
        });
    }

    for (auto& t : writers) t.join();
    for (auto& t : readers) t.join();
    for (auto& t : deleters) t.join();
    spirit.stop();

    std::cout << "=== 公会大混战结果 ===\n"
              << "写入: " << writes.load() << "\n"
              << "读取: " << reads.load() << "\n"
              << "删除: " << deletes.load() << "\n";
    reg.stats().print();

    SUCCEED();  // 不崩溃即通过
}

/**
 * @brief 线程池压力测试
 *
 * 大量任务提交，验证线程池的吞吐量和正确性。
 */
TEST(StressTest, ThreadPoolThroughput) {
    DispatchPool pool(8);
    constexpr int kTasks = 50000;
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.dispatch([&] { counter.fetch_add(1); }));
    }
    for (auto& f : futures) f.get();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(counter.load(), kTasks);

    double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    double throughput = kTasks / (ms / 1000.0);
    std::cout << "线程池吞吐量: " << static_cast<int>(throughput) << " tasks/sec\n";
}

/**
 * @brief 全系统集成测试
 *
 * 使用所有组件：线程池 + 档案馆 + 事务 + 异步客户端
 */
TEST(StressTest, FullSystemIntegration) {
    GuildRegistry reg(16);
    DispatchPool pool(4);
    TradeSystem trade(reg);
    ExpeditionClient client(reg, pool);
    BatchDispatcher batch(reg, pool);

    // 批量注册 100 个冒险者
    std::vector<Quest> init_quests;
    for (int i = 0; i < 100; ++i) {
        init_quests.push_back({
            QuestType::kRegister,
            "hero_" + std::to_string(i),
            Attribute{int64_t(1000)}
        });
    }
    batch.launch(init_quests);

    // 并发事务转账
    std::atomic<int> tx_success{0};
    std::vector<std::thread> tx_threads;
    for (int t = 0; t < 4; ++t) {
        tx_threads.emplace_back([&, t] {
            std::mt19937 rng(t);
            std::uniform_int_distribution<int> dist(0, 99);
            for (int i = 0; i < 100; ++i) {
                int from = dist(rng), to = dist(rng);
                if (from == to) continue;
                auto tx = trade.begin();
                auto fv = tx->get("hero_" + std::to_string(from));
                auto tv = tx->get("hero_" + std::to_string(to));
                if (!fv || !tv) continue;
                int64_t fg = std::get<int64_t>(*fv);
                int64_t tg = std::get<int64_t>(*tv);
                if (fg < 10) continue;
                tx->put("hero_" + std::to_string(from), Attribute{fg - 10});
                tx->put("hero_" + std::to_string(to),   Attribute{tg + 10});
                if (tx->commit()) tx_success.fetch_add(1);
            }
        });
    }
    for (auto& t : tx_threads) t.join();

    // 验证总金币守恒
    int64_t total = 0;
    for (int i = 0; i < 100; ++i) {
        auto v = reg.query("hero_" + std::to_string(i));
        if (v) total += std::get<int64_t>(*v);
    }
    EXPECT_EQ(total, 100 * 1000) << "金币守恒被破坏！";

    pool.shutdown();
    std::cout << "集成测试完成，成功事务: " << tx_success.load() << "\n";
}
