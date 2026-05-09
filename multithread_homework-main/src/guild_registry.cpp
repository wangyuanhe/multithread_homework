#include "guild_registry.h"
#include <iostream>
#include <mutex>
#include <algorithm>
#include <map>

namespace guild {
    // ============================================================
    // StatsOrb
    // ============================================================

    void StatsOrb::print() const {
        std::cout << "=== 公会统计水晶球 ===\n"
                << "  查询次数: " << query_count.load() << "\n"
                << "  命中次数: " << hit_count.load() << "\n"
                << "  未命中:   " << miss_count.load() << "\n"
                << "  注册次数: " << register_count.load() << "\n"
                << "  删除次数: " << remove_count.load() << "\n"
                << "  过期清理: " << expired_count.load() << "\n"
                << "=====================\n";
    }

    // ============================================================
    // Shard
    // ============================================================

    std::optional<Attribute> Shard::get(const std::string &key) const {
        std::shared_lock lock(mutex_);
        auto tmp = data_.find(key);
        if (tmp != data_.end() && !tmp->second.is_expired()) {
            return tmp->second.value;
        }
        return std::nullopt;    
    }

    void Shard::put(const std::string &key, Attribute value, std::chrono::seconds ttl) {
        Record record;
        record.value = value;
        if (ttl > std::chrono::seconds(0)) {
            record.expire_at = std::chrono::steady_clock::now() + ttl;
        }
        std::unique_lock lock(mutex_);
        data_[key] = record;
    }

    bool Shard::remove(const std::string &key) {
        std::unique_lock lock(mutex_);
        auto tmp = data_.find(key);
        if (tmp != data_.end()) {
            data_.erase(tmp);
            return true;
        }
        return false;
    }

    size_t Shard::evict_expired() {
        std::unique_lock lock(mutex_);
        size_t count = 0;
        for (auto i = data_.begin(); i != data_.end(); ) {
            if (i->second.expire_at <= std::chrono::steady_clock::now()) {
                i = data_.erase(i);
                ++count;
            } else {
                ++i;
            }
        }
        return count;
    }

    size_t Shard::size() const {
        std::shared_lock lock(mutex_);
        return data_.size();
    }

    std::vector<std::string> Shard::keys() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> result;
        result.reserve(data_.size());
        for (const auto &pair : data_) {
            result.push_back(pair.first);
        }
        return result;
    }

    // ============================================================
    // GuildRegistry
    // ============================================================

    GuildRegistry::GuildRegistry(size_t shard_count) {
        shards_.reserve(shard_count);
        for (size_t i = 0; i < shard_count; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    Shard &GuildRegistry::get_shard(const std::string &key) {
        return *shards_[std::hash<std::string>{}(key) % shards_.size()];
    }

    const Shard &GuildRegistry::get_shard(const std::string &key) const {
        return *shards_[std::hash<std::string>{}(key) % shards_.size()];
    }

    std::optional<Attribute> GuildRegistry::query(const std::string &key) {
        stats_.query_count.fetch_add(1);           // 总查询次数+1
        auto result = get_shard(key).get(key);
        if (result.has_value()) {
            stats_.hit_count.fetch_add(1);         // 命中+1
        } else {
            stats_.miss_count.fetch_add(1);        // 未命中+1
        }
        return result;
    }

    void GuildRegistry::register_adventurer(const std::string &key, Attribute value,
                                            std::chrono::seconds ttl) {
        get_shard(key).put(key, value, ttl);
        stats_.register_count.fetch_add(1);
    }

    bool GuildRegistry::dismiss(const std::string &key) {
        if (get_shard(key).remove(key)) {
            stats_.remove_count.fetch_add(1);
            return true;
        }
        return false;
    }

    size_t GuildRegistry::evict_all_expired() {
        size_t total = 0;
        for (auto &shard: shards_) {
            total += shard->evict_expired();
        }
        stats_.expired_count.fetch_add(total);
        return total;
    }

    size_t GuildRegistry::total_size() const {
        size_t total = 0;
        for (const auto &shard: shards_) {
            total += shard->size();
        }
        return total;
    }

    std::vector<std::string> GuildRegistry::all_keys() const {
        std::vector<std::string> result;
        for (const auto &shard: shards_) {
            auto keys = shard->keys();
            result.insert(result.end(), keys.begin(), keys.end());
        }
        return result;
    }

    // ============================================================
    // 加分项2：细粒度锁 + OCC（可选实现）
    // ============================================================

    std::pair<std::optional<Attribute>, uint64_t> Shard::get_versioned(
        const std::string &key) const {
        std::shared_lock lock(mutex_);
        auto tmp = data_.find(key);
        if (tmp != data_.end() && !tmp->second.is_expired()) {
            return make_pair(tmp->second.value,tmp->second.version);
        }
        return make_pair(std::nullopt,0);

    }

    void Shard::put_unlocked(const std::string &key, Attribute value) {
        auto tmp = data_.find(key);
        if (tmp != data_.end()) {
            tmp->second.value = std::move(value);
            tmp->second.version++;
        }else {
            Record rec;
            rec.value = std::move(value);
            rec.version = 1;
            data_.emplace(key, std::move(rec));
        }
    }

    bool Shard::remove_unlocked(const std::string &key) {
        return data_.erase(key) > 0;;
    }

    std::pair<std::optional<Attribute>, uint64_t> GuildRegistry::query_versioned(
        const std::string &key) {
        stats_.query_count.fetch_add(1);
        auto [value, version] = get_shard(key).get_versioned(key);
        if (value.has_value()) {
            stats_.hit_count.fetch_add(1);
        } else {
            stats_.miss_count.fetch_add(1);
        }
        return {std::move(value), version};
    }

    bool GuildRegistry::atomic_write(
        const std::map<std::string, std::optional<Attribute>> &write_set,
        const std::map<std::string, uint64_t> &read_set) {
        // 在 atomic_write 的基础上，持锁后先校验版本号：
        //   1. 收集 write_set + read_set 中所有 key 的 shard 索引，去重排序
        //   2. 按序加锁
        //   3. 遍历 read_set，检查每个 key 的当前版本是否等于记录的版本
        //      - 不等则说明有冲突，直接 return false（锁会在 unique_lock 析构时自动释放）
        //   4. 版本校验通过，应用 write_set 中的写操作
        //   5. return true
        std::vector<size_t> shard_indices;
        auto collect_shard = [&](const std::string &key) {
            size_t idx = std::hash<std::string>{}(key) % shards_.size();
            if (std::find(shard_indices.begin(), shard_indices.end(), idx) == shard_indices.end()) {
                shard_indices.push_back(idx);
            }
        };
        for (const auto &[key, _] : write_set) collect_shard(key);
        for (const auto &[key, _] : read_set) collect_shard(key);
        std::sort(shard_indices.begin(), shard_indices.end());

        std::vector<std::unique_lock<std::shared_mutex>> locks;
        locks.reserve(shard_indices.size());
        for (size_t idx : shard_indices) {
            locks.emplace_back(shards_[idx]->mutex());
        }

        for (const auto &[key, expected_version] : read_set) {
            Shard &shard = get_shard(key);
            auto it = shard.data_.find(key);
            uint64_t current_version = 0;
            if (it != shard.data_.end() && !it->second.is_expired()) {
                    current_version = it->second.version;
            }
            if (current_version != expected_version) {
                return false;
            }
        }

        for (const auto &[key, opt_value] : write_set) {
            Shard &shard = get_shard(key);
            if (opt_value.has_value()) {
                shard.put_unlocked(key, opt_value.value());
            } else {
                shard.remove_unlocked(key);
            }
        }

        return true;
    }
} // namespace guild