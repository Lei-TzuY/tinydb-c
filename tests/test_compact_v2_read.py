import glob
import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_compact_v2_read_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_compact_v2_read_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_compact_v2_read_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_compact_v2_read_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_compact_v2_read_probe executable")

    db_path = os.path.join(os.path.dirname(__file__), "test_compact_v2_read.db")
    cleanup(db_path)
    try:
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=60,
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise AssertionError(output)
        marker = "PASS: production existing-row UPDATE shrinks/grows compact V2 payloads"
        if marker not in output:
            raise AssertionError(f"missing compact V2 production-update marker\n{output}")
        print(
            "PASS: production generic UPDATE rewrites variable-length compact V2 "
            "rows in mixed trees with rollback/reopen durability while keeping "
            "structural mutation fail-closed"
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
