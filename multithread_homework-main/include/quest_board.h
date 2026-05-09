#pragma once

/**
 * @file quest_board.h
 * @brief Phase 1 — 公会任务告示板（线程安全队列）
 *
 * 【背景】
 * 公会大厅的告示板上张贴着各种委托任务。冒险者们（生产者线程）把新委托
 * 贴上去，服务窗口的工作人员（消费者线程）依次取下委托并处理。
 * 告示板必须支持多人同时操作，不能出现两人同时取到同一张委托的情况。
 *
 * 【学习目标】
 * - std::mutex + std::lock_guard（简单加锁）
 * - std::unique_lock（配合条件变量使用）
 * - std::condition_variable（等待/唤醒机制）
 * - std::optional<T>（表示"可能没有值"的返回类型）
 * - 模板类的编写
 * - RAII 资源管理
 *
 * 【实现要求】
 * 完成下方 QuestBoard<T> 类中所有标注 TODO 的方法。
 * 不得修改类的 public 接口（方法签名）。
 */

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <stdexcept>

namespace guild {
    /**
     * @brief 线程安全的任务告示板（有界/无界阻塞队列）
     *
     * @tparam T 委托任务的类型（需要可移动构造）
     *
     * 使用示例：
     * @code
     * QuestBoard<std::string> board;
     * board.post_quest("讨伐史莱姆 x10");
     *
     * // 另一个线程中：
     * auto quest = board.wait_and_take();  // 阻塞直到有任务
     * process(*quest);
     * @endcode
     */
    template<typename T>
    class QuestBoard {
    public:
        QuestBoard() = default;

        // 禁止拷贝（含有 mutex，不可拷贝）
        QuestBoard(const QuestBoard &) = delete;

        QuestBoard &operator=(const QuestBoard &) = delete;

        /**
         * @brief 张贴一个新委托（生产者调用）
         *
         * 将任务加入队列尾部，并唤醒一个等待中的消费者。
         * 如果告示板已关闭（close_board() 被调用），抛出 std::runtime_error。
         *
         * @param quest 要张贴的任务（使用移动语义避免拷贝）
         */
        void post_quest(T quest) {
            std::lock_guard lock(mutex_);
            if (closed_) {
                throw std::runtime_error("QuestBoard is closed");
            }
            queue_.push(std::move(quest));
            cv_.notify_one();
        }

        /**
         * @brief 尝试立即取走一个委托（非阻塞）
         *
         * 如果告示板上有任务，取走并返回；否则立即返回 std::nullopt。
         * 告示板关闭后仍可取走剩余任务，直到队列为空。
         *
         * @return 取到的任务，或 std::nullopt（队列为空时）
         */
        std::optional<T> try_take() {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                return std::nullopt;
            }
            T quest = std::move(queue_.front());
            queue_.pop();
            cv_.notify_one();
            return quest;
        }

        /**
         * @brief 等待并取走一个委托（阻塞）
         *
         * 如果队列为空，阻塞当前线程直到：
         *   - 有新任务被张贴（返回该任务），或
         *   - 告示板被关闭且队列为空（返回 std::nullopt）
         *
         * @return 取到的任务，或 std::nullopt（关闭且队列为空时）
         *
         * 注意：wait() 可能发生虚假唤醒（spurious wakeup），
         *       使用带 predicate 的 wait 重载可以自动处理这种情况。
         */
        std::optional<T> wait_and_take() {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() {return !queue_.empty() || closed_;});
            if (closed_ && queue_.empty()) {
                return std::nullopt;
            }
            T quest = std::move(queue_.front());
            queue_.pop();
            cv_.notify_one();
            return quest;
        }

        /**
         * @brief 关闭告示板（不再接受新委托）
         *
         * 设置关闭标志，并唤醒所有等待中的消费者线程，
         * 让它们能够检测到关闭状态并退出。
         */
        void close_board() {
            {
                std::lock_guard lock(mutex_);
                closed_ = true;
            }
            cv_.notify_all();
        }

        /// 返回当前队列中的任务数量（近似值，仅供参考）
        size_t size() const {
            std::lock_guard lock(mutex_);
            return queue_.size();
        }

        /// 告示板是否已关闭
        bool is_closed() const {
            std::lock_guard lock(mutex_);
            return closed_;
        }

    private:
        // TODO: 思考为什么 mutex_ 要声明为 mutable？
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<T> queue_;
        bool closed_{false};
    };
} // namespace guild
