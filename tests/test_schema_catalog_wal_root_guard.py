import os
import struct
import subprocess
import sys


SCHEMA_MAGIC = 0x4D435354
SCHEMA_V2_VERSION = 2
SCHEMA_WAL_COMMIT_MAGIC = 0x57435354
FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def find_executable(base_dir):
    candidates = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb'),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def remove_database(db_file):
    suffixes = ('', '.wal', '.free', '.schema', '.schema.wal', '.idxepoch')
    for suffix in suffixes:
        path = db_file + suffix
        if os.path.exists(path):
            os.remove(path)


def run_commands(executable, db_file, commands):
    process = subprocess.Popen(
        [executable, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, stderr = process.communicate(input=commands)
    return process.returncode, stdout, stderr


def fnv64(data):
    value = FNV64_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV64_PRIME) & MASK64
    return value


def forge_committed_impossible_root_wal(schema_bytes):
    if len(schema_bytes) < 20:
        raise AssertionError('schema catalog is shorter than the V2 header')

    magic, version, payload_size = struct.unpack_from('<III', schema_bytes, 0)
    if magic != SCHEMA_MAGIC or version != SCHEMA_V2_VERSION:
        raise AssertionError('expected a V2 schema catalog')
    if len(schema_bytes) != 20 + payload_size:
        raise AssertionError('unexpected schema catalog length')

    payload = bytearray(schema_bytes[20:])
    # V2 payload starts with num_tables, num_views, then the first table name.
    # The first table root follows the fixed 32-byte name at payload offset 40.
    if len(payload) < 44:
        raise AssertionError('schema payload is too short for the first table root')
    struct.pack_into('<I', payload, 40, 0xFFFFFFFF)

    header = bytearray(schema_bytes[:20])
    struct.pack_into('<Q', header, 12, fnv64(payload))
    return bytes(header) + bytes(payload) + struct.pack('<I', SCHEMA_WAL_COMMIT_MAGIC)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_schema_catalog_wal_root_guard.db')
    schema_file = db_file + '.schema'
    schema_wal = db_file + '.schema.wal'
    remove_database(db_file)

    rc, stdout, stderr = run_commands(
        executable,
        db_file,
        "CREATE TABLE products (id INT, price INT);\n.exit\n",
    )
    if rc != 0:
        print('FAIL: failed to create a database with a persisted schema catalog.')
        print(stdout)
        print(stderr)
        sys.exit(1)
    if not os.path.exists(schema_file):
        print('FAIL: CREATE TABLE did not persist a schema catalog.')
        sys.exit(1)

    with open(schema_file, 'rb') as handle:
        healthy_main = handle.read()
    forged_wal = forge_committed_impossible_root_wal(healthy_main)
    with open(schema_wal, 'wb') as handle:
        handle.write(forged_wal)

    rc, stdout, stderr = run_commands(executable, db_file, '.exit\n')
    if rc != 0:
        print('FAIL: a committed WAL with an impossible root prevented reopen.')
        print(stdout)
        print(stderr)
        sys.exit(1)
    if 'preserving main catalog' not in stdout:
        print('FAIL: expected impossible-root WAL rejection diagnostic.')
        print(stdout)
        sys.exit(1)
    if os.path.exists(schema_wal):
        print('FAIL: rejected schema WAL was not removed.')
        sys.exit(1)

    with open(schema_file, 'rb') as handle:
        recovered_main = handle.read()
    if recovered_main != healthy_main:
        print('FAIL: impossible-root WAL overwrote the healthy main schema catalog.')
        sys.exit(1)

    rc, stdout, stderr = run_commands(executable, db_file, '.exit\n')
    if rc != 0:
        print('FAIL: database did not reopen after invalid WAL removal.')
        print(stdout)
        print(stderr)
        sys.exit(1)

    print('PASS: impossible-root schema WAL is discarded before it can replace the healthy catalog.')
    remove_database(db_file)


if __name__ == '__main__':
    run_test()
