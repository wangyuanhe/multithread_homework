#pragma once

/**
 * @file expedition.h
 * @brief Phase 4 — 远征系统（异步客户端）
 *
 * 【背景】
 * 远征队在外执行任务时，经常需要远程查阅档案馆的资料——查询冒险者属性、
 * 注册新成员、注销离队者。这些请求通过"通信水晶"（std::future）发出后，
 * 远征队不必干等回复，可以继续行军、扎营、侦察，等真正需要结果时再查看水晶。
 *
 * 公会也可以同时派出多支远征队（BatchDispatcher），所有队伍并行执行各自的任务。
 *
 * 【核心思路】
 * ExpeditionClient 的本质是把同步的档案馆操作包装成异步接口：
 *   同步：registry_.query(key)          — 阻塞直到返回
 *   异步：pool_.dispatch([]{query(key)}) — 立即返回 future，查询在 worker 线程执行
 *
 * BatchDispatcher 的核心是并行：对每个任务提交 dispatch，收集所有 future，
 * 最后统一 .get() 等待全部完成。提交是串行的，执行是并行的。
 *
 * 【学习目标】
 * - std::future / std::promise（异步结果传递）
 * - std::invoke（统一的可调用对象调用方式，处理不同类型的任务）
 * - std::function（类型擦除的可调用对象）
 * - 回调模式（任务完成后自动触发，无需手动 .get()）
 *
 * 【实现要求】
 * 完成 ExpeditionClient 和 BatchDispatcher 类中所有标注 TODO 的方法。
 */

#include "common.h"
#include "guild_registry.h"
#include "dispatch_pool.h"
#include <future>
#include <functional>
#include <vector>
#include <string>

namespace guild {

    using namespace std::chrono_literals;

    /**
     * @brief 远征系统异步客户端
     *
     * 将档案馆的同步操作包装为异步接口，内部通过 DispatchPool 在 worker 线程执行。
     * 调用方提交请求后立即拿到 future，无需等待，可以继续处理其他事务。
     *
     * 使用示例：
     * @code
     * GuildRegistry registry;
     * DispatchPool pool(4);
     * ExpeditionClient client(registry, pool);
     *
     * // 远征队发出查询请求，立即返回 future，不阻塞
     * auto future = client.async_query("hero_001");
     *
     * // 继续处理其他事务（行军、扎营、侦察……）
     * do_other_work();
     *
     * // 需要结果时再查看通信水晶
     * auto result = future.get();
     * if (result) {
     *     // 处理查询结果
     * }
     * @endcode
     */
    class ExpeditionClient {
    public:
        ExpeditionClient(GuildRegistry &registry, DispatchPool &pool)
            : registry_(registry), pool_(pool) {
        }

        /**
         * @brief 异步查询冒险者属性
         *
         * @param key 冒险者唯一标识
         * @return std::future<std::optional<Attribute>>
         *
         */
        std::future<std::optional<Attribute> > async_query(const std::string &key);

        /**
         * @brief 异步注册冒险者
         *
         * @return std::future<void>（等待完成，不需要返回值）
         *
         */
        std::future<void> async_register(const std::string &key, Attribute value,
                                         std::chrono::seconds ttl = 0s);

        /**
         * @brief 异步删除冒险者
         *
         * @return std::future<bool>
         */
        std::future<bool> async_dismiss(const std::string &key);

        /**
         * @brief 带回调的异步查询（加分项）
         *
         * 查询完成后自动调用 callback，无需手动 .get()。
         *
         * @param key      冒险者唯一标识
         * @param callback 回调函数，接受 std::optional<Attribute>
         *
         */
        void async_query_with_callback(
            const std::string &key,
            std::function<void(std::optional<Attribute>)> callback);

    private:
        GuildRegistry &registry_;
        DispatchPool &pool_;
    };

    // ============================================================
    // 批量派遣器
    // ============================================================

    /**
     * @brief 批量远征派遣器
     *
     * 一次性提交多个任务，并行执行，等待所有任务完成后返回结果列表。
     * 任务提交是串行的（逐个 dispatch），但执行是并行的（由线程池调度）。
     *
     * 使用示例：
     * @code
     * BatchDispatcher dispatcher(registry, pool);
     *
     * // 同时派出三支远征队，各自查阅不同档案
     * std::vector<Quest> quests = {
     *     {QuestType::kQuery,    "hero_001"},
     *     {QuestType::kRegister, "hero_002", Attribute{int64_t(100)}},
     *     {QuestType::kRemove,   "hero_003"},
     * };
     *
     * // 并行执行，等待全部完成
     * auto results = dispatcher.launch(quests);
     * // results[i] 对应 quests[i] 的执行结果
     * @endcode
     */
    class BatchDispatcher {
    public:
        BatchDispatcher(GuildRegistry &registry, DispatchPool &pool)
            : registry_(registry), pool_(pool) {
        }

        /**
         * @brief 并行执行一批远征任务，等待所有任务完成
         *
         * @param quests 任务列表
         * @return 每个任务的执行结果（顺序与输入对应）
         */
        std::vector<QuestResult> launch(const std::vector<Quest> &quests);

    private:
        GuildRegistry &registry_;
        DispatchPool &pool_;
    };
} // namespace guild
