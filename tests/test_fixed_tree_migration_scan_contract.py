from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "leaf_cursor_read.h"


def _scan_body() -> str:
    source = HEADER.read_text(encoding="utf-8")
    marker = "static inline bool tinydb_fixed_v1_tree_scan_compact_v2("
    assert marker in source, "fixed-tree compact migration scan seam is missing"
    return source[source.index(marker) :]


def test_fixed_tree_migration_scan_reuses_canonical_row_admission():
    body = _scan_body()
    assert "tinydb_leaf_page_is_fixed_v1" in body
    assert "value_length != ROW_SIZE" in body
    assert "tinydb_fixed_v1_row_encode_compact_v2" in body
    assert "envelope_length == 0u" in body


def test_fixed_tree_migration_scan_uses_checked_topology_traversal():
    body = _scan_body()
    assert "tinydb_leaf_read_start" in body
    assert "tinydb_leaf_read_advance_checked" in body
    assert "visitor != NULL" in body
    assert "!visitor(key, envelope, envelope_length, context)" in body


def test_fixed_tree_migration_scan_publishes_count_only_after_success():
    body = _scan_body()
    success = body.index("ok = true;")
    cleanup = body.index("done:")
    publish = body.index("if (ok && rows_scanned != NULL) *rows_scanned = count;")
    assert success < cleanup < publish
    assert "table->root_page_num = previous_root;" in body[cleanup:publish]
