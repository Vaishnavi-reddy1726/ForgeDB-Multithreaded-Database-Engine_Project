#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <cstdint>
#include <atomic>
#include <unordered_map>
#include "common.hpp"

namespace sqlengine {

// ------------------------------------------------------------------
// Write-Ahead Log
//
// Every mutation (INSERT/UPDATE/DELETE) is first appended to an
// in-memory "staging" buffer tagged with its owning transaction id.
// Nothing is applied to the durable log file until COMMIT, at which
// point all staged records for that transaction are serialized and
// fsync'd to disk in a single durable flush (the "group commit"
// pattern), then applied to in-memory table state. ROLLBACK simply
// discards the staged records — nothing ever touched disk.
//
// On startup, replayCommitted() reads the on-disk log and replays
// only records belonging to committed transactions, giving crash
// resilience: a crash between staging and commit loses nothing
// durable (nothing was on disk yet); a crash after commit's fsync
// is fully recoverable by replay.
// ------------------------------------------------------------------
enum class OpType : uint8_t { INSERT = 1, UPDATE = 2, DELETE = 3, BEGIN = 4, COMMIT = 5, ROLLBACK = 6 };

struct WalRecord {
    OpType op;
    uint64_t txnId;
    std::string table;
    std::string pkKey;      // primary key value (as string) this record concerns
    std::string payload;    // serialized row (for INSERT/UPDATE), empty for DELETE/BEGIN/COMMIT/ROLLBACK
};

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& path) : path_(path) {
        file_.open(path_, std::ios::app | std::ios::binary);
        if (!file_.is_open())
            throw std::runtime_error("Cannot open WAL file: " + path_);
    }

    // Stage a record under a transaction; NOT yet durable.
    void stage(uint64_t txnId, const WalRecord& rec) {
        std::lock_guard<std::mutex> lock(stageMu_);
        staged_[txnId].push_back(rec);
    }

    // Durably flush all staged records for txnId, followed by a COMMIT
    // marker, then fsync. Returns the flushed records so the caller can
    // apply them to in-memory state under the same commit.
    std::vector<WalRecord> commit(uint64_t txnId) {
        std::vector<WalRecord> records;
        {
            std::lock_guard<std::mutex> lock(stageMu_);
            auto it = staged_.find(txnId);
            if (it != staged_.end()) {
                records = std::move(it->second);
                staged_.erase(it);
            }
        }
        std::lock_guard<std::mutex> flock(fileMu_);
        for (auto& r : records) writeRecord(r);
        WalRecord commitMarker{OpType::COMMIT, txnId, "", "", ""};
        writeRecord(commitMarker);
        file_.flush();
        durableFlushCount_++;
        return records;
    }

    // Discard staged records without ever touching disk.
    void rollback(uint64_t txnId) {
        std::lock_guard<std::mutex> lock(stageMu_);
        staged_.erase(txnId);
    }

    uint64_t flushCount() const { return durableFlushCount_; }

    // Replays the on-disk WAL. onApply is invoked once per committed
    // record, in order, so the caller (Database) can rebuild table state.
    template <typename Fn>
    void replayCommitted(Fn onApply) {
        std::ifstream in(path_, std::ios::binary);
        if (!in.is_open()) return;
        std::vector<WalRecord> pending; // records for txns not yet known committed
        std::unordered_map<uint64_t, std::vector<WalRecord>> byTxn;
        WalRecord rec;
        while (readRecord(in, rec)) {
            if (rec.op == OpType::COMMIT) {
                auto it = byTxn.find(rec.txnId);
                if (it != byTxn.end()) {
                    for (auto& r : it->second) onApply(r);
                    byTxn.erase(it);
                }
            } else if (rec.op == OpType::ROLLBACK) {
                byTxn.erase(rec.txnId);
            } else {
                byTxn[rec.txnId].push_back(rec);
            }
        }
        // Any txn without a trailing COMMIT record is incomplete -> discarded (crash resilience).
    }

private:
    void writeRecord(const WalRecord& r) {
        uint8_t op = static_cast<uint8_t>(r.op);
        file_.write(reinterpret_cast<const char*>(&op), sizeof(op));
        file_.write(reinterpret_cast<const char*>(&r.txnId), sizeof(r.txnId));
        writeString(r.table);
        writeString(r.pkKey);
        writeString(r.payload);
    }

    void writeString(const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        file_.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len) file_.write(s.data(), len);
    }

    static bool readString(std::ifstream& in, std::string& out) {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;
        out.resize(len);
        if (len && !in.read(&out[0], len)) return false;
        return true;
    }

    static bool readRecord(std::ifstream& in, WalRecord& r) {
        uint8_t op;
        if (!in.read(reinterpret_cast<char*>(&op), sizeof(op))) return false;
        r.op = static_cast<OpType>(op);
        if (!in.read(reinterpret_cast<char*>(&r.txnId), sizeof(r.txnId))) return false;
        if (!readString(in, r.table)) return false;
        if (!readString(in, r.pkKey)) return false;
        if (!readString(in, r.payload)) return false;
        return true;
    }

    std::string path_;
    std::ofstream file_;
    std::mutex fileMu_;
    std::mutex stageMu_;
    std::unordered_map<uint64_t, std::vector<WalRecord>> staged_;
    std::atomic<uint64_t> durableFlushCount_{0};
};

} // namespace sqlengine

