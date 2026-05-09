#include "trade_system.h"

namespace guild {
    Transaction::Transaction(GuildRegistry &registry, std::mutex &tx_mutex)
        : registry_(registry), tx_mutex_(tx_mutex) {
    }

    std::optional<Attribute> Transaction::get(const std::string &key) {
        std::lock_guard lock(tx_mutex_);
        auto tmp = write_set_.find(key);
        if (tmp != write_set_.end()) {
            return tmp->second;
        }
        auto [value, version] = registry_.query_versioned(key);
        if (value.has_value()) {
            read_set_[key] = version;
            return value;
        } else {
            read_set_[key] = 0;
            return std::nullopt;
        }
    }

    void Transaction::put(const std::string &key, Attribute value) {
        std::lock_guard lock(tx_mutex_);
        write_set_[key] = std::move(value);
    }

    void Transaction::remove(const std::string &key) {
        std::lock_guard lock(tx_mutex_);
        write_set_[key] = std::nullopt;
    }

    bool Transaction::commit() {
        if (committed_ || rolled_back_) return false;

        bool success = registry_.atomic_write(write_set_, read_set_);
        if (success) {
            committed_ = true;
            return true;
        }
        committed_ = true;
        return false;
    }

    void Transaction::rollback() {
        if (committed_ || rolled_back_) return;
        write_set_.clear();
        rolled_back_ = true;
    }

    Transaction::~Transaction() {
        if (!committed_ && !rolled_back_) {
            rollback();
        }
    }
} // namespace guild
