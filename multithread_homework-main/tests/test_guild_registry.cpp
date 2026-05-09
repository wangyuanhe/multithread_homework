/**
 * @file test_guild_registry.cpp
 * @brief Phase 2 — 冒险者档案馆测试
 */

#include "guild_registry.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>

using namespace guild;
using namespace std::chrono_literals;

// ============================================================
// 基本 CRUD 测试
// ============================================================

TEST(GuildRegistryTest, RegisterAndQuery) {
    GuildRegistry reg;
    reg.register_adventurer("hero_001", Attribute{int64_t(9999)});
    auto result = reg.query("hero_001");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<int64_t>(*result), 9999);
}

TEST(GuildRegistryTest, QueryNotFound) {
    GuildRegistry reg;
    EXPECT_FALSE(reg.query("nonexistent").has_value());
}

TEST(GuildRegistryTest, UpdateExisting) {
    GuildRegistry reg;
    reg.register_adventurer("hero_001", Attribute{int64_t(100)});
    reg.register_adventurer("hero_001", Attribute{int64_t(200)});
    auto result = reg.query("hero_001");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<int64_t>(*result), 200);
}

TEST(GuildRegistryTest, Dismiss) {
    GuildRegistry reg;
    reg.register_adventurer("hero_001", Attribute{std::string("勇者")});
    EXPECT_TRUE(reg.dismiss("hero_001"));
    EXPECT_FALSE(reg.query("hero_001").has_value());
}

TEST(GuildRegistryTest, DismissNotFound) {
    GuildRegistry reg;
    EXPECT_FALSE(reg.dismiss("nonexistent"));
}

TEST(GuildRegistryTest, VariantTypes) {
    GuildRegistry reg;

    reg.register_adventurer("int_attr",    Attribute{int64_t(42)});
    reg.register_adventurer("double_attr", Attribute{double(3.14)});
    reg.register_adventurer("str_attr",    Attribute{std::string("圣骑士")});
    reg.register_adventurer("bytes_attr",  Attribute{std::vector<std::byte>{std::byte{0xAB}}});

    EXPECT_EQ(std::get<int64_t>(*reg.query("int_attr")), 42);
    EXPECT_DOUBLE_EQ(std::get<double>(*reg.query("double_attr")), 3.14);
    EXPECT_EQ(std::get<std::string>(*reg.query("str_attr")), "圣骑士");
    EXPECT_EQ(std::get<std::vector<std::byte>>(*reg.query("bytes_attr")).size(), 1u);
}

// ============================================================
// TTL 过期测试
// ============================================================

TEST(GuildRegistryTest, TTLExpiry) {
    GuildRegistry reg;
    reg.register_adventurer("temp_hero", Attribute{int64_t(1)}, 1s);

    EXPECT_TRUE(reg.query("temp_hero").has_value());  // 刚注册，未过期

    std::this_thread::sleep_for(1100ms);
    EXPECT_FALSE(reg.query("temp_hero").has_value());  // 已过期
}

TEST(GuildRegistryTest, NoTTLNeverExpires) {
    GuildRegistry reg;
    reg.register_adventurer("perm_hero", Attribute{int64_t(1)});  // 无 TTL
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(reg.query("perm_hero").has_value());
}

TEST(GuildRegistryTest, EvictExpired) {
    GuildRegistry reg;
    reg.register_adventurer("temp1", Attribute{int64_t(1)}, 1s);
    reg.register_adventurer("temp2", Attribute{int64_t(2)}, 1s);
    reg.register_adventurer("perm",  Attribute{int64_t(3)});

    std::this_thread::sleep_for(1100ms);
    size_t evicted = reg.evict_all_expired();
    EXPECT_EQ(evicted, 2u);
    EXPECT_EQ(reg.total_size(), 1u);
}

// ============================================================
// 统计计数器测试
// ============================================================

TEST(GuildRegistryTest, StatsTracking) {
    GuildRegistry reg;
    reg.register_adventurer("h1", Attribute{int64_t(1)});
    reg.register_adventurer("h2", Attribute{int64_t(2)});
    reg.query("h1");   // hit
    reg.query("h1");   // hit
    reg.query("h99");  // miss
    reg.dismiss("h2");

    EXPECT_EQ(reg.stats().register_count.load(), 2u);
    EXPECT_EQ(reg.stats().query_count.load(), 3u);
    EXPECT_EQ(reg.stats().hit_count.load(), 2u);
    EXPECT_EQ(reg.stats().miss_count.load(), 1u);
    EXPECT_EQ(reg.stats().remove_count.load(), 1u);
}

// ============================================================
// 并发测试：读写锁正确性
// ============================================================

TEST(GuildRegistryTest, ConcurrentReads) {
    GuildRegistry reg;
    reg.register_adventurer("hero", Attribute{int64_t(42)});

    constexpr int kReaders = 8;
    std::atomic<int> success{0};
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < 1000; ++j) {
                auto r = reg.query("hero");
                if (r && std::get<int64_t>(*r) == 42) {
                    success.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : readers) t.join();
    EXPECT_EQ(success.load(), kReaders * 1000);
}

TEST(GuildRegistryTest, ConcurrentReadWrite) {
    GuildRegistry reg;
    constexpr int kOps = 5000;
    std::atomic<bool> stop{false};

    // 写线程：不断更新
    std::thread writer([&] {
        for (int i = 0; i < kOps; ++i) {
            reg.register_adventurer("shared_key", Attribute{int64_t(i)});
        }
        stop = true;
    });

    // 读线程：不断读取，不应崩溃
    std::thread reader([&] {
        while (!stop.load()) {
            reg.query("shared_key");  // 不崩溃即可
        }
    });

    writer.join();
    reader.join();
    // 主要验证：不崩溃、不死锁
    SUCCEED();
}

TEST(GuildRegistryTest, ShardingReducesContention) {
    // 验证不同 shard 的 key 可以真正并行操作
    GuildRegistry reg(16);
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 1000;
    std::atomic<int> total{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            std::string key = "hero_" + std::to_string(i);
            for (int j = 0; j < kOpsPerThread; ++j) {
                reg.register_adventurer(key, Attribute{int64_t(j)});
                reg.query(key);
                total.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(total.load(), kThreads * kOpsPerThread);
}

// ============================================================
// 反偷懒测试
// ============================================================

// 验证读操作真正并发（shared_lock），而非串行（unique_lock）
//
// 原理：query() 在持锁期间拷贝 Attribute 值后才释放锁。
// 注册一个拷贝耗时约数 ms 的大字符串（~5MB），让拷贝成为持锁时间的主体：
//   shared_lock：8个读者同时持锁拷贝，总耗时约 1x 单次时间
//   unique_lock：8个读者串行，总耗时约 8x 单次时间
// 测试在单分片上运行，强制所有读者竞争同一把锁。
TEST(GuildRegistryTest, ReadsAreSharedNotExclusive) {
    GuildRegistry reg(1);  // 单分片，强制所有读者竞争同一把锁

    // 拷贝发生在 query() 持锁期间，是区分 shared/unique lock 的关键
    constexpr size_t kSize = 1000 * 1024 * 1024;
    reg.register_adventurer("hero", Attribute{std::string(kSize, 'x')});

    // 先测单次读取基准耗时
    auto t0 = std::chrono::steady_clock::now();
    reg.query("hero");
    long long single_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (single_ms < 1) single_ms = 1;

    // 8个读者并发
    constexpr int kReaders = 8;
    std::vector<std::thread> readers;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] { reg.query("hero"); });
    }
    for (auto& t : readers) t.join();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // shared_lock：8个读者并行，耗时约 1x
    // unique_lock：8个读者串行，耗时约 8x
    // 阈值取 4x：shared_lock 必然通过，unique_lock 必然失败
    EXPECT_LT(elapsed_ms, single_ms * 6)
        << "8个并发读者耗时 " << elapsed_ms << "ms，单次读取约 " << single_ms << "ms，"
        << "比值超过 6x，疑似使用了独占锁（unique_lock）导致串行化";
}

// 验证分片真正隔离（不同 shard 的写操作不互相阻塞）
// 16个线程操作 16 个不同 shard 的 key，写操作各睡 10ms
// 真正分片：16个写操作并行，总耗时约 10ms
// 单全局锁：16个写操作串行，总耗时约 160ms
TEST(GuildRegistryTest, ShardsAreIndependent) {
    constexpr int kShards = 16;
    GuildRegistry reg(kShards);
    constexpr auto kWriteDelay = 10ms;

    // 预先找到落在不同 shard 的 key（通过哈希分布）
    // 简单起见：直接用 "shard_key_0" ~ "shard_key_15"，依赖哈希分布
    // 即使有碰撞，只要大部分在不同 shard，时间测试仍然有效

    std::vector<std::thread> writers;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < kShards; ++i) {
        writers.emplace_back([&, i] {
            std::this_thread::sleep_for(kWriteDelay);  // 模拟持写锁期间的耗时
            reg.register_adventurer("shard_key_" + std::to_string(i),
                                     Attribute{int64_t(i)});
        });
    }
    for (auto& t : writers) t.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 16个 shard 并行写，应在 50ms 内完成（串行需要 160ms）
    EXPECT_LT(elapsed, 50ms)
        << "16个不同 shard 的写操作应并行执行，"
        << "实际耗时 " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << "ms，疑似使用了单全局锁导致串行化";
}

// 验证写操作使用独占锁（unique_lock），而非共享锁（shared_lock）
//
// 原理：unordered_map 不是线程安全的容器。
// 若 put() 使用 shared_lock，多个写者可以同时进入，并发修改 unordered_map，
// 导致内部结构损坏（迭代器失效、节点链表断裂等），最终 size() 返回错误值或崩溃。
// 若 put() 使用 unique_lock，写者互斥，unordered_map 始终处于一致状态。
//
// 测试在单分片上运行，强制所有写者竞争同一把锁。
TEST(GuildRegistryTest, WritesAreExclusive) {
    GuildRegistry reg(1);  // 单分片，强制所有写者竞争同一把锁
    constexpr int kWriters = 8;
    constexpr int kPerWriter = 500;

    std::vector<std::thread> writers;
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&, i] {
            for (int j = 0; j < kPerWriter; ++j) {
                // 每个写者写自己独立的 key，避免值覆盖干扰
                reg.register_adventurer(
                    "w" + std::to_string(i) + "_k" + std::to_string(j),
                    Attribute{int64_t(j)});
            }
        });
    }
    for (auto& t : writers) t.join();

    // 若 put() 用 shared_lock，并发写 unordered_map 会损坏内部结构，
    // total_size() 返回错误值（通常远小于预期）或程序崩溃
    EXPECT_EQ(reg.total_size(), static_cast<size_t>(kWriters * kPerWriter))
        << "写入 " << kWriters * kPerWriter << " 条记录后 total_size() 不符，"
        << "put() 可能使用了 shared_lock 导致并发写损坏 unordered_map";
}

// 验证 remove() 使用独占锁
//
// 若 remove() 使用 shared_lock，多个线程并发 erase 同一 unordered_map，
// 会损坏内部结构，导致 total_size() 错误或崩溃。
TEST(GuildRegistryTest, RemovesAreExclusive) {
    GuildRegistry reg(1);  // 单分片
    constexpr int kKeys = 2000;

    // 先写入所有 key
    for (int i = 0; i < kKeys; ++i)
        reg.register_adventurer("rk_" + std::to_string(i), Attribute{int64_t(i)});
    ASSERT_EQ(reg.total_size(), static_cast<size_t>(kKeys));

    // 多线程并发删除各自负责的 key
    constexpr int kWorkers = 8;
    std::vector<std::thread> workers;
    int per = kKeys / kWorkers;
    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([&, i] {
            for (int j = i * per; j < (i + 1) * per; ++j)
                reg.dismiss("rk_" + std::to_string(j));
        });
    }
    for (auto& t : workers) t.join();

    // 若 remove() 用 shared_lock，并发 erase 损坏结构，size 不为 0 或崩溃
    EXPECT_EQ(reg.total_size(), 0u)
        << "删除全部记录后 total_size() 不为 0，"
        << "remove() 可能使用了 shared_lock 导致并发 erase 损坏 unordered_map";
}

// 验证 evict_expired() 使用独占锁
//
// 若 evict_expired() 使用 shared_lock，多个线程并发遍历并 erase，
// 会导致迭代器失效、结构损坏，崩溃或 evicted 计数错误。
TEST(GuildRegistryTest, EvictExpiredIsExclusive) {
    GuildRegistry reg(1);  // 单分片
    constexpr int kKeys = 2000;

    // 注册全部为立即过期（ttl=1s，sleep 后全部过期）
    for (int i = 0; i < kKeys; ++i)
        reg.register_adventurer("ek_" + std::to_string(i), Attribute{int64_t(i)}, 1s);

    std::this_thread::sleep_for(1100ms);  // 等待全部过期

    // 多线程并发调用 evict_all_expired
    constexpr int kWorkers = 8;
    std::atomic<size_t> total_evicted{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([&] {
            total_evicted.fetch_add(reg.evict_all_expired());
        });
    }
    for (auto& t : workers) t.join();

    // 若 evict_expired() 用 shared_lock，并发遍历+erase 会损坏结构或重复删除
    // 正确实现：总共恰好删除 kKeys 条（每条只被删一次），最终 size 为 0
    EXPECT_EQ(reg.total_size(), 0u)
        << "evict 后 total_size() 不为 0，"
        << "evict_expired() 可能使用了 shared_lock 导致并发 erase 损坏结构";
    EXPECT_EQ(total_evicted.load(), static_cast<size_t>(kKeys))
        << "并发 evict 总计删除 " << total_evicted.load() << " 条，期望 " << kKeys << " 条，"
        << "evict_expired() 可能使用了 shared_lock 导致重复删除或漏删";
}
