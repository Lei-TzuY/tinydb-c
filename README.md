# TinyDB C

An educational relational database engine written in C. TinyDB implements a disk-backed B+ tree, write-ahead logging, transactions, secondary indexes, a buffer pool, a SQL execution layer, and a growing set of relational features in one compact codebase.

> **Goal:** explore how storage, recovery, indexing and query execution fit together — not replace SQLite or claim production-level performance.

## Verification at a glance

- automated regression suites covering SQL, B+ tree, WAL, checksums, indexes and recovery paths
- persistent B+ tree storage with split / merge behavior
- growable heap-backed Pager metadata instead of a fixed page-table ceiling
- no-steal buffer eviction: uncommitted dirty pages are never evicted into the main database file
- WAL-based recovery and explicit checkpointing
- secondary and composite indexes
- transactions and savepoints
- page checksums and integrity verification
- native benchmark runner with text and JSON output
- deterministic repeated crash/recovery stress runner
- regression coverage that writes and reopens more than 4096 database pages
- large uncommitted-transaction crash coverage that forces eviction beyond the 16-frame buffer pool
- offline binary page/checksum inspector that does not depend on opening the DB through TinyDB
- `EXPLAIN ANALYZE` query profiling with execution time and buffer-pool deltas
- REPL inspection commands for pages, B+ trees, cache state and schema metadata

Run the complete suite with:

```sh
python tests/run_all.py
```

## Architecture

```text
SQL / REPL
    |
    v
Parser / planner / execution
    |
    +--------------------+
    |                    |
    v                    v
Indexes / catalog    Transactions
    |                    |
    +---------+----------+
              v
        Buffer pool
              |
       +------+------+
       |             |
       v             v
  dirty spill     clean pages
  (no-steal)          |
       |              |
       +------+-------+
              v
          B+ tree
              |
       +------+------+
       |             |
       v             v
     WAL          DB pages
```

The interesting part of the project is the interaction between these layers. A feature is only useful if the storage engine, index metadata, transaction path and recovery behavior agree on the same state.

## Core subsystems

### Storage engine

- disk-backed B+ tree
- internal / leaf node splitting, borrowing and merging
- page-level checksums
- physical integrity checking
- 16-frame buffer-pool LRU caching
- heap-backed page table, dirty map and free-page list that grow on demand
- metadata capacity starts at `PAGER_INITIAL_CAPACITY` (64 pages by default) and doubles as required
- 32-bit page numbers remain the logical page identifier; the old `TABLE_MAX_PAGES=4096` value is no longer the Pager's hard page ceiling
- 64-bit file offsets for database positioning and truncation
- page inspection and B+ tree visualization from the REPL

`TABLE_MAX_PAGES` is currently retained as a legacy compatibility/sanity constant for older auxiliary-file validation. Code that needs the current Pager allocation should use `pager_metadata_capacity()` rather than treating `TABLE_MAX_PAGES` as database capacity.

### Transactions and recovery

- write-ahead logging (WAL)
- commit / rollback
- savepoints
- log replay for recovery
- manual checkpointing
- no-steal dirty-page spill when an uncommitted page is evicted from the 16-frame buffer pool
- committed shadow pages keep post-WAL state visible before checkpoint without writing uncommitted state to the main file
- WAL transaction records include the final logical page count so shrink/truncate state can be recovered after a crash
- backward recovery support for the repository's previous WAL record layout
- exact transaction free-page-list snapshot/restore on rollback
- backup / compaction workflows such as `VACUUM` and `VACUUM INTO`
- deterministic crash/recovery stress testing for committed and uncommitted transactions

### Indexing and catalog

- primary and secondary B+ tree indexes
- composite indexes
- persisted schema / index metadata
- `sqlite_master`-style catalog inspection
- automatic index selection where supported by the current query path

### Query layer

The SQL layer includes common DDL / DML operations plus selected higher-level features such as:

- `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, `SELECT`
- filtering predicates
- `ORDER BY`, `GROUP BY`, `HAVING`, `LIMIT` / `OFFSET`
- scalar subqueries and `IN` / `EXISTS`
- views and common table expressions
- `UNION` / `UNION ALL`
- window functions such as `ROW_NUMBER()`
- `EXPLAIN` and `EXPLAIN ANALYZE`
- a small full-text-search path
- built-in string, math and system functions

This is intentionally a teaching-oriented SQL surface rather than a claim of SQL-standard completeness.

## Build

CMake is used for the native build. The engine implementation is built as the reusable `tinydb_core` static library, with separate REPL, benchmark and pager-growth probe executables linked against it.

```sh
cmake -S . -B build
cmake --build build
```

Example REPL launch on Windows:

```powershell
.\build\Debug\tinydb.exe my_database.db
```

The exact executable path depends on the generator / platform used by CMake.

## Benchmark

`tinydb_bench` provides a reproducible storage-engine workload instead of relying on unverified performance claims. It performs one transactional insert batch followed by randomized primary-key lookups, verifies every lookup, and reports throughput plus page / buffer-pool statistics.

Unix-like build layout:

```sh
./build/tinydb_bench benchmark.db 5000 20000
```

Windows multi-config build layout:

```powershell
.\build\Debug\tinydb_bench.exe benchmark.db 5000 20000
```

Arguments are `DATABASE ROWS LOOKUPS`. The benchmark refuses to overwrite an existing database path so a previous result cannot silently contaminate a new run.

Example text output fields include:

```text
insert: inserted=5000 seconds=... rows_per_sec=...
lookup: requested=20000 lookup_hits=20000 seconds=... lookups_per_sec=...
storage: pages=... leaf_pages=... internal_pages=... rows=5000 metadata_capacity=...
cache_lookup_phase: hits=... misses=... evictions=...
BENCHMARK_OK
```

Machine-readable output is available with `--json`:

```sh
./build/tinydb_bench benchmark.db 5000 20000 --json
```

The JSON payload includes workload counts, throughput, storage-page counts, lookup cache metrics, `dynamic_page_table`, `initial_metadata_capacity`, current `metadata_capacity`, the retained `legacy_page_ceiling`, and an `ok` correctness flag. This is intended for automated before/after regression analysis.

Use the benchmark for before/after comparisons on the same machine, compiler, build type, dataset size and storage device. Cross-machine numbers are not directly comparable without controlling those variables.

## Query profiling with EXPLAIN ANALYZE

Plain `EXPLAIN` only reports the chosen execution path and does not run the query:

```sql
EXPLAIN SELECT * FROM users WHERE id = 42;
```

`EXPLAIN ANALYZE` prints the same plan, executes the `SELECT`, and then reports execution-time and buffer-pool deltas measured around the real VM execution path:

```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE id = 42;
```

Representative output:

```text
QUERY PLAN
PLAN: PRIMARY KEY LOOKUP (id = 42)
ACTUAL RESULT
(42, user42, u42@example.com)
ANALYZE: execution_time_ms=... cache_hits=... cache_misses=... evictions=... page_accesses=...
Executed.
```

The counters are deltas for that query execution, not process-lifetime totals. `page_accesses` is the sum of buffer-pool hits and misses during the measured query. This makes it useful for controlled before/after comparisons such as creating an index and checking whether the selected path and page-access profile change.

For example:

```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE email = 'alice@example.com';
CREATE INDEX idx_users_email ON users(email);
EXPLAIN ANALYZE SELECT * FROM users WHERE email = 'alice@example.com';
```

Timing uses the C runtime clock and should be treated as local diagnostic evidence rather than a cross-machine benchmark. Use `tinydb_bench` for larger repeatable workload comparisons.

## Pager growth and eviction regression tests

The test suite now contains two targeted storage regressions in addition to normal SQL tests.

`tinydb_pager_growth_probe` writes pages `0..4128`, deliberately crossing the historical 4096-page boundary while a 16-frame buffer pool repeatedly evicts pages. It commits, checkpoints, reopens the database, and verifies markers on both sides of the old boundary. The Python wrapper also checks that the physical file contains all 4129 pages.

`test_large_transaction_crash.py` seeds committed data, starts one explicit transaction, inserts 700 additional rows (enough to dirty far more pages than fit in the buffer pool), then hard-kills the process before `COMMIT`. Reopening must show only the previously committed rows and must still pass `PRAGMA integrity_check`. This specifically protects the no-steal invariant: buffer eviction must not make uncommitted state durable.

## Crash / recovery stress runner

`tools/recovery_stress.py` repeatedly injects hard process kills against the same database. Every round has two phases:

1. `BEGIN` + inserts + `COMMIT`, followed by an immediate kill without graceful `.exit`; all committed rows must survive recovery.
2. `BEGIN` + inserts, followed by a kill before `COMMIT`; none of those rows may become visible.

After each recovery it reopens the database, checks the complete expected/forbidden row set, and runs `PRAGMA integrity_check`. The workload uses a fixed seed by default so failures are reproducible.

```sh
python tools/recovery_stress.py --iterations 20 --rows-per-round 2 --seed 1337
```

For a shorter smoke run:

```sh
python tools/recovery_stress.py --iterations 3 --rows-per-round 2
```

The runner creates a temporary database unless `--db` is supplied and refuses to overwrite an existing explicit database path.

## Offline page inspector

`tools/page_inspect.py` reads a TinyDB file directly without going through the pager. This is useful for damaged-database inspection because it can still report page metadata even when TinyDB itself refuses to open the file.

It currently checks:

- file/page alignment
- per-page FNV-1a checksum
- root/internal/leaf page type and header fields
- leaf key ordering and duplicate keys
- internal separator ordering
- basic leaf sibling and internal child pointer bounds
- total rows, page counts and checksum failures

Text summary:

```sh
python tools/page_inspect.py my_database.db --pages --strict
```

Machine-readable report:

```sh
python tools/page_inspect.py my_database.db --json --strict
```

`--strict` returns a non-zero exit code when the inspector finds a checksum, header, key-order or pointer-range problem.

## Example

```sql
CREATE TABLE users (
    id INT PRIMARY KEY,
    username VARCHAR(32),
    email VARCHAR(64)
);

CREATE INDEX idx_users_email ON users(email);

INSERT INTO users VALUES (1, 'alice', 'alice@example.com');
INSERT INTO users VALUES (2, 'bob',   'bob@example.com');

SELECT *
FROM users
WHERE id BETWEEN 1 AND 100
ORDER BY id DESC;
```

Transactions and savepoints:

```sql
BEGIN;
SAVEPOINT before_update;
UPDATE users SET username = 'alice_new' WHERE id = 1;
ROLLBACK TO before_update;
COMMIT;
```

## REPL inspection tools

TinyDB exposes internal state instead of hiding it, which makes the project easier to debug and learn from.

```text
.tables                 list tables
.schema                 inspect schemas and indexes
.btree                  visualize B+ tree structure
.page <n>               inspect a physical page
.stats                  show storage / WAL / transaction metrics
.buffer_pool / .cache   inspect cache state, including dynamic metadata capacity
.check                   run structural / checksum verification
.checkpoint              flush / checkpoint WAL state
```

## Selected SQL examples

```sql
CREATE VIEW active_users AS
SELECT * FROM users WHERE id > 0;

WITH active AS (
    SELECT * FROM users WHERE id > 0
)
SELECT * FROM active;

SELECT id,
       ROW_NUMBER() OVER (PARTITION BY username ORDER BY id)
FROM users;

SELECT *
FROM users
WHERE id IN (SELECT id FROM users WHERE id > 10);

VACUUM;
VACUUM INTO 'backup.db';
PRAGMA integrity_check;
```

## Scope and limitations

TinyDB is an educational database engine. The project is useful for studying implementation tradeoffs, but it should not be described as a drop-in SQLite replacement or a production database.

In particular:

- SQL coverage is intentionally incomplete.
- the Pager now grows its in-memory metadata on demand, but page identifiers are still 32-bit and mature databases use more sophisticated allocation/free-space structures.
- the no-steal implementation uses heap-backed page shadows for educational clarity; it is not intended to model the memory efficiency of a production buffer manager.
- durability / crash semantics should be evaluated against the tests and documented recovery paths, not inferred from feature names alone.
- performance claims require benchmarks against defined workloads; this repository includes a benchmark harness, but results still depend on hardware, compiler settings, build type and workload.
- the offline inspector performs structural sanity checks but is not a formal proof that every possible B+ tree invariant holds.
- concurrency, locking and production-hardening expectations are different from mature database systems.

The strongest signal in this repository is the implementation + testable invariants, not the number of supported SQL keywords.

## License

MIT — see [LICENSE](LICENSE).
