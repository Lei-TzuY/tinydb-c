import glob
import os
import struct
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


def projected_id_lines(output):
    values = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line.startswith("db > "):
            line = line[5:].strip()
        if line.isdigit():
            values.append(int(line))
    return values


def require_projected_id(output, value, count=1):
    actual = projected_id_lines(output).count(value)
    if actual < count:
        raise AssertionError(
            f"expected at least {count} projected id line(s) {value!r}; got {actual}\n{output}"
        )


def read_epoch(db_path):
    path = db_path + ".gidx.epoch"
    if not os.path.exists(path):
        raise AssertionError(f"missing generic index epoch file: {path}")
    with open(path, "rb") as handle:
        payload = handle.read()
    if len(payload) != 24:
        raise AssertionError(f"unexpected epoch payload size {len(payload)}")
    magic, version, epoch, checksum = struct.unpack("<IIQQ", payload)
    if magic != 0x47494550 or version != 1:
        raise AssertionError(
            f"unexpected epoch header magic={magic:#x} version={version}"
        )
    if checksum == 0:
        raise AssertionError("epoch checksum must be non-zero")
    return epoch


def range_snapshots(db_path):
    return set(glob.glob(db_path + "*.range"))


def corrupt_epoch(db_path):
    path = db_path + ".gidx.epoch"
    with open(path, "wb") as handle:
        handle.write(b"corrupt-epoch")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_alter_index_epoch.db")
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
        require_projected_id(first, 20)
        epoch_before = read_epoch(db_path)

        second = run_session(
            executable,
            db_path,
            [
                "ALTER TABLE wide_docs ADD COLUMN tag VARCHAR(5);",
                "SELECT * FROM wide_docs WHERE id = 10;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                ".exit",
            ],
        )
        require(second, "Column 'tag' added to table 'wide_docs'.")
        require(second, "(10, left-a, right-a, )")
        require_projected_id(second, 20)
        epoch_after = read_epoch(db_path)
        if epoch_after == epoch_before:
            raise AssertionError(
                "append-only schema evolution must invalidate pre-ALTER generic index snapshots"
            )

        third = run_session(
            executable,
            db_path,
            [
                "CREATE INDEX idx_wide_tag ON wide_docs (tag);",
                "SELECT id FROM wide_docs WHERE tag = '';",
                "UPDATE wide_docs SET tag = 'new' WHERE id = 20;",
                "SELECT id FROM wide_docs WHERE tag = 'new';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require_projected_id(third, 10)
        require_projected_id(third, 20, count=2)
        require(third, "ok")

        fourth = run_session(
            executable,
            db_path,
            [
                "SELECT * FROM wide_docs WHERE id = 10;",
                "SELECT * FROM wide_docs WHERE id = 20;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-a';",
                "SELECT id FROM wide_docs WHERE tag = '';",
                "SELECT id FROM wide_docs WHERE tag = 'new';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(fourth, "(10, left-a, right-a, )")
        require(fourth, "(20, left-b, right-b, new)")
        require_projected_id(fourth, 10, count=2)
        require_projected_id(fourth, 20)
        require(fourth, "ok")

        snapshots_before_loss = range_snapshots(db_path)
        if len(snapshots_before_loss) < 2:
            raise AssertionError(
                "expected both generic indexes to have persistent range snapshots before epoch corruption; "
                f"found {sorted(snapshots_before_loss)!r}"
            )

        corrupt_epoch(db_path)
        fifth = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-a';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require_projected_id(fifth, 10)
        require(fifth, "ok")
        read_epoch(db_path)

        snapshots_after_loss = range_snapshots(db_path)
        removed = snapshots_before_loss - snapshots_after_loss
        if not removed:
            raise AssertionError(
                "recreating corrupt epoch metadata must purge unrelated stale generic-index snapshots "
                "before rebuilding the index used by the current query"
            )
        if not snapshots_after_loss:
            raise AssertionError("the queried generic index snapshot should be rebuilt after epoch recovery")

        sixth = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE tag = '';",
                "SELECT id FROM wide_docs WHERE tag = 'new';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require_projected_id(sixth, 10)
        require_projected_id(sixth, 20)
        require(sixth, "ok")
        if len(range_snapshots(db_path)) < 2:
            raise AssertionError(
                "an unrelated purged snapshot must rebuild normally when its index is queried later"
            )

        print(
            "PASS: append-only wide schema evolution advances the generic index epoch, "
            "and missing/corrupt epoch authority purges all persistent candidate snapshots "
            "before a fresh epoch is published, preventing stale sidecar reuse."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
