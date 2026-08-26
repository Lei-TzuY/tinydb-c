import glob
import os
import subprocess
import sys


def find_executable(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find the tinydb executable")


def run_session(executable, db_file, commands):
    result = subprocess.run(
        [executable, db_file],
        input="".join(command + "\n" for command in commands),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=90,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    db_file = os.path.join(os.path.dirname(__file__), "test_multi_tbl.db")
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "CREATE TABLE posts (id INT, title VARCHAR, user_id INT);",
                "INSERT INTO users VALUES (1, 'main-user', 'main@example.com');",
                "INSERT INTO archive VALUES (1, 'archive-one', 'archive1@example.com');",
                "INSERT INTO archive VALUES (2, 'archive-two', 'archive2@example.com');",
                "UPDATE archive SET username = 'archive-two-updated' WHERE id = 2;",
                "DELETE FROM archive WHERE id = 1;",
                ".tables",
                ".exit",
            ],
        )

        assert "users" in first, first
        assert "archive" in first, first
        assert "posts" in first, first
        assert "Duplicate key" not in first, first
        assert "table or view not found" not in first, first

        second = run_session(
            executable,
            db_file,
            [
                ".tables",
                "SELECT * FROM users WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 2;",
                "DELETE FROM archive;",
                ".exit",
            ],
        )

        assert "archive" in second and "posts" in second, second
        assert "(1, main-user, main@example.com)" in second, second
        assert "(2, archive-two-updated, archive2@example.com)" in second, second

        third = run_session(
            executable,
            db_file,
            [
                "SELECT * FROM users WHERE id = 1;",
                "SELECT * FROM archive;",
                ".tables",
                ".exit",
            ],
        )

        assert "(1, main-user, main@example.com)" in third, third
        assert "archive-two-updated" not in third, third
        assert "archive" in third and "posts" in third, third

        incompatible = run_session(
            executable,
            db_file,
            [
                "INSERT INTO posts VALUES (9, 'not-a-row-layout', 'ignored');",
                "SELECT * FROM users WHERE id = 9;",
                ".exit",
            ],
        )
        assert "not compatible with the current fixed Row storage layout" in incompatible, incompatible
        assert "(9," not in incompatible, incompatible

        print("PASS: persistent independent multi-table B+ tree roots verified")
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
