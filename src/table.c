/*
 * table.c — B+ Tree implementation, cursor, and table lifecycle
 *
 * This file contains the heart of the database engine:
 *  1. Node accessor helpers (read/write fields in raw page memory)
 *  2. Table open/close (calls the pager, initializes the root page)
 *  3. Cursor navigation (table_start, table_find, cursor_advance)
 *  4. Leaf node operations (insert, split)
 *  5. Internal node operations (find, insert key, split)
 *  6. Row serialization / deserialization
 *  7. Tree pretty-printer for the .btree command
 */

#include "table.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define INDEX_CATALOG_MAGIC   0x58444954u /* TIDX */
#define INDEX_CATALOG_VERSION 2u
#define INDEX_CATALOG_WAL_COMMIT_MAGIC 0x49445843u /* IDXC */
#define USERNAME_INDEX_MAGIC 0x55494458u /* UIDX */
#define USERNAME_INDEX_VERSION 1u
#define USERNAME_INDEX_WAL_COMMIT_MAGIC 0x55494443u /* UIDC */
#define USERNAME_INDEX_MAX_ENTRIES (TABLE_MAX_PAGES * LEAF_NODE_MAX_CELLS)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t username_index_enabled;
    uint32_t username_index_entries_valid;
} IndexCatalog;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
} UsernameIndexFileHeader;

static void table_load_index_catalog(Table* table);
static void table_save_index_catalog_state(Table* table, bool entries_valid);

/* ═══════════════════════════════════════════════════════════════
 * SECTION 1 — Node accessor helpers
 *
 * INTERNALS: WHY RAW POINTER ARITHMETIC?
 * ────────────────────────────────────────
 * We store the entire page as a raw `void*` buffer.  Using a C struct
 * with __attribute__((packed)) would also work, but pointer arithmetic
 * is explicit about byte offsets and makes the on-disk format obvious.
 * Each accessor returns a POINTER into the buffer so callers can both
 * read and write through the same pointer.
 * ═══════════════════════════════════════════════════════════════ */

NodeType get_node_type(void* node) {
    uint8_t value = *((uint8_t*)((char*)node + NODE_TYPE_OFFSET));
    return (NodeType)value;
}

void set_node_type(void* node, NodeType type) {
    uint8_t value = (uint8_t)type;
    *((uint8_t*)((char*)node + NODE_TYPE_OFFSET)) = value;
}

bool is_node_root(void* node) {
    uint8_t value = *((uint8_t*)((char*)node + IS_ROOT_OFFSET));
    return (bool)value;
}

void set_node_root(void* node, bool is_root) {
    uint8_t value = (uint8_t)is_root;
    *((uint8_t*)((char*)node + IS_ROOT_OFFSET)) = value;
}

uint32_t* node_parent(void* node) {
    return (uint32_t*)((char*)node + PARENT_POINTER_OFFSET);
}

/* ─── Leaf node accessors ─────────────────────────────────────── */

uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

/*
 * INTERNALS: SIBLING POINTER
 * next_leaf stores the page number of the right sibling leaf.
 * Value 0 is the sentinel meaning "no sibling" (page 0 is always root).
 * During a leaf split we update both the old node's next_leaf and the
 * new node's next_leaf to maintain the linked list.
 */
uint32_t* leaf_node_next_leaf(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}

uint32_t* leaf_node_prev_leaf(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_PREV_LEAF_OFFSET);
}

static void* leaf_node_cell(void* node, uint32_t cell_num) {
    return (char*)node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
    return (uint32_t*)leaf_node_cell(node, cell_num);
}

static void* leaf_node_value(void* node, uint32_t cell_num) {
    return (char*)leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

/* ─── Internal node accessors ────────────────────────────────── */

uint32_t* internal_node_num_keys(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

uint32_t* internal_node_right_child(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE
                       + cell_num * INTERNAL_NODE_CELL_SIZE);
}

uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        printf("ERROR: child_num %u > num_keys %u\n", child_num, num_keys);
        exit(EXIT_FAILURE);
    }
    return (child_num == num_keys)
        ? internal_node_right_child(node)
        : internal_node_cell(node, child_num);
}

uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)((char*)internal_node_cell(node, key_num)
                       + INTERNAL_NODE_CHILD_SIZE);
}

/*
 * INTERNALS: FINDING THE CHILD INDEX
 * Returns a pointer to the child page number that should contain `key`.
 * We binary-search the separator keys and pick the leftmost child whose
 * separator key >= key.  If key is larger than all separators, we
 * follow right_child_ptr.
 */
uint32_t* internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys = *internal_node_num_keys(node);
    uint32_t min_index = 0;
    uint32_t max_index = num_keys;

    while (min_index != max_index) {
        uint32_t index = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return internal_node_child(node, min_index);
}

/*
 * get_node_max_key — the largest key stored anywhere under this node.
 * For a leaf: it's the last cell's key (cells are sorted).
 * For an internal node: it's the last separator key (right subtree may
 * have larger keys, but the max separator is sufficient for our split
 * logic which only needs to update the parent's view of the max).
 */
uint32_t get_node_max_key(void* node) {
    switch (get_node_type(node)) {
        case NODE_LEAF:
            return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
        case NODE_INTERNAL:
            return *internal_node_key(node, *internal_node_num_keys(node) - 1);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 2 — Initialization helpers
 * ═══════════════════════════════════════════════════════════════ */

static void initialize_leaf_node(void* node) {
    set_node_type(node, NODE_LEAF);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0; /* sentinel: no right sibling */
    *leaf_node_prev_leaf(node) = 0; /* sentinel: no left sibling  */
}

static void initialize_internal_node(void* node) {
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 3 — Table lifecycle
 *
 * INTERNALS: DATABASE FILE FORMAT
 * ─────────────────────────────────
 * The .db file is a flat sequence of PAGE_SIZE (4096) byte pages.
 * Page 0 is always the B+ tree root (even if the root changes type
 * over time — see create_new_root).  The pager tracks how many pages
 * exist via the file length.
 * ═══════════════════════════════════════════════════════════════ */

Table* db_open(const char* filename) {
    Pager* pager = pager_open(filename);

    Table* table = (Table*)malloc(sizeof(Table));
    table->pager          = pager;
    table->root_page_num  = 0;
    table->in_transaction = false;
    table->username_index_enabled = false;
    table->username_index_dirty = false;
    table->username_index_entries = NULL;
    table->username_index_count = 0;
    table->username_index_capacity = 0;
    int catalog_len = snprintf(table->index_catalog_filename,
                               sizeof(table->index_catalog_filename),
                               "%s.catalog",
                               filename);
    int catalog_wal_len = snprintf(table->index_catalog_wal_filename,
                                   sizeof(table->index_catalog_wal_filename),
                                   "%s.catalog.wal",
                                   filename);
    int username_index_len = snprintf(table->username_index_filename,
                                      sizeof(table->username_index_filename),
                                      "%s.username.idx",
                                      filename);
    int username_index_wal_len = snprintf(table->username_index_wal_filename,
                                          sizeof(table->username_index_wal_filename),
                                          "%s.username.idx.wal",
                                          filename);
    if (catalog_len < 0 ||
        (size_t)catalog_len >= sizeof(table->index_catalog_filename) ||
        catalog_wal_len < 0 ||
        (size_t)catalog_wal_len >= sizeof(table->index_catalog_wal_filename) ||
        username_index_len < 0 ||
        (size_t)username_index_len >= sizeof(table->username_index_filename) ||
        username_index_wal_len < 0 ||
        (size_t)username_index_wal_len >= sizeof(table->username_index_wal_filename)) {
        printf("Database filename too long.\n");
        exit(EXIT_FAILURE);
    }
    table_load_index_catalog(table);

    if (pager->num_pages == 0) {
        /* Brand new database: initialize page 0 as an empty leaf root. */
        void* root_node = get_page(pager, 0);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);
    }

    return table;
}

void db_close(Table* table) {
    Pager* pager = table->pager;
    if (table->in_transaction) {
        pager_rollback(pager);
        table->in_transaction = false;
    }
    pager_checkpoint(pager); /* flush all pages and delete the WAL */

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (pager->pages[i] != NULL) {
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }

    fclose(pager->file);
    free(table->username_index_entries);
    free(pager);
    free(table);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 4 — Cursor operations
 *
 * INTERNALS: WHY A CURSOR?
 * ──────────────────────────
 * A cursor represents a position inside the table (a specific cell in a
 * specific leaf page).  It separates the "where am I?" state from the
 * B+ tree structure.  Multiple cursors could point to different rows
 * simultaneously (important for joins / iterators in more advanced DBs).
 * ═══════════════════════════════════════════════════════════════ */

static Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node      = get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Cursor* cursor    = malloc(sizeof(Cursor));
    cursor->table     = table;
    cursor->page_num  = page_num;
    cursor->end_of_table = false;

    /* Binary search for insertion point of `key` */
    uint32_t min_index        = 0;
    uint32_t one_past_max_index = num_cells;
    while (one_past_max_index != min_index) {
        uint32_t index         = (min_index + one_past_max_index) / 2;
        uint32_t key_at_index  = *leaf_node_key(node, index);
        if (key == key_at_index) {
            cursor->cell_num = index;
            return cursor;
        }
        if (key < key_at_index) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }

    cursor->cell_num = min_index;
    return cursor;
}

static Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node      = get_page(table->pager, page_num);
    uint32_t child_num = *internal_node_find_child(node, key);
    void*    child     = get_page(table->pager, child_num);

    switch (get_node_type(child)) {
        case NODE_LEAF:
            return leaf_node_find(table, child_num, key);
        case NODE_INTERNAL:
            return internal_node_find(table, child_num, key);
    }
    return NULL;
}

Cursor* table_find(Table* table, uint32_t key) {
    uint32_t root_page_num = table->root_page_num;
    void*    root_node     = get_page(table->pager, root_page_num);

    if (get_node_type(root_node) == NODE_LEAF) {
        return leaf_node_find(table, root_page_num, key);
    } else {
        return internal_node_find(table, root_page_num, key);
    }
}

Cursor* table_start(Table* table) {
    /* Find the position of the smallest key (key = 0 → leftmost leaf). */
    Cursor*  cursor    = table_find(table, 0);
    void*    node      = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    cursor->end_of_table = (num_cells == 0);
    return cursor;
}

void* cursor_value(Cursor* cursor) {
    void* page = get_page(cursor->table->pager, cursor->page_num);
    return leaf_node_value(page, cursor->cell_num);
}

/*
 * INTERNALS: MULTI-LEAF TRAVERSAL
 * ─────────────────────────────────
 * When we advance past the last cell in a leaf, we read next_leaf.
 * If it's non-zero, we move the cursor to cell 0 of that sibling.
 * If it's zero (no sibling), we mark end_of_table.
 * This is what makes a full-table SELECT O(n) instead of O(n log n).
 */
void cursor_advance(Cursor* cursor) {
    void* node = get_page(cursor->table->pager, cursor->page_num);
    cursor->cell_num += 1;

    /* Follow next_leaf pointers, skipping any empty leaves, until we land
     * on a cell or exhaust the chain. */
    while (cursor->cell_num >= *leaf_node_num_cells(node)) {
        uint32_t next_page = *leaf_node_next_leaf(node);
        if (next_page == 0) {
            cursor->end_of_table = true;
            return;
        }
        cursor->page_num = next_page;
        cursor->cell_num = 0;
        node = get_page(cursor->table->pager, next_page);
    }
}

/* Position a cursor at the last cell of the rightmost leaf (for DESC scans). */
Cursor* table_end(Table* table) {
    uint32_t page_num = table->root_page_num;
    void*    node     = get_page(table->pager, page_num);

    while (get_node_type(node) == NODE_INTERNAL) {
        page_num = *internal_node_right_child(node);
        node     = get_page(table->pager, page_num);
    }

    uint32_t num_cells = *leaf_node_num_cells(node);
    Cursor* cursor     = malloc(sizeof(Cursor));
    cursor->table        = table;
    cursor->page_num     = page_num;
    cursor->end_of_table = (num_cells == 0);
    cursor->cell_num     = (num_cells > 0) ? num_cells - 1 : 0;
    return cursor;
}

/* Move the cursor one step backwards (prev_leaf chain, right-to-left). */
void cursor_retreat(Cursor* cursor) {
    if (cursor->cell_num > 0) {
        cursor->cell_num--;
        return;
    }
    /* At cell 0 — must cross to the previous leaf. */
    void*    node      = get_page(cursor->table->pager, cursor->page_num);
    uint32_t prev_page = *leaf_node_prev_leaf(node);
    if (prev_page == 0) {
        cursor->end_of_table = true;
        return;
    }
    cursor->page_num = prev_page;
    node = get_page(cursor->table->pager, prev_page);

    /* Skip any empty leaves (guard against degenerate states). */
    while (*leaf_node_num_cells(node) == 0) {
        prev_page = *leaf_node_prev_leaf(node);
        if (prev_page == 0) { cursor->end_of_table = true; return; }
        cursor->page_num = prev_page;
        node = get_page(cursor->table->pager, prev_page);
    }
    cursor->cell_num = *leaf_node_num_cells(node) - 1;
}

static int compare_username_index_entries(const void* left, const void* right) {
    const UsernameIndexEntry* a = (const UsernameIndexEntry*)left;
    const UsernameIndexEntry* b = (const UsernameIndexEntry*)right;
    int name_cmp = strcmp(a->username, b->username);
    if (name_cmp != 0) return name_cmp;
    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return 0;
}

static void sync_index_file(FILE* file) {
    if (fflush(file) != 0) {
        printf("Unable to flush index file.\n");
        exit(EXIT_FAILURE);
    }

#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        printf("Unable to sync index file.\n");
        exit(EXIT_FAILURE);
    }
#else
    if (fsync(fileno(file)) != 0) {
        printf("Unable to sync index file.\n");
        exit(EXIT_FAILURE);
    }
#endif
}

static bool index_catalog_is_valid(IndexCatalog* catalog) {
    return catalog->magic == INDEX_CATALOG_MAGIC &&
           catalog->version == INDEX_CATALOG_VERSION;
}

static bool username_index_header_is_valid(UsernameIndexFileHeader* header) {
    return header->magic == USERNAME_INDEX_MAGIC &&
           header->version == USERNAME_INDEX_VERSION &&
           header->count <= USERNAME_INDEX_MAX_ENTRIES;
}

static bool read_username_index_record(FILE* file, UsernameIndexEntry* entry) {
    if (fread(entry->username, 1, sizeof(entry->username), file) != sizeof(entry->username)) {
        return false;
    }
    entry->username[sizeof(entry->username) - 1] = '\0';
    return fread(&entry->id, sizeof(entry->id), 1, file) == 1;
}

static void write_username_index_record(FILE* file, UsernameIndexEntry* entry) {
    if (fwrite(entry->username, 1, sizeof(entry->username), file) != sizeof(entry->username) ||
        fwrite(&entry->id, sizeof(entry->id), 1, file) != 1) {
        printf("Unable to write username index record.\n");
        exit(EXIT_FAILURE);
    }
}

static bool read_username_index_payload(FILE* file,
                                        UsernameIndexEntry** entries,
                                        uint32_t* count) {
    UsernameIndexFileHeader header;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        !username_index_header_is_valid(&header)) {
        return false;
    }

    UsernameIndexEntry* loaded = NULL;
    if (header.count > 0) {
        loaded = (UsernameIndexEntry*)malloc(header.count * sizeof(UsernameIndexEntry));
        if (loaded == NULL) {
            printf("Unable to allocate username index.\n");
            exit(EXIT_FAILURE);
        }
    }

    for (uint32_t i = 0; i < header.count; i++) {
        if (!read_username_index_record(file, &loaded[i])) {
            free(loaded);
            return false;
        }
    }

    *entries = loaded;
    *count = header.count;
    return true;
}

static void write_username_index_payload(FILE* file,
                                         UsernameIndexEntry* entries,
                                         uint32_t count) {
    UsernameIndexFileHeader header;
    header.magic = USERNAME_INDEX_MAGIC;
    header.version = USERNAME_INDEX_VERSION;
    header.count = count;

    if (fwrite(&header, sizeof(header), 1, file) != 1) {
        printf("Unable to write username index header.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < count; i++) {
        write_username_index_record(file, &entries[i]);
    }
}

static void table_write_username_index_file(Table* table,
                                            UsernameIndexEntry* entries,
                                            uint32_t count) {
    FILE* file = fopen(table->username_index_filename, "wb");
    if (file == NULL) {
        printf("Unable to write username index file.\n");
        exit(EXIT_FAILURE);
    }

    write_username_index_payload(file, entries, count);
    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close username index file.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_write_username_index_wal(Table* table,
                                           UsernameIndexEntry* entries,
                                           uint32_t count) {
    FILE* file = fopen(table->username_index_wal_filename, "wb");
    if (file == NULL) {
        printf("Unable to write username index WAL.\n");
        exit(EXIT_FAILURE);
    }

    write_username_index_payload(file, entries, count);
    uint32_t commit_magic = USERNAME_INDEX_WAL_COMMIT_MAGIC;
    if (fwrite(&commit_magic, sizeof(commit_magic), 1, file) != 1) {
        fclose(file);
        printf("Unable to write username index WAL.\n");
        exit(EXIT_FAILURE);
    }

    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close username index WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_recover_username_index_file(Table* table) {
    FILE* file = fopen(table->username_index_wal_filename, "rb");
    if (file == NULL) {
        return;
    }

    printf("Username index WAL found. Recovering...\n");

    UsernameIndexEntry* entries = NULL;
    uint32_t count = 0;
    uint32_t commit_magic = 0;
    bool complete = read_username_index_payload(file, &entries, &count) &&
                    fread(&commit_magic, sizeof(commit_magic), 1, file) == 1;
    fclose(file);

    if (!complete || commit_magic != USERNAME_INDEX_WAL_COMMIT_MAGIC) {
        printf("Ignoring incomplete username index WAL.\n");
        free(entries);
        if (remove(table->username_index_wal_filename) != 0) {
            printf("Unable to remove incomplete username index WAL.\n");
            exit(EXIT_FAILURE);
        }
        return;
    }

    table_write_username_index_file(table, entries, count);
    free(entries);

    if (remove(table->username_index_wal_filename) != 0) {
        printf("Unable to remove recovered username index WAL.\n");
        exit(EXIT_FAILURE);
    }
    printf("Username index recovery complete.\n");
}

static bool table_load_username_index_file(Table* table) {
    FILE* file = fopen(table->username_index_filename, "rb");
    if (file == NULL) {
        return false;
    }

    UsernameIndexEntry* entries = NULL;
    uint32_t count = 0;
    bool ok = read_username_index_payload(file, &entries, &count);
    fclose(file);

    if (!ok) {
        free(entries);
        return false;
    }

    if (count > 1) {
        qsort(entries,
              count,
              sizeof(UsernameIndexEntry),
              compare_username_index_entries);
    }

    free(table->username_index_entries);
    table->username_index_entries = entries;
    table->username_index_count = count;
    table->username_index_capacity = count;
    table->username_index_dirty = false;
    return true;
}

static void table_persist_username_index_file(Table* table) {
    table_write_username_index_wal(table,
                                   table->username_index_entries,
                                   table->username_index_count);
    table_write_username_index_file(table,
                                    table->username_index_entries,
                                    table->username_index_count);

    if (remove(table->username_index_wal_filename) != 0) {
        printf("Unable to remove committed username index WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void remove_index_file_if_exists(const char* filename,
                                        const char* error_message) {
    if (remove(filename) != 0 && errno != ENOENT) {
        printf("%s\n", error_message);
        exit(EXIT_FAILURE);
    }
}

static bool table_read_index_catalog_file(const char* filename,
                                          IndexCatalog* catalog) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        return false;
    }

    size_t read_count = fread(catalog, sizeof(IndexCatalog), 1, file);
    fclose(file);

    return read_count == 1 && index_catalog_is_valid(catalog);
}

static void table_write_index_catalog_file(Table* table,
                                           IndexCatalog* catalog) {
    FILE* file = fopen(table->index_catalog_filename, "wb");
    if (file == NULL) {
        printf("Unable to write index catalog.\n");
        exit(EXIT_FAILURE);
    }

    if (fwrite(catalog, sizeof(IndexCatalog), 1, file) != 1) {
        fclose(file);
        printf("Unable to write index catalog.\n");
        exit(EXIT_FAILURE);
    }

    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close index catalog.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_write_index_catalog_wal(Table* table,
                                          IndexCatalog* catalog) {
    FILE* file = fopen(table->index_catalog_wal_filename, "wb");
    if (file == NULL) {
        printf("Unable to write index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }

    uint32_t commit_magic = INDEX_CATALOG_WAL_COMMIT_MAGIC;
    if (fwrite(catalog, sizeof(IndexCatalog), 1, file) != 1 ||
        fwrite(&commit_magic, sizeof(commit_magic), 1, file) != 1) {
        fclose(file);
        printf("Unable to write index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }

    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_recover_index_catalog(Table* table) {
    FILE* file = fopen(table->index_catalog_wal_filename, "rb");
    if (file == NULL) {
        return;
    }

    printf("Index catalog WAL found. Recovering...\n");

    IndexCatalog catalog;
    uint32_t commit_magic = 0;
    bool complete = fread(&catalog, sizeof(IndexCatalog), 1, file) == 1 &&
                    fread(&commit_magic, sizeof(commit_magic), 1, file) == 1;
    fclose(file);

    if (!complete ||
        commit_magic != INDEX_CATALOG_WAL_COMMIT_MAGIC ||
        !index_catalog_is_valid(&catalog)) {
        printf("Ignoring incomplete index catalog WAL.\n");
        if (remove(table->index_catalog_wal_filename) != 0) {
            printf("Unable to remove incomplete index catalog WAL.\n");
            exit(EXIT_FAILURE);
        }
        return;
    }

    table_write_index_catalog_file(table, &catalog);
    if (remove(table->index_catalog_wal_filename) != 0) {
        printf("Unable to remove recovered index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }
    printf("Index catalog recovery complete.\n");
}

static void table_load_index_catalog(Table* table) {
    table_recover_username_index_file(table);
    table_recover_index_catalog(table);

    IndexCatalog catalog;
    if (!table_read_index_catalog_file(table->index_catalog_filename,
                                       &catalog)) {
        return;
    }

    if (catalog.username_index_enabled != 0) {
        table->username_index_enabled = true;
        if (catalog.username_index_entries_valid != 0 &&
            table_load_username_index_file(table)) {
            table->username_index_dirty = false;
        } else {
            table->username_index_dirty = true;
        }
    }
}

static void table_save_index_catalog_state(Table* table, bool entries_valid) {
    IndexCatalog catalog;
    catalog.magic = INDEX_CATALOG_MAGIC;
    catalog.version = INDEX_CATALOG_VERSION;
    catalog.username_index_enabled = table->username_index_enabled ? 1u : 0u;
    catalog.username_index_entries_valid = entries_valid ? 1u : 0u;

    table_write_index_catalog_wal(table, &catalog);
    table_write_index_catalog_file(table, &catalog);

    if (remove(table->index_catalog_wal_filename) != 0) {
        printf("Unable to remove committed index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void username_index_append(Table* table, Row* row) {
    if (table->username_index_count == table->username_index_capacity) {
        uint32_t new_capacity = table->username_index_capacity == 0
            ? 16
            : table->username_index_capacity * 2;
        UsernameIndexEntry* new_entries = realloc(
            table->username_index_entries,
            new_capacity * sizeof(UsernameIndexEntry)
        );
        if (new_entries == NULL) {
            printf("Unable to allocate username index.\n");
            exit(EXIT_FAILURE);
        }
        table->username_index_entries = new_entries;
        table->username_index_capacity = new_capacity;
    }

    UsernameIndexEntry* entry = &table->username_index_entries[table->username_index_count++];
    strncpy(entry->username, row->username, sizeof(entry->username) - 1);
    entry->username[sizeof(entry->username) - 1] = '\0';
    entry->id = row->id;
}

void table_mark_username_index_dirty(Table* table) {
    if (table->username_index_enabled) {
        table->username_index_dirty = true;
    }
}

void table_ensure_username_index(Table* table) {
    if (!table->username_index_enabled || !table->username_index_dirty) {
        return;
    }

    table->username_index_count = 0;

    Cursor* cursor = table_start(table);
    Row row;
    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row);
        username_index_append(table, &row);
        cursor_advance(cursor);
    }
    free(cursor);

    if (table->username_index_count > 1) {
        qsort(table->username_index_entries,
              table->username_index_count,
              sizeof(UsernameIndexEntry),
              compare_username_index_entries);
    }

    if (table->in_transaction) {
        return;
    }

    table_persist_username_index_file(table);
    table_save_index_catalog_state(table, true);
    table->username_index_dirty = false;
}

void table_create_username_index(Table* table) {
    table->username_index_enabled = true;
    table->username_index_dirty = true;
    table_ensure_username_index(table);
}

void table_drop_username_index(Table* table) {
    table->username_index_enabled = false;
    table->username_index_dirty = false;
    table->username_index_count = 0;
    table->username_index_capacity = 0;
    free(table->username_index_entries);
    table->username_index_entries = NULL;

    remove_index_file_if_exists(table->username_index_filename,
                                "Unable to remove username index file.");
    remove_index_file_if_exists(table->username_index_wal_filename,
                                "Unable to remove username index WAL.");
    table_save_index_catalog_state(table, false);
}

void table_prepare_username_index_commit(Table* table) {
    if (table->username_index_enabled && table->username_index_dirty) {
        table_save_index_catalog_state(table, false);
    }
}

void table_finalize_username_index_commit(Table* table) {
    if (table->username_index_enabled && table->username_index_dirty) {
        table_ensure_username_index(table);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 5 — Internal node: update parent separator key
 *
 * After inserting into a child, the parent's separator key for that
 * child may need to be updated if the child's max key changed (e.g.
 * because we inserted the largest key into a right sibling that was
 * just created).
 * ═══════════════════════════════════════════════════════════════ */

static void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t old_child_index = *internal_node_find_child(node, old_key);
    /* old_child_index is actually the VALUE of the child pointer; walk
     * through keys to find which separator to update. */
    uint32_t num_keys = *internal_node_num_keys(node);
    for (uint32_t i = 0; i < num_keys; i++) {
        if (*internal_node_key(node, i) == old_key) {
            *internal_node_key(node, i) = new_key;
            return;
        }
    }
    (void)old_child_index;
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 6 — Internal node split
 *
 * INTERNALS: INTERNAL NODE SPLIT
 * ────────────────────────────────
 * When an internal node is full (has INTERNAL_NODE_MAX_KEYS keys),
 * inserting another key requires splitting it.
 *
 * Split strategy:
 *  1. Create a new right internal node.
 *  2. The middle key is pushed UP to the parent (unlike leaf splits
 *     where the key is COPIED up).  This is the key difference between
 *     leaf splits and internal splits in a B+ tree.
 *  3. Left node keeps the left half of keys/children.
 *  4. Right node gets the right half.
 *  5. Recurse up if the parent is also full.
 * ═══════════════════════════════════════════════════════════════ */

/* Forward declaration */
static void internal_node_insert(Table* table, uint32_t parent_page_num,
                                  uint32_t child_page_num);

static void internal_node_split_and_insert(Table* table, uint32_t page_num,
                                            uint32_t child_page_num) {
    void*    old_node      = get_page(table->pager, page_num);
    uint32_t old_max       = get_node_max_key(old_node);
    mark_page_dirty(table->pager, page_num);

    void*    child         = get_page(table->pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(child);

    uint32_t new_page_num  = get_unused_page_num(table->pager);
    void*    new_node      = get_page(table->pager, new_page_num);
    initialize_internal_node(new_node);
    mark_page_dirty(table->pager, new_page_num);

    bool splitting_root = is_node_root(old_node);

    /* Temporary scratch space: copy all current child ptrs + keys,
     * then rebuild left/right.  Max children = MAX_KEYS + 1, plus one
     * new child = MAX_KEYS + 2 total.  We'll use a local array. */
    uint32_t total_children = *internal_node_num_keys(old_node) + 2; /* +1 right child, +1 new */
    uint32_t* tmp_children  = malloc(total_children * sizeof(uint32_t));
    uint32_t* tmp_keys       = malloc((total_children - 1) * sizeof(uint32_t));

    /* Read existing children and keys from old_node */
    uint32_t num_keys = *internal_node_num_keys(old_node);
    uint32_t insert_idx = 0;

    /* Find insertion position for new child */
    for (insert_idx = 0; insert_idx <= num_keys; insert_idx++) {
        uint32_t existing_child = *internal_node_child(old_node, insert_idx);
        void* ec = get_page(table->pager, existing_child);
        uint32_t ec_max = get_node_max_key(ec);
        if (child_max_key < ec_max) break;
    }

    /* Build the merged, sorted arrays */
    uint32_t ci = 0; /* index into child_page_num */
    bool inserted = false;
    uint32_t out = 0;
    for (uint32_t i = 0; i <= num_keys; i++) {
        if (!inserted && out == insert_idx) {
            tmp_children[out] = child_page_num;
            inserted = true;
            if (i <= num_keys && i > 0) {
                /* separator key between previous child and new child */
                tmp_keys[out - 1] = child_max_key;
            }
            /* Don't increment i — re-process the current existing child */
            tmp_children[out + 1] = *internal_node_child(old_node, i);
            if (i < num_keys) {
                tmp_keys[out] = *internal_node_key(old_node, i);
            }
            out += 2;
            ci++;
        } else {
            tmp_children[out] = *internal_node_child(old_node, i);
            if (i < num_keys) tmp_keys[out - 1 + (out == 0 ? 1 : 0)] = *internal_node_key(old_node, i);
            out++;
        }
    }
    if (!inserted) {
        /* new child goes at the end */
        tmp_children[out] = child_page_num;
        tmp_keys[out - 1] = child_max_key;
        out++;
    }
    (void)ci;

    /* The above scratch-space approach is complex; use a simpler
     * in-place method instead: just call internal_node_insert which
     * inserts into old_node first, then split. */

    /* ── Simpler approach: insert into old first, then split ── */
    free(tmp_children);
    free(tmp_keys);

    /* Insert the new child into old_node (it's temporarily over-full by 1) */
    uint32_t cur_num_keys = *internal_node_num_keys(old_node);
    /* Find where the new child's separator key goes */
    uint32_t new_separator = child_max_key;
    /* Shift keys and children right to make room */
    uint32_t idx;
    for (idx = cur_num_keys; idx > 0; idx--) {
        if (*internal_node_key(old_node, idx - 1) > new_separator) {
            *internal_node_key(old_node, idx)   = *internal_node_key(old_node, idx - 1);
            *internal_node_child(old_node, idx + 1) = *internal_node_child(old_node, idx);
        } else {
            break;
        }
    }
    *internal_node_key(old_node, idx)       = new_separator;
    *internal_node_child(old_node, idx + 1) = *internal_node_right_child(old_node);
    *internal_node_right_child(old_node)    = child_page_num;
    *internal_node_num_keys(old_node)       = cur_num_keys + 1;

    /* Now split: left keeps LEFT_SPLIT_COUNT keys, middle key is pushed up,
     * right gets the remaining keys. */
    uint32_t left_count  = INTERNAL_NODE_LEFT_SPLIT_COUNT;
    uint32_t right_count = *internal_node_num_keys(old_node) - left_count - 1;
    uint32_t mid_key     = *internal_node_key(old_node, left_count);

    /* Copy right half into new_node */
    *internal_node_num_keys(new_node)      = right_count;
    *internal_node_right_child(new_node)   = *internal_node_right_child(old_node);

    for (uint32_t i = 0; i < right_count; i++) {
        *internal_node_child(new_node, i) =
            *internal_node_child(old_node, left_count + 1 + i);
        *internal_node_key(new_node, i) =
            *internal_node_key(old_node, left_count + 1 + i);
    }

    /* Update right_child of left (old) node */
    *internal_node_right_child(old_node) =
        *internal_node_child(old_node, left_count);
    *internal_node_num_keys(old_node)    = left_count;

    /* Update parent pointers for all children we moved to new_node */
    for (uint32_t i = 0; i <= right_count; i++) {
        uint32_t moved_child_page = *internal_node_child(new_node, i);
        void* moved_child = get_page(table->pager, moved_child_page);
        *node_parent(moved_child) = new_page_num;
        mark_page_dirty(table->pager, moved_child_page);
    }

    if (splitting_root) {
        /* Create a brand-new root that points to old_node (left) and new_node (right). */
        uint32_t left_page_num = get_unused_page_num(table->pager);
        void* left_node = get_page(table->pager, left_page_num);
        memcpy(left_node, old_node, PAGE_SIZE);
        set_node_root(left_node, false);
        mark_page_dirty(table->pager, left_page_num);

        initialize_internal_node(old_node);
        set_node_root(old_node, true);
        *internal_node_num_keys(old_node)    = 1;
        *internal_node_child(old_node, 0)    = left_page_num;
        *internal_node_key(old_node, 0)      = mid_key;
        *internal_node_right_child(old_node) = new_page_num;
        *node_parent(left_node) = table->root_page_num;
        *node_parent(new_node)  = table->root_page_num;
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        /* Update the old_node's separator in the parent */
        uint32_t new_max = get_node_max_key(old_node);
        void* parent = get_page(table->pager, parent_page_num);
        update_internal_node_key(parent, old_max, new_max);
        *node_parent(new_node) = parent_page_num;
        mark_page_dirty(table->pager, parent_page_num);
        internal_node_insert(table, parent_page_num, new_page_num);
    }
}

/*
 * internal_node_insert — insert a new child page number into the given
 * internal node, splitting if necessary.
 *
 * INTERNALS: INSERT INTO INTERNAL NODE
 * ──────────────────────────────────────
 * The new child's maximum key serves as the separator key.  We find the
 * correct slot (binary search), shift existing cells right, and write
 * the new (child_ptr, separator_key) pair.
 * If the node is already full we delegate to internal_node_split_and_insert.
 */
static void internal_node_insert(Table* table, uint32_t parent_page_num,
                                  uint32_t child_page_num) {
    void*    parent   = get_page(table->pager, parent_page_num);
    void*    child    = get_page(table->pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(child);
    uint32_t num_keys = *internal_node_num_keys(parent);

    if (num_keys >= INTERNAL_NODE_MAX_KEYS) {
        internal_node_split_and_insert(table, parent_page_num, child_page_num);
        return;
    }

    /* Find insertion index: first key slot whose key >= child_max_key */
    uint32_t right_child_page_num = *internal_node_right_child(parent);
    void*    right_child          = get_page(table->pager, right_child_page_num);

    if (child_max_key > get_node_max_key(right_child)) {
        /* New child becomes the new right child; old right_child becomes a
         * regular interior child with key = its max key. */
        *internal_node_cell(parent, num_keys)  = right_child_page_num;
        *internal_node_key(parent, num_keys)   = get_node_max_key(right_child);
        *internal_node_right_child(parent)      = child_page_num;
    } else {
        uint32_t index = 0;
        while (index < num_keys &&
               *internal_node_key(parent, index) < child_max_key) {
            index++;
        }

        /* Shift cells right to make room for the new separator */
        for (uint32_t i = num_keys; i > index; i--) {
            void* dest = internal_node_cell(parent, i);
            void* src  = internal_node_cell(parent, i - 1);
            memcpy(dest, src, INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_cell(parent, index)  = child_page_num;
        *internal_node_key(parent, index)   = child_max_key;
    }

    *internal_node_num_keys(parent) = num_keys + 1;
    *node_parent(child) = parent_page_num;
    mark_page_dirty(table->pager, child_page_num);
    mark_page_dirty(table->pager, parent_page_num);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 7 — Root creation
 *
 * INTERNALS: HOW THE TREE GROWS TALLER
 * ──────────────────────────────────────
 * When the root (which starts as a leaf) overflows:
 *  1. We allocate a new left_child page and copy the old root there.
 *  2. We re-initialize the old root page as an internal node.
 *  3. The internal root now points to left_child (left) and the newly
 *     split right_child (right).
 * By always keeping the root at page 0, we avoid having to update any
 * pointer that stores "root_page_num" — it's always 0.
 * ═══════════════════════════════════════════════════════════════ */

static void create_new_root(Table* table, uint32_t right_child_page_num) {
    void*    root        = get_page(table->pager, table->root_page_num);
    void*    right_child = get_page(table->pager, right_child_page_num);
    mark_page_dirty(table->pager, table->root_page_num);

    uint32_t left_child_page_num = get_unused_page_num(table->pager);
    void*    left_child          = get_page(table->pager, left_child_page_num);
    mark_page_dirty(table->pager, left_child_page_num);

    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);
    *node_parent(left_child) = table->root_page_num;

    /* After the copy, right_child->prev_leaf still points to old page 0
     * (now an internal root).  Fix it to point to the real left leaf. */
    if (get_node_type(left_child) == NODE_LEAF) {
        *leaf_node_prev_leaf(right_child) = left_child_page_num;
        mark_page_dirty(table->pager, right_child_page_num);
    }

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root)      = 1;
    *internal_node_child(root, 0)      = left_child_page_num;
    *internal_node_key(root, 0)        = get_node_max_key(left_child);
    *internal_node_right_child(root)   = right_child_page_num;
    *node_parent(right_child)          = table->root_page_num;
    mark_page_dirty(table->pager, right_child_page_num);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 8 — Leaf node insert and split
 *
 * INTERNALS: LEAF SPLIT
 * ──────────────────────
 * When a leaf is full and we need to insert another cell:
 *  1. Allocate a new leaf page.
 *  2. Redistribute cells: left keeps LEFT_SPLIT_COUNT, right gets the rest.
 *  3. Wire the sibling pointers: old->next_leaf = new; new->next_leaf = old's old next.
 *  4. Push the right leaf's min key up to the parent.
 *     - If the old leaf was the root, call create_new_root.
 *     - Otherwise, call internal_node_insert(parent, new_page).
 * ═══════════════════════════════════════════════════════════════ */

void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
    void*    old_node     = get_page(cursor->table->pager, cursor->page_num);
    uint32_t old_max      = get_node_max_key(old_node);
    mark_page_dirty(cursor->table->pager, cursor->page_num);

    uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
    void*    new_node     = get_page(cursor->table->pager, new_page_num);
    initialize_leaf_node(new_node);
    mark_page_dirty(cursor->table->pager, new_page_num);

    /* Maintain the doubly-linked sibling list.
     *  old_node <--> new_node <--> old_right (if any)            */
    uint32_t old_right = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(new_node) = old_right;
    *leaf_node_prev_leaf(new_node) = cursor->page_num; /* old_node's page */
    *leaf_node_next_leaf(old_node) = new_page_num;
    if (old_right != 0) {
        void* old_right_node = get_page(cursor->table->pager, old_right);
        *leaf_node_prev_leaf(old_right_node) = new_page_num;
        mark_page_dirty(cursor->table->pager, old_right);
    }

    /* Set parent pointer on new node */
    *node_parent(new_node) = *node_parent(old_node);

    /*
     * Redistribute: iterate over all existing cells + 1 new cell (total
     * LEAF_NODE_MAX_CELLS + 1) from right to left, placing each into
     * either the old (left) or new (right) node.
     */
    for (int32_t i = LEAF_NODE_MAX_CELLS; i >= 0; i--) {
        void*    destination_node;
        if (i >= LEAF_NODE_LEFT_SPLIT_COUNT) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }
        uint32_t index_within_node = (uint32_t)i % LEAF_NODE_LEFT_SPLIT_COUNT;
        void*    destination       = leaf_node_cell(destination_node, index_within_node);

        if (i == (int32_t)cursor->cell_num) {
            /* This is the newly inserted cell */
            serialize_row(value, leaf_node_value(destination_node, index_within_node));
            *leaf_node_key(destination_node, index_within_node) = key;
        } else if (i > (int32_t)cursor->cell_num) {
            memcpy(destination, leaf_node_cell(old_node, (uint32_t)i - 1), LEAF_NODE_CELL_SIZE);
        } else {
            memcpy(destination, leaf_node_cell(old_node, (uint32_t)i), LEAF_NODE_CELL_SIZE);
        }
    }

    *(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
    *(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;

    if (is_node_root(old_node)) {
        create_new_root(cursor->table, new_page_num);
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        void*    parent          = get_page(cursor->table->pager, parent_page_num);
        uint32_t new_max         = get_node_max_key(old_node);

        update_internal_node_key(parent, old_max, new_max);
        *node_parent(new_node) = parent_page_num;
        internal_node_insert(cursor->table, parent_page_num, new_page_num);
    }
}

void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
    void*    node      = get_page(cursor->table->pager, cursor->page_num);
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (num_cells >= LEAF_NODE_MAX_CELLS) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }

    /* Shift cells right to open a slot for the new cell */
    if (cursor->cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
            memcpy(leaf_node_cell(node, i),
                   leaf_node_cell(node, i - 1),
                   LEAF_NODE_CELL_SIZE);
        }
    }

    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cursor->cell_num)) = key;
    serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 8b — Empty-leaf removal helpers
 *
 * When a leaf loses its last cell it must be excised from the tree so
 * that internal-node routing and the leaf linked-list stay consistent.
 *
 * Strategy (simplified for educational purposes):
 *  1. Unlink the empty leaf from the next_leaf chain.
 *  2. Remove its slot from its parent internal node.
 *  3. If the parent is now a 0-key root, collapse it: its sole remaining
 *     child becomes the new root content (the tree shrinks by one level).
 *  4. Non-root internal nodes that become 0-key are left in place; their
 *     routing still works (internal_node_find_child always returns
 *     right_child when num_keys == 0), at the cost of one wasted level.
 * ═══════════════════════════════════════════════════════════════ */

/* Remove empty_page from the doubly-linked leaf chain in O(1). */
static void unlink_from_leaf_chain(Table* table, uint32_t empty_page) {
    void*    empty_node  = get_page(table->pager, empty_page);
    uint32_t predecessor = *leaf_node_prev_leaf(empty_node);
    uint32_t successor   = *leaf_node_next_leaf(empty_node);

    if (predecessor != 0) {
        void* pred = get_page(table->pager, predecessor);
        *leaf_node_next_leaf(pred) = successor;
        mark_page_dirty(table->pager, predecessor);
    }
    if (successor != 0) {
        void* succ = get_page(table->pager, successor);
        *leaf_node_prev_leaf(succ) = predecessor;
        mark_page_dirty(table->pager, successor);
    }
}

/* Remove child_page from parent's children array.
 *
 * Layout reminder:
 *   cells[0..(num_keys-1)] each hold (child_ptr, separator_key).
 *   right_child is stored separately in the header.
 *
 * Case A – removing a regular cell (child_idx < num_keys):
 *   Shift cells [child_idx+1 .. num_keys-1] left by one; decrement num_keys.
 *   right_child is unchanged.
 *
 * Case B – removing the right child (child_idx == num_keys):
 *   The previous last cell's child pointer becomes the new right_child;
 *   decrement num_keys (dropping that cell's separator key implicitly).
 *
 * Root collapse: if the parent is the root and ends up with 0 keys,
 *   copy its sole remaining child into page 0 so the root is always page 0.
 */
static void remove_child_from_parent(Table* table, uint32_t parent_page,
                                     uint32_t child_page) {
    void*    parent   = get_page(table->pager, parent_page);
    uint32_t num_keys = *internal_node_num_keys(parent);

    /* Locate child_page among the parent's children. */
    uint32_t child_idx = num_keys; /* assume right_child until found */
    for (uint32_t i = 0; i <= num_keys; i++) {
        if (*internal_node_child(parent, i) == child_page) {
            child_idx = i;
            break;
        }
    }

    if (child_idx == num_keys) {
        /* Case B: removing the right_child. */
        if (num_keys == 0) return; /* degenerate; shouldn't happen */
        *internal_node_right_child(parent) =
            *internal_node_child(parent, num_keys - 1);
    } else {
        /* Case A: shift cells left to fill the gap. */
        for (uint32_t i = child_idx; i < num_keys - 1; i++) {
            void* dst = internal_node_cell(parent, i);
            void* src = internal_node_cell(parent, i + 1);
            memcpy(dst, src, INTERNAL_NODE_CELL_SIZE);
        }
    }
    *internal_node_num_keys(parent) = num_keys - 1;
    mark_page_dirty(table->pager, parent_page);

    /* Root collapse: the tree shrinks by one level. */
    if (*internal_node_num_keys(parent) == 0 && is_node_root(parent)) {
        uint32_t sole_child_page = *internal_node_right_child(parent);
        void*    sole_child      = get_page(table->pager, sole_child_page);
        memcpy(parent, sole_child, PAGE_SIZE);
        set_node_root(parent, true);
        mark_page_dirty(table->pager, parent_page);

        /* sole_child_page is now orphaned — its content lives in page 0. */
        pager_free_page(table->pager, sole_child_page);

        /* If the new root is an internal node, update its children's
         * parent pointers to point back to page 0. */
        if (get_node_type(parent) == NODE_INTERNAL) {
            uint32_t nk = *internal_node_num_keys(parent);
            for (uint32_t i = 0; i <= nk; i++) {
                uint32_t child_p = *internal_node_child(parent, i);
                void*    child   = get_page(table->pager, child_p);
                *node_parent(child) = table->root_page_num;
                mark_page_dirty(table->pager, child_p);
            }
        }
    }
}

/* Called after a leaf loses its last cell.
 * If it is the root (single-leaf tree), leave it as an empty root —
 * the table is simply empty.  Otherwise excise it from the tree and
 * add the page to the free list for reuse. */
static void remove_empty_leaf(Table* table, uint32_t leaf_page) {
    void* leaf = get_page(table->pager, leaf_page);
    if (is_node_root(leaf)) return; /* empty table: root stays as empty leaf */

    unlink_from_leaf_chain(table, leaf_page);

    uint32_t parent_page = *node_parent(leaf);
    remove_child_from_parent(table, parent_page, leaf_page);

    /* leaf_page is now unreachable from the tree — reclaim it. */
    pager_free_page(table->pager, leaf_page);
}

void leaf_node_delete(Cursor* cursor) {
    uint32_t page_num  = cursor->page_num;
    void*    node      = get_page(cursor->table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
        memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i + 1), LEAF_NODE_CELL_SIZE);
    }

    *leaf_node_num_cells(node) = num_cells - 1;
    mark_page_dirty(cursor->table->pager, page_num);

    if (num_cells - 1 == 0) {
        remove_empty_leaf(cursor->table, page_num);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 9 — Row serialization
 *
 * INTERNALS: ROW SERIALIZATION
 * ─────────────────────────────
 * We copy each field individually using memcpy to avoid struct padding
 * issues.  The on-disk layout is always:
 *   [id: 4 bytes][username: 33 bytes][email: 256 bytes]
 * regardless of how the compiler lays out the Row struct in memory.
 * ═══════════════════════════════════════════════════════════════ */

void serialize_row(Row* source, void* destination) {
    memcpy((char*)destination + ID_OFFSET,       &(source->id),       ID_SIZE);
    memcpy((char*)destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy((char*)destination + EMAIL_OFFSET,    &(source->email),    EMAIL_SIZE);
}

void deserialize_row(void* source, Row* destination) {
    memcpy(&(destination->id),       (char*)source + ID_OFFSET,       ID_SIZE);
    memcpy(&(destination->username), (char*)source + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email),    (char*)source + EMAIL_OFFSET,    EMAIL_SIZE);
}

void print_row(Row* row) {
    printf("(%u, %s, %s)\n", row->id, row->username, row->email);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 11 — Table truncate
 *
 * Discards every page except the root and re-initialises page 0 as an
 * empty leaf.  Orphaned pages remain in the file but are unreachable
 * from the tree; subsequent inserts allocate fresh pages beyond them.
 * ═══════════════════════════════════════════════════════════════ */

void table_truncate(Table* table) {
    Pager* pager = table->pager;

    /* Drop all pages except the root, clear the free list, and truncate
     * the file on disk so reopening gives the correct page count. */
    pager_shrink(pager, 1);

    void* root = get_page(pager, 0);
    memset(root, 0, PAGE_SIZE);
    set_node_type(root, NODE_LEAF);
    set_node_root(root, true);
    /* num_cells and next_leaf are already 0 from memset */
    mark_page_dirty(pager, 0);
    table_mark_username_index_dirty(table);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 10 — Tree printer
 *
 * INTERNALS: READING THE .btree OUTPUT
 * ──────────────────────────────────────
 * Prints the B+ tree structure for debugging / learning.  Example output
 * for a tree with 15 rows (after one root split):
 *
 *   - internal (size 1)
 *     - leaf (size 7)       ← left child, keys 1..7
 *       - 1
 *       - 2 ...
 *     - key 7               ← separator key
 *     - leaf (size 8)       ← right child, keys 8..15
 *       - 8 ...
 * ═══════════════════════════════════════════════════════════════ */

static void indent(uint32_t level) {
    for (uint32_t i = 0; i < level; i++) printf("  ");
}

void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level) {
    void*    node     = get_page(pager, page_num);
    uint32_t num_keys, child;

    switch (get_node_type(node)) {
        case NODE_LEAF:
            num_keys = *leaf_node_num_cells(node);
            indent(indentation_level);
            printf("- leaf (size %u)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                indent(indentation_level + 1);
                printf("- %u\n", *leaf_node_key(node, i));
            }
            break;
        case NODE_INTERNAL:
            num_keys = *internal_node_num_keys(node);
            indent(indentation_level);
            printf("- internal (size %u)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                child = *internal_node_child(node, i);
                print_tree(pager, child, indentation_level + 1);
                indent(indentation_level + 1);
                printf("- key %u\n", *internal_node_key(node, i));
            }
            child = *internal_node_right_child(node);
            print_tree(pager, child, indentation_level + 1);
            break;
    }
}
