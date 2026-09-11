# Multithreaded C++17 Client-Server SQL Engine

A from-scratch relational database engine with a TCP client-server
protocol, ACID-ish transactions (BEGIN/COMMIT/ROLLBACK), write-ahead
logging for crash resilience, explicit row-level locking for
concurrency control, and an LRU-cached primary-key hash index for
sub-millisecond point reads.

## Highlights

- **Multithreaded C++17 client-server engine over TCP** — the server
  (`src/main_server.cpp`, `include/server.hpp`) spawns one worker
  thread per client connection and shares a single `Database`
  instance across all of them; concurrency correctness comes entirely
  from the locking primitives below, not from serializing clients.
- **BEGIN / COMMIT / ROLLBACK** — full transaction lifecycle
  (`include/database.hpp`, `include/sql_engine.hpp`). Statements
  issued outside an explicit `BEGIN` run as single-statement
  autocommit transactions.
- **Write-ahead log (WAL)** (`include/wal.hpp`) — every mutation is
  first *staged* in memory under its owning transaction id. Nothing
  touches disk until `COMMIT`, at which point all staged records for
  that transaction are serialized and `fsync`'d in one durable flush
  ("group commit"), then applied to in-memory table state.
  `ROLLBACK` discards staged records without ever touching disk. On
  startup, the WAL is replayed and only transactions with a trailing
  `COMMIT` marker are reapplied — incomplete transactions (crash
  mid-write) are silently discarded, giving crash resilience for
  free. A companion schema catalog (`data/schema.catalog`) persists
  `CREATE TABLE` definitions so replay knows how to deserialize rows.
- **Explicit row-level locking** (`include/lock_manager.hpp`) —
  single-writer-per-row locks keyed by `"table:pk"`, tracked by owning
  transaction id with a condition-variable wait queue and timeout.
  This is a **separate subsystem** from the table's hash-index mutex:
  the index mutex only ever protects the O(1) structural map
  operation itself (insert/erase/find), while row locks protect
  *logical* write ownership of a row across a transaction's entire
  lifetime (which may span many client round-trips). Conflating the
  two would mean holding the index mutex for a whole transaction,
  serializing every reader/writer on unrelated rows.
- **Sub-millisecond point queries** via a primary-key `unordered_map`
  hash index (`include/table.hpp`, O(1) average get/put/erase) guarded
  by a `std::shared_mutex` (shared for reads, exclusive for
  structural writes), plus a **512-entry LRU cache**
  (`include/lru_cache.hpp`) in front of it with **commit
  invalidation**: cached rows are purged the moment the transaction
  that wrote them durably commits, so no reader ever observes stale
  post-commit data, while a transaction reading its own uncommitted
  writes always bypasses the cache and reads live table state.

## Layout

```
include/
  common.hpp        Value/Row/Schema types, (de)serialization
  lru_cache.hpp      Thread-safe fixed-capacity LRU cache
  lock_manager.hpp   Row-level lock manager (separate from index mutex)
  wal.hpp            Write-ahead log: stage / commit / rollback / replay
  table.hpp          Primary-key hash-indexed table storage
  database.hpp       Ties tables + WAL + locks + cache + txns together
  sql_engine.hpp      Tokenizer + statement executor (SQL surface)
  server.hpp         Multithreaded TCP server, line protocol
src/
  main_server.cpp    Server entrypoint (flags: --port --wal --schema)
client/
  main_client.cpp    Interactive TCP client
tests/
  integration_test.sh  End-to-end test incl. simulated crash recovery
data/                Schema catalog written here at runtime
logs/                WAL file written here at runtime
```

## Build

Requires g++ with C++17 and POSIX sockets (Linux/macOS). No external
dependencies.

```bash
make            # builds bin/sqlengine_server and bin/sqlengine_client
```

## Run

```bash
./bin/sqlengine_server --port 5432
# in another terminal:
./bin/sqlengine_client --port 5432
```

## SQL surface

```sql
CREATE TABLE accounts (id INT, name TEXT, balance INT) PRIMARY KEY(id)
INSERT INTO accounts VALUES (1, 'Alice', 500)
SELECT * FROM accounts
SELECT * FROM accounts WHERE id = 1
SELECT * FROM accounts WHERE id = 1 FOR UPDATE   -- locks the row for this txn
UPDATE accounts SET balance = 600 WHERE id = 1
DELETE FROM accounts WHERE id = 1

BEGIN
UPDATE accounts SET balance = balance - 100 WHERE id = 1   -- (compute client-side)
COMMIT | ROLLBACK

SHOW TABLES
STATS            -- cache hit/miss counts, WAL flush count, active row locks
```

`WHERE` predicates are supported only on the primary-key column
(this is a hash-indexed engine, not a general query planner). Full
table scans are used for `SELECT *` without a `WHERE` clause.

### Why `FOR UPDATE` matters

A read-modify-write transaction spanning multiple round trips
(`BEGIN` → `SELECT balance` → client computes new value → `UPDATE`)
has a race: the row lock is normally only acquired at `UPDATE` time,
so a concurrent transaction could commit an update in between,
producing a lost update. `SELECT ... FOR UPDATE` acquires the row
lock immediately at read time, serializing the whole sequence on that
row for the duration of the transaction. This was verified under
load: 10 threads issuing 50 concurrent increment-transactions each on
one row landed on the exact expected total with `FOR UPDATE`, and
lost updates without it.

## Testing

```bash
make test
```

Runs `tests/integration_test.sh`, which builds the project, drives
the real server/client binaries over TCP, and verifies:
- DDL/DML correctness
- COMMIT durability and ROLLBACK undo
- **crash recovery**: kills the server mid-session and restarts it
  against the same WAL + schema catalog, asserting only committed
  transactions reappear
- primary-key uniqueness enforcement

Concurrency was additionally stress-tested manually with a Python
harness spinning up dozens of real TCP client threads doing
concurrent `BEGIN`/`SELECT ... FOR UPDATE`/`UPDATE`/`COMMIT` cycles
against shared and independent rows, confirming: (a) zero lost
updates on a contended row across 500 concurrent transactions, and
(b) independent rows proceed with true parallelism (20 rows × 30
transactions each completed in ~50ms across 20 threads).

## Design notes / limitations

- Single `=` predicates on the primary key only; no secondary
  indexes, joins, or range scans (out of scope for this engine's
  focus on transaction/locking/caching mechanics).
- WAL records are binary, length-prefixed, one file, append-only;
  there's no log compaction/checkpointing, so the file grows
  unboundedly over a long-lived server — fine for a demo/learning
  engine, would need periodic snapshotting in production.
- Lock manager uses a fixed timeout (5s default) rather than full
  deadlock detection; a transaction that can't acquire a row lock in
  time throws rather than hanging forever.
