# FluxQL | Multithreaded Database Engine

A from-scratch relational database engine with a TCP client-server protocol, ACID-ish transactions (`BEGIN`/`COMMIT`/`ROLLBACK`), write-ahead logging for crash resilience, explicit row-level locking for concurrency control, and an LRU-cached primary-key hash index for sub-millisecond point reads.

## **Highlights**

### **1. Multithreaded C++17 Client-Server Engine**

Multithreaded C++17 client-server engine over TCP. The server (`src/main_server.cpp`, `include/server.hpp`) spawns one worker thread per client connection and shares a single `Database` instance across all clients.

Concurrency correctness comes entirely from the locking primitives below rather than serializing clients.

### **2. BEGIN / COMMIT / ROLLBACK**

Full transaction lifecycle implemented in `include/database.hpp` and `include/sql_engine.hpp`.

Statements issued outside an explicit `BEGIN` run as single-statement autocommit transactions.

### **3. Write-Ahead Logging (WAL)**

The write-ahead log is implemented in `include/wal.hpp`.

Every mutation is first staged in memory under its owning transaction ID. Nothing touches disk until `COMMIT`.

At commit time:

- All staged records for the transaction are serialized.
- Records are written to the WAL.
- `fsync` is used for a durable flush.
- The staged mutations are then applied to the in-memory table state.

This provides a simple **group commit** mechanism.

`ROLLBACK` discards staged records without writing them to disk.

On startup, the WAL is replayed. Only transactions with a trailing `COMMIT` marker are reapplied, while incomplete transactions from a crash are discarded.

A companion schema catalog (`data/schema.catalog`) persists `CREATE TABLE` definitions so WAL replay can correctly deserialize rows.

### **4. Explicit Row-Level Locking**

Implemented in `include/lock_manager.hpp`.

The lock manager provides:

- Single-writer-per-row locking
- Locks keyed by `"table:pk"`
- Ownership tracked by transaction ID
- Condition-variable wait queues
- Configurable lock timeout

The row-lock subsystem is separate from the table's hash-index mutex.

The index mutex only protects the structural map operations (`insert`/`erase`/`find`), while row locks protect logical write ownership of a row across the entire transaction lifetime.

This prevents the index mutex from being held across multiple client round-trips and avoids unnecessarily serializing operations on unrelated rows.

### **5. Sub-Millisecond Point Queries**

Primary-key lookups use an `unordered_map` hash index implemented in `include/table.hpp`.

The index provides:

- **O(1) average** lookup
- **O(1) average** insertion
- **O(1) average** deletion
- `std::shared_mutex` for concurrent access

A **512-entry LRU cache** (`include/lru_cache.hpp`) sits in front of the hash index.

The cache uses commit-time invalidation:

- Cached rows written by a transaction are purged when that transaction commits.
- Readers therefore do not observe stale post-commit data.
- Transactions reading their own uncommitted writes bypass the cache and read the live table state.

## **Layout**

```text
include/
  common.hpp          Value/Row/Schema types and serialization
  lru_cache.hpp       Thread-safe fixed-capacity LRU cache
  lock_manager.hpp    Row-level lock manager
  wal.hpp             Write-ahead log: stage / commit / rollback / replay
  table.hpp           Primary-key hash-indexed table storage
  database.hpp        Tables + WAL + locks + cache + transactions
  sql_engine.hpp      Tokenizer + SQL statement executor
  server.hpp          Multithreaded TCP server and line protocol

src/
  main_server.cpp     Server entrypoint
                      Flags: --port --wal --schema

client/
  main_client.cpp     Interactive TCP client

tests/
  integration_test.sh End-to-end test with crash recovery

data/                 Schema catalog written at runtime
logs/                 WAL file written at runtime
```

## **Build**

Requires **g++ with C++17** and POSIX sockets on Linux/macOS.

No external dependencies are required.

```bash
make
```

This builds:

```text
bin/sqlengine_server
bin/sqlengine_client
```

## **Run**

Start the server:

```bash
./bin/sqlengine_server --port 5432
```

In another terminal, start the client:

```bash
./bin/sqlengine_client --port 5432
```

## **SQL Surface**

```sql
CREATE TABLE accounts (id INT, name TEXT, balance INT) PRIMARY KEY(id)

INSERT INTO accounts VALUES (1, 'Alice', 500)

SELECT * FROM accounts

SELECT * FROM accounts WHERE id = 1

SELECT * FROM accounts WHERE id = 1 FOR UPDATE

UPDATE accounts SET balance = 600 WHERE id = 1

DELETE FROM accounts WHERE id = 1

BEGIN

UPDATE accounts SET balance = balance - 100 WHERE id = 1

COMMIT

ROLLBACK

SHOW TABLES

STATS
```

`FOR UPDATE` locks the selected row for the current transaction.

`STATS` reports:

- Cache hit/miss counts
- WAL flush count
- Active row locks

`WHERE` predicates are supported only on the primary-key column because this engine uses a hash-indexed lookup model rather than a general query planner.

Full table scans are used for `SELECT *` queries without a `WHERE` clause.

## **Why `FOR UPDATE` Matters**

A read-modify-write transaction spanning multiple client round-trips can have a race condition:

```text
BEGIN
    ↓
SELECT balance
    ↓
Client computes new value
    ↓
UPDATE balance
    ↓
COMMIT
```

If the row lock is acquired only during `UPDATE`, another transaction can modify and commit the same row between the `SELECT` and `UPDATE`, potentially causing a lost update.

Using:

```sql
SELECT * FROM accounts WHERE id = 1 FOR UPDATE
```

acquires the row lock immediately during the read and keeps ownership for the duration of the transaction.

This serializes competing transactions on that row.

The behavior was verified under concurrent load:

- **10 threads**
- **50 concurrent increment transactions per thread**
- Shared target row
- Exact expected final value with `FOR UPDATE`
- Lost updates observed when `FOR UPDATE` was omitted

## **Testing**

Run the integration test suite:

```bash
make test
```

The test suite:

- Builds the project
- Starts the real server/client binaries
- Communicates over TCP
- Verifies DDL/DML correctness
- Verifies `COMMIT` durability
- Verifies `ROLLBACK` behavior
- Simulates crash recovery
- Restarts using the same WAL and schema catalog
- Verifies that only committed transactions are recovered
- Verifies primary-key uniqueness enforcement

Concurrency was additionally stress-tested with a Python harness that created dozens of real TCP client threads performing concurrent:

```text
BEGIN
SELECT ... FOR UPDATE
UPDATE
COMMIT
```

The stress tests verified:

- **Zero lost updates** across 500 concurrent transactions on a contended row
- Independent rows can proceed in parallel
- **20 rows × 30 transactions each** completed in approximately **50 ms across 20 threads**

## **Design Notes & Limitations**

### **Query Support**

Only single `=` predicates on the primary-key column are supported.

The engine does not currently support:

- Secondary indexes
- Joins
- Range scans
- General query planning

These features are outside the scope of the project, which focuses on transaction handling, concurrency control, caching, and crash recovery.

### **WAL Limitations**

WAL records are stored as binary, length-prefixed records in a single append-only file.

There is currently no:

- Log compaction
- Checkpointing
- Periodic snapshotting

Therefore, the WAL can grow indefinitely during long-running usage.

This is acceptable for a learning/demo database but would require periodic snapshotting or checkpointing in a production system.

### **Lock Manager Limitations**

The lock manager uses a fixed **5-second default timeout** rather than full deadlock detection.

If a transaction cannot acquire a required row lock within the timeout, it throws an error instead of waiting indefinitely.
