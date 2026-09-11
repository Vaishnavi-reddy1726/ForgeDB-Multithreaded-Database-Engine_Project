#pragma once
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <optional>

namespace sqlengine {

// ------------------------------------------------------------------
// Value: a single cell in a row. Supports INT, TEXT (and NULL).
// ------------------------------------------------------------------
enum class ColumnType { INT, TEXT };

struct Value {
    std::variant<std::monostate, int64_t, std::string> data;

    Value() : data(std::monostate{}) {}
    Value(int64_t i) : data(i) {}
    Value(const std::string& s) : data(s) {}

    bool isNull() const { return std::holds_alternative<std::monostate>(data); }
    bool isInt()  const { return std::holds_alternative<int64_t>(data); }
    bool isText() const { return std::holds_alternative<std::string>(data); }

    int64_t asInt() const { return std::get<int64_t>(data); }
    const std::string& asText() const { return std::get<std::string>(data); }

    std::string toString() const {
        if (isNull()) return "NULL";
        if (isInt()) return std::to_string(std::get<int64_t>(data));
        return std::get<std::string>(data);
    }

    // Serialization key used for hash-index lookups (primary key values).
    std::string key() const { return toString(); }

    bool operator==(const Value& o) const { return data == o.data; }
};

// Parse a raw token from SQL text into a typed Value based on column type.
inline Value parseValueToken(const std::string& tok, ColumnType type) {
    // Strip surrounding quotes for text literals.
    if (!tok.empty() && (tok.front() == '\'' || tok.front() == '"')) {
        std::string inner = tok.substr(1, tok.size() >= 2 ? tok.size() - 2 : 0);
        return Value(inner);
    }
    if (tok == "NULL" || tok == "null") return Value();
    if (type == ColumnType::INT) {
        try {
            return Value(static_cast<int64_t>(std::stoll(tok)));
        } catch (...) {
            throw std::runtime_error("Invalid integer literal: " + tok);
        }
    }
    return Value(tok);
}

// ------------------------------------------------------------------
// Schema / Row
// ------------------------------------------------------------------
struct ColumnDef {
    std::string name;
    ColumnType type;
};

struct Schema {
    std::string tableName;
    std::vector<ColumnDef> columns;
    int primaryKeyIndex = -1; // index into columns

    int indexOf(const std::string& colName) const {
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i].name == colName) return static_cast<int>(i);
        return -1;
    }
};

using Row = std::vector<Value>;

// Row identifier used by the lock manager & LRU cache: "table:pkvalue"
inline std::string makeRowId(const std::string& table, const std::string& pkKey) {
    return table + ":" + pkKey;
}

// ------------------------------------------------------------------
// Row <-> string serialization for WAL payloads and cache values.
// Format: each field length-prefixed, typed with a 1-char tag.
// ------------------------------------------------------------------
inline std::string serializeRow(const Row& row) {
    std::ostringstream out;
    for (const auto& v : row) {
        if (v.isNull()) {
            out << "N:0:;";
        } else if (v.isInt()) {
            std::string s = std::to_string(v.asInt());
            out << "I:" << s.size() << ":" << s << ";";
        } else {
            const std::string& s = v.asText();
            out << "T:" << s.size() << ":" << s << ";";
        }
    }
    return out.str();
}

inline Row deserializeRow(const std::string& data) {
    Row row;
    size_t pos = 0;
    while (pos < data.size()) {
        char tag = data[pos]; pos += 2; // skip tag and ':'
        size_t colonPos = data.find(':', pos);
        size_t len = std::stoul(data.substr(pos, colonPos - pos));
        pos = colonPos + 1;
        std::string field = data.substr(pos, len);
        pos += len + 1; // skip field and trailing ';'
        if (tag == 'N') row.emplace_back();
        else if (tag == 'I') row.emplace_back(static_cast<int64_t>(std::stoll(field)));
        else row.emplace_back(field);
    }
    return row;
}

} // namespace sqlengine
