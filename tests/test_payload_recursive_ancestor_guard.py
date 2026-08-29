from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTE = (ROOT / "src" / "record_payload_nonroot_overflow_route.c").read_text(encoding="utf-8")
ANCESTRY = (ROOT / "src" / "record_payload_ancestor_chain.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    recursive_boundary = (
        '"payload-native recursive internal overflow beyond the grandparent remains fail-closed"'
    )
    require(recursive_boundary in ROUTE,
            "router must classify the explicit full-grandparent capacity boundary")

    handler = ROUTE.index("tinydb_record_payload_try_full_nonroot_parent_split")
    guard = ROUTE.index("tinydb_record_payload_validate_ancestor_chain", handler)
    final_message = ROUTE.rindex("set_message(message, message_size, nonroot_parent_message)")
    require(handler < guard < final_message,
            "ancestor validation must run after full-parent routing but before capacity fallback is returned")

    require("depth < table->pager->num_pages" in ANCESTRY,
            "ancestor walk must be bounded by the database page count")
    require("tinydb_payload_parent_references_child(parent, current_page_num)" in ANCESTRY,
            "ancestor walk must verify reciprocal parent->child references")
    require("parent_page_num == schema->root_page_num" in ANCESTRY,
            "ancestor walk must terminate only at the catalog-stable root")
    require("parent[IS_ROOT_OFFSET] == 0u" in ANCESTRY,
            "catalog root must be explicitly marked as root")
    require("parent[IS_ROOT_OFFSET] != 0u" in ANCESTRY,
            "foreign roots encountered below the catalog root must fail closed")
    require("memcpy(parent, get_page(table->pager, parent_page_num), PAGE_SIZE)" in ANCESTRY,
            "ancestor validation must use local page images rather than retain Pager pointers across traversal")


if __name__ == "__main__":
    main()
    print("PASS: payload recursive ancestor guard")
