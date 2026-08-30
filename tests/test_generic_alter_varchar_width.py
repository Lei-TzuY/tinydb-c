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
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_generic_alter_varchar_width.db"
    )
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE contacts (id INT, name VARCHAR(40), score INT);",
                "INSERT INTO contacts VALUES (1, 'alpha', 100);",
                "ALTER TABLE contacts ADD COLUMN nickname VARCHAR(20);",
                "UPDATE contacts SET nickname = 'ally' WHERE id = 1;",
                "INSERT INTO contacts VALUES (2, 'beta', 200, 'bee');",
                "PREPARE add_note FROM ALTER TABLE contacts ADD COLUMN note VARCHAR(10);",
                "EXECUTE add_note;",
                "UPDATE contacts SET note = 'memo' WHERE id = 2;",
                "PRAGMA table_info(contacts);",
                "SELECT * FROM contacts WHERE id = 1;",
                "SELECT * FROM contacts WHERE id = 2;",
                "BEGIN;",
                "ALTER TABLE contacts ADD COLUMN blocked VARCHAR(5);",
                "ROLLBACK;",
                "CREATE TABLE near_limit (id INT, payload VARCHAR(250));",
                "INSERT INTO near_limit VALUES (1, 'seed');",
                "ALTER TABLE near_limit ADD COLUMN tail VARCHAR(37);",
                "INSERT INTO near_limit VALUES (2, 'short', 'edge');",
                "ALTER TABLE near_limit ADD COLUMN overflow VARCHAR(1);",
                "PRAGMA table_info(near_limit);",
                "CREATE TABLE empty_crossing (id INT, payload VARCHAR(250), tail VARCHAR(37));",
                "ALTER TABLE empty_crossing ADD COLUMN overflow VARCHAR(1);",
                "PRAGMA table_info(empty_crossing);",
                "INSERT INTO empty_crossing VALUES (3, 'cross', 'edge', 'x');",
                "SELECT * FROM empty_crossing WHERE id = 3;",
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (7, 'left', 'right');",
                "ALTER TABLE wide_docs ADD COLUMN tag VARCHAR(5);",
                "PRAGMA table_info(wide_docs);",
                "SELECT * FROM wide_docs WHERE id = 7;",
                "CREATE TABLE empty_wide (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "ALTER TABLE empty_wide ADD COLUMN tag VARCHAR(5);",
                "PRAGMA table_info(empty_wide);",
                "INSERT INTO empty_wide VALUES (9, 'empty-left', 'empty-right', 'tag5');",
                "SELECT * FROM empty_wide WHERE id = 9;",
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "ALTER TABLE archive ADD COLUMN extra VARCHAR(5);",
                "ALTER TABLE contacts ADD COLUMN bad_zero VARCHAR(0);",
                "ALTER TABLE contacts ADD COLUMN bad_large VARCHAR(256);",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        for marker in [
            "Column 'nickname' added to table 'contacts'.",
            "Column 'note' added to table 'contacts'.",
            "nickname | VARCHAR(20)",
            "note | VARCHAR(10)",
            "(1, alpha, 100, ally, )",
            "(2, beta, 200, bee, memo)",
            "schema DDL is not allowed inside a transaction",
            "Column 'tail' added to table 'near_limit'.",
            "payload | VARCHAR(250)",
            "tail | VARCHAR(37)",
            "ALTER TABLE ADD COLUMN would exceed the fixed generic record slot; variable-size row migration is not implemented",
            "Column 'overflow' added to table 'empty_crossing'.",
            "overflow | VARCHAR(1)",
            "(3, cross, edge, x)",
            "ALTER TABLE ADD COLUMN is disabled for non-empty schema-sized payload tables until physical row migration is implemented",
            "(7, left, right)",
            "Column 'tag' added to table 'empty_wide'.",
            "tag | VARCHAR(5)",
            "(9, empty-left, empty-right, tag5)",
            "ALTER TABLE ADD COLUMN is disabled for executable fixed-Row table roots until physical row migration is implemented",
            "Syntax error. Could not parse statement.",
            "ok",
        ]:
            require(first, marker)

        if "blocked | VARCHAR(5)" in first:
            raise AssertionError("rejected transaction ALTER leaked into catalog\n" + first)

        second = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(contacts);",
                "PRAGMA table_info(near_limit);",
                "PRAGMA table_info(empty_crossing);",
                "PRAGMA table_info(wide_docs);",
                "PRAGMA table_info(empty_wide);",
                "SELECT * FROM contacts WHERE id = 1;",
                "SELECT * FROM contacts WHERE id = 2;",
                "SELECT * FROM near_limit WHERE id = 1;",
                "SELECT * FROM near_limit WHERE id = 2;",
                "INSERT INTO near_limit VALUES (5, 'still', 'fixed');",
                "SELECT * FROM near_limit WHERE id = 5;",
                "SELECT * FROM empty_crossing WHERE id = 3;",
                "SELECT * FROM wide_docs WHERE id = 7;",
                "SELECT * FROM empty_wide WHERE id = 9;",
                "INSERT INTO contacts VALUES (3, 'gamma', 300, 'g', 'persist');",
                "SELECT note FROM contacts WHERE id = 3;",
                "INSERT INTO empty_crossing VALUES (4, 'cross-2', 'edge-2', 'y');",
                "SELECT * FROM empty_crossing WHERE id = 4;",
                "INSERT INTO wide_docs VALUES (8, 'left-2', 'right-2');",
                "SELECT * FROM wide_docs WHERE id = 8;",
                "INSERT INTO empty_wide VALUES (10, 'left-10', 'right-10', 'again');",
                "SELECT * FROM empty_wide WHERE id = 10;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        for marker in [
            "nickname | VARCHAR(20)",
            "note | VARCHAR(10)",
            "payload | VARCHAR(250)",
            "tail | VARCHAR(37)",
            "overflow | VARCHAR(1)",
            "left_text | VARCHAR(145)",
            "right_text | VARCHAR(145)",
            "tag | VARCHAR(5)",
            "(1, alpha, 100, ally, )",
            "(2, beta, 200, bee, memo)",
            "(1, seed, )",
            "(2, short, edge)",
            "(5, still, fixed)",
            "(3, cross, edge, x)",
            "(7, left, right)",
            "(9, empty-left, empty-right, tag5)",
            "db > persist\nExecuted.",
            "(4, cross-2, edge-2, y)",
            "(8, left-2, right-2)",
            "(10, left-10, right-10, again)",
            "ok",
        ]:
            require(second, marker)

        print(
            "PASS: compact VARCHAR(n) ADD COLUMN persists n+1-byte layouts, "
            "supports prepared routing, honors the exact 293-byte boundary, "
            "allows provably empty tables to cross safely into payload-sized storage, "
            "allows provably empty schema-sized payload tables to extend safely, "
            "keeps non-empty rows fail-closed until row migration, "
            "and preserves fixed-Row/transaction safety guards."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
