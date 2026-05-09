#pragma once

/**
 * @file common.h
 * @brief 异世界冒险者公会管理系统 — 公共类型定义
 *
 * 定义了整个系统共用的数据类型、枚举和编译期常量。
 */

#include <variant>
#include <string>
#include <vector>
#include <cstddef>
#include <optional>
#include <chrono>
#include <functional>
#include <stdexcept>

namespace guild {

    using namespace std::chrono_literals;

    // ============================================================
    // 冒险者属性类型
    // ============================================================

    /**
     * @brief 冒险者档案中可存储的属性值类型
     *
     * 使用 std::variant 表示多态值，避免继承体系的开销：
     *   - int64_t  : 战斗力、金币数、等级等整数属性
     *   - double   : 幸运值、暴击率等浮点属性
     *   - string   : 称号、种族名、技能名等文本属性
     *   - vector<byte> : 技能数据、装备序列化数据等二进制属性
     */
    using Attribute = std::variant<int64_t, double, std::string, std::vector<std::byte> >;

    // ============================================================
    // 操作状态码
    // ============================================================

    enum class Status {
        kOk, ///< 操作成功
        kNotFound, ///< 冒险者/物品不存在
        kError, ///< 内部错误
        kTimeout, ///< 操作超时（任务限时已过）
        kConflict, ///< 事务冲突（需要重试）
        kShutdown, ///< 系统已关闭
    };

    // ============================================================
    // 操作结果
    // ============================================================

    struct QuestResult {
        Status status;
        std::optional<Attribute> value; ///< 查询成功时携带的值

        /// 便捷构造：成功结果
        static QuestResult ok(Attribute val) {
            return {Status::kOk, std::move(val)};
        }

        /// 便捷构造：未找到
        static QuestResult not_found() {
            return {Status::kNotFound, std::nullopt};
        }

        /// 便捷构造：成功（无返回值，如 put/del）
        static QuestResult ok() {
            return {Status::kOk, std::nullopt};
        }

        bool is_ok() const { return status == Status::kOk; }
    };

    // ============================================================
    // 冒险者档案条目（带任务限时 TTL）
    // ============================================================

    struct Record {
        Attribute value;
        /// 过期时间点；time_point::max() 表示永不过期
        std::chrono::steady_clock::time_point expire_at{
            std::chrono::steady_clock::time_point::max()
        };
        uint64_t version{0}; ///< 版本号，每次写入递增

        /// 判断该条目是否已过期
        bool is_expired() const {
            return expire_at != std::chrono::steady_clock::time_point::max()
                   && std::chrono::steady_clock::now() > expire_at;
        }
    };

    // ============================================================
    // 远征任务（用于批量派遣）
    // ============================================================

    enum class QuestType {
        kQuery, ///< 查询冒险者档案
        kRegister, ///< 注册/更新冒险者档案
        kRemove, ///< 删除冒险者档案
    };

    struct Quest {
        QuestType type;
        std::string key; ///< 冒险者唯一标识（如 "adventurer_42"）
        std::optional<Attribute> value{std::nullopt}; ///< 仅 kRegister 时有值
        std::chrono::seconds ttl{0}; ///< 0 表示永不过期
    };

    // ============================================================
    // 编译期常量
    // ============================================================

    /// 档案馆默认分区数（分片数越多，并发冲突越少）
    inline constexpr size_t kDefaultShardCount = 16;

    /// 任务默认限时（1小时后自动作废）
    inline constexpr auto kDefaultQuestTTL = 3600s;

    /// 派遣大厅默认服务窗口数（worker 线程数）
    inline constexpr size_t kDefaultWindowCount = 4;

    /// 守护精灵默认巡逻间隔（每隔多久扫描一次过期条目）
    inline constexpr auto kDefaultPatrolInterval = 30s;
} // namespace guild
