#pragma once

/**
 * @file guild_daemon.h
 * @brief Phase 2 — 公会后台守护精灵（后台工作线程）
 *
 * 【背景】
 * 公会有两个默默工作的守护精灵：
 *   1. 过期清理精灵（ExpirySpirit）：定期巡逻档案馆，清理超时的委托档案
 *   2. 统计精灵（StatsDaemon）：定期读取水晶球数据，打印公会运营报告
 *
 * 守护精灵在后台独立线程中运行，不影响主业务。
 * 公会关闭时，守护精灵需要能够被优雅地唤醒并退出。
 *
 * 【学习目标】
 * - 后台线程的创建和管理
 * - std::condition_variable::wait_for（可中断的定时等待）
 * - 线程的优雅停止（stop token 模式）
 * - RAII 管理后台线程生命周期
 *
 * 【实现要求】
 * 完成 ExpirySpirit 和 StatsDaemon 类中所有标注 TODO 的方法。
 */

#include "common.h"
#include "guild_registry.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <iostream>

namespace guild {

    using namespace std::chrono_literals;

    // ============================================================
    // 过期清理精灵
    // ============================================================

    /**
     * @brief 后台过期清理精灵
     *
     * 每隔 patrol_interval 时间，扫描档案馆中所有过期条目并清理。
     * 支持随时停止（stop() 后立即唤醒，不需要等到下次巡逻时间）。
     *
     * 使用示例：
     * @code
     * GuildRegistry registry;
     * ExpirySpirit spirit(registry, 5s);
     * spirit.start();
     * // ... 运行一段时间 ...
     * spirit.stop();  // 立即停止，不需要等待下次巡逻
     * @endcode
     */
    class ExpirySpirit {
    public:
        /**
         * @brief 构造守护精灵
         * @param registry       要守护的档案馆
         * @param patrol_interval 巡逻间隔（默认 kDefaultPatrolInterval）
         */
        explicit ExpirySpirit(GuildRegistry &registry,
                              std::chrono::milliseconds patrol_interval = 30s);

        ExpirySpirit(const ExpirySpirit &) = delete;

        ExpirySpirit &operator=(const ExpirySpirit &) = delete;

        /**
         * @brief 启动守护精灵（在后台线程中运行）
         */
        void start();

        /**
         * @brief 停止守护精灵（立即唤醒并等待线程退出）
         */
        void stop();

        /// 析构函数：RAII 保证停止
        ~ExpirySpirit();

    private:
        /**
         * @brief 巡逻主循环
         *
         */
        void patrol_loop();

        GuildRegistry &registry_;
        std::chrono::milliseconds patrol_interval_;
        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<bool> stop_flag_{false};
    };

    // ============================================================
    // 统计精灵
    // ============================================================

    /**
     * @brief 后台统计精灵
     *
     * 每隔 report_interval 时间，读取水晶球数据并打印统计报告。
     * 注意：读取 std::atomic 计数器无需加锁。
     */
    class StatsDaemon {
    public:
        explicit StatsDaemon(const GuildRegistry &registry,
                             std::chrono::milliseconds report_interval = 10s);

        StatsDaemon(const StatsDaemon &) = delete;

        StatsDaemon &operator=(const StatsDaemon &) = delete;

        void start();

        void stop();

        ~StatsDaemon();

    private:
        /**
         * @brief 统计报告主循环
         */
        void report_loop();

        const GuildRegistry &registry_;
        std::chrono::milliseconds report_interval_;
        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<bool> stop_flag_{false};
    };
} // namespace guild
