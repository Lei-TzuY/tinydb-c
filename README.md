# TinyDB C

A small SQLite-inspired relational database implemented in C.

The project focuses on database internals rather than SQL coverage. It includes:

- A pager and on-disk table storage
- B+ tree leaf/internal node behavior
- SQL parsing and VM execution for a compact statement set
- Inserts, selects, updates, deletes, ordering, and aggregates
- Transactions, WAL recovery, and checksum coverage
- Secondary-index experiments with durability tests

## Build

```powershell
cmake -S . -B build
cmake --build build
```

## Run

```powershell
.\build\Debug\tinydb.exe test.db
```

## Test

Run all 40 Python test suites:

```powershell
python tests\run_all.py
```

Current local audit: 40 test scripts passing (100% pass rate).

## Features & Commands

### REPL Meta Commands

- `.tables`: List database tables
- `.schema`: Print table & index schema
- `.btree`: Print visual B+ Tree structure
- `.constants`: Display storage engine constants (PAGE_SIZE, CELL_SIZE, MAX_KEYS)
- `.stats`: Display live database stats (pages, rows, WAL, transactions)
- `.buffer_pool` / `.cache`: Display Buffer Pool Manager & LRU eviction statistics
- `.page <n>`: Inspect physical page `<n>` headers, cell layout, and keys
- `.checkpoint`: Truncate WAL log file, flush dirty frames to database file, and reclaim space
- `.check`: Run full B+ tree, sibling links, and checksum integrity verifier
- `.help`: Display REPL help
- `.exit`: Save and exit database

### SQL Statements

- `INSERT INTO users VALUES (id, 'username', 'email');`
- `SELECT * FROM users [WHERE id >= A AND id <= B AND username LIKE 'p%' AND email LIKE '%s'] [ORDER BY id DESC] [LIMIT N];`
- `SELECT [<col>,] COUNT(*)|MIN(id)|MAX(id)|SUM(id)|AVG(id) FROM users [GROUP BY <col>] [HAVING <agg> <op> <val>];`
- `UPDATE users SET username = 'x' [, email = 'y'] WHERE id = N;`
- `DELETE FROM users WHERE id = N;` / `DELETE FROM users;`
- `CREATE INDEX <index_name> ON <table_name>(<column_name>);`
- `DROP INDEX <index_name>;`
- `VACUUM;`: Re-pack B+ tree pages and defragment disk storage
- `BEGIN;` / `COMMIT;` / `ROLLBACK;`: ACID Transactions via Write-Ahead Logging (WAL)
- `SAVEPOINT sp1;` / `ROLLBACK TO sp1;` / `RELEASE sp1;`: Nested Transaction Savepoints
- `CHECKPOINT;`: Manual Write-Ahead Log truncation and page flushing
- `PRAGMA integrity_check;`: Verify B+ tree node invariants, key sorting, and leaf pointer integrity
- `PRAGMA table_info;` / `PRAGMA table_info(users);`: Inspect table column schema catalog
- `PRAGMA index_list;` / `PRAGMA index_list(users);`: Inspect active secondary indexes metadata
- `PRAGMA user_version;` / `PRAGMA user_version = N;`: Read & write 4-byte database version metadata
- `EXPLAIN SELECT ...`: Show query plan

## Notes

This is an educational C database engine. It exposes storage-engine and query-execution mechanics including B+ Tree node splitting/merging, WAL crash recovery, page checksums, and secondary index persistence.

