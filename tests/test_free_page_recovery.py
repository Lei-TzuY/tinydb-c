import glob
import os
import struct
import subprocess
import sys
import time


FREE_MAGIC = 0x46524545
FREE_VERSION = 1
PAGE_SIZE = 4096


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


def read_free_sidecar(path):
    with open(path, "rb") as file:
        payload = file.read()
    if len(payload) < 20:
        raise AssertionError("free-page sidecar is truncated")

    magic, version, num_pages, count = struct.unpack_from("<IIII", payload, 0)
    expected_length = 20 + count * 4
    if len(payload) != expected_length:
        raise AssertionError(
            f"free-page sidecar length mismatch: expected {expected_length}, got {len(payload)}"
        )
    pages = list(struct.unpack_from(f"<{count}I", payload, 16)) if count else []
    checksum = struct.unpack_from("<I", payload, 16 + count * 4)[0]
    return magic, version, num_pages, pages, checksum


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_free_page_recovery.db")
    free_file = db_file + ".free"
    free_wal_file = db_file + ".free.wal"
    wal_file = db_file + ".wal"
    cleanup(db_file)

    try:
        process = subprocess.Popen(
            [executable, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        assert process.stdin is not None
        commands = ["CREATE TABLE metrics (id INT, value INT, tag VARCHAR);"]
        commands.extend(
            f"INSERT INTO metrics VALUES ({i}, {i * 10}, 'tag-{i}');"
            for i in range(1, 61)
        )
        commands.extend(
            f"DELETE FROM metrics WHERE id = {i};"
            for i in range(1, 56)
        )
        process.stdin.write("\n".join(commands) + "\n")
        process.stdin.flush()
        time.sleep(2.0)
        process.kill()
        process.wait(timeout=30)

        if not os.path.exists(wal_file):
            raise AssertionError("main WAL missing after hard kill")
        if not os.path.exists(free_file):
            raise AssertionError("free-page sidecar was not published before hard kill")

        # Simulate losing the allocator sidecar while retaining the durable
        # main WAL. WAL2 must contain enough committed allocator state to
        # reconstruct it during database recovery.
        os.remove(free_file)
        if os.path.exists(free_wal_file):
            os.remove(free_wal_file)

        recovered = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM metrics;",
                "PRAGMA integrity_check;",
                ".stats metrics",
                ".exit",
            ],
        )
        if "WAL file found. Recovering..." not in recovered:
            raise AssertionError("database did not run WAL recovery")
        if "db > 5\nExecuted." not in recovered:
            raise AssertionError(recovered)
        if "page ownership:" in recovered or "\nok\n" not in recovered:
            raise AssertionError(recovered)
        if not os.path.exists(free_file):
            raise AssertionError("WAL recovery did not recreate free-page sidecar")

        magic, version, num_pages, free_pages, _ = read_free_sidecar(free_file)
        physical_pages = os.path.getsize(db_file) // PAGE_SIZE
        if magic != FREE_MAGIC or version != FREE_VERSION:
            raise AssertionError("unexpected free-page sidecar format")
        if num_pages != physical_pages:
            raise AssertionError(
                f"sidecar page count {num_pages} does not match database {physical_pages}"
            )
        if not free_pages:
            raise AssertionError("expected recovered reusable pages after tree collapse")
        if len(free_pages) != len(set(free_pages)):
            raise AssertionError("recovered free-page list contains duplicates")
        if any(page == 0 or page >= num_pages for page in free_pages):
            raise AssertionError("recovered free-page list contains an invalid page")

        size_before_reuse = os.path.getsize(db_file)
        refill_commands = [
            f"INSERT INTO metrics VALUES ({i}, {i * 10}, 'refill-{i}');"
            for i in range(61, 81)
        ]
        refill_commands.extend([
            "SELECT COUNT(*) FROM metrics;",
            "PRAGMA integrity_check;",
            ".exit",
        ])
        refilled = run_session(executable, db_file, refill_commands)
        if "db > 25\nExecuted." not in refilled or "\nok\n" not in refilled:
            raise AssertionError(refilled)
        if os.path.getsize(db_file) != size_before_reuse:
            raise AssertionError(
                "database grew even though recovered free pages should have been reusable"
            )

        print(
            "PASS: WAL2 reconstructs durable free-page state after crash and reused pages avoid file growth."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
