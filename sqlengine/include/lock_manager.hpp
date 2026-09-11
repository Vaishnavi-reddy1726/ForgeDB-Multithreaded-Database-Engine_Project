#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <chrono>
#include <unordered_set>

namespace sqlengine {

// ------------------------------------------------------------------
// Explicit row-level lock manager.
//
// This is deliberately a separate subsystem from Table's hash-index
// mutex: the hash index mutex only ever protects the *structure* of
// the index (insert/erase/find of the map itself) for a bounded,
// O(1) critical section, while row locks protect *logical* write
// access to a row's contents across the lifetime of a transaction
// (which may span many client round-trips). Conflating the two would
// mean holding the index mutex for the duration of a transaction,
// serializing all unrelated readers/writers on unrelated rows.
//
// Locking model: single-writer per row (exclusive), tracked by
// owning transaction id, with a wait queue via condition_variable.
// Re-entrant for the same txn (a txn already holding the lock may
// "acquire" it again, e.g. UPDATE followed by DELETE in one txn).
// ------------------------------------------------------------------
class LockManager {
public:
    // Blocks until the lock for rowId is available or owned by txnId,
    // or until timeoutMs elapses (throws on timeout).
    void lockRow(const std::string& rowId, uint64_t txnId, int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lock(mu_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        auto it = owners_.find(rowId);
        while (it != owners_.end() && it->second != txnId) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                it = owners_.find(rowId);
                if (it != owners_.end() && it->second != txnId)
                    throw std::runtime_error("Lock timeout on row " + rowId +
                                              " (held by txn " + std::to_string(it->second) + ")");
                break;
            }
            it = owners_.find(rowId);
        }
        owners_[rowId] = txnId;
        heldBy_[txnId].insert(rowId);
    }

    // Releases every row lock held by a transaction (called on COMMIT/ROLLBACK).
    void releaseAll(uint64_t txnId) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = heldBy_.find(txnId);
        if (it == heldBy_.end()) return;
        for (const auto& rowId : it->second) {
            owners_.erase(rowId);
        }
        heldBy_.erase(it);
        cv_.notify_all();
    }

    bool isLockedByOther(const std::string& rowId, uint64_t txnId) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = owners_.find(rowId);
        return it != owners_.end() && it->second != txnId;
    }

    size_t activeLockCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return owners_.size();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, uint64_t> owners_;                 // rowId -> txnId
    std::unordered_map<uint64_t, std::unordered_set<std::string>> heldBy_; // txnId -> rowIds
};

} // namespace sqlengine

