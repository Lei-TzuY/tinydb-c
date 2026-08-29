#define main payload_recursive_three_level_original_main
#include "payload_recursive_three_level_probe.c"
#undef main

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_recursive_three_level_diag <db>\n");
        return EXIT_FAILURE;
    }
    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;

    TableSchema schema;
    uint32_t baseline_pages = 0u;
    bool seeded = seed_tree(table, &schema, &baseline_pages);
    unsigned char* root = (unsigned char*)get_page(table->pager, 0u);
    uint32_t root_keys = root != NULL && get_node_type(root) == NODE_INTERNAL
        ? read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET)
        : UINT32_MAX;
    bool present = schema.row_size == 308u && candidate_present(table, &schema);

    fprintf(stderr,
            "THREE_LEVEL_SEED_DIAG seeded=%u root_keys=%u row_size=%u candidate=%u pages=%u baseline=%u\n",
            seeded ? 1u : 0u,
            root_keys,
            schema.row_size,
            present ? 1u : 0u,
            table->pager->num_pages,
            baseline_pages);
    db_close(table);
    return EXIT_FAILURE;
}
