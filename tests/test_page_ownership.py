import glob
import os
import subprocess
import sys

PAGE_SIZE = 4096


def find_binary(repo_root, name):
    candidates = [
        os.path.join(repo_root, "build", "Debug", name + ".exe"),
        os.path.join(repo_root, "build", "Release", name + ".exe"),
        os.path.join(repo_root, "build", name + ".exe"),
        os.path.join(repo_root, "build", name),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError(f"Could not find {name}")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_repl(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    repl = find_binary(repo_root, "tinydb")
    probe = find_binary(repo_root, "tinydb_page_ownership_probe")
    probe_db = os.path.join(os.path.dirname(__file__), "test_page_ownership_probe.db")
    orphan_db = os.path.join(os.path.dirname(__file__), "test_page_ownership_orphan.db")
    cleanup(probe_db)
    cleanup(orphan_db)

    try:
        direct = subprocess.run(
            [probe, probe_db],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        assert direct.returncode == 0, direct.stdout + "\n" + direct.stderr
        assert "PAGE_OWNERSHIP_OK" in direct.stdout, direct.stdout
        assert "diagnostic_pin_pressure=yes" in direct.stdout, direct.stdout

        run_repl(
            repl,
            orphan_db,
            [
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "INSERT INTO users VALUES (1, 'main', 'main@example.com');",
                "INSERT INTO archive VALUES (1, 'archive', 'archive@example.com');",
                ".exit",
            ],
        )

        # Add one physically allocated page that is absent from every catalog root.
        # The ownership pass should reject this page graph even though each tree on
        # its own remains structurally valid.
        with open(orphan_db, "ab") as database_file:
            database_file.write(b"\0" * PAGE_SIZE)

        output = run_repl(
            repl,
            orphan_db,
            [
                ".check all",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        needle = "allocated but unreachable from every catalog root"
        assert output.count(needle) >= 2, output
        assert "page ownership:" in output, output

        print(
            "PASS: page ownership catches orphan/shared pages and whole-database "
            "diagnostics return non-fatally under full pin pressure"
        )
    finally:
        cleanup(probe_db)
        cleanup(orphan_db)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
