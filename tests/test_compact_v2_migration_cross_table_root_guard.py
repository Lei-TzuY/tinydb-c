import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_open_adapter.h"
PAGER_RECOVERY = ROOT / "src" / "compact_v2_migration_pager_recovery.h"


def test_source_contract():
    adapter = HEADER.read_text(encoding="utf-8")
    pager_recovery = PAGER_RECOVERY.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_open_adapter_is_authoritative_root(",
        "tinydb_compact_v2_migration_open_adapter_reclaim_staging(",
        "tinydb_compact_v2_migration_open_adapter_reclaim_old_tree(",
        "adapter_out->reclaim_staging_pages =",
        "adapter_out->reclaim_old_tree =",
    ]:
        assert token in adapter, f"missing cross-table recovery guard: {token}"
    assert "TinyDBCompactV2MigrationReclaimStagingFn reclaim_staging_pages;" in pager_recovery
    assert "if (adapter->reclaim_staging_pages != NULL)" in pager_recovery


def configure_and_build(tmp_path: Path):
    cmake = (
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(TinyDBCompactV2CrossTableRootGuard C)\n"
        "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
        "if(MSVC)\n"
        "  add_compile_options(/W4 /WX /utf-8)\n"
        "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
        "else()\n"
        "  add_compile_options(-Wall -Wextra -Werror)\n"
        "  add_compile_definitions(_XOPEN_SOURCE=700)\n"
        "  find_package(Threads REQUIRED)\n"
        "endif()\n"
        "add_executable(cross_table_root_guard_probe probe.c)\n"
        f'target_include_directories(cross_table_root_guard_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        "if(NOT MSVC)\n  target_link_libraries(cross_table_root_guard_probe PRIVATE Threads::Threads)\nendif()\n"
    )
    (tmp_path / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    build = tmp_path / "build"
    configure = subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(build)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=60,
    )
    if configure.returncode != 0:
        raise AssertionError(configure.stdout + configure.stderr)
    compiled = subprocess.run(
        ["cmake", "--build", str(build), "--config", "Debug"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    if compiled.returncode != 0:
        raise AssertionError(compiled.stdout + compiled.stderr)
    return build


def run_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for cross-table migration recovery regression")

    source = r'''#include "compact_v2_migration_open_adapter.h"
#include <stdio.h>
#include <string.h>

static void init_schema(TableSchema* schema, const char* name, uint32_t root) {
    memset(schema, 0, sizeof(*schema));
    (void)snprintf(schema->name, sizeof(schema->name), "%s", name);
    schema->root_page_num = root;
    schema->num_columns = 1u;
    (void)snprintf(schema->columns[0].name, sizeof(schema->columns[0].name), "id");
    schema->columns[0].type = COL_TYPE_INT;
    schema->columns[0].size = 4u;
    schema->columns[0].offset = 0u;
    schema->row_size = 4u;
}

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(void) {
    Catalog catalog;
    Pager pager;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    TinyDBCompactV2MigrationOpenAdapterContext context;
    TinyDBCompactV2MigrationPagerRecoveryAdapter adapter;
    uint32_t claims[2] = { 9u, 17u };

    memset(&catalog, 0, sizeof(catalog));
    memset(&pager, 0, sizeof(pager));
    catalog.num_tables = 2u;
    init_schema(&catalog.schemas[0], "orders", 4u);
    init_schema(&catalog.schemas[1], "audit", 17u);
    pager.num_pages = 32u;

    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot)) return 100;
    snapshot.entries[0].schema_generation = UINT64_C(7);
    snapshot.entries[1].schema_generation = UINT64_C(3);

    if (!tinydb_compact_v2_migration_open_adapter_init(
            &context, "cross-table.db", &catalog, &snapshot, &pager)) return 101;
    if (!tinydb_compact_v2_migration_open_adapter_build(&context, &adapter)) return 102;

    if (!expect(tinydb_compact_v2_migration_open_adapter_is_authoritative_root(&context, 4u),
                "target root was not recognized")) return 1;
    if (!expect(tinydb_compact_v2_migration_open_adapter_is_authoritative_root(&context, 17u),
                "other table root was not recognized")) return 2;
    if (!expect(!tinydb_compact_v2_migration_open_adapter_is_authoritative_root(&context, 9u),
                "non-root page was misclassified")) return 3;

    pager.in_transaction = true;
    if (!expect(!adapter.reclaim_staging_pages(adapter.context, claims, 2u),
                "staging claims were allowed to contain another table root")) return 4;
    if (!expect(pager.free_page_count == 0u,
                "cross-table staging rejection mutated allocator state")) return 5;

    if (!expect(!adapter.reclaim_old_tree(adapter.context, &pager, 17u),
                "retired-root reclaim was allowed to target another live table root")) return 6;
    if (!expect(pager.free_page_count == 0u,
                "cross-table old-root rejection mutated allocator state")) return 7;

    puts("all_catalog_roots_recognized=yes");
    puts("staging_cross_table_root_rejected=yes");
    puts("old_tree_cross_table_root_rejected=yes");
    puts("allocator_unmodified=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-cross-table-root-guard-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        build = configure_and_build(tmp_path)
        exe = build / (
            "Debug/cross_table_root_guard_probe.exe"
            if sys.platform.startswith("win")
            else "cross_table_root_guard_probe"
        )
        run = subprocess.run(
            [str(exe)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "all_catalog_roots_recognized=yes",
            "staging_cross_table_root_rejected=yes",
            "old_tree_cross_table_root_rejected=yes",
            "allocator_unmodified=yes",
        ]:
            assert marker in run.stdout, f"missing {marker}: {run.stdout}"


def main():
    test_source_contract()
    run_probe()
    print("PASS: compact V2 reopen recovery protects every authoritative multi-table root")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
