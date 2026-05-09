#include "expedition.h"

namespace guild {
    // ============================================================
    // ExpeditionClient
    // ============================================================

    std::future<std::optional<Attribute>> ExpeditionClient::async_query(const std::string &key) {
        return pool_.dispatch([this, key]() {
            return registry_.query(key);
        });
    }

    std::future<void> ExpeditionClient::async_register(const std::string &key, Attribute value,
                                                       std::chrono::seconds ttl) {
        return pool_.dispatch([this, key, value, ttl]() {
            registry_.register_adventurer(key, value, ttl);
        });
    }

    std::future<bool> ExpeditionClient::async_dismiss(const std::string &key) {
        return pool_.dispatch([this, key]() {
            return registry_.dismiss(key);
        });
    }

    void ExpeditionClient::async_query_with_callback(
        const std::string &key,
        std::function<void(std::optional<Attribute>)> callback) {
        pool_.dispatch([this, key, callback]() {
            auto result = registry_.query(key);
            callback(result);
        });
    }

    // ============================================================
    // BatchDispatcher
    // ============================================================

    std::vector<QuestResult> BatchDispatcher::launch(const std::vector<Quest> &quests) {
        // 收集所有任务的 future
        std::vector<std::future<QuestResult>> futures;
        futures.reserve(quests.size());

        for (const auto &quest : quests) {
            futures.push_back(pool_.dispatch([this, quest]() {
                switch (quest.type) {
                    case QuestType::kQuery: {
                        auto result = registry_.query(quest.key);
                        if (result) {
                            return QuestResult::ok(*result);
                        } else {
                            return QuestResult::not_found();
                        }
                    }
                    case QuestType::kRegister: {
                        if (quest.value) {
                            registry_.register_adventurer(quest.key, *quest.value, quest.ttl);
                            return QuestResult::ok();
                        } else {
                            return QuestResult{Status::kError, std::nullopt};
                        }
                    }
                    case QuestType::kRemove: {
                        bool success = registry_.dismiss(quest.key);
                        if (success) {
                            return QuestResult::ok();
                        } else {
                            return QuestResult::not_found();
                        }
                    }
                    default:
                        return QuestResult{Status::kError, std::nullopt};
                }
            }));
        }

        // 等待所有任务完成并收集结果
        std::vector<QuestResult> results;
        results.reserve(quests.size());
        for (auto &future : futures) {
            results.push_back(future.get());
        }

        return results;
    }
} // namespace guild