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
    process = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if process.returncode != 0:
        raise AssertionError(process.stdout + "\n" + process.stderr)
    return process.stdout + process.stderr


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_create_table_policy.db")
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE too_wide (id INT, left_text VARCHAR, right_text VARCHAR);",
                "CREATE TABLE metadata_only (key INT, name VARCHAR);",
                ".tables",
                "CREATE TABLE metrics (id INT, name VARCHAR, price INT, stock INT);",
                "INSERT INTO metrics VALUES (1, 'alpha', 10, 3);",
                "INSERT INTO metrics VALUES (2, 'beta', 25, 0);",
                "INSERT INTO metrics VALUES (3, 'gamma', 40, 7);",
                "SELECT COUNT(*) FROM metrics WHERE price >= 20 AND stock >= 0;",
                "SELECT name FROM metrics WHERE id = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(
            first,
            "CREATE TABLE row layout exceeds the fixed generic record slot; variable-size/slotted-page rows are not implemented",
        )
        if "too_wide" in re.sub(
            r"Error: CREATE TABLE row layout exceeds[^\n]*\n", "", first
        ):
            raise AssertionError("too-wide executable table leaked into catalog output\n" + first)
        require(first, "metadata_only")
        require(first, "beta")
        require(first, "ok")
        if scalar_results(first) != [2]:
            raise AssertionError("valid mixed-layout query returned wrong count\n" + first)

        reopened = run_session(
            executable,
            db_file,
            [
                ".tables",
                "PRAGMA table_info(metrics);",
                "SELECT COUNT(*) FROM metrics WHERE stock >= 0;",
                "SELECT COUNT(*) FROM metrics WHERE name LIKE '%a';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(reopened, "metrics")
        require(reopened, "metadata_only")
        require(reopened, "name")
        require(reopened, "price")
        require(reopened, "stock")
        if "too_wide" in reopened:
            raise AssertionError("rejected executable schema was persisted across reopen\n" + reopened)
        if scalar_results(reopened) != [3, 3]:
            raise AssertionError("valid mixed-layout rows did not survive reopen\n" + reopened)
        require(reopened, "ok")

        print(
            "PASS: CREATE TABLE rejects id-INT generic schemas that exceed the fixed record "
            "slot before catalog persistence, preserves historical metadata-only schema "
            "DDL, and keeps a mixed VARCHAR/INT executable layout durable after reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
