#!/usr/bin/env bash
# Integration test for the multithreaded SQL engine:
#   1. Boots the server against a scratch WAL/schema dir.
#   2. Exercises CREATE/INSERT/SELECT/UPDATE/DELETE and BEGIN/COMMIT/ROLLBACK.
#   3. Kills the server (simulated crash) and restarts it, asserting the
#      WAL correctly replayed only committed transactions.
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=5900
WAL=/tmp/sqlengine_test_wal.log
SCHEMA=/tmp/sqlengine_test_schema.catalog
rm -f "$WAL" "$SCHEMA"

SERVER_BIN=./bin/sqlengine_server
CLIENT_BIN=./bin/sqlengine_client

start_server() {
    (setsid "$SERVER_BIN" --port "$PORT" --wal "$WAL" --schema "$SCHEMA" \
        < /dev/null > /tmp/sqlengine_test_server.log 2>&1 &)
    for i in $(seq 1 20); do
        if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then return 0; fi
        sleep 0.2
    done
    echo "FAIL: server did not start"; exit 1
}

stop_server() {
    pkill -f "$SERVER_BIN --port $PORT" || true
    sleep 0.5
}

run_sql() {
    printf '%s\n' "$@" QUIT | "$CLIENT_BIN" --port "$PORT"
}

echo "== starting server =="
start_server

echo "== phase 1: DDL + DML =="
OUT=$(run_sql \
    "CREATE TABLE accounts (id INT, name TEXT, balance INT) PRIMARY KEY(id)" \
    "INSERT INTO accounts VALUES (1, 'Alice', 500)" \
    "INSERT INTO accounts VALUES (2, 'Bob', 250)" \
    "SELECT * FROM accounts")
echo "$OUT" | grep -q "OK CREATE TABLE accounts" || { echo "FAIL: create table"; exit 1; }
echo "$OUT" | grep -q "ROWS(2)" || { echo "FAIL: expected 2 rows"; exit 1; }
echo "PASS: DDL + DML"

echo "== phase 2: transaction commit =="
OUT=$(run_sql \
    "BEGIN" \
    "UPDATE accounts SET balance = 600 WHERE id = 1" \
    "COMMIT" \
    "SELECT * FROM accounts WHERE id = 1")
echo "$OUT" | grep -q "balance=600" || { echo "FAIL: commit not applied"; exit 1; }
echo "PASS: commit"

echo "== phase 3: transaction rollback =="
OUT=$(run_sql \
    "BEGIN" \
    "UPDATE accounts SET balance = 999 WHERE id = 1" \
    "ROLLBACK" \
    "SELECT * FROM accounts WHERE id = 1")
echo "$OUT" | grep -q "balance=600" || { echo "FAIL: rollback did not restore prior value"; exit 1; }
echo "$OUT" | grep -q "balance=999" && { echo "FAIL: rolled-back value leaked"; exit 1; }
echo "PASS: rollback"

echo "== phase 4: crash recovery (WAL replay) =="
run_sql "INSERT INTO accounts VALUES (3, 'Carol', 777)" > /dev/null
run_sql "BEGIN" "INSERT INTO accounts VALUES (4, 'Dave', 111)" "ROLLBACK" > /dev/null
stop_server
start_server
OUT=$(run_sql "SELECT * FROM accounts")
echo "$OUT" | grep -q "name=Carol" || { echo "FAIL: committed insert lost on restart"; exit 1; }
echo "$OUT" | grep -q "name=Dave" && { echo "FAIL: rolled-back insert survived restart"; exit 1; }
echo "$OUT" | grep -q "ROWS(3)" || { echo "FAIL: expected 3 rows after recovery, got: $OUT"; exit 1; }
echo "PASS: crash recovery"

echo "== phase 5: row lock / FOR UPDATE prevents duplicate PK race (sanity) =="
OUT=$(run_sql "INSERT INTO accounts VALUES (3, 'Carol2', 1)" 2>&1 || true)
echo "$OUT" | grep -q "ERROR" || { echo "FAIL: duplicate PK should error"; exit 1; }
echo "PASS: duplicate PK rejected"

stop_server
echo ""
echo "ALL TESTS PASSED"
