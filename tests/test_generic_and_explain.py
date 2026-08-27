import glob
import os
import re
import subprocess
import sys


def find_tinydb(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_file, commands):
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def require_metrics(output):
    pattern = re.compile(
        r"ANALYZE: execution_time_ms=[0-9.]+ "
        r"cache_hits=\d+ cache_misses=\d+ evictions=\d+ page_accesses=\d+"
    )
    if pattern.search(output) is None:
        raise AssertionError(f"missing ANALYZE metrics\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_and_explain.db")
    cleanup(db_file)

    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'keyboard', 2599);",
                "INSERT INTO products VALUES (2, 'mouse', 1299);",
                "INSERT INTO products VALUES (3, 'cable', 399);",
                "INSERT INTO products VALUES (4, 'monitor', 4999);",
                "CREATE INDEX idx_products_price ON products(price);",
                ".exit",
            ],
        )
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        scan = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND price <= 3000;",
                ".exit",
            ],
        )
        require(scan, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(scan, "PROJECTION: name")
        require(scan, "FILTER: price >= 1000 AND price <= 3000")
        if "ACTUAL RESULT" in scan:
            raise AssertionError("plain compound EXPLAIN executed the query\n" + scan)

        pk = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE id = 2 AND price >= 1000;",
                ".exit",
            ],
        )
        require(pk, "PLAN: GENERIC PRIMARY KEY LOOKUP")
        require(pk, "FILTER: id = 2 AND price >= 1000")

        indexed_residual = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price = 1299 AND id > 1;",
                "SELECT name FROM products WHERE price = 1299 AND id > 1;",
                ".exit",
            ],
        )
        require(indexed_residual, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(indexed_residual, "FILTER: price = 1299 AND id > 1")
        require(indexed_residual, "mouse")
        if "PLAN: GENERIC SECONDARY INDEX LOOKUP" in indexed_residual:
            raise AssertionError(
                "compound predicate was incorrectly promoted to single-term index plan\n"
                + indexed_residual
            )

        disjunction = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 4000 OR price <= 500;",
                "SELECT name FROM products WHERE price >= 4000 OR price <= 500;",
                ".exit",
            ],
        )
        require(disjunction, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(disjunction, "FILTER: price >= 4000 OR price <= 500")
        require(disjunction, "monitor")
        require(disjunction, "cable")
        if "PLAN: GENERIC SECONDARY INDEX LOOKUP" in disjunction:
            raise AssertionError(
                "OR predicate was incorrectly promoted to a single-term index plan\n"
                + disjunction
            )

        analyzed = run_session(
            executable,
            db_file,
            [
                "EXPLAIN ANALYZE SELECT name FROM products WHERE price >= 1000 AND price <= 3000;",
                "EXPLAIN ANALYZE SELECT name FROM products WHERE id = 1 OR price >= 4000;",
                ".exit",
            ],
        )
        require(analyzed, "QUERY PLAN")
        require(analyzed, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(analyzed, "FILTER: price >= 1000 AND price <= 3000")
        require(analyzed, "FILTER: id = 1 OR price >= 4000")
        require(analyzed, "ACTUAL RESULT")
        require(analyzed, "keyboard")
        require(analyzed, "mouse")
        require(analyzed, "monitor")
        require_metrics(analyzed)

        invalid = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT * FROM products WHERE price >= 1000 AND price <= '3000';",
                ".exit",
            ],
        )
        if invalid.count("Syntax error. Could not parse statement.") < 1:
            raise AssertionError("invalid typed EXPLAIN predicate was accepted\n" + invalid)

        print(
            "PASS: generic EXPLAIN/ANALYZE renders AND and OR predicates, preserves "
            "PK equality lookup for conjunctive filters, keeps residual/disjunctive "
            "predicates on the scan path, and rejects invalid typed filters."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
