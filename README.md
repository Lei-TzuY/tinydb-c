# TinyDB C

An educational relational database engine written in C. TinyDB implements a disk-backed B+ tree, write-ahead logging, transactions, secondary indexes, a buffer pool, a SQL execution layer, and a growing set of relational features in one compact codebase.

> **Goal:** explore how storage, recovery, indexing and query execution fit together — not replace SQLite or claim production-level performance.

## Verification at a glance

- **66 / 66 automated test suites passing** in the current project checkpoint
- persistent B+ tree storage with split / merge behavior
- WAL-based recovery and explicit checkpointing
- secondary and composite indexes
- transactions and savepoints
- page checksums and integrity verification
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
- buffer-pool LRU caching
- page inspection and B+ tree visualization from the REPL

### Transactions and recovery

- write-ahead logging (WAL)
- commit / rollback
- savepoints
- log replay for recovery
- manual checkpointing
- backup / compaction workflows such as `VACUUM` and `VACUUM INTO`

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
- a small full-text-search path
- built-in string, math and system functions

This is intentionally a teaching-oriented SQL surface rather than a claim of SQL-standard completeness.

## Build

CMake is used for the native build.

```sh
cmake -S . -B build
cmake --build build
```

Example REPL launch on Windows:

```powershell
.\build\Debug\tinydb.exe my_database.db
```

The exact executable path depends on the generator / platform used by CMake.

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
.buffer_pool / .cache   inspect cache state
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
- durability / crash semantics should be evaluated against the tests and documented recovery paths, not inferred from feature names alone.
- performance claims require benchmarks against defined workloads; this README deliberately avoids calling the engine "high-performance" without that evidence.
- concurrency, locking and production-hardening expectations are different from mature database systems.

The strongest signal in this repository is the implementation + testable invariants, not the number of supported SQL keywords.

## License

MIT — see [LICENSE](LICENSE).
