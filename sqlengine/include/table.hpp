#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <optional>
#include "common.hpp"

namespace sqlengine {

// ------------------------------------------------------------------
// Table: an in-memory relation backed by a primary-key hash index
// for O(1) point lookups, insert, and delete.
//
// indexMu_ protects ONLY the structural integrity of the hash map
// itself (the {key -> Row} bindings). It is intentionally a narrow,
// short-held lock (shared for reads via std::shared_mutex, exclusive
// for structural writes) and is fully decoupled from the LockManager,
// which governs *logical* write ownership of a row across an entire
// transaction. A transaction acquires a row lock once (via
// LockManager) and can then read/mutate that row across multiple
// WAL-staged operations without ever holding indexMu_ for more than
// the duration of a single map operation.
// ------------------------------------------------------------------
class Table {
public:
    Table() = default;
    Table(Schema schema) : schema_(std::move(schema)) {}

    const Schema& schema() const { return schema_; }

    // O(1) point lookup by primary key.
    std::optional<Row> get(const std::string& pkKey) const {
        std::shared_lock<std::shared_mutex> lock(indexMu_);
        auto it = index_.find(pkKey);
        if (it == index_.end()) return std::nullopt;
        return it->second;
    }

    bool contains(const std::string& pkKey) const {
        std::shared_lock<std::shared_mutex> lock(indexMu_);
        return index_.count(pkKey) > 0;
    }

    void put(const std::string& pkKey, const Row& row) {
        std::unique_lock<std::shared_mutex> lock(indexMu_);
        index_[pkKey] = row;
    }

    void erase(const std::string& pkKey) {
        std::unique_lock<std::shared_mutex> lock(indexMu_);
        index_.erase(pkKey);
    }

    // Full scan snapshot (used for SELECT * without a WHERE pk = ...).
    std::vector<Row> scanAll() const {
        std::shared_lock<std::shared_mutex> lock(indexMu_);
        std::vector<Row> out;
        out.reserve(index_.size());
        for (const auto& [k, v] : index_) out.push_back(v);
        return out;
    }

    size_t rowCount() const {
        std::shared_lock<std::shared_mutex> lock(indexMu_);
        return index_.size();
    }

private:
    Schema schema_;
    mutable std::shared_mutex indexMu_;
    std::unordered_map<std::string, Row> index_; // pkKey -> Row, O(1) avg
};

} // namespace sqlengine
