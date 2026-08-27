import glob
import os
import struct
import subprocess
import sys


SCHEMA_MAGIC = 0x4D435354
SCHEMA_V1 = 1
SCHEMA_V2 = 2
WAL_COMMIT_MAGIC = 0x57435354
FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211


def find_binary(base_dir, name):
    candidates = [
        os.path.join(base_dir, "build", "Debug", name + ".exe"),
        os.path.join(base_dir, "build", "Release", name + ".exe"),
        os.path.join(base_dir, "build", name + ".exe"),
        os.path.join(base_dir, "build", name),
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


def run_session(executable, db_file, commands, check=True):
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    output = result.stdout + result.stderr
    if check and result.returncode != 0:
        raise AssertionError(output)
    return result.returncode, output


def fnv64(data):
    value = FNV64_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def parse_v2(data):
    if len(data) < 20:
        raise AssertionError("schema catalog too short for V2 header")
    magic, version, payload_size, checksum = struct.unpack_from("<IIIQ", data, 0)
    if magic != SCHEMA_MAGIC or version != SCHEMA_V2:
        raise AssertionError(f"unexpected schema header magic/version: {magic:#x}/{version}")
    if len(data) != 20 + payload_size:
        raise AssertionError("schema catalog payload length does not match header")
    payload = data[20:]
    if fnv64(payload) != checksum:
        raise AssertionError("schema catalog checksum does not match payload")
    return payload


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tinydb = find_binary(repo_root, "tinydb")
    v1_probe = find_binary(repo_root, "tinydb_schema_catalog_v1_probe")
    if tinydb is None or v1_probe is None:
        print("FAIL: Could not find schema catalog test binaries.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_schema_catalog_v2.db")
    schema_file = db_file + ".schema"
    wal_file = db_file + ".schema.wal"
    cleanup(db_file)

    try:
        _, setup = run_session(
            tinydb,
            db_file,
            [
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "INSERT INTO archive VALUES (7, 'archived', 'archive@example.com');",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(setup, "ok")
        if not os.path.exists(schema_file):
            raise AssertionError("schema catalog was not created")

        with open(schema_file, "rb") as handle:
            v2_bytes = handle.read()
        parse_v2(v2_bytes)
        if os.path.exists(wal_file):
            raise AssertionError("committed schema WAL was not removed")

        _, reopened = run_session(
            tinydb,
            db_file,
            [
                "SELECT COUNT(*) FROM archive;",
                ".schema archive",
                ".exit",
            ],
        )
        require(reopened, "db > 1\nExecuted.")
        require(reopened, "Table: archive")

        downgrade = subprocess.run(
            [v1_probe, db_file],
            capture_output=True,
            text=True,
            timeout=120,
        )
        downgrade_output = downgrade.stdout + downgrade.stderr
        if downgrade.returncode != 0:
            raise AssertionError(downgrade_output)
        require(downgrade_output, "SCHEMA_CATALOG_V1_OK")
        with open(schema_file, "rb") as handle:
            legacy_prefix = handle.read(8)
        if len(legacy_prefix) != 8:
            raise AssertionError("legacy schema catalog is truncated")
        legacy_magic, legacy_version = struct.unpack("<II", legacy_prefix)
        if legacy_magic != SCHEMA_MAGIC or legacy_version != SCHEMA_V1:
            raise AssertionError("V1 probe did not write a legacy catalog")

        _, legacy_read = run_session(
            tinydb,
            db_file,
            [
                "SELECT COUNT(*) FROM archive;",
                "CREATE VIEW archive_view AS SELECT * FROM archive;",
                ".exit",
            ],
        )
        require(legacy_read, "db > 1\nExecuted.")
        with open(schema_file, "rb") as handle:
            upgraded_bytes = handle.read()
        parse_v2(upgraded_bytes)

        committed_wal = upgraded_bytes + struct.pack("<I", WAL_COMMIT_MAGIC)
        with open(wal_file, "wb") as handle:
            handle.write(committed_wal)
        with open(schema_file, "wb") as handle:
            handle.write(upgraded_bytes[:7])

        _, recovered = run_session(
            tinydb,
            db_file,
            [
                "SELECT COUNT(*) FROM archive;",
                ".exit",
            ],
        )
        require(recovered, "Schema catalog recovery complete.")
        require(recovered, "db > 1\nExecuted.")
        if os.path.exists(wal_file):
            raise AssertionError("recovered V2 schema WAL was not removed")
        with open(schema_file, "rb") as handle:
            recovered_bytes = handle.read()
        if recovered_bytes != upgraded_bytes:
            raise AssertionError("V2 WAL recovery did not restore the committed catalog")
        parse_v2(recovered_bytes)

        corrupted_wal = bytearray(committed_wal)
        if len(corrupted_wal) <= 28:
            raise AssertionError("unexpectedly short committed schema WAL")
        corrupted_wal[28] ^= 0x5A
        with open(wal_file, "wb") as handle:
            handle.write(corrupted_wal)
        with open(schema_file, "wb") as handle:
            handle.write(upgraded_bytes)

        _, ignored_wal = run_session(
            tinydb,
            db_file,
            [
                "SELECT COUNT(*) FROM archive;",
                ".exit",
            ],
        )
        require(ignored_wal, "Ignoring invalid checksummed schema catalog WAL.")
        require(ignored_wal, "db > 1\nExecuted.")
        if os.path.exists(wal_file):
            raise AssertionError("invalid V2 schema WAL was not removed")

        corrupted_main = bytearray(upgraded_bytes)
        corrupted_main[28] ^= 0x33
        with open(schema_file, "wb") as handle:
            handle.write(corrupted_main)
        _, corrupt_output = run_session(tinydb, db_file, [".exit"], check=False)
        require(corrupt_output, "Ignoring invalid checksummed schema catalog.")
        require(corrupt_output, "schema catalog could not be loaded")

        with open(schema_file, "wb") as handle:
            handle.write(upgraded_bytes)
        _, final = run_session(
            tinydb,
            db_file,
            [
                "SELECT COUNT(*) FROM archive;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(final, "db > 1\nExecuted.")
        require(final, "ok")

        print(
            "PASS: schema catalog V2 uses a fixed little-endian checksummed envelope, "
            "reads legacy V1 catalogs, upgrades them on DDL, recovers committed V2 WAL, "
            "rejects corrupted WAL/main payloads, and preserves multi-root catalog state."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
