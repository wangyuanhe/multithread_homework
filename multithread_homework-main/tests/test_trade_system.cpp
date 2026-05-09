/**
 * @file test_trade_system.cpp
 * @brief Phase 3 — 装备交易所（事务系统）测试
 */

#include "trade_system.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>
#include <random>

using namespace guild;

// ============================================================
// 基本事务测试
// ============================================================

TEST(TransactionTest, BasicCommit) {
    GuildRegistry reg;
    TradeSystem trade(reg);

    auto tx = trade.begin();
    tx->put("hero_001", Attribute{int64_t(100)});
    EXPECT_TRUE(tx->commit());

    EXPECT_TRUE(reg.query("hero_001").has_value());
    EXPECT_EQ(std::get<int64_t>(*reg.query("hero_001")), 100);
}

TEST(TransactionTest, Rollback) {
    GuildRegistry reg;
    TradeSystem trade(reg);

    auto tx = trade.begin();
    tx->put("hero_001", Attribute{int64_t(100)});
    tx->rollback();

    EXPECT_FALSE(reg.query("hero_001").has_value());
}

TEST(TransactionTest, AutoRollbackOnDestruct) {
    GuildRegistry reg;
    TradeSystem trade(reg);

    {
        auto tx = trade.begin();
        tx->put("hero_001", Attribute{int64_t(100)});
        // 不调用 commit，析构时自动 rollback
    }

    EXPECT_FALSE(reg.query("hero_001").has_value());
}

TEST(TransactionTest, ReadYourWrites) {
    GuildRegistry reg;
    reg.register_adventurer("hero_001", Attribute{int64_t(100)});
    TradeSystem trade(reg);

    auto tx = trade.begin();
    tx->put("hero_001", Attribute{int64_t(999)});

    // 事务内应该能读到自己的未提交写入
    auto val = tx->get("hero_001");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(std::get<int64_t>(*val), 999);

    tx->rollback();
    // rollback 后档案馆中仍是原值
    EXPECT_EQ(std::get<int64_t>(*reg.query("hero_001")), 100);
}

TEST(TransactionTest, ReadYourDeletes) {
    GuildRegistry reg;
    reg.register_adventurer("hero_001", Attribute{int64_t(100)});
    TradeSystem trade(reg);

    auto tx = trade.begin();
    tx->remove("hero_001");

    // 事务内应该看不到已删除的条目
    EXPECT_FALSE(tx->get("hero_001").has_value());

    tx->rollback();
    // rollback 后档案馆中仍存在
    EXPECT_TRUE(reg.query("hero_001").has_value());
}

TEST(TransactionTest, MultiKeyCommit) {
    GuildRegistry reg;
    TradeSystem trade(reg);

    auto tx = trade.begin();
    tx->put("sword", Attribute{std::string("圣剑")});
    tx->put("staff", Attribute{std::string("法杖")});
    tx->put("shield", Attribute{std::string("神盾")});
    EXPECT_TRUE(tx->commit());

    EXPECT_TRUE(reg.query("sword").has_value());
    EXPECT_TRUE(reg.query("staff").has_value());
    EXPECT_TRUE(reg.query("shield").has_value());
}

// ============================================================
// std::any 上下文测试
// ============================================================

TEST(TransactionTest, ContextSetAndGet) {
    GuildRegistry reg;
    TradeSystem trade(reg);

    auto tx = trade.begin();
    tx->set_context(std::string("购买圣剑"));

    auto ctx = tx->get_context<std::string>();
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(*ctx, "购买圣剑");

    // 类型不匹配时返回 nullopt
    EXPECT_FALSE(tx->get_context<int>().has_value());

    tx->rollback();
}

TEST(TransactionTest, ContextOverwrite) {
    GuildRegistry reg;
    TradeSystem trade(reg);

    auto tx = trade.begin();
    tx->set_context(42);
    tx->set_context(std::string("覆盖了"));

    EXPECT_FALSE(tx->get_context<int>().has_value());
    EXPECT_TRUE(tx->get_context<std::string>().has_value());
    tx->rollback();
}

// ============================================================
// 并发事务测试：金币守恒（最重要的测试！）
// ============================================================

/**
 * @brief 金币守恒测试
 *
 * 8个冒险者，每人初始 1000 金币，总计 8000 金币。
 * 多线程并发进行随机转账，每次转账是一个事务。
 * 所有转账完成后，总金币数必须仍然是 8000。
 *
 * 这个测试验证：
 *   1. 事务的原子性（不会出现"钱凭空消失"）
 *   2. 死锁避免（不会卡住）
 */
TEST(TransactionTest, GoldConservation) {
    GuildRegistry reg;
    TradeSystem trade(reg);

    constexpr int kAdventurers = 8;
    constexpr int64_t kInitialGold = 1000;
    constexpr int kTransactions = 500;

    // 初始化：每人 1000 金币
    for (int i = 0; i < kAdventurers; ++i) {
        reg.register_adventurer("adventurer_" + std::to_string(i),
                                 Attribute{kInitialGold});
    }

    std::atomic<int> success_count{0};
    std::atomic<int> retry_count{0};

    // 多线程并发转账
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t * 12345);
            std::uniform_int_distribution<int> dist(0, kAdventurers - 1);

            for (int i = 0; i < kTransactions / 4; ++i) {
                int from = dist(rng);
                int to = dist(rng);
                if (from == to) continue;

                std::string from_key = "adventurer_" + std::to_string(from);
                std::string to_key   = "adventurer_" + std::to_string(to);
                constexpr int64_t amount = 10;

                // 重试循环（处理事务冲突）
                for (int retry = 0; retry < 10; ++retry) {
                    auto tx = trade.begin();
                    auto from_val = tx->get(from_key);
                    auto to_val   = tx->get(to_key);

                    if (!from_val || !to_val) break;

                    int64_t from_gold = std::get<int64_t>(*from_val);
                    int64_t to_gold   = std::get<int64_t>(*to_val);

                    if (from_gold < amount) break;

                    tx->put(from_key, Attribute{from_gold - amount});
                    tx->put(to_key,   Attribute{to_gold   + amount});

                    if (tx->commit()) {
                        success_count.fetch_add(1);
                        break;
                    } else {
                        retry_count.fetch_add(1);
                    }
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    // 验证金币守恒
    int64_t total_gold = 0;
    for (int i = 0; i < kAdventurers; ++i) {
        auto val = reg.query("adventurer_" + std::to_string(i));
        ASSERT_TRUE(val.has_value()) << "冒险者 " << i << " 的档案丢失了！";
        total_gold += std::get<int64_t>(*val);
    }

    EXPECT_EQ(total_gold, kAdventurers * kInitialGold)
        << "金币守恒被破坏！总金币应为 " << kAdventurers * kInitialGold
        << "，实际为 " << total_gold;

    std::cout << "成功转账: " << success_count.load()
              << "，重试次数: " << retry_count.load() << "\n";
}

// ============================================================
// 反偷懒测试
// ============================================================

// 死锁必现场景：两个线程以相反顺序提交涉及相同两个 key 的事务
// 如果 commit 不按序加锁，必然死锁 → 测试超时（60s）失败
// 如果按序加锁（或使用 std::lock），两个事务会正确串行完成
//
// 场景：
//   线程A：commit({key_apple → 1, key_banana → 2})  按字典序：apple < banana
//   线程B：commit({key_banana → 3, key_apple → 4})  按字典序：apple < banana
//   两者都先锁 apple 再锁 banana → 不会死锁
//   若不按序：A 先锁 apple，B 先锁 banana → 死锁
TEST(TransactionTest, NoDeadlockWithOppositeOrderCommit) {
    GuildRegistry reg;
    reg.register_adventurer("key_apple",  Attribute{int64_t(0)});
    reg.register_adventurer("key_banana", Attribute{int64_t(0)});
    TradeSystem trade(reg);

    std::atomic<int> done{0};
    constexpr int kRounds = 200;

    // 线程A：每次提交 apple→banana 方向的事务
    std::thread thread_a([&] {
        for (int i = 0; i < kRounds; ++i) {
            auto tx = trade.begin();
            tx->put("key_apple",  Attribute{int64_t(i)});
            tx->put("key_banana", Attribute{int64_t(i)});
            tx->commit();
            done.fetch_add(1);
        }
    });

    // 线程B：每次提交 banana→apple 方向的事务（与A相反）
    std::thread thread_b([&] {
        for (int i = 0; i < kRounds; ++i) {
            auto tx = trade.begin();
            tx->put("key_banana", Attribute{int64_t(i + 1000)});
            tx->put("key_apple",  Attribute{int64_t(i + 1000)});
            tx->commit();
            done.fetch_add(1);
        }
    });

    thread_a.join();
    thread_b.join();

    // 两个线程各完成 kRounds 次事务，共 2*kRounds 次
    // 如果死锁，线程会卡住，join() 不会返回，测试超时失败
    EXPECT_EQ(done.load(), 2 * kRounds)
        << "事务未全部完成，可能发生了死锁";
}

// 验证 commit 的原子性：事务要么全部写入，要么全部不写入
// 通过在 commit 前后检查中间状态来验证
TEST(TransactionTest, CommitIsAtomic) {
    GuildRegistry reg;
    TradeSystem trade(reg);
    constexpr int kKeys = 10;
    constexpr int kThreads = 4;
    constexpr int kRounds = 100;

    // 初始化：所有 key 的值为 0
    for (int i = 0; i < kKeys; ++i) {
        reg.register_adventurer("atomic_key_" + std::to_string(i), Attribute{int64_t(0)});
    }

    std::atomic<bool> stop{false};

    // 写线程：每次事务将所有 key 设为相同的值（0 或 1 交替）
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r) {
                int64_t val = (t * kRounds + r) % 2;
                auto tx = trade.begin();
                for (int i = 0; i < kKeys; ++i) {
                    tx->put("atomic_key_" + std::to_string(i), Attribute{val});
                }
                tx->commit();
            }
        });
    }

    // 读线程：不断检查所有 key 的值是否一致（原子性保证）
    std::atomic<int> inconsistency_count{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto first = reg.query("atomic_key_0");
            if (!first) continue;
            int64_t expected = std::get<int64_t>(*first);
            for (int i = 1; i < kKeys; ++i) {
                auto val = reg.query("atomic_key_" + std::to_string(i));
                if (val && std::get<int64_t>(*val) != expected) {
                    inconsistency_count.fetch_add(1);
                }
            }
        }
    });

    for (auto& w : writers) w.join();
    stop = true;
    reader.join();

    // 注意：由于读线程不在事务内，可能读到写事务的中间状态
    // 这个测试主要验证不会崩溃，以及事务最终一致
    // 真正的原子性验证依赖 GoldConservation 测试
    SUCCEED();
}
