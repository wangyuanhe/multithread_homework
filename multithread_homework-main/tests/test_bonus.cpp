/**
 * @file test_bonus.cpp
 * @brief 加分项自动测试
 *
 * 本文件测试加分项，每个加分项对应独立的测试组。
 * 基础分测试全部通过后，再挑战这些测试。
 */

#include "expedition.h"
#include "trade_system.h"
#include "guild_registry.h"
#include "dispatch_pool.h"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <future>

using namespace guild;
using namespace std::chrono_literals;

// ============================================================
// 加分项1：带回调的异步接口
// ============================================================

class BonusCallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool   = std::make_unique<DispatchPool>(4);
        reg    = std::make_unique<GuildRegistry>();
        client = std::make_unique<ExpeditionClient>(*reg, *pool);
    }
    void TearDown() override {
        client.reset();
        pool->shutdown();
    }
    std::unique_ptr<DispatchPool>      pool;
    std::unique_ptr<GuildRegistry>     reg;
    std::unique_ptr<ExpeditionClient>  client;
};

// 基本回调：查询存在的 key，回调收到正确值
TEST_F(BonusCallbackTest, CallbackReceivesCorrectValue) {
    reg->register_adventurer("hero_001", Attribute{int64_t(42)});

    std::promise<std::optional<Attribute>> promise;
    auto future = promise.get_future();

    client->async_query_with_callback("hero_001",
        [&](std::optional<Attribute> val) {
            promise.set_value(std::move(val));
        });

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<int64_t>(*result), 42);
}

// 回调在 worker 线程中执行（不阻塞调用线程）
TEST_F(BonusCallbackTest, CallbackExecutedInWorkerThread) {
    reg->register_adventurer("hero_001", Attribute{int64_t(1)});

    std::promise<std::thread::id> promise;
    auto future = promise.get_future();
    std::thread::id caller_id = std::this_thread::get_id();

    client->async_query_with_callback("hero_001",
        [&](std::optional<Attribute>) {
            promise.set_value(std::this_thread::get_id());
        });

    std::thread::id callback_id = future.get();
    EXPECT_NE(callback_id, caller_id)
        << "回调应在 worker 线程中执行，不应阻塞调用线程";
}

// 回调查询不存在的 key，收到 nullopt
TEST_F(BonusCallbackTest, CallbackReceivesNulloptForMissingKey) {
    std::atomic<bool> called{false};
    std::atomic<bool> got_value{true};

    client->async_query_with_callback("nonexistent",
        [&](std::optional<Attribute> val) {
            got_value = val.has_value();
            called = true;
        });

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!called.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(called.load());
    EXPECT_FALSE(got_value.load());
}

// 多个并发回调都被正确调用
TEST_F(BonusCallbackTest, MultipleCallbacksAllInvoked) {
    constexpr int kCount = 20;
    for (int i = 0; i < kCount; ++i)
        reg->register_adventurer("cb_hero_" + std::to_string(i), Attribute{int64_t(i)});

    std::atomic<int> callback_count{0};
    std::atomic<int64_t> value_sum{0};

    for (int i = 0; i < kCount; ++i) {
        client->async_query_with_callback("cb_hero_" + std::to_string(i),
            [&](std::optional<Attribute> val) {
                if (val) value_sum.fetch_add(std::get<int64_t>(*val));
                callback_count.fetch_add(1);
            });
    }

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (callback_count.load() < kCount && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(callback_count.load(), kCount) << "部分回调未被调用";
    // 0+1+2+...+19 = 190
    EXPECT_EQ(value_sum.load(), int64_t(kCount * (kCount - 1) / 2));
}

// ============================================================
// 加分项2（+4）：事务只锁涉及的 shard（细粒度锁）
// ============================================================
//
// 验证思路：
//   两个事务分别操作落在不同 shard 的 key，commit 应能真正并行。
//   若 commit 使用全局大锁，两个事务串行，总耗时约 2*delay。
//   若 commit 使用 shard 级别锁，两个事务并行，总耗时约 1*delay。
//
// 为了让 commit 期间有可测量的耗时，我们需要在 commit 持锁期间插入延迟。
// 由于无法直接 hook commit，改用大量 key 操作来放大锁竞争时间差。

TEST(BonusShardLockTest, TransactionsOnDifferentShardsRunInParallel) {
    // 使用 2 个 shard，确保两个事务的 key 落在不同 shard
    // key 的 shard 由 hash(key) % shard_count 决定
    // 我们找两个哈希值模 2 分别为 0 和 1 的 key
    constexpr size_t kShards = 2;
    GuildRegistry reg(kShards);
    TradeSystem trade(reg);

    // 找落在不同 shard 的 key 对
    // shard_index = std::hash<string>{}(key) % 2
    auto shard_of = [&](const std::string& key) {
        return std::hash<std::string>{}(key) % kShards;
    };

    // 预先找好两个分别落在 shard 0 和 shard 1 的 key
    std::string key_shard0, key_shard1;
    for (int i = 0; key_shard0.empty() || key_shard1.empty(); ++i) {
        std::string k = "tx_key_" + std::to_string(i);
        if (key_shard0.empty() && shard_of(k) == 0) key_shard0 = k;
        if (key_shard1.empty() && shard_of(k) == 1) key_shard1 = k;
    }

    reg.register_adventurer(key_shard0, Attribute{int64_t(0)});
    reg.register_adventurer(key_shard1, Attribute{int64_t(0)});

    // 两个事务分别只操作一个 shard 的 key
    // 用大量重复写入来放大持锁时间
    constexpr int kWritesPerTx = 5000;
    std::atomic<bool> both_started{false};
    std::atomic<int> started_count{0};

    auto run_tx = [&](const std::string& key, int64_t val) {
        // 等两个线程都就绪后同时开始，最大化并发冲突
        started_count.fetch_add(1);
        while (started_count.load() < 2) std::this_thread::yield();

        for (int i = 0; i < kWritesPerTx; ++i) {
            auto tx = trade.begin();
            tx->put(key, Attribute{val + i});
            tx->commit();
        }
    };

    auto start = std::chrono::steady_clock::now();

    std::thread t1([&] { run_tx(key_shard0, 100); });
    std::thread t2([&] { run_tx(key_shard1, 200); });
    t1.join();
    t2.join();

    auto elapsed = std::chrono::steady_clock::now() - start;

    // 串行基准：单线程跑 2*kWritesPerTx 次事务
    started_count = 0;
    auto serial_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kWritesPerTx; ++i) {
        auto tx = trade.begin();
        tx->put(key_shard0, Attribute{int64_t(i)});
        tx->commit();
    }
    for (int i = 0; i < kWritesPerTx; ++i) {
        auto tx = trade.begin();
        tx->put(key_shard1, Attribute{int64_t(i)});
        tx->commit();
    }
    auto serial_elapsed = std::chrono::steady_clock::now() - serial_start;

    double parallel_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    double serial_ms   = std::chrono::duration<double, std::milli>(serial_elapsed).count();
    double speedup     = serial_ms / parallel_ms;

    std::cout << "[加分项2] 并行耗时: " << parallel_ms << "ms"
              << "，串行基准: " << serial_ms << "ms"
              << "，加速比: " << speedup << "x\n";

    // 不同 shard 的事务并行，加速比应 > 1.3（留足余量）
    // 若使用全局大锁，加速比接近 1.0
    EXPECT_GT(speedup, 1.3)
        << "不同 shard 的事务加速比过低（" << speedup << "x），"
        << "commit 可能使用了全局大锁而非 shard 级别锁";
}

// ============================================================
// 加分项2：OCC 冲突检测 — 并发转账金币守恒
// ============================================================
//
// 验证思路：
//   两个账户各有 10000 金币，多个线程并发执行"从 A 转 1 金币到 B"。
//   每次转账是一个事务：读 A、读 B → 写 A-1、写 B+1 → commit。
//   若 commit 有 OCC 冲突检测，冲突时返回 false，调用方重试，金币总量守恒。
//   若 commit 无冲突检测（纯 2PL），两个事务同时读到 A=100，各自写 A=99，
//   导致 lost update，金币凭空消失。
//
// 这个测试是区分"只有细粒度锁"和"OCC + 细粒度锁"的关键。

TEST(BonusShardLockTest, ConcurrentTransferPreservesTotal) {
    GuildRegistry reg(16);
    TradeSystem trade(reg);

    constexpr int64_t kInitial = 10000;
    constexpr int kTransfers = 2000;
    constexpr int kThreads = 4;

    reg.register_adventurer("account_A", Attribute{kInitial});
    reg.register_adventurer("account_B", Attribute{kInitial});

    // 每个线程执行 kTransfers 次 "A → B 转 1 金币"
    // 事务：读 A、读 B → 写 A-1、写 B+1 → commit
    // commit 返回 false 时重试（OCC 冲突）
    std::atomic<int> retry_count{0};
    std::atomic<int> success_count{0};

    auto transfer_loop = [&] {
        for (int i = 0; i < kTransfers; ++i) {
            bool committed = false;
            while (!committed) {
                auto tx = trade.begin();
                auto a_val = tx->get("account_A");
                auto b_val = tx->get("account_B");

                if (!a_val || !b_val) break;  // 不应发生

                int64_t a = std::get<int64_t>(*a_val);
                int64_t b = std::get<int64_t>(*b_val);

                tx->put("account_A", Attribute{a - 1});
                tx->put("account_B", Attribute{b + 1});

                committed = tx->commit();
                if (!committed) retry_count.fetch_add(1);
            }
            if (committed) success_count.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(transfer_loop);
    for (auto& t : threads) t.join();

    // 验证金币守恒
    auto a_final = reg.query("account_A");
    auto b_final = reg.query("account_B");
    ASSERT_TRUE(a_final.has_value());
    ASSERT_TRUE(b_final.has_value());

    int64_t a = std::get<int64_t>(*a_final);
    int64_t b = std::get<int64_t>(*b_final);
    int64_t total = a + b;

    std::cout << "[加分项2-OCC] A=" << a << " B=" << b
              << " 总计=" << total << " (期望 " << 2 * kInitial << ")"
              << " 成功=" << success_count.load()
              << " 重试=" << retry_count.load() << "\n";

    // 金币总量必须守恒
    EXPECT_EQ(total, 2 * kInitial)
        << "并发转账后金币总量不守恒（" << total << " != " << 2 * kInitial << "），"
        << "commit 可能缺少 OCC 冲突检测，导致 lost update";

    // 所有转账都应成功完成
    EXPECT_EQ(success_count.load(), kThreads * kTransfers);

    // OCC 在高并发下应有一定重试次数（若 retry_count=0 说明可能没有冲突检测）
    EXPECT_GT(retry_count.load(), 0)
        << "并发转账无任何重试，commit 可能未实现 OCC 冲突检测";
}
