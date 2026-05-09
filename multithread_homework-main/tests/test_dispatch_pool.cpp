/**
 * @file test_dispatch_pool.cpp
 * @brief Phase 1 — 派遣大厅（线程池）测试
 */

#include "dispatch_pool.h"
#include <gtest/gtest.h>
#include <atomic>
#include <vector>
#include <chrono>
#include <numeric>

using namespace guild;
using namespace std::chrono_literals;

TEST(DispatchPoolTest, BasicDispatchAndGet) {
    DispatchPool pool(2);
    auto future = pool.dispatch([] { return 42; });
    EXPECT_EQ(future.get(), 42);
}

TEST(DispatchPoolTest, DispatchWithArgs) {
    DispatchPool pool(2);
    auto future = pool.dispatch([](int a, int b) { return a + b; }, 10, 20);
    EXPECT_EQ(future.get(), 30);
}

TEST(DispatchPoolTest, DispatchReturnsVoid) {
    DispatchPool pool(2);
    std::atomic<bool> executed{false};
    auto future = pool.dispatch([&] { executed = true; });
    future.get();
    EXPECT_TRUE(executed.load());
}

TEST(DispatchPoolTest, MultipleTasksExecuted) {
    DispatchPool pool(4);
    constexpr int kTasks = 100;
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.dispatch([&] { counter.fetch_add(1); }));
    }
    for (auto& f : futures) f.get();

    EXPECT_EQ(counter.load(), kTasks);
}

TEST(DispatchPoolTest, TasksRunInParallel) {
    DispatchPool pool(4);
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    constexpr int kTasks = 8;
    std::vector<std::future<void>> futures;

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.dispatch([&] {
            int c = concurrent.fetch_add(1) + 1;
            // 更新最大并发数
            int expected = max_concurrent.load();
            while (c > expected && !max_concurrent.compare_exchange_weak(expected, c));
            std::this_thread::sleep_for(20ms);
            concurrent.fetch_sub(1);
        }));
    }
    for (auto& f : futures) f.get();

    // 4个 worker，应该有至少 2 个任务并发执行
    EXPECT_GE(max_concurrent.load(), 2);
}

TEST(DispatchPoolTest, ExceptionPropagatedViaFuture) {
    DispatchPool pool(2);
    auto future = pool.dispatch([]() -> int {
        throw std::runtime_error("任务失败了");
        return 0;
    });
    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST(DispatchPoolTest, ShutdownWaitsForPendingTasks) {
    DispatchPool pool(2);
    std::atomic<int> done{0};

    for (int i = 0; i < 10; ++i) {
        pool.dispatch([&] {
            std::this_thread::sleep_for(5ms);
            done.fetch_add(1);
        });
    }

    pool.shutdown();
    EXPECT_EQ(done.load(), 10);
}

TEST(DispatchPoolTest, DispatchAfterShutdownThrows) {
    DispatchPool pool(2);
    pool.shutdown();
    EXPECT_THROW(pool.dispatch([] { return 1; }), std::runtime_error);
}

TEST(DispatchPoolTest, DestructorShutdownsGracefully) {
    std::atomic<int> done{0};
    {
        DispatchPool pool(2);
        for (int i = 0; i < 5; ++i) {
            pool.dispatch([&] {
                std::this_thread::sleep_for(10ms);
                done.fetch_add(1);
            });
        }
        // pool 析构时应等待所有任务完成
    }
    EXPECT_EQ(done.load(), 5);
}

TEST(DispatchPoolTest, LambdaCapture) {
    DispatchPool pool(2);
    std::string msg = "冒险者公会";
    auto future = pool.dispatch([msg] { return msg + " 欢迎你！"; });
    EXPECT_EQ(future.get(), "冒险者公会 欢迎你！");
}

// ============================================================
// 反偷懒测试
// ============================================================

// 验证任务真正在独立线程中执行（不是在 dispatch 调用线程里同步执行）
// 如果同步执行：4个任务各睡 50ms，串行共需 200ms
// 如果并行执行：4个任务同时睡 50ms，共需约 50ms
TEST(DispatchPoolTest, TasksAreExecutedInWorkerThreads) {
    DispatchPool pool(4);
    constexpr int kTasks = 4;

    // 记录每个任务实际运行的线程 ID
    std::vector<std::thread::id> task_thread_ids(kTasks);
    std::thread::id dispatch_thread_id = std::this_thread::get_id();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.dispatch([&, i] {
            task_thread_ids[i] = std::this_thread::get_id();
        }));
    }
    for (auto& f : futures) f.get();

    // 每个任务都必须在非 dispatch 调用线程中执行
    for (int i = 0; i < kTasks; ++i) {
        EXPECT_NE(task_thread_ids[i], dispatch_thread_id)
            << "任务 " << i << " 在 dispatch 调用线程中同步执行了，应该在 worker 线程中执行";
    }
}

// 验证并行执行的时间收益
// 4个 worker，提交 4 个各睡 50ms 的任务
// 并行：约 50ms；串行：约 200ms
TEST(DispatchPoolTest, ParallelSpeedup) {
    DispatchPool pool(4);
    constexpr int kTasks = 4;
    constexpr auto kTaskDuration = 50ms;

    auto start = std::chrono::steady_clock::now();
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.dispatch([kTaskDuration] {
            std::this_thread::sleep_for(kTaskDuration);
        }));
    }
    for (auto& f : futures) f.get();
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 并行执行应在 150ms 内完成（串行需要 200ms）
    EXPECT_LT(elapsed, 150ms)
        << "4个任务各需 50ms，4个 worker 并行应在 ~50ms 完成，"
        << "实际耗时 " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << "ms，疑似串行执行";
}

// ============================================================
// Move-only callable 测试
// 验证 dispatch 能接受只能移动、不能拷贝的可调用对象
//
// 偷懒写法：[f, args...]() mutable { return f(args...); }
// 若 f 含有 unique_ptr 等不可拷贝成员，该 lambda 本身也不可拷贝，
// 无法直接传入 std::packaged_task 构造函数（要求可调用对象可拷贝）。
// 正确做法：用 std::shared_ptr<packaged_task> 包装，lambda 捕获 shared_ptr。
// ============================================================

// 不可拷贝的可调用对象（持有 unique_ptr）
struct MoveOnlyCallable {
    std::unique_ptr<int> value;
    explicit MoveOnlyCallable(int v) : value(std::make_unique<int>(v)) {}
    MoveOnlyCallable(MoveOnlyCallable &&) = default;
    MoveOnlyCallable(const MoveOnlyCallable &) = delete;
    int operator()() const { return *value; }
};

// 若实现中用 [f, args...] 按值捕获，MoveOnlyCallable 无法拷贝进 lambda，
// 导致 packaged_task 构造失败 → 编译错误
TEST(DispatchPoolTest, MoveOnlyCallableDispatch) {
    DispatchPool pool(2);
    auto future = pool.dispatch(MoveOnlyCallable{42});
    EXPECT_EQ(future.get(), 42);
}

// 验证捕获了 unique_ptr 的 lambda 也能被 dispatch（同样是 move-only callable）
TEST(DispatchPoolTest, LambdaCapturingUniquePtr) {
    DispatchPool pool(2);
    auto ptr = std::make_unique<int>(99);
    // 捕获 unique_ptr 的 lambda 不可拷贝
    auto future = pool.dispatch([p = std::move(ptr)]() { return *p; });
    EXPECT_EQ(future.get(), 99);
}

// ============================================================
// 成员函数指针测试
// 验证 dispatch 内部使用 std::invoke 而非直接调用 f(args...)
//
// 偷懒写法：std::move(f)(std::move(args)...)
// 成员函数指针不能用 () 直接调用：(&Foo::method)(obj, arg) 不合法
// 必须用 std::invoke(&Foo::method, obj, arg) 才能正确分派
// 若实现中未使用 std::invoke，此测试将编译失败
// ============================================================

struct Adventurer {
    int level;
    explicit Adventurer(int lv) : level(lv) {}
    int get_level() const { return level; }
    int power(int multiplier) const { return level * multiplier; }
};

TEST(DispatchPoolTest, MemberFunctionPointerDispatch) {
    DispatchPool pool(2);
    Adventurer hero{10};

    // 无参成员函数指针
    auto f1 = pool.dispatch(&Adventurer::get_level, hero);
    EXPECT_EQ(f1.get(), 10);

    // 带参成员函数指针
    auto f2 = pool.dispatch(&Adventurer::power, hero, 3);
    EXPECT_EQ(f2.get(), 30);
}


