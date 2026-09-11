#pragma once
#include <list>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <atomic>

namespace sqlengine {

// ------------------------------------------------------------------
// Thread-safe LRU cache mapping RowId -> cached Row snapshot (serialized).
// Fixed capacity (default 512 entries). Supports point invalidation
// (used on COMMIT of a write touching a given key) and full clear
// (used e.g. on DROP TABLE).
// ------------------------------------------------------------------
template <typename K, typename V, size_t Capacity = 512>
class LRUCache {
public:
    LRUCache() = default;

    // Returns cached value if present; promotes entry to most-recently-used.
    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return std::nullopt;
        }
        // Move to front (MRU).
        list_.splice(list_.begin(), list_, it->second);
        ++hits_;
        return it->second->second;
    }

    void put(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = value;
            list_.splice(list_.begin(), list_, it->second);
            return;
        }
        list_.emplace_front(key, value);
        index_[key] = list_.begin();
        if (list_.size() > Capacity) {
            auto last = std::prev(list_.end());
            index_.erase(last->first);
            list_.pop_back();
        }
    }

    // Invalidate a single key. Called on commit of a transaction that
    // wrote to this row, so the cache never serves stale post-commit data.
    void invalidate(const K& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            list_.erase(it->second);
            index_.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        list_.clear();
        index_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return list_.size();
    }

    struct Stats { uint64_t hits; uint64_t misses; };
    Stats stats() const { return { hits_.load(), misses_.load() }; }

private:
    using ListType = std::list<std::pair<K, V>>;
    mutable std::mutex mu_;
    ListType list_;
    std::unordered_map<K, typename ListType::iterator> index_;
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

} // namespace sqlengine
