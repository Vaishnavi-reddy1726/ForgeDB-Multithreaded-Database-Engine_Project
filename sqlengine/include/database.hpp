#pragma once
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <mutex>
#include <atomic>
#include <memory>
#include <vector>
#include <stdexcept>
#include "common.hpp"
#include "table.hpp"
#include "wal.hpp"
#include "lock_manager.hpp"
#include "lru_cache.hpp"

namespace sqlengine {

// One entry per in-flight write, kept so ROLLBACK can restore prior state.
struct UndoEntry {
    std::string table;
    std::string pkKey;
    bool hadOld;
    Row oldRow;
};

struct Transaction {
    uint64_t id = 0;
    bool active = false;
    std::vector<UndoEntry> undoLog;
    std::unordered_set<std::string> dirtyRowIds; // rowIds touched this txn -> bypass cache on read
};

class Database {
public:
    explicit Database(const std::string& walPath, const std::string& schemaPath = "data/schema.catalog")
        : schemaPath_(schemaPath), wal_(walPath) {
        loadSchemaCatalog();
        recover();
    }

    // ---------------- DDL ----------------
    void createTable(const Schema& schema) {
        {
            std::unique_lock<std::shared_mutex> lock(tablesMu_);
            if (tables_.count(schema.tableName))
                throw std::runtime_error("Table already exists: " + schema.tableName);
            tables_[schema.tableName] = std::make_shared<Table>(schema);
        }
        persistSchema(schema);
    }

    std::shared_ptr<Table> getTable(const std::string& name) {
        std::shared_lock<std::shared_mutex> lock(tablesMu_);
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("No such table: " + name);
        return it->second;
    }

    bool hasTable(const std::string& name) {
        std::shared_lock<std::shared_mutex> lock(tablesMu_);
        return tables_.count(name) > 0;
    }

    std::vector<std::string> listTables() {
        std::shared_lock<std::shared_mutex> lock(tablesMu_);
        std::vector<std::string> out;
        for (auto& [k, v] : tables_) out.push_back(k);
        return out;
    }

    // ---------------- Transaction lifecycle ----------------
    uint64_t beginTransaction() {
        uint64_t id = nextTxnId_.fetch_add(1) + 1;
        std::lock_guard<std::mutex> lock(txnMu_);
        Transaction t;
        t.id = id;
        t.active = true;
        txns_[id] = std::move(t);
        return id;
    }

    Transaction& txnRef(uint64_t id) {
        std::lock_guard<std::mutex> lock(txnMu_);
        auto it = txns_.find(id);
        if (it == txns_.end() || !it->second.active)
            throw std::runtime_error("No active transaction: " + std::to_string(id));
        return it->second;
    }

    void commit(uint64_t txnId) {
        std::vector<std::string> dirtyRowIds;
        {
            std::lock_guard<std::mutex> lock(txnMu_);
            auto it = txns_.find(txnId);
            if (it == txns_.end() || !it->second.active)
                throw std::runtime_error("No active transaction: " + std::to_string(txnId));
            dirtyRowIds.assign(it->second.dirtyRowIds.begin(), it->second.dirtyRowIds.end());
        }
        // Durable flush: staged WAL records for this txn hit disk + fsync here.
        wal_.commit(txnId);
        // Commit invalidation: now that changes are durable & visible,
        // purge any cached (pre-write) snapshots of touched rows.
        for (auto& rid : dirtyRowIds) cache_.invalidate(rid);
        lockMgr_.releaseAll(txnId);
        std::lock_guard<std::mutex> lock(txnMu_);
        txns_.erase(txnId);
    }

    void rollback(uint64_t txnId) {
        std::vector<UndoEntry> undo;
        {
            std::lock_guard<std::mutex> lock(txnMu_);
            auto it = txns_.find(txnId);
            if (it == txns_.end() || !it->second.active)
                throw std::runtime_error("No active transaction: " + std::to_string(txnId));
            undo = std::move(it->second.undoLog);
        }
        wal_.rollback(txnId); // discard staged records; nothing was ever on disk
        // Undo in-memory mutations in reverse order.
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
            auto table = getTable(it->table);
            if (it->hadOld) table->put(it->pkKey, it->oldRow);
            else table->erase(it->pkKey);
        }
        lockMgr_.releaseAll(txnId);
        std::lock_guard<std::mutex> lock(txnMu_);
        txns_.erase(txnId);
    }

    // ---------------- Row operations (must be called within a txn) --
    void insertRow(uint64_t txnId, const std::string& tableName, const std::string& pkKey, const Row& row) {
        auto table = getTable(tableName);
        std::string rowId = makeRowId(tableName, pkKey);
        lockMgr_.lockRow(rowId, txnId);
        if (table->contains(pkKey))
            throw std::runtime_error("Duplicate primary key: " + pkKey);
        auto& txn = txnRef(txnId);
        txn.undoLog.push_back({tableName, pkKey, false, {}});
        txn.dirtyRowIds.insert(rowId);
        wal_.stage(txnId, WalRecord{OpType::INSERT, txnId, tableName, pkKey, serializeRow(row)});
        table->put(pkKey, row);
    }

    void updateRow(uint64_t txnId, const std::string& tableName, const std::string& pkKey, const Row& newRow) {
        auto table = getTable(tableName);
        std::string rowId = makeRowId(tableName, pkKey);
        lockMgr_.lockRow(rowId, txnId);
        auto old = table->get(pkKey);
        if (!old) throw std::runtime_error("Row not found for update: " + pkKey);
        auto& txn = txnRef(txnId);
        txn.undoLog.push_back({tableName, pkKey, true, *old});
        txn.dirtyRowIds.insert(rowId);
        wal_.stage(txnId, WalRecord{OpType::UPDATE, txnId, tableName, pkKey, serializeRow(newRow)});
        table->put(pkKey, newRow);
    }

    void deleteRow(uint64_t txnId, const std::string& tableName, const std::string& pkKey) {
        auto table = getTable(tableName);
        std::string rowId = makeRowId(tableName, pkKey);
        lockMgr_.lockRow(rowId, txnId);
        auto old = table->get(pkKey);
        if (!old) throw std::runtime_error("Row not found for delete: " + pkKey);
        auto& txn = txnRef(txnId);
        txn.undoLog.push_back({tableName, pkKey, true, *old});
        txn.dirtyRowIds.insert(rowId);
        wal_.stage(txnId, WalRecord{OpType::DELETE, txnId, tableName, pkKey, ""});
        table->erase(pkKey);
    }

    // Acquires the row lock for pkKey under txnId without necessarily
    // reading/writing it -- this is what backs "SELECT ... FOR UPDATE",
    // letting a transaction serialize a read-modify-write sequence
    // across multiple client round-trips instead of only locking at
    // the final UPDATE/DELETE call (which would allow lost updates).
    void lockRowForUpdate(uint64_t txnId, const std::string& tableName, const std::string& pkKey) {
        std::string rowId = makeRowId(tableName, pkKey);
        lockMgr_.lockRow(rowId, txnId);
        auto& txn = txnRef(txnId);
        txn.dirtyRowIds.insert(rowId); // bypass cache for the remainder of this txn
    }

    // Point read by primary key, cache-accelerated. If the calling txn
    // has an uncommitted write to this exact row this call, the cache is
    // bypassed so the transaction always observes its own writes.
    std::optional<Row> getRow(uint64_t txnId, const std::string& tableName, const std::string& pkKey) {
        std::string rowId = makeRowId(tableName, pkKey);
        bool bypassCache = false;
        {
            std::lock_guard<std::mutex> lock(txnMu_);
            auto it = txns_.find(txnId);
            if (it != txns_.end() && it->second.dirtyRowIds.count(rowId)) bypassCache = true;
        }
        auto table = getTable(tableName);
        if (!bypassCache) {
            if (auto cached = cache_.get(rowId)) {
                return deserializeRow(*cached);
            }
        }
        auto row = table->get(pkKey);
        if (row && !bypassCache) cache_.put(rowId, serializeRow(*row));
        return row;
    }

    std::vector<Row> scanTable(const std::string& tableName) {
        return getTable(tableName)->scanAll();
    }

    WriteAheadLog& wal() { return wal_; }
    LockManager& lockManager() { return lockMgr_; }
    LRUCache<std::string, std::string, 512>& cache() { return cache_; }

private:
    // Replays committed WAL records from disk to rebuild in-memory state
    // after a restart (crash resilience). Requires that CREATE TABLE
    // statements ran again before insert/update/delete replay reaches a
    // table not yet known — in this simple engine, schemas are not
    // WAL-logged, so callers should issue CREATE TABLE before relying on
    // replay for that table's rows within the same run. Replay here
    // fixes up any table whose schema already exists at startup.
    void recover() {
        wal_.replayCommitted([this](const WalRecord& r) {
            if (!hasTable(r.table)) return; // schema unknown this run; skip
            auto table = getTable(r.table);
            if (r.op == OpType::INSERT || r.op == OpType::UPDATE) {
                table->put(r.pkKey, deserializeRow(r.payload));
            } else if (r.op == OpType::DELETE) {
                table->erase(r.pkKey);
            }
        });
    }

    // Simple on-disk schema catalog so table definitions survive a
    // restart: one line per table, "name|col:TYPE,col:TYPE,...|pkIndex".
    void persistSchema(const Schema& schema) {
        std::ofstream out(schemaPath_, std::ios::app);
        if (!out.is_open()) return; // best-effort; in-memory schema still valid this run
        out << schema.tableName << "|";
        for (size_t i = 0; i < schema.columns.size(); ++i) {
            if (i) out << ",";
            out << schema.columns[i].name << ":"
                << (schema.columns[i].type == ColumnType::INT ? "INT" : "TEXT");
        }
        out << "|" << schema.primaryKeyIndex << "\n";
    }

    void loadSchemaCatalog() {
        std::ifstream in(schemaPath_);
        if (!in.is_open()) return; // no catalog yet
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            size_t p1 = line.find('|');
            size_t p2 = line.find('|', p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) continue;
            Schema schema;
            schema.tableName = line.substr(0, p1);
            std::string colsStr = line.substr(p1 + 1, p2 - p1 - 1);
            schema.primaryKeyIndex = std::stoi(line.substr(p2 + 1));
            std::stringstream ss(colsStr);
            std::string colTok;
            while (std::getline(ss, colTok, ',')) {
                size_t cp = colTok.find(':');
                ColumnDef cd;
                cd.name = colTok.substr(0, cp);
                cd.type = (colTok.substr(cp + 1) == "INT") ? ColumnType::INT : ColumnType::TEXT;
                schema.columns.push_back(cd);
            }
            tables_[schema.tableName] = std::make_shared<Table>(schema);
        }
    }

    std::string schemaPath_;
    std::shared_mutex tablesMu_;
    std::unordered_map<std::string, std::shared_ptr<Table>> tables_;

    WriteAheadLog wal_;
    LockManager lockMgr_;
    LRUCache<std::string, std::string, 512> cache_;

    std::atomic<uint64_t> nextTxnId_{0};
    std::mutex txnMu_;
    std::unordered_map<uint64_t, Transaction> txns_;
};

} // namespace sqlengine
