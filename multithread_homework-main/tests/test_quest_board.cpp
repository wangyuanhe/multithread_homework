/**
 * @file test_quest_board.cpp
 * @brief Phase 1 — 任务告示板测试
 *
 * 测试覆盖：
 *   1. 基本功能：post/take 正确性
 *   2. 线程安全：多生产者多消费者
 *   3. 关闭行为：close_board 后 wait_and_take 正确退出
 *   4. try_take 非阻塞行为
 */

#include "quest_board.h"
#include <gtest/gtest.h>
#include <thread>
#include <future>
#include <vector>
#include <atomic>
#include <set>
#include <chrono>

using namespace guild;
using namespace std::chrono_literals;

// ============================================================
// 基本功能测试
// ============================================================

TEST(QuestBoardTest, BasicPostAndTake) {
    QuestBoard<int> board;
    board.post_quest(42);
    auto result = board.try_take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(QuestBoardTest, FIFOOrder) {
    QuestBoard<int> board;
    for (int i = 0; i < 5; ++i) board.post_quest(i);
    for (int i = 0; i < 5; ++i) {
        auto r = board.try_take();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, i);
    }
}

TEST(QuestBoardTest, TryTakeEmptyReturnsNullopt) {
    QuestBoard<int> board;
    EXPECT_FALSE(board.try_take().has_value());
}

TEST(QuestBoardTest, SizeTracking) {
    QuestBoard<std::string> board;
    EXPECT_EQ(board.size(), 0u);
    board.post_quest("任务A");
    board.post_quest("任务B");
    EXPECT_EQ(board.size(), 2u);
    board.try_take();
    EXPECT_EQ(board.size(), 1u);
}

// ============================================================
// 关闭行为测试
// ============================================================

TEST(QuestBoardTest, CloseWakesUpWaitingConsumer) {
    QuestBoard<int> board;
    std::atomic<bool> consumer_exited{false};

    std::thread consumer([&] {
        auto result = board.wait_and_take();
        EXPECT_FALSE(result.has_value());  // 关闭后返回 nullopt
        consumer_exited = true;
    });

    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(consumer_exited.load());  // 消费者还在等待

    board.close_board();
    consumer.join();
    EXPECT_TRUE(consumer_exited.load());
}

TEST(QuestBoardTest, CloseAfterPostDrainsQueue) {
    QuestBoard<int> board;
    board.post_quest(1);
    board.post_quest(2);
    board.close_board();

    // 关闭后仍可取出已有任务
    EXPECT_TRUE(board.try_take().has_value());
    EXPECT_TRUE(board.try_take().has_value());
    EXPECT_FALSE(board.try_take().has_value());
}

TEST(QuestBoardTest, PostAfterCloseThrows) {
    QuestBoard<int> board;
    board.close_board();
    EXPECT_THROW(board.post_quest(1), std::runtime_error);
}

// ============================================================
// 并发测试：多生产者多消费者
// ============================================================

TEST(QuestBoardTest, MultiProducerMultiConsumer) {
    QuestBoard<int> board;
    constexpr int kTotal = 10000;
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;

    std::atomic<int> consumed{0};
    std::atomic<int> produced{0};

    // 消费者线程
    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            while (true) {
                auto item = board.wait_and_take();
                if (!item.has_value()) break;
                consumed.fetch_add(1);
            }
        });
    }

    // 生产者线程
    std::vector<std::thread> producers;
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&, i] {
            int per = kTotal / kProducers;
            for (int j = 0; j < per; ++j) {
                board.post_quest(i * per + j);
                produced.fetch_add(1);
            }
        });
    }

    for (auto& p : producers) p.join();
    board.close_board();
    for (auto& c : consumers) c.join();

    EXPECT_EQ(consumed.load(), kTotal);
}

TEST(QuestBoardTest, NoItemLostOrDuplicated) {
    QuestBoard<int> board;
    constexpr int kTotal = 1000;

    std::vector<int> received;
    std::mutex recv_mutex;

    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i) board.post_quest(i);
        board.close_board();
    });

    std::vector<std::thread> consumers;
    for (int i = 0; i < 3; ++i) {
        consumers.emplace_back([&] {
            while (true) {
                auto item = board.wait_and_take();
                if (!item.has_value()) break;
                std::lock_guard lock(recv_mutex);
                received.push_back(*item);
            }
        });
    }

    producer.join();
    for (auto& c : consumers) c.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kTotal));
    std::set<int> unique_items(received.begin(), received.end());
    EXPECT_EQ(unique_items.size(), static_cast<size_t>(kTotal));  // 无重复
}

// ============================================================
// 反偷懒测试
// ============================================================

// 验证 wait_and_take 阻塞期间不持有锁
// 如果用忙等且持锁实现，生产者的 post_quest 会永远拿不到锁 → 死锁 → 超时失败
// 测试超时由 CMakeLists.txt 中的 TIMEOUT 60 保证
TEST(QuestBoardTest, WaitAndTakeReleasesLockWhileWaiting) {
    QuestBoard<int> board;

    // 消费者先启动，进入 wait_and_take 阻塞状态
    std::atomic<bool> consumer_waiting{false};
    std::thread consumer([&] {
        consumer_waiting = true;
        auto result = board.wait_and_take();  // 阻塞在这里
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(*result, 777);
    });

    // 等消费者进入等待状态
    while (!consumer_waiting.load()) std::this_thread::yield();
    std::this_thread::sleep_for(20ms);  // 确保消费者已进入 wait

    // 此时消费者应该已释放锁，生产者必须能立即 post（不能被锁住）
    auto start = std::chrono::steady_clock::now();
    board.post_quest(777);
    auto elapsed = std::chrono::steady_clock::now() - start;

    consumer.join();

    // post_quest 应该几乎立即完成（< 100ms），如果消费者持锁忙等则会超时
    EXPECT_LT(elapsed, 100ms) << "post_quest 耗时过长，wait_and_take 可能在持锁忙等";
}

// 验证多个消费者可以同时阻塞等待（不互相持锁阻塞）
TEST(QuestBoardTest, MultipleConsumersCanWaitSimultaneously) {
    QuestBoard<int> board;
    constexpr int kConsumers = 4;
    std::atomic<int> waiting_count{0};
    std::vector<std::future<std::optional<int>>> futures;

    for (int i = 0; i < kConsumers; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            waiting_count.fetch_add(1);
            return board.wait_and_take();
        }));
    }

    // 等所有消费者都进入等待
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (waiting_count.load() < kConsumers
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    // 如果消费者互相持锁阻塞，waiting_count 不会达到 kConsumers
    EXPECT_EQ(waiting_count.load(), kConsumers)
        << "消费者互相阻塞，wait_and_take 可能在持锁忙等";

    // 唤醒所有消费者
    for (int i = 0; i < kConsumers; ++i) board.post_quest(i);
    for (auto& f : futures) f.get();
}

// ============================================================
// Move-only 类型测试
// 验证队列内部使用 std::move 取出元素，而非拷贝
// 若实现中使用了拷贝（如 T task = queue_.front()），此测试将编译失败
// ============================================================

// 不可拷贝、只能移动的任务类型（模拟 std::unique_ptr 语义）
struct MoveOnlyQuest {
    int id;
    explicit MoveOnlyQuest(int id) : id(id) {}
    MoveOnlyQuest(MoveOnlyQuest &&) = default;
    MoveOnlyQuest &operator=(MoveOnlyQuest &&) = default;
    MoveOnlyQuest(const MoveOnlyQuest &) = delete;
    MoveOnlyQuest &operator=(const MoveOnlyQuest &) = delete;
};

TEST(QuestBoardTest, MoveOnlyTypeCanBePosted) {
    QuestBoard<MoveOnlyQuest> board;

    // post_quest 必须用移动语义接收参数
    board.post_quest(MoveOnlyQuest{42});

    auto result = board.try_take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, 42);
}

TEST(QuestBoardTest, MoveOnlyTypeWaitAndTake) {
    QuestBoard<MoveOnlyQuest> board;

    std::thread producer([&] {
        std::this_thread::sleep_for(10ms);
        board.post_quest(MoveOnlyQuest{99});
    });

    auto result = board.wait_and_take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, 99);

    producer.join();
}

TEST(QuestBoardTest, MoveOnlyTypeMultipleItems) {
    QuestBoard<MoveOnlyQuest> board;
    constexpr int kCount = 5;

    for (int i = 0; i < kCount; ++i)
        board.post_quest(MoveOnlyQuest{i});
    board.close_board();

    std::vector<int> ids;
    while (true) {
        auto item = board.wait_and_take();
        if (!item.has_value()) break;
        ids.push_back(item->id);
    }

    ASSERT_EQ(ids.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i)
        EXPECT_EQ(ids[i], i);  // FIFO 顺序
}
