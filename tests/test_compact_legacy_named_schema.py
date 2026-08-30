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


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_compact_legacy_named_schema.db"
    )
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE archive (id INT, username VARCHAR(20), email VARCHAR(40));",
                "INSERT INTO archive VALUES (1, 'alpha', 'a@example.test');",
                "ALTER TABLE archive ADD COLUMN note VARCHAR(12);",
                "UPDATE archive SET note = 'existing' WHERE id = 1;",
                "INSERT INTO archive VALUES (2, 'beta', 'b@example.test', 'new-row');",
                "PRAGMA table_info(archive);",
                "SELECT * FROM archive WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        for marker in [
            "Column 'note' added to table 'archive'.",
            "username | VARCHAR(20)",
            "email | VARCHAR(40)",
            "note | VARCHAR(12)",
            "(1, alpha, a@example.test, existing)",
            "(2, beta, b@example.test, new-row)",
            "ok",
        ]:
            require(first, marker)

        if "disabled for executable fixed-Row table roots" in first:
            raise AssertionError(
                "compact generic schema was incorrectly classified as legacy Row\n"
                + first
            )

        second = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(archive);",
                "SELECT * FROM archive WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 2;",
                "INSERT INTO archive VALUES (3, 'gamma', 'g@example.test', 'persisted');",
                "SELECT * FROM archive WHERE id = 3;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        for marker in [
            "username | VARCHAR(20)",
            "email | VARCHAR(40)",
            "note | VARCHAR(12)",
            "(1, alpha, a@example.test, existing)",
            "(2, beta, b@example.test, new-row)",
            "(3, gamma, g@example.test, persisted)",
            "ok",
        ]:
            require(second, marker)

        print(
            "PASS: id/username/email naming alone does not force the historical "
            "293-byte Row ABI; compact VARCHAR(n) layouts remain schema-aware, "
            "support ADD COLUMN on existing rows, and persist across reopen."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
