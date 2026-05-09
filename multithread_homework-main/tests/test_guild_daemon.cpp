/**
 * @file test_guild_daemon.cpp
 * @brief Phase 2 — 公会守护精灵测试
 */

#include "guild_daemon.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <sstream>

using namespace guild;
using namespace std::chrono_literals;

TEST(ExpiritSpiritTest, StartsAndStops) {
    GuildRegistry reg;
    ExpirySpirit spirit(reg, 1s);
    spirit.start();
    std::this_thread::sleep_for(50ms);
    spirit.stop();
    SUCCEED();
}

TEST(ExpiritSpiritTest, CleansExpiredEntries) {
    GuildRegistry reg;
    reg.register_adventurer("temp1", Attribute{int64_t(1)}, 1s);
    reg.register_adventurer("temp2", Attribute{int64_t(2)}, 1s);
    reg.register_adventurer("perm",  Attribute{int64_t(3)});

    // 此时 3 条记录都在 map 中
    ASSERT_EQ(reg.total_size(), 3u);

    ExpirySpirit spirit(reg, 500ms);  // 每 500ms 巡逻一次
    spirit.start();

    std::this_thread::sleep_for(1600ms);  // 等待过期 + 至少一次巡逻完成
    spirit.stop();

    // 关键：用 total_size() 检测条目是否被物理删除
    // query() 内部的 is_expired() 会让过期条目返回 nullopt，
    // 即使 daemon 没有工作也能通过，所以不能用 query() 来验证清理效果。
    EXPECT_EQ(reg.total_size(), 1u) << "过期条目应被守护精灵从 map 中物理删除";

    // 永久条目仍然存在
    EXPECT_TRUE(reg.query("perm").has_value());

    // 统计计数器应记录清理数量
    EXPECT_GE(reg.stats().expired_count.load(), 2u)
        << "expired_count 应至少为 2（清理了 temp1 和 temp2）";
}

TEST(ExpiritSpiritTest, StopIsImmediate) {
    GuildRegistry reg;
    ExpirySpirit spirit(reg, 5s);  // 巡逻间隔很长
    spirit.start();

    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(50ms);
    spirit.stop();  // 应立即返回，不需要等 60 秒
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 1s);  // 应在 1 秒内返回
}

TEST(ExpiritSpiritTest, DestructorStops) {
    GuildRegistry reg;
    auto start = std::chrono::steady_clock::now();
    {
        ExpirySpirit spirit(reg, 60s);
        spirit.start();
        // 析构时应立即停止
    }
    std::this_thread::sleep_for(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 1s);
}

TEST(StatsDaemonTest, StartsAndStops) {
    GuildRegistry reg;
    StatsDaemon daemon(reg, 1s);
    daemon.start();
    std::this_thread::sleep_for(50ms);
    daemon.stop();
    SUCCEED();
}

// ============================================================
// 多重 start 测试
// 验证 start() 被调用多次时不会创建多个线程（线程泄漏）
//
// 若 start() 未检查 started_/joinable()，重复调用会覆盖 thread_ 成员，
// 导致第一个线程对象被析构时调用 std::terminate()（joinable 但未 join）。
// ============================================================

TEST(ExpiritSpiritTest, MultipleStartIsSafe) {
    GuildRegistry reg;
    ExpirySpirit spirit(reg, 1s);

    // 多次调用 start()，不应崩溃或创建多个线程
    spirit.start();
    spirit.start();
    spirit.start();

    std::this_thread::sleep_for(50ms);
    spirit.stop();
    SUCCEED();  // 能走到这里说明没有 terminate
}

TEST(StatsDaemonTest, MultipleStartIsSafe) {
    GuildRegistry reg;
    StatsDaemon daemon(reg, 1s);

    daemon.start();
    daemon.start();
    daemon.start();

    std::this_thread::sleep_for(50ms);
    daemon.stop();
    SUCCEED();
}

// ============================================================
// StatsDaemon 报告输出测试
// 验证 report_loop() 确实调用了 StatsOrb::print()，
// 通过捕获 stdout 检测是否有统计报告输出。
// ============================================================

TEST(StatsDaemonTest, PrintsReportPeriodically) {
    GuildRegistry reg;
    // 先产生一些统计数据
    reg.register_adventurer("hero_001", Attribute{int64_t(42)});
    reg.query("hero_001");
    reg.query("nonexistent");

    // 捕获 stdout
    std::streambuf* old_buf = std::cout.rdbuf();
    std::ostringstream captured;
    std::cout.rdbuf(captured.rdbuf());

    {
        StatsDaemon daemon(reg, 200ms);  // 每 200ms 打印一次
        daemon.start();
        std::this_thread::sleep_for(500ms);  // 等待至少一次报告
        daemon.stop();
    }

    // 恢复 stdout
    std::cout.rdbuf(old_buf);

    std::string output = captured.str();
    // 验证输出中包含统计报告的关键字
    EXPECT_NE(output.find("查询次数"), std::string::npos)
        << "StatsDaemon 应定期打印包含'查询次数'的统计报告\n"
        << "实际输出: " << output;
    EXPECT_NE(output.find("命中次数"), std::string::npos)
        << "统计报告应包含'命中次数'";
}

// ============================================================
// 大批量过期清理测试
// 验证守护精灵能正确清理大量过期条目，
// 通过 total_size() 确认条目被物理删除而非仅逻辑过期。
// ============================================================

TEST(ExpiritSpiritTest, BulkExpiryCleansPhysically) {
    GuildRegistry reg;

    // 注册 1000 个短 TTL 条目 + 100 个永久条目
    for (int i = 0; i < 1000; ++i) {
        reg.register_adventurer("temp_" + std::to_string(i),
                                Attribute{int64_t(i)}, 1s);
    }
    for (int i = 0; i < 100; ++i) {
        reg.register_adventurer("perm_" + std::to_string(i),
                                Attribute{int64_t(i)});
    }

    ASSERT_EQ(reg.total_size(), 1100u);

    ExpirySpirit spirit(reg, 300ms);
    spirit.start();

    std::this_thread::sleep_for(1800ms);  // 等待过期 + 多次巡逻
    spirit.stop();

    // 1000 个临时条目应被物理删除，只剩 100 个永久条目
    EXPECT_EQ(reg.total_size(), 100u)
        << "守护精灵应将所有过期条目从 map 中物理删除";

    // expired_count 应至少为 1000
    EXPECT_GE(reg.stats().expired_count.load(), 1000u);
}
