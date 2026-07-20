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

The tests are Python scripts that build and drive the database binary.

```powershell
python tests\test_in_memory.py
python tests\test_btree_leaf.py
python tests\test_btree_split.py
python tests\test_btree_random_insert.py
python tests\test_delete.py
python tests\test_delete_all.py
python tests\test_free_page.py
python tests\test_disk_pager.py
python tests\test_wal_atomicity.py
python tests\test_wal_recovery.py
python tests\test_checksum.py
python tests\test_sql_compiler.py
python tests\test_select_advanced.py
python tests\test_update.py
python tests\test_transactions.py
python tests\test_order_by.py
python tests\test_aggregates.py
python tests\test_repl.py
python tests\test_secondary_index.py
python tests\test_node_merge.py
python tests\test_vm_skeleton.py
```

Current local audit: 21 test scripts passing.

## Notes

This is a learning database. It is intended to expose storage-engine and query-execution mechanics, not to replace SQLite or another production database.

