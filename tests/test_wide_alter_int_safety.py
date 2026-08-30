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


def reject(output, marker):
    if marker in output:
        raise AssertionError(f"unexpected marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_alter_int_safety.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_nonempty (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_nonempty VALUES (1, 'left-a', 'right-a');",
                "ALTER TABLE wide_nonempty ADD COLUMN score INT;",
                "PRAGMA table_info(wide_nonempty);",
                "SELECT * FROM wide_nonempty WHERE id = 1;",
                "CREATE TABLE wide_empty (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "ALTER TABLE wide_empty ADD COLUMN score INT;",
                "INSERT INTO wide_empty VALUES (2, 'left-b', 'right-b', 7);",
                "PRAGMA table_info(wide_empty);",
                "SELECT * FROM wide_empty WHERE id = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(
            first,
            "ALTER TABLE ADD COLUMN is disabled for non-empty schema-sized payload tables until physical row migration is implemented",
        )
        require(first, "(1, left-a, right-a)")
        require(first, "Column 'score' added to table 'wide_empty'.")
        require(first, "score | INT")
        require(first, "(2, left-b, right-b, 7)")
        require(first, "ok")

        # The rejected ALTER must not have published a catalog-only fourth
        # column for the non-empty table.  There should be exactly one score
        # entry, belonging to wide_empty.
        if first.count("score | INT") != 1:
            raise AssertionError(
                "rejected wide_nonempty INT ALTER changed persisted schema\n" + first
            )

        second = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(wide_nonempty);",
                "SELECT * FROM wide_nonempty WHERE id = 1;",
                "INSERT INTO wide_nonempty VALUES (3, 'left-c', 'right-c');",
                "SELECT * FROM wide_nonempty WHERE id = 3;",
                "PRAGMA table_info(wide_empty);",
                "SELECT * FROM wide_empty WHERE id = 2;",
                "INSERT INTO wide_empty VALUES (4, 'left-d', 'right-d', 9);",
                "SELECT * FROM wide_empty WHERE id = 4;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(second, "(1, left-a, right-a)")
        require(second, "(3, left-c, right-c)")
        require(second, "score | INT")
        require(second, "(2, left-b, right-b, 7)")
        require(second, "(4, left-d, right-d, 9)")
        require(second, "ok")
        if second.count("score | INT") != 1:
            raise AssertionError(
                "rejected non-empty ALTER leaked into catalog after reopen\n" + second
            )

        print(
            "PASS: INT ADD COLUMN cannot bypass schema-sized payload migration "
            "safety; non-empty rows remain decodable and unchanged across reopen, "
            "while an empty wide table can grow safely."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
