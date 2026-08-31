import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_pager_reclaim.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_pager_reclaim_claims(",
        "pager->in_transaction",
        "page_num == 0u",
        "page_num == INVALID_PAGE_NUM",
        "pager_pin_transition_busy_locked",
        "pager_frame_for_page_is_pinned_locked",
        "pager_free_page(pager, page_num)",
        "tinydb_compact_v2_migration_pager_claims_are_reclaimed(",
    ]:
        assert token in text, f"missing pager reclaim invariant: {token}"

    preflight = text.index("/* Preflight the complete manifest-owned set before the first free. */")
    mutate = text.index("pager_free_page(pager, page_num)", preflight)
    verify = text.index("return tinydb_compact_v2_migration_pager_claims_are_reclaimed(", mutate)
    assert preflight < mutate < verify


def compile_and_run_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for pager reclaim regression")

    c_source = r'''#include "compact_v2_migration_pager_reclaim.h"
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 100;
    const char* path = argv[1];
    Pager* pager = pager_open(path);

    void* root = get_page(pager, 0u);
    memset(root, 0, PAGE_SIZE);
    mark_page_dirty(pager, 0u);
    pager_checkpoint(pager);

    pager_begin_transaction(pager);
    for (uint32_t expected = 1u; expected <= 4u; expected++) {
        uint32_t page_num = get_unused_page_num(pager);
        if (!expect(page_num == expected, "unexpected allocated page number")) return 1;
        void* page = get_page(pager, page_num);
        memset(page, (int)(0x20u + expected), PAGE_USABLE_SIZE);
        mark_page_dirty(pager, page_num);
    }
    pager_commit(pager);
    pager_checkpoint(pager);

    uint32_t claims[3] = {1u, 2u, 3u};
    uint32_t duplicate[2] = {1u, 1u};
    uint32_t page_zero[1] = {0u};

    if (!expect(!tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, claims, 3u),
                "reclaim outside a transaction was accepted")) return 2;

    pager_begin_transaction(pager);
    if (!expect(!tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, duplicate, 2u) && pager->free_page_count == 0u,
                "duplicate manifest claims mutated allocator state")) return 3;
    if (!expect(!tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, page_zero, 1u) && pager->free_page_count == 0u,
                "page zero claim was accepted")) return 4;

    if (!expect(tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, claims, 3u),
                "initial claim reclaim failed")) return 5;
    if (!expect(pager->free_page_count == 3u,
                "initial reclaim did not free exactly three pages")) return 6;

    if (!expect(tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, claims, 3u),
                "same-transaction reclaim retry failed")) return 7;
    if (!expect(pager->free_page_count == 3u,
                "same-transaction retry duplicated free-list entries")) return 8;

    PagerPageHandle pinned;
    if (!expect(pager_pin_page_handle(pager, 4u, &pinned),
                "unable to pin allocated page 4")) return 9;
    uint32_t claim4[1] = {4u};
    if (!expect(!tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, claim4, 1u) && pager->free_page_count == 3u,
                "pinned migration page was reclaimed")) return 10;
    if (!expect(pager_release_page_handle(&pinned),
                "unable to release page 4 pin")) return 11;
    if (!expect(tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, claim4, 1u) && pager->free_page_count == 4u,
                "unpinned migration page was not reclaimed")) return 12;

    pager_commit(pager);
    pager_checkpoint(pager);
    if (!expect(pager_try_close(pager), "unable to close first pager")) return 13;

    pager = pager_open(path);
    if (!expect(pager->free_page_count == 4u,
                "durable free-list did not survive reopen")) return 14;

    uint32_t all_claims[4] = {1u, 2u, 3u, 4u};
    pager_begin_transaction(pager);
    if (!expect(tinydb_compact_v2_migration_pager_reclaim_claims(
                    pager, all_claims, 4u),
                "post-crash-style reclaim retry failed")) return 15;
    if (!expect(pager->free_page_count == 4u,
                "post-reopen retry duplicated durable free-list entries")) return 16;
    pager_commit(pager);
    pager_checkpoint(pager);

    if (!expect(pager_try_close(pager), "unable to close reopened pager")) return 17;
    puts("transaction_gate=yes");
    puts("idempotent_reclaim=yes");
    puts("pin_guard=yes");
    puts("reopen_retry=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-pager-reclaim-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(c_source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBPagerReclaimProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\n"
            "set(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "find_package(Threads REQUIRED)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "endif()\n"
            f'add_executable(pager_reclaim_probe probe.c "{(ROOT / "src" / "pager.c").as_posix()}" "{(ROOT / "src" / "pager_checkpoint_order.c").as_posix()}")\n'
            f'target_include_directories(pager_reclaim_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
            f'set_source_files_properties("{(ROOT / "src" / "pager.c").as_posix()}" PROPERTIES COMPILE_DEFINITIONS "pager_checkpoint=pager_checkpoint_legacy_base")\n'
            "target_link_libraries(pager_reclaim_probe PRIVATE Threads::Threads)\n",
            encoding="utf-8",
        )
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
        compile_result = subprocess.run(
            ["cmake", "--build", str(build), "--config", "Debug"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        if compile_result.returncode != 0:
            raise AssertionError(compile_result.stdout + compile_result.stderr)

        exe = build / "pager_reclaim_probe"
        if sys.platform.startswith("win"):
            exe = build / "Debug" / "pager_reclaim_probe.exe"
        db_path = tmp_path / "reclaim.db"
        run = subprocess.run(
            [str(exe), str(db_path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "transaction_gate=yes",
            "idempotent_reclaim=yes",
            "pin_guard=yes",
            "reopen_retry=yes",
        ]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_source_contract()
    compile_and_run_probe()
    print("PASS: compact V2 migration claims reclaim idempotently through Pager transactions and survive reopen")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
