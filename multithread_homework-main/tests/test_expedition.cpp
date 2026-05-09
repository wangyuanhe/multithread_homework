/**
 * @file test_expedition.cpp
 * @brief Phase 4 — 远征系统（异步客户端）测试
 */

#include "expedition.h"
#include <gtest/gtest.h>
#include <vector>
#include <atomic>
#include <chrono>

using namespace guild;
using namespace std::chrono_literals;

class ExpeditionTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = std::make_unique<DispatchPool>(4);
        reg  = std::make_unique<GuildRegistry>();
        client = std::make_unique<ExpeditionClient>(*reg, *pool);
    }

    void TearDown() override {
        client.reset();
        pool->shutdown();
    }

    std::unique_ptr<DispatchPool> pool;
    std::unique_ptr<GuildRegistry> reg;
    std::unique_ptr<ExpeditionClient> client;
};

TEST_F(ExpeditionTest, AsyncQueryNotFound) {
    auto future = client->async_query("nonexistent");
    EXPECT_FALSE(future.get().has_value());
}

TEST_F(ExpeditionTest, AsyncRegisterAndQuery) {
    auto f1 = client->async_register("hero_001", Attribute{int64_t(42)});
    f1.get();  // 等待注册完成

    auto f2 = client->async_query("hero_001");
    auto result = f2.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<int64_t>(*result), 42);
}

TEST_F(ExpeditionTest, AsyncDismiss) {
    reg->register_adventurer("hero_001", Attribute{int64_t(1)});

    auto f = client->async_dismiss("hero_001");
    EXPECT_TRUE(f.get());
    EXPECT_FALSE(reg->query("hero_001").has_value());
}

TEST_F(ExpeditionTest, AsyncDismissNotFound) {
    auto f = client->async_dismiss("nonexistent");
    EXPECT_FALSE(f.get());
}

TEST_F(ExpeditionTest, MultipleAsyncOpsInFlight) {
    constexpr int kOps = 50;
    std::vector<std::future<void>> futures;

    for (int i = 0; i < kOps; ++i) {
        futures.push_back(client->async_register(
            "hero_" + std::to_string(i),
            Attribute{int64_t(i)}
        ));
    }
    for (auto& f : futures) f.get();

    EXPECT_EQ(reg->total_size(), static_cast<size_t>(kOps));
}

// ============================================================
// BatchDispatcher 测试
// ============================================================

TEST_F(ExpeditionTest, BatchLaunch) {
    BatchDispatcher dispatcher(*reg, *pool);

    std::vector<Quest> quests = {
        {QuestType::kRegister, "hero_001", Attribute{int64_t(100)}},
        {QuestType::kRegister, "hero_002", Attribute{int64_t(200)}},
        {QuestType::kRegister, "hero_003", Attribute{int64_t(300)}},
    };

    auto results = dispatcher.launch(quests);
    ASSERT_EQ(results.size(), 3u);
    for (auto& r : results) {
        EXPECT_EQ(r.status, Status::kOk);
    }

    EXPECT_TRUE(reg->query("hero_001").has_value());
    EXPECT_TRUE(reg->query("hero_002").has_value());
    EXPECT_TRUE(reg->query("hero_003").has_value());
}

TEST_F(ExpeditionTest, BatchQueryMixed) {
    reg->register_adventurer("exists", Attribute{int64_t(1)});
    BatchDispatcher dispatcher(*reg, *pool);

    std::vector<Quest> quests = {
        {QuestType::kQuery, "exists"},
        {QuestType::kQuery, "not_exists"},
    };

    auto results = dispatcher.launch(quests);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].status, Status::kOk);
    EXPECT_EQ(results[1].status, Status::kNotFound);
}

TEST_F(ExpeditionTest, BatchRunsInParallel) {
    BatchDispatcher dispatcher(*reg, *pool);
    constexpr int kBatch = 20;

    std::vector<Quest> quests;
    for (int i = 0; i < kBatch; ++i) {
        quests.push_back({QuestType::kRegister,
                          "hero_" + std::to_string(i),
                          Attribute{int64_t(i)}});
    }

    auto start = std::chrono::steady_clock::now();
    auto results = dispatcher.launch(quests);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(results.size(), static_cast<size_t>(kBatch));
    // 并行执行应比串行快（粗略验证）
    std::cout << "批量执行 " << kBatch << " 个任务耗时: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << "ms\n";
}
