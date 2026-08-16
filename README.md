# TinyDB C

A high-performance, SQLite-inspired relational database engine written in C, featuring B+ Tree indexing, Write-Ahead Logging (WAL) for ACID compliance, Buffer Pool LRU caching, Full-Text Search (FTS), View/CTE engines, Window Functions, and a rich SQL Virtual Machine.

[![Build & Test](https://img.shields.io/badge/Tests-66%20Passed%20(100%25)-brightgreen.svg)](#test)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

---

## Key Architecture & Features

### 📦 Storage Engine & Memory Management
- **B+ Tree Storage Engine**: Variable-capacity internal & leaf node splitting, borrowing, and merging.
- **Buffer Pool Manager**: Dynamic LRU cache eviction policy for page pin/unpin and disk I/O reduction.
- **Write-Ahead Logging (WAL)**: Full ACID transaction guarantees, crash-recovery log replay, and frame checkpointing.
- **Checksum & Integrity Verification**: 32-bit CRC checksums per page and physical node invariant verifier (`PRAGMA integrity_check`).
- **Disk Defragmentation (`VACUUM`)**: Dynamic B+ tree repacking and online database hot backups (`VACUUM INTO 'backup.db'`).

### ⚡ Query Engine & SQL Compiler
- **Primary & Secondary B+ Tree Indexing**: Automatic index selection, composite multi-column indexing, and secondary index persistence (`CREATE INDEX`).
- **Common Table Expressions (CTE)**: Virtual `WITH cte_name AS (...)` query evaluation.
- **Database Views**: System catalog-backed `CREATE VIEW` and `DROP VIEW` execution.
- **Window Functions**: Partitioned analytics with `ROW_NUMBER() OVER (PARTITION BY ... ORDER BY ...)`.
- **Full-Text Search (FTS)**: Inverted term index posting lists with instant keyword lookup (`WHERE MATCH 'term'`).
- **Set Operations**: `UNION` and `UNION ALL` result set merging.
- **Subquery Engine**: Embedded scalar subqueries in `SELECT` projections and filter predicates (`WHERE id IN (SELECT ...)` / `WHERE EXISTS (...)`).
- **Rich Predicates**: `BETWEEN ... AND ...`, `IN (...)`, `NOT IN (...)`, `IS NULL`, `IS NOT NULL`, `LIKE`, `NOT LIKE`, `ILIKE` (case-folded matching).
- **Pagination & Sorting**: Multi-column sorting (`ORDER BY col1 ASC, col2 DESC`) and `LIMIT N OFFSET M` pagination.
- **Built-in Functions**:
  - **String**: `LENGTH()`, `UPPER()`, `LOWER()`, `CONCAT()`
  - **Math**: `ABS()`, `MOD()`
  - **System**: `VERSION()`, `DATABASE()`, `sqlite_master` catalog tables

---

## Build & Usage

### 1. Building the Project (CMake + MSVC/GCC/Clang)

```powershell
cmake -S . -B build
cmake --build build
```

### 2. Running the REPL Interface

```powershell
.\build\Debug\tinydb.exe my_database.db
```

### 3. Running All 66 Test Suites

```powershell
python tests\run_all.py
```

> **Test Coverage Summary**: All **66 / 66** automated Python test suites pass with 100% success rate.

---

## REPL Commands & SQL Syntax Reference

### REPL Meta Commands

- `.tables`: List active database tables
- `.schema`: Display catalog table schemas & indexes
- `.btree`: Visualize physical B+ Tree page hierarchy
- `.constants`: Show storage engine page/cell layout constants
- `.stats`: Display live page, row, WAL, and transaction metrics
- `.buffer_pool` / `.cache`: Inspect Buffer Pool Manager hit rates & LRU cache status
- `.page <n>`: Inspect physical page `<n>` headers, cell layout, and keys
- `.checkpoint`: Manual Write-Ahead Log truncation and page flushing
- `.check`: Execute deep B+ tree invariant, sibling link, and checksum verifier
- `.help`: Display command list
- `.exit`: Flush changes and close database cleanly

### Supported SQL Expressions

```sql
-- DDL & Views
CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR(32), price INT);
CREATE VIEW v_users AS SELECT * FROM users;
DROP VIEW v_users;
CREATE INDEX idx_users_email ON users(email);
DROP INDEX idx_users_email;

-- DML & Auto Increment
INSERT INTO users VALUES (1, 'alice', 'alice@example.com');
INSERT INTO users (username, email) VALUES ('bob', 'bob@example.com'); -- AUTO_INCREMENT ID
UPDATE users SET username = 'alice_new' WHERE id = 1;
DELETE FROM users WHERE id = 1;

-- Advanced Querying & Functions
SELECT id, (SELECT COUNT(*) FROM users) FROM users;
SELECT CONCAT(username, email), UPPER(username), LENGTH(email) FROM users;
SELECT ABS(id), MOD(id, 2) FROM users;
SELECT VERSION(), DATABASE();
SELECT * FROM sqlite_master;

-- Filtering & Pagination
SELECT * FROM users WHERE username ILIKE '%alice%' LIMIT 10 OFFSET 5;
SELECT * FROM users WHERE id BETWEEN 1 AND 100 AND username NOT IN ('charlie', 'david');
SELECT username, COUNT(*) FROM users GROUP BY username HAVING COUNT(*) > 1 ORDER BY username ASC, id DESC;

-- Set Operations & CTEs
WITH active_users AS (SELECT * FROM users WHERE id > 0) SELECT * FROM active_users;
SELECT * FROM users WHERE id = 1 UNION ALL SELECT * FROM users WHERE id = 2;

-- Window Functions
SELECT id, username, ROW_NUMBER() OVER (PARTITION BY username ORDER BY id ASC) FROM users;

-- Transactions & Savepoints
BEGIN;
SAVEPOINT sp1;
ROLLBACK TO sp1;
RELEASE sp1;
COMMIT;

-- Maintenance & Backup
VACUUM;
VACUUM INTO 'backup.db';
PRAGMA integrity_check;
PRAGMA table_info(users);
PRAGMA index_list(users);
PRAGMA user_version = 1;
```

---

## License

This project is licensed under the [MIT License](LICENSE).
