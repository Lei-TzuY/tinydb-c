import glob
import os
import subprocess
import sys


def find_tinydb(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb executable")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)
    return output


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def projected_ids(output):
    values = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line.startswith("db > "):
            line = line[5:].strip()
        if line.isdigit():
            values.append(int(line))
    return values


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def write_orphan_temp(epoch_path):
    temporary = epoch_path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(b"interrupted-epoch-publication")
        handle.flush()
        os.fsync(handle.fileno())
    return temporary


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_generic_index_epoch_temp_recovery.db"
    )
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (10, 'left-a', 'right-a');",
                "INSERT INTO wide_docs VALUES (20, 'left-b', 'right-b');",
                "CREATE INDEX idx_wide_right ON wide_docs (right_text);",
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                ".exit",
            ],
        )
        if 20 not in projected_ids(first):
            raise AssertionError(first)

        epoch_path = db_path + ".gidx.epoch"
        if not os.path.exists(epoch_path):
            raise AssertionError("generic-index epoch authority was not created")
        before_read = read_bytes(epoch_path)
        snapshots_before = set(glob.glob(db_path + "*.range"))
        if not snapshots_before:
            raise AssertionError("expected a persistent generic-index snapshot")

        orphan = write_orphan_temp(epoch_path)
        second = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if 20 not in projected_ids(second):
            raise AssertionError(second)
        require(second, "ok")
        if os.path.exists(orphan):
            raise AssertionError(
                "read-side recovery must remove an orphan epoch publication temp file"
            )
        if read_bytes(epoch_path) != before_read:
            raise AssertionError(
                "discarding an orphan temp beside a valid epoch must not replace authority"
            )
        if not snapshots_before.issubset(set(glob.glob(db_path + "*.range"))):
            raise AssertionError(
                "valid snapshots must survive cleanup of a non-authoritative epoch temp"
            )

        orphan = write_orphan_temp(epoch_path)
        before_mutation = read_bytes(epoch_path)
        third = run_session(
            executable,
            db_path,
            [
                "UPDATE wide_docs SET right_text = 'right-c' WHERE id = 20;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if 20 not in projected_ids(third):
            raise AssertionError(third)
        require(third, "ok")
        if os.path.exists(orphan):
            raise AssertionError(
                "mutation-side epoch barrier must remove an orphan publication temp"
            )
        if read_bytes(epoch_path) == before_mutation:
            raise AssertionError("indexed mutation must still advance durable epoch authority")

        fourth = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if 20 not in projected_ids(fourth):
            raise AssertionError(fourth)
        require(fourth, "ok")

        print(
            "PASS: orphan generic-index epoch temp files are discarded before read reuse "
            "and mutation barriers without replacing valid authority; indexed rows remain "
            "correct across reopen."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
