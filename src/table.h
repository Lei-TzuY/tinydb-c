/*
 * table.h — Schema, B+ Tree Node Layout, Cursor, and Table API
 *
 * INTERNALS: DATABASE PAGES AND ROWS
 * ─────────────────────────────────────────────────────────────
 * A "page" is a fixed-size block of bytes (PAGE_SIZE = 4096) that maps
 * directly to a disk block.  Every read/write goes through the Pager,
 * which keeps pages cached in memory.
 *
 * A "row" is a record in our single-table database.  We store three
 * columns: id (uint32), username (32 chars), email (255 chars).
 * Rows are serialized into binary blobs that sit inside B+ tree leaf nodes.
 *
 * ROW SERIALIZATION
 * ─────────────────────────────────────────────────────────────
 *   | id (4 B) | username (33 B) | email (256 B) | = ROW_SIZE bytes
 *
 * serialize_row copies each field into a flat memory region.
 * deserialize_row reconstructs the Row struct from that region.
 * This lets us store and load rows without caring about struct padding.
 *
 * B+ TREE NODE LAYOUT
 * ─────────────────────────────────────────────────────────────
 * Every page is either a LEAF node or an INTERNAL node.
 * All nodes share a common header (type, is_root, parent_ptr).
 *
 *  Common header (6 bytes):
 *   [ node_type (1) | is_root (1) | parent_ptr (4) ]
 *
 *  Leaf node extra header (8 bytes):
 *   [ num_cells (4) | next_leaf (4) ]
 *   next_leaf = page number of the right sibling leaf (0 = none).
 *   This is the defining property of a B+ TREE (vs. plain B-tree):
 *   leaf nodes are linked, enabling efficient sequential scans.
 *
 *  Each leaf cell:
 *   [ key (4) | row_data (ROW_SIZE) ]
 *
 *  Internal node extra header (8 bytes):
 *   [ num_keys (4) | right_child_ptr (4) ]
 *
 *  Each internal cell:
 *   [ child_ptr (4) | key (4) ]
 *   The rightmost child is stored separately in the header.
 */

#ifndef TABLE_H
#define TABLE_H

#include "common.h"
#include "pager.h"

/* ─── Row definition ─────────────────────────────────────────── */

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

typedef struct {
    uint32_t id;
    char username[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef struct {
    char username[COLUMN_USERNAME_SIZE + 1];
    uint32_t id;
} UsernameIndexEntry;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

#define ID_SIZE       size_of_attribute(Row, id)
#define USERNAME_SIZE size_of_attribute(Row, username)
#define EMAIL_SIZE    size_of_attribute(Row, email)

#define ID_OFFSET       0
#define USERNAME_OFFSET (ID_OFFSET + ID_SIZE)
#define EMAIL_OFFSET    (USERNAME_OFFSET + USERNAME_SIZE)
#define ROW_SIZE        (ID_SIZE + USERNAME_SIZE + EMAIL_SIZE)

/* ─── Common node header ─────────────────────────────────────── */

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

#define NODE_TYPE_SIZE          sizeof(uint8_t)
#define NODE_TYPE_OFFSET        0
#define IS_ROOT_SIZE            sizeof(uint8_t)
#define IS_ROOT_OFFSET          (NODE_TYPE_OFFSET + NODE_TYPE_SIZE)
#define PARENT_POINTER_SIZE     sizeof(uint32_t)
#define PARENT_POINTER_OFFSET   (IS_ROOT_OFFSET + IS_ROOT_SIZE)
#define COMMON_NODE_HEADER_SIZE (NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE)

/* ─── Leaf node layout ───────────────────────────────────────── */
/*
 * INTERNALS: SIBLING POINTER (next_leaf)
 * ──────────────────────────────────────
 * A B+ tree stores ALL data in leaf nodes.  Internal nodes hold only
 * separator keys to route searches.  The leaf linked-list means a full
 * table scan is O(n) — just walk next_leaf pointers from the leftmost
 * leaf.  Without this, SELECT would have to traverse the entire tree
 * for every row, which is far slower.
 *
 * Sentinel: next_leaf = 0 means "no right sibling" (page 0 is always
 * the root, so it can never be a sibling of another leaf).
 */
#define LEAF_NODE_NUM_CELLS_SIZE   sizeof(uint32_t)
#define LEAF_NODE_NUM_CELLS_OFFSET COMMON_NODE_HEADER_SIZE
#define LEAF_NODE_NEXT_LEAF_SIZE   sizeof(uint32_t)
#define LEAF_NODE_NEXT_LEAF_OFFSET (LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE)
#define LEAF_NODE_PREV_LEAF_SIZE   sizeof(uint32_t)
#define LEAF_NODE_PREV_LEAF_OFFSET (LEAF_NODE_NEXT_LEAF_OFFSET + LEAF_NODE_NEXT_LEAF_SIZE)
#define LEAF_NODE_HEADER_SIZE      (COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE \
                                    + LEAF_NODE_NEXT_LEAF_SIZE + LEAF_NODE_PREV_LEAF_SIZE)

/* Leaf cell: [ key (4) | row_data (ROW_SIZE) ] */
#define LEAF_NODE_KEY_SIZE         sizeof(uint32_t)
#define LEAF_NODE_KEY_OFFSET       0
#define LEAF_NODE_VALUE_SIZE       ROW_SIZE
#define LEAF_NODE_VALUE_OFFSET     (LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE)
#define LEAF_NODE_CELL_SIZE        (LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE)
#define LEAF_NODE_SPACE_FOR_CELLS  (PAGE_USABLE_SIZE - LEAF_NODE_HEADER_SIZE)
#define LEAF_NODE_MAX_CELLS        (LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE)

/* Split counts: distribute cells evenly, right gets the larger half */
#define LEAF_NODE_RIGHT_SPLIT_COUNT ((LEAF_NODE_MAX_CELLS + 1) / 2)
#define LEAF_NODE_LEFT_SPLIT_COUNT  ((LEAF_NODE_MAX_CELLS + 1) - LEAF_NODE_RIGHT_SPLIT_COUNT)

/* ─── Internal node layout ───────────────────────────────────── */
/*
 * INTERNALS: INTERNAL NODE STRUCTURE
 * ───────────────────────────────────
 * An internal node stores n keys and n+1 child pointers.
 * Layout:  [ child_0 | key_0 | child_1 | key_1 | ... | child_n ]
 * The final child (child_n) is stored in the header as right_child_ptr.
 *
 * To find the leaf for key K: walk down, at each internal node find the
 * first key_i >= K and follow child_i (or right_child if K > all keys).
 */
#define INTERNAL_NODE_NUM_KEYS_SIZE      sizeof(uint32_t)
#define INTERNAL_NODE_NUM_KEYS_OFFSET    COMMON_NODE_HEADER_SIZE
#define INTERNAL_NODE_RIGHT_CHILD_SIZE   sizeof(uint32_t)
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET (INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE)
#define INTERNAL_NODE_HEADER_SIZE        (COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE)

#define INTERNAL_NODE_KEY_SIZE    sizeof(uint32_t)
#define INTERNAL_NODE_CHILD_SIZE  sizeof(uint32_t)
#define INTERNAL_NODE_CELL_SIZE   (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)

/* Max keys that fit in one internal node page */
#define INTERNAL_NODE_MAX_KEYS \
    ((PAGE_USABLE_SIZE - INTERNAL_NODE_HEADER_SIZE) / INTERNAL_NODE_CELL_SIZE)
#define INTERNAL_NODE_RIGHT_SPLIT_COUNT ((INTERNAL_NODE_MAX_KEYS + 1) / 2)
#define INTERNAL_NODE_LEFT_SPLIT_COUNT  ((INTERNAL_NODE_MAX_KEYS + 1) - INTERNAL_NODE_RIGHT_SPLIT_COUNT)

/* ─── Table & Cursor ─────────────────────────────────────────── */

typedef struct {
    uint32_t root_page_num;
    Pager*   pager;
    bool     in_transaction; /* true between BEGIN and COMMIT/ROLLBACK */
    bool     username_index_enabled;
    bool     username_index_dirty;
    UsernameIndexEntry* username_index_entries;
    uint32_t username_index_count;
    uint32_t username_index_capacity;
    char     index_catalog_filename[512];
    char     index_catalog_wal_filename[512];
    char     username_index_filename[512];
    char     username_index_wal_filename[512];
} Table;

typedef struct {
    Table*   table;
    uint32_t page_num;
    uint32_t cell_num;
    bool     end_of_table;
} Cursor;

/* ─── Public API ─────────────────────────────────────────────── */

Table*  db_open(const char* filename);
void    db_close(Table* table);
void    table_truncate(Table* table);
void    table_create_username_index(Table* table);
void    table_drop_username_index(Table* table);
void    table_mark_username_index_dirty(Table* table);
void    table_ensure_username_index(Table* table);
void    table_prepare_username_index_commit(Table* table);
void    table_finalize_username_index_commit(Table* table);

Cursor* table_start(Table* table);
Cursor* table_end(Table* table);
Cursor* table_find(Table* table, uint32_t key);
void*   cursor_value(Cursor* cursor);
void    cursor_advance(Cursor* cursor);
void    cursor_retreat(Cursor* cursor);

/* Leaf node accessors */
uint32_t* leaf_node_num_cells(void* node);
uint32_t* leaf_node_next_leaf(void* node);
uint32_t* leaf_node_prev_leaf(void* node);
uint32_t* leaf_node_key(void* node, uint32_t cell_num);
void      leaf_node_insert(Cursor* cursor, uint32_t key, Row* value);
void      leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value);
void      leaf_node_delete(Cursor* cursor);

/* Internal node accessors */
uint32_t* internal_node_num_keys(void* node);
uint32_t* internal_node_right_child(void* node);
uint32_t* internal_node_cell(void* node, uint32_t cell_num);
uint32_t* internal_node_child(void* node, uint32_t child_num);
uint32_t* internal_node_key(void* node, uint32_t key_num);
uint32_t* internal_node_find_child(void* node, uint32_t key);

/* Node type helpers */
NodeType get_node_type(void* node);
void     set_node_type(void* node, NodeType type);
bool     is_node_root(void* node);
void     set_node_root(void* node, bool is_root);
uint32_t get_node_max_key(void* node);
uint32_t* node_parent(void* node);

/* Tree printing (for .btree meta command) */
void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level);

/* Row serialization */
void serialize_row(Row* source, void* destination);
void deserialize_row(void* source, Row* destination);
void print_row(Row* row);

#endif /* TABLE_H */
