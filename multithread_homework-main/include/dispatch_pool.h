#pragma once

/**
 * @file dispatch_pool.h
 * @brief Phase 1 — 公会派遣大厅（线程池）
 *
 * 【背景】
 * 公会大厅有多个服务窗口（worker 线程），每个窗口的工作人员不断从
 * 任务告示板（QuestBoard）取下委托并执行。公会长可以向大厅提交任意
 * 类型的任务，并拿到一张"通知书"（std::future），等任务完成后凭通知书
 * 取回结果。
 *
 * 【学习目标】
 * - std::thread 的创建、管理和 join
 * - std::packaged_task（将可调用对象包装为可获取返回值的任务）
 * - std::future / std::promise（异步结果传递）
 * - 模板函数 + 完美转发（接受任意可调用对象）
 * - std::invoke_result_t（推导函数返回类型）
 * - RAII（析构函数保证线程安全关闭）
 * - 移动语义（packaged_task 不可拷贝，只能移动）
 *
 * 【实现要求】
 * 完成 DispatchPool 类中所有标注 TODO 的方法。
 */

#include "common.h"
#include "quest_board.h"
#include <thread>
#include <future>
#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace guild {
    /**
     * @brief 公会派遣大厅（固定大小线程池）
     *
     * 使用示例：
     * @code
     * DispatchPool pool(4);  // 4个服务窗口
     *
     * // 提交一个计算任务，立即返回 future
     * auto future = pool.dispatch([](int x) { return x * x; }, 7);
     *
     * // 做其他事情...
     *
     * int result = future.get();  // 等待结果：49
     * @endcode
     */
    class DispatchPool {
    public:
        /**
         * @brief 开设派遣大厅，创建 num_windows 个服务窗口
         *
         * @param num_windows worker 线程数量，默认为 kDefaultWindowCount
         */
        explicit DispatchPool(size_t num_windows = kDefaultWindowCount);

        // 禁止拷贝和移动（含有线程，语义复杂）
        DispatchPool(const DispatchPool &) = delete;

        DispatchPool &operator=(const DispatchPool &) = delete;

        /**
         * @brief 向大厅提交一个任务，返回 future 以获取结果
         *
         * @tparam F  可调用对象类型（函数、lambda、函数对象等）
         * @tparam Args 参数类型包
         * @param f    要执行的任务
         * @param args 任务的参数
         * @return     std::future<返回值类型>，调用 .get() 可等待并获取结果
         *
         * 如果大厅已关闭，抛出 std::runtime_error。
         */
        template<typename F, typename... Args>
        auto dispatch(F &&f, Args &&... args)
            -> std::future<std::invoke_result_t<F, Args...> > {
            using R = std::invoke_result_t<F, Args...>;

            if (shutdown_.load()) {
                throw std::runtime_error("DispatchPool is closed");
            }

            auto bound_task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            auto task = std::make_shared<std::packaged_task<R()>>(std::move(bound_task));
            auto future = task->get_future();

            board_.post_quest([task]() { (*task)(); });

            return future;
        }
        /**
         * @brief 优雅关闭大厅：等待所有已提交任务完成后退出
         */
        void shutdown();

        /// 返回服务窗口数量（worker 线程数）
        size_t window_count() const noexcept { return workers_.size(); }

        /// 返回当前待处理任务数量（近似值）
        size_t pending_count() const noexcept { return board_.size(); }

        /**
         * @brief 析构函数：RAII 保证关闭
         *
         * 如果用户忘记调用 shutdown()，析构函数自动完成清理。
         */
        ~DispatchPool();

    private:
        /**
         * @brief worker 线程的主循环
         *
         * 每个 worker 线程不断从告示板取任务并执行，直到告示板关闭且为空。
         *
         */
        void worker_loop();

        QuestBoard<std::function<void()> > board_; // 任务告示板
        std::vector<std::thread> workers_; // worker 线程容器
        std::atomic<bool> shutdown_{false}; // 关闭标志
    };
} // namespace guild
