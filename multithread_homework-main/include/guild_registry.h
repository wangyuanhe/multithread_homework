#pragma once

/**
 * @file guild_registry.h
 * @brief Phase 2 — 冒险者档案馆（分片并发 KV 存储）
 *
 * 【背景】
 * 档案馆存放着所有冒险者的属性档案。为了支持大量冒险者同时查阅和修改，
 * 档案馆按冒险者 ID 的哈希值分成 N 个区域（Shard）。
 *
 * 档案馆规则：
 *   - 多名冒险者可以同时查阅同一区域的档案（共享读锁）
 *   - 修改某区域档案时，需要独占该区域（独占写锁）
 *   - 不同区域之间互不干扰，可以完全并行
 *
 * 【学习目标】
 * - std::shared_mutex（读写锁）
 * - std::shared_lock（共享读锁，允许多个线程同时持有）
 * - std::unique_lock（独占写锁，同一时刻只有一个线程持有）
 * - std::variant + std::visit（多类型值的存储和访问）
 * - std::atomic（无锁原子操作，用于统计计数）
 * - 结构化绑定 auto [key, value] = ...
 * - std::chrono（时间处理，用于 TTL 过期）
 * - constexpr（编译期常量）
 * - if constexpr（编译期条件分支）
 *
 * 【实现要求】
 * 完成 Shard 和 GuildRegistry 类中所有标注 TODO 的方法。
 */

#include "common.h"
#include <unordered_map>
#include <map>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <string>
#include <memory>
#include <chrono>

namespace guild {

    using namespace std::chrono_literals;

    // ============================================================
    // 档案馆统计信息（水晶球）
    // ============================================================

    /**
     * @brief 公会统计水晶球
     *
     * 使用 std::atomic 实现无锁统计，多线程可以安全地同时更新计数器，
     * 无需加锁，性能极高。
     */
    struct StatsOrb {
        std::atomic<uint64_t> query_count{0}; ///< 查询次数
        std::atomic<uint64_t> register_count{0}; ///< 注册/更新次数
        std::atomic<uint64_t> remove_count{0}; ///< 删除次数
        std::atomic<uint64_t> hit_count{0}; ///< 查询命中次数（找到了）
        std::atomic<uint64_t> miss_count{0}; ///< 查询未命中次数（没找到）
        std::atomic<uint64_t> expired_count{0}; ///< 过期清理次数

        /// 打印统计信息
        void print() const;
    };

    // ============================================================
    // 单个档案区域（Shard）
    // ============================================================

    /**
     * @brief 档案馆的一个分区
     *
     * 每个 Shard 管理一部分冒险者的档案，拥有独立的读写锁。
     * 不同 Shard 之间完全独立，可以并行操作。
     */
    class Shard {
    public:
        /**
         * @brief 查询冒险者档案
         *
         * 使用共享读锁（std::shared_lock），允许多个线程同时查询。
         *
         * @param key 冒险者唯一标识
         * @return 找到则返回属性值，未找到或已过期返回 std::nullopt
         *
         */
        std::optional<Attribute> get(const std::string &key) const;

        /**
         * @brief 注册或更新冒险者档案
         *
         * 使用独占写锁（std::unique_lock），同一时刻只有一个线程可以写入。
         *
         * @param key   冒险者唯一标识
         * @param value 属性值
         * @param ttl   有效期（0 表示永不过期）
         *
         */
        void put(const std::string &key, Attribute value,
                 std::chrono::seconds ttl = 0s);

        /**
         * @brief 删除冒险者档案
         *
         * @return true 表示成功删除，false 表示档案不存在
         *
         */
        bool remove(const std::string &key);

        /**
         * @brief 清理所有过期档案（由守护精灵调用）
         *
         * @return 清理的条目数量
         *
         */
        size_t evict_expired();

        /// 返回该分区的档案数量（含过期未清理的）
        size_t size() const;

        /// 返回该分区所有 key 的快照（用于遍历）
        std::vector<std::string> keys() const;

        // ---- 以下为加分项2（细粒度锁 + OCC）可选接口 ----
        // 基础实现不需要这些方法；若要实现 atomic_write_occ，需要补全它们。

        /**
         * @brief [加分项] 查询并返回版本号（用于 OCC 冲突检测）
         *
         * 与 get() 相同，但同时返回当前版本号。
         * 版本号在每次写入时递增，用于检测"读取后被他人修改"的情况。
         *
         * @return {属性值, 版本号}；key 不存在时返回 {nullopt, 0}
         */
        std::pair<std::optional<Attribute>, uint64_t> get_versioned(const std::string &key) const;

        /// [加分项] 获取内部互斥锁（供 GuildRegistry::atomic_write_occ 持锁后调用）
        std::shared_mutex &mutex() { return mutex_; }

        /**
         * @brief [加分项] 不加锁的写入（调用方必须已持有该 Shard 的 unique_lock）
         *
         * 注意：只更新 value 字段，然后将 version 递增。
         * 不要用 `data_[key] = Record{value}` 覆盖整个 Record，否则会重置 version！
         */
        void put_unlocked(const std::string &key, Attribute value);

        /// [加分项] 不加锁的删除（调用方必须已持有该 Shard 的 unique_lock）
        bool remove_unlocked(const std::string &key);

        friend class GuildRegistry; // 允许 GuildRegistry 访问 data_（用于 OCC 版本校验）

    private:
        // TODO: 思考为什么用 mutable std::shared_mutex 而不是普通 mutex？
        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, Record> data_;
    };

    // ============================================================
    // 冒险者档案馆（分片 KV 存储）
    // ============================================================

    /**
     * @brief 冒险者档案馆
     *
     * 内部将所有冒险者按 key 的哈希值分配到不同的 Shard，
     * 从而降低锁竞争，提高并发性能。
     *
     * 使用示例：
     * @code
     * GuildRegistry registry(16);  // 16个分区
     *
     * registry.register_adventurer("hero_001", Attribute{int64_t(9999)});
     * auto attr = registry.query("hero_001");
     * if (attr) {
     *     // 使用 std::visit 访问 variant 值
     *     std::visit([](auto&& v) { std::cout << v; }, *attr);
     * }
     * @endcode
     */
    class GuildRegistry {
    public:
        /**
         * @brief 构造档案馆
         * @param shard_count 分区数量，建议为 2 的幂次（默认 kDefaultShardCount）
         */
        explicit GuildRegistry(size_t shard_count = kDefaultShardCount);

        // ---- 基本 CRUD 操作 ----

        /**
         * @brief 查询冒险者属性
         */
        std::optional<Attribute> query(const std::string &key);

        /**
         * @brief 注册或更新冒险者档案
         */
        void register_adventurer(const std::string &key, Attribute value,
                                 std::chrono::seconds ttl = 0s);

        /**
         * @brief 删除冒险者档案
         *
         * @return true 表示成功删除
         */
        bool dismiss(const std::string &key);

        // ---- 批量操作 ----

        /// 清理所有分区的过期档案，返回总清理数量
        size_t evict_all_expired();

        /// 返回所有分区的档案总数
        size_t total_size() const;

        /// 返回所有 key 的快照
        std::vector<std::string> all_keys() const;

        // ---- 以下为加分项2（细粒度锁 + OCC）可选接口 ----
        // 基础实现不需要这些方法；若要实现 shard 级别细粒度锁，需要补全它们。

        /**
         * @brief [加分项] 查询并返回版本号（用于 OCC 事务）
         *
         * 包装 Shard::get_versioned，同时更新统计计数器。
         */
        std::pair<std::optional<Attribute>, uint64_t> query_versioned(const std::string &key);

        /**
         * @brief [加分项] 带 OCC 校验的原子写入
         *
         * 在 atomic_write 的基础上，持锁后先校验 read_set 中每个 key 的版本号：
         *   - 若版本号与读取时一致，说明无冲突，继续写入
         *   - 若版本号已变，说明有其他事务修改了该 key，返回 false（调用方重试）
         *
         * @param write_set 要写入的数据（nullopt 表示删除）
         * @param read_set  读取时记录的版本号（key -> version）
         * @return true 表示提交成功，false 表示版本冲突
         */
        bool atomic_write(const std::map<std::string, std::optional<Attribute>> &write_set,
                              const std::map<std::string, uint64_t> &read_set);

        // ---- 统计信息 ----

        /// 获取统计水晶球（只读引用）
        const StatsOrb &stats() const { return stats_; }

        /// 获取分区数量
        size_t shard_count() const { return shards_.size(); }

    private:
        /**
         * @brief 根据 key 找到对应的分区
         *
         */
        Shard &get_shard(const std::string &key);

        const Shard &get_shard(const std::string &key) const;

        std::vector<std::unique_ptr<Shard> > shards_;
        StatsOrb stats_;
    };
} // namespace guild
