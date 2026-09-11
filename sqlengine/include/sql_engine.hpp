#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include "common.hpp"
#include "database.hpp"

namespace sqlengine {

// ------------------------------------------------------------------
// Session: per-connection state (current transaction, if any).
// Statements issued with no explicit BEGIN run in single-statement
// autocommit transactions.
// ------------------------------------------------------------------
struct Session {
    bool inTxn = false;
    uint64_t txnId = 0;
};

inline std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Tokenizes a SQL statement, keeping quoted strings and parenthesized
// groups intact as single tokens where useful, otherwise splitting on
// whitespace and top-level commas/parens.
inline std::vector<std::string> tokenize(const std::string& sql) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuote = false;
    char quoteChar = 0;
    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];
        if (inQuote) {
            cur += c;
            if (c == quoteChar) { inQuote = false; tokens.push_back(cur); cur.clear(); }
            continue;
        }
        if (c == '\'' || c == '"') { inQuote = true; quoteChar = c; cur += c; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            continue;
        }
        if (c == '(' || c == ')' || c == ',' || c == ';' || c == '=' || c == '*') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            tokens.push_back(std::string(1, c));
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

class SqlEngine {
public:
    explicit SqlEngine(Database& db) : db_(db) {}

    // Executes one statement in the context of a session; returns a
    // human-readable response line (mirrors typical SQL client output).
    std::string execute(Session& session, const std::string& rawSql) {
        std::string sql = rawSql;
        // strip trailing semicolon/whitespace
        while (!sql.empty() && (sql.back() == ';' || std::isspace((unsigned char)sql.back())))
            sql.pop_back();
        if (sql.empty()) return "";
        auto tokens = tokenize(sql);
        if (tokens.empty()) return "";
        std::string cmd = upper(tokens[0]);

        try {
            if (cmd == "BEGIN") return doBegin(session);
            if (cmd == "COMMIT") return doCommit(session);
            if (cmd == "ROLLBACK") return doRollback(session);
            if (cmd == "CREATE") return doCreateTable(tokens);
            if (cmd == "INSERT") return withAutocommit(session, [&] { return doInsert(session, tokens); });
            if (cmd == "SELECT") return withAutocommit(session, [&] { return doSelect(session, tokens); });
            if (cmd == "UPDATE") return withAutocommit(session, [&] { return doUpdate(session, tokens); });
            if (cmd == "DELETE") return withAutocommit(session, [&] { return doDelete(session, tokens); });
            if (cmd == "SHOW" ) return doShowTables();
            if (cmd == "STATS") return doStats();
            return "ERROR: Unrecognized command '" + cmd + "'";
        } catch (const std::exception& e) {
            // If we're mid-autocommit and something threw before the
            // commit lambda finished, make sure we don't leak the txn.
            return std::string("ERROR: ") + e.what();
        }
    }

private:
    Database& db_;

    // Wraps a single statement in an implicit BEGIN/COMMIT if the
    // session isn't already inside an explicit transaction. On
    // exception, rolls back the implicit txn and rethrows as an
    // ERROR string via the outer catch in execute().
    template <typename Fn>
    std::string withAutocommit(Session& session, Fn&& fn) {
        bool implicit = !session.inTxn;
        uint64_t txnId;
        if (implicit) {
            txnId = db_.beginTransaction();
        } else {
            txnId = session.txnId;
        }
        try {
            std::string result = fn2(session, txnId, fn);
            if (implicit) db_.commit(txnId);
            return result;
        } catch (...) {
            if (implicit) db_.rollback(txnId);
            throw;
        }
    }

    // Helper because lambdas capturing txnId need it passed to the
    // per-statement handlers, which take (session, tokens) currently;
    // handlers read txnId via session when explicit, or via the local
    // implicit id otherwise -- we thread it through a thread_local-free
    // approach by temporarily stashing it on the session.
    template <typename Fn>
    std::string fn2(Session& session, uint64_t txnId, Fn&& fn) {
        uint64_t saved = session.txnId;
        bool savedInTxn = session.inTxn;
        session.txnId = txnId;
        session.inTxn = true; // so handlers always read session.txnId uniformly
        std::string result;
        try {
            result = fn();
        } catch (...) {
            session.txnId = saved;
            session.inTxn = savedInTxn;
            throw;
        }
        session.txnId = saved;
        session.inTxn = savedInTxn;
        return result;
    }

    std::string doBegin(Session& session) {
        if (session.inTxn) return "ERROR: Transaction already in progress";
        session.txnId = db_.beginTransaction();
        session.inTxn = true;
        return "OK BEGIN txn=" + std::to_string(session.txnId);
    }

    std::string doCommit(Session& session) {
        if (!session.inTxn) return "ERROR: No transaction in progress";
        db_.commit(session.txnId);
        session.inTxn = false;
        return "OK COMMIT";
    }

    std::string doRollback(Session& session) {
        if (!session.inTxn) return "ERROR: No transaction in progress";
        db_.rollback(session.txnId);
        session.inTxn = false;
        return "OK ROLLBACK";
    }

    // CREATE TABLE name (col TYPE, col TYPE, ...) PRIMARY KEY(col)
    std::string doCreateTable(std::vector<std::string>& t) {
        size_t i = 1;
        if (i >= t.size() || upper(t[i]) != "TABLE") throw std::runtime_error("Expected TABLE");
        ++i;
        Schema schema;
        schema.tableName = t[i++];
        if (i >= t.size() || t[i] != "(") throw std::runtime_error("Expected '('");
        ++i;
        while (i < t.size() && t[i] != ")") {
            ColumnDef cd;
            cd.name = t[i++];
            std::string typeTok = upper(t[i++]);
            cd.type = (typeTok == "INT" || typeTok == "INTEGER") ? ColumnType::INT : ColumnType::TEXT;
            schema.columns.push_back(cd);
            if (i < t.size() && t[i] == ",") ++i;
        }
        ++i; // skip ')'
        // Optional PRIMARY KEY(col)
        if (i < t.size() && upper(t[i]) == "PRIMARY") {
            i += 2; // PRIMARY KEY
            if (i < t.size() && t[i] == "(") {
                ++i;
                std::string pkCol = t[i++];
                schema.primaryKeyIndex = schema.indexOf(pkCol);
                if (i < t.size() && t[i] == ")") ++i;
            }
        }
        if (schema.primaryKeyIndex < 0) schema.primaryKeyIndex = 0; // default: first column
        db_.createTable(schema);
        return "OK CREATE TABLE " + schema.tableName;
    }

    // INSERT INTO table VALUES (v1, v2, ...)
    std::string doInsert(Session& session, std::vector<std::string>& t) {
        size_t i = 1;
        if (upper(t[i]) != "INTO") throw std::runtime_error("Expected INTO");
        ++i;
        std::string tableName = t[i++];
        auto table = db_.getTable(tableName);
        const auto& schema = table->schema();
        if (upper(t[i]) != "VALUES") throw std::runtime_error("Expected VALUES");
        ++i;
        if (t[i] != "(") throw std::runtime_error("Expected '('");
        ++i;
        Row row;
        size_t colIdx = 0;
        while (i < t.size() && t[i] != ")") {
            if (colIdx >= schema.columns.size()) throw std::runtime_error("Too many values");
            row.push_back(parseValueToken(t[i++], schema.columns[colIdx++].type));
            if (i < t.size() && t[i] == ",") ++i;
        }
        if (colIdx != schema.columns.size()) throw std::runtime_error("Column count mismatch");
        std::string pkKey = row[schema.primaryKeyIndex].key();
        db_.insertRow(session.txnId, tableName, pkKey, row);
        return "OK INSERT 1";
    }

    // SELECT * FROM table [WHERE pkcol = value]
    std::string doSelect(Session& session, std::vector<std::string>& t) {
        size_t i = 1;
        if (t[i] != "*") throw std::runtime_error("Only SELECT * is supported");
        ++i;
        if (upper(t[i]) != "FROM") throw std::runtime_error("Expected FROM");
        ++i;
        std::string tableName = t[i++];
        auto table = db_.getTable(tableName);
        const auto& schema = table->schema();

        std::vector<Row> rows;
        bool forUpdate = false;
        if (i < t.size() && upper(t[i]) == "WHERE") {
            i++;
            std::string col = t[i++];
            if (t[i] != "=") throw std::runtime_error("Only '=' predicates are supported");
            ++i;
            int colIdx = schema.indexOf(col);
            if (colIdx != schema.primaryKeyIndex)
                throw std::runtime_error("WHERE only supported on primary key column '" +
                                          schema.columns[schema.primaryKeyIndex].name + "'");
            Value val = parseValueToken(t[i++], schema.columns[colIdx].type);
            if (i < t.size() && upper(t[i]) == "FOR") {
                ++i;
                if (i < t.size() && upper(t[i]) == "UPDATE") { ++i; forUpdate = true; }
            }
            if (forUpdate) {
                // Acquire the row lock BEFORE reading, so this txn is
                // guaranteed no concurrent writer can slip in between
                // this read and a later UPDATE/DELETE in the same txn.
                db_.lockRowForUpdate(session.txnId, tableName, val.key());
            }
            auto row = db_.getRow(session.txnId, tableName, val.key());
            if (row) rows.push_back(*row);
        } else {
            rows = db_.scanTable(tableName);
        }
        return formatRows(schema, rows);
    }

    // UPDATE table SET col=val[, col=val...] WHERE pkcol = value
    std::string doUpdate(Session& session, std::vector<std::string>& t) {
        size_t i = 1;
        std::string tableName = t[i++];
        auto table = db_.getTable(tableName);
        const auto& schema = table->schema();
        if (upper(t[i]) != "SET") throw std::runtime_error("Expected SET");
        ++i;
        std::unordered_map<std::string, Value> assignments;
        while (i < t.size() && upper(t[i]) != "WHERE") {
            std::string col = t[i++];
            if (t[i] != "=") throw std::runtime_error("Expected '='");
            ++i;
            int colIdx = schema.indexOf(col);
            if (colIdx < 0) throw std::runtime_error("No such column: " + col);
            assignments[col] = parseValueToken(t[i++], schema.columns[colIdx].type);
            if (i < t.size() && t[i] == ",") ++i;
        }
        if (i >= t.size() || upper(t[i]) != "WHERE") throw std::runtime_error("UPDATE requires WHERE on primary key");
        ++i;
        std::string col = t[i++];
        if (t[i] != "=") throw std::runtime_error("Expected '='");
        ++i;
        int pkColIdx = schema.indexOf(col);
        if (pkColIdx != schema.primaryKeyIndex)
            throw std::runtime_error("WHERE only supported on primary key column");
        Value pkVal = parseValueToken(t[i++], schema.columns[pkColIdx].type);
        std::string pkKey = pkVal.key();

        auto existing = db_.getRow(session.txnId, tableName, pkKey);
        if (!existing) throw std::runtime_error("Row not found: " + pkKey);
        Row newRow = *existing;
        for (auto& [colName, val] : assignments) {
            int idx = schema.indexOf(colName);
            newRow[idx] = val;
        }
        db_.updateRow(session.txnId, tableName, pkKey, newRow);
        return "OK UPDATE 1";
    }

    // DELETE FROM table WHERE pkcol = value
    std::string doDelete(Session& session, std::vector<std::string>& t) {
        size_t i = 1;
        if (upper(t[i]) != "FROM") throw std::runtime_error("Expected FROM");
        ++i;
        std::string tableName = t[i++];
        auto table = db_.getTable(tableName);
        const auto& schema = table->schema();
        if (i >= t.size() || upper(t[i]) != "WHERE") throw std::runtime_error("DELETE requires WHERE on primary key");
        ++i;
        std::string col = t[i++];
        if (t[i] != "=") throw std::runtime_error("Expected '='");
        ++i;
        int pkColIdx = schema.indexOf(col);
        if (pkColIdx != schema.primaryKeyIndex)
            throw std::runtime_error("WHERE only supported on primary key column");
        Value pkVal = parseValueToken(t[i++], schema.columns[pkColIdx].type);
        db_.deleteRow(session.txnId, tableName, pkVal.key());
        return "OK DELETE 1";
    }

    std::string doShowTables() {
        auto tables = db_.listTables();
        std::ostringstream out;
        out << "TABLES(" << tables.size() << "): ";
        for (auto& n : tables) out << n << " ";
        return out.str();
    }

    std::string doStats() {
        auto s = db_.cache().stats();
        std::ostringstream out;
        out << "cache_hits=" << s.hits << " cache_misses=" << s.misses
            << " cache_size=" << db_.cache().size()
            << " wal_flushes=" << db_.wal().flushCount()
            << " active_locks=" << db_.lockManager().activeLockCount();
        return out.str();
    }

    static std::string formatRows(const Schema& schema, const std::vector<Row>& rows) {
        std::ostringstream out;
        out << "ROWS(" << rows.size() << ")";
        for (auto& row : rows) {
            out << "\n";
            for (size_t c = 0; c < row.size(); ++c) {
                if (c) out << " | ";
                out << schema.columns[c].name << "=" << row[c].toString();
            }
        }
        return out.str();
    }
};

} // namespace sqlengine
