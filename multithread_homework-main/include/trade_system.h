#pragma once

/**
 * @file trade_system.h
 * @brief Phase 3 — 装备交易所（事务系统 + 死锁避免）
 *
 * 【背景】
 * 冒险者之间可以在交易所交换装备和属性。交易必须是原子的：
 *   - 要么双方都完成交换（commit）
 *   - 要么交易取消，双方都不变（rollback）
 *
 * 【死锁问题】
 * 假设勇者A想用圣剑换魔法师B的法杖：
 *   - A 先锁住"圣剑"档案，等待锁住"法杖"档案
 *   - B 先锁住"法杖"档案，等待锁住"圣剑"档案
 *   → 两人互相等待，永远无法完成 → 死锁！
 *
 * 【解决方案：按 key 排序加锁】
 * 公会规定：所有交易必须按装备名称的字典序依次锁定。
 * 这样 A 和 B 都会先尝试锁"圣剑"，后锁"法杖"，不会出现循环等待。
 *
 * 【学习目标】
 * - std::lock()：同时获取多把锁，内部使用死锁避免算法
 * - std::adopt_lock：告诉 lock_guard "锁已经被锁上了，只需要管理释放"
 * - std::any + std::any_cast：类型擦除的通用容器
 * - 移动语义：write-set 的高效转移
 * - RAII：Transaction 析构时自动 rollback（如果未 commit）
 *
 * 【实现要求】
 * 完成 Transaction 和 TradeSystem 类中所有标注 TODO 的方法。
 */

#include "common.h"
#include "guild_registry.h"
#include <map>
#include <unordered_map>
#include <any>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <mutex>

namespace guild {
    // ============================================================
    // 事务（一次交易）
    // ============================================================

    /**
     * @brief 一次装备交易事务
     *
     * 事务内的所有操作先记录在本地 write-set 中，
     * commit() 时才真正写入档案馆。
     *
     * 使用示例：
     * @code
     * TradeSystem trade(registry);
     *
     * auto tx = trade.begin();
     * auto sword = tx->get("hero_001_sword");   // 读取（支持 read-your-writes）
     * tx->put("hero_001_sword", new_value);      // 写入（先记录到 write-set）
     * tx->remove("hero_002_staff");
     *
     * if (tx->commit()) {
     *     // 交易成功
     * } else {
     *     // 冲突，需要重试
     * }
     * // 如果不调用 commit()，析构时自动 rollback
     * @endcode
     */
    class Transaction {
    public:
        explicit Transaction(GuildRegistry &registry, std::mutex &tx_mutex);

        // 事务不可拷贝（含有锁的语义）
        Transaction(const Transaction &) = delete;

        Transaction &operator=(const Transaction &) = delete;

        /**
         * @brief 在事务中查询属性（支持 read-your-writes）
         *
         * 先检查本地 write-set，再查询档案馆。
         * 这样可以读到自己在本次事务中的未提交写入。
         *
         * @return 属性值，或 std::nullopt（不存在或已被本事务删除）
         */
        std::optional<Attribute> get(const std::string &key);

        /**
         * @brief 在事务中写入属性（记录到 write-set，不立即写入档案馆）
         */
        void put(const std::string &key, Attribute value);

        /**
         * @brief 在事务中删除属性（记录到 write-set，用 nullopt 标记删除）
         */
        void remove(const std::string &key);

        /**
         * @brief 提交事务（将 write-set 原子地写入档案馆）
         *
         * 这是最关键的方法，需要正确处理死锁避免。
         *
         * @return true 表示提交成功，false 表示冲突（需要重试）
         */
        bool commit();

        /**
         * @brief 回滚事务（丢弃 write-set，不写入任何内容）
         */
        void rollback();

        /**
         * @brief 析构函数：RAII 保证未提交的事务自动回滚
         */
        ~Transaction();

        // ---- 事务上下文（使用 std::any 存储任意类型的附加信息）----

        /**
         * @brief 设置事务附加信息（如交易备注、时间戳等）
         *
         * @tparam T 任意可拷贝类型
         * @param value 要存储的值
         *
         * 使用示例：
         * @code
         * tx->set_context(std::string("购买圣剑"));
         * tx->set_context(42);  // 覆盖之前的值
         * @endcode
         */
        template<typename T>
        void set_context(T value) {
            context_ = std::any(std::move(value));
        }

        /**
         * @brief 获取事务附加信息
         *
         * @tparam T 期望的类型
         * @return 若类型匹配则返回值，否则返回 std::nullopt
         */
        template<typename T>
        std::optional<T> get_context() const {
            auto *p = std::any_cast<T>(&context_);
            if (p) {
                return *p;
            }
            return std::nullopt;
        }

    private:
        GuildRegistry &registry_;
        std::mutex &tx_mutex_;

        // write-set：key -> 新值（nullopt 表示删除）
        // 使用 std::map（有序）方便按 key 排序加锁
        std::map<std::string, std::optional<Attribute> > write_set_;

        // [加分项] read-set：记录读取时的版本号，用于 OCC 冲突检测
        // Transaction::get() 调用 query_versioned() 时，将版本号存入此 map。
        // commit() 调用 atomic_write_occ() 时，将此 map 传入用于校验。
        std::map<std::string, uint64_t> read_set_;

        std::any context_; ///< 事务附加信息（std::any 可存储任意类型）
        bool committed_{false};
        bool rolled_back_{false};
    };

    // ============================================================
    // 交易系统
    // ============================================================

    /**
     * @brief 装备交易系统
     *
     * 工厂类，负责创建事务对象。
     */
    class TradeSystem {
    public:
        explicit TradeSystem(GuildRegistry &registry) : registry_(registry) {
        }

        /**
         * @brief 开始一笔新交易
         * @return 事务对象（unique_ptr，析构时自动 rollback）
         */
        std::unique_ptr<Transaction> begin() {
            return std::make_unique<Transaction>(registry_, tx_mutex_);
        }

    private:
        GuildRegistry &registry_;
        std::mutex tx_mutex_; ///< 全局提交锁（基础实现），保证 commit 原子性
    };
} // namespace guild
