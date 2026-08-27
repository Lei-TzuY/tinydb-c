#include "multitable.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCHEMA_CATALOG_MAGIC 0x4d435354u /* TSCM */
#define SCHEMA_CATALOG_V2_VERSION 2u
#define SCHEMA_CATALOG_WAL_COMMIT_MAGIC 0x57435354u /* TSCW */
#define SCHEMA_CATALOG_V2_HEADER_SIZE 20u
#define SCHEMA_CATALOG_V2_MAX_PAYLOAD 32768u
#define SCHEMA_CATALOG_FNV64_OFFSET 1469598103934665603ULL
#define SCHEMA_CATALOG_FNV64_PRIME 1099511628211ULL
#define SCHEMA_CATALOG_V2_COLUMN_SIZE (MAX_NAME_SIZE + 12u)
#define SCHEMA_CATALOG_V2_TABLE_TRAILER_SIZE (4u + 1u + (3u * MAX_NAME_SIZE) + 1u)

bool multitable_catalog_load_checksums_base(Table* table,
                                            const char* database_filename);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (ci_char(*left) != ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static uint32_t decode_u32(const unsigned char input[4]) {
    return ((uint32_t)input[0]) |
           ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static uint64_t decode_u64(const unsigned char input[8]) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++) {
        value |= ((uint64_t)input[i]) << (8u * i);
    }
    return value;
}

static uint64_t fnv64(const unsigned char* data, size_t size) {
    uint64_t hash = SCHEMA_CATALOG_FNV64_OFFSET;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)data[i];
        hash *= SCHEMA_CATALOG_FNV64_PRIME;
    }
    return hash;
}

static bool build_catalog_path(char* output,
                               size_t output_size,
                               const char* database_filename,
                               const char* suffix) {
    int written = snprintf(output, output_size, "%s%s", database_filename, suffix);
    return written >= 0 && (size_t)written < output_size;
}

static bool payload_has_impossible_root(const Table* table,
                                        const unsigned char* payload,
                                        size_t payload_size,
                                        uint32_t* bad_root) {
    if (table == NULL || table->pager == NULL || payload == NULL ||
        payload_size < 8u) {
        return false;
    }

    uint32_t num_tables = decode_u32(payload);
    uint32_t num_views = decode_u32(payload + 4u);
    if (num_tables == 0 || num_tables > MAX_TABLES || num_views > MAX_VIEWS) {
        return false;
    }

    size_t position = 8u;
    for (uint32_t i = 0; i < num_tables; i++) {
        size_t table_prefix = MAX_NAME_SIZE + 8u;
        if (position > payload_size || table_prefix > payload_size - position) {
            return false;
        }

        position += MAX_NAME_SIZE;
        uint32_t root_page_num = decode_u32(payload + position);
        position += 4u;
        uint32_t num_columns = decode_u32(payload + position);
        position += 4u;

        if (num_columns == 0 || num_columns > MAX_COLUMNS_PER_TABLE) {
            return false;
        }

        size_t column_bytes = (size_t)num_columns * SCHEMA_CATALOG_V2_COLUMN_SIZE;
        size_t remaining = column_bytes + SCHEMA_CATALOG_V2_TABLE_TRAILER_SIZE;
        if (position > payload_size || remaining > payload_size - position) {
            return false;
        }

        if (root_page_num >= table->pager->num_pages) {
            if (bad_root != NULL) *bad_root = root_page_num;
            return true;
        }

        position += remaining;
    }

    return false;
}

static bool discard_committed_v2_wal_with_impossible_root(
    const Table* table,
    const char* database_filename
) {
    char wal_path[768];
    if (table == NULL || database_filename == NULL ||
        !build_catalog_path(wal_path,
                            sizeof(wal_path),
                            database_filename,
                            ".schema.wal")) {
        return true;
    }

    FILE* wal = fopen(wal_path, "rb");
    if (wal == NULL) return true;

    unsigned char header[SCHEMA_CATALOG_V2_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), wal) != sizeof(header)) {
        fclose(wal);
        return true;
    }

    if (decode_u32(header) != SCHEMA_CATALOG_MAGIC ||
        decode_u32(header + 4u) != SCHEMA_CATALOG_V2_VERSION) {
        fclose(wal);
        return true;
    }

    uint32_t payload_size = decode_u32(header + 8u);
    uint64_t expected_checksum = decode_u64(header + 12u);
    if (payload_size == 0 || payload_size > SCHEMA_CATALOG_V2_MAX_PAYLOAD) {
        fclose(wal);
        return true;
    }

    unsigned char payload[SCHEMA_CATALOG_V2_MAX_PAYLOAD];
    unsigned char commit[4];
    bool complete = fread(payload, 1, payload_size, wal) == payload_size &&
                    fread(commit, 1, sizeof(commit), wal) == sizeof(commit) &&
                    fgetc(wal) == EOF;
    fclose(wal);

    if (!complete ||
        fnv64(payload, payload_size) != expected_checksum ||
        decode_u32(commit) != SCHEMA_CATALOG_WAL_COMMIT_MAGIC) {
        return true;
    }

    uint32_t bad_root = 0;
    if (!payload_has_impossible_root(table, payload, payload_size, &bad_root)) {
        return true;
    }

    if (remove(wal_path) != 0 && errno != ENOENT) {
        printf("Unable to discard schema catalog WAL with invalid root page %u.\n",
               bad_root);
        return false;
    }

    printf("Ignoring schema catalog WAL with invalid root page %u; preserving main catalog.\n",
           bad_root);
    return true;
}

static bool name_present(const char* name, size_t capacity) {
    if (name == NULL || capacity == 0 || name[0] == '\0') return false;
    return memchr(name, '\0', capacity) != NULL;
}

static bool catalog_identity_valid(const Table* table) {
    if (table == NULL || table->catalog.num_tables == 0 ||
        table->catalog.num_tables > MAX_TABLES ||
        table->catalog.num_views > MAX_VIEWS) {
        printf("Ignoring schema catalog with invalid object counts.\n");
        return false;
    }

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        const TableSchema* current = &table->catalog.schemas[i];
        if (!name_present(current->name, sizeof(current->name))) {
            printf("Ignoring schema catalog with an empty or unterminated table name.\n");
            return false;
        }

        for (uint32_t j = 0; j < i; j++) {
            const TableSchema* previous = &table->catalog.schemas[j];
            if (ci_equal(previous->name, current->name)) {
                printf("Ignoring schema catalog with duplicate table name '%s'.\n",
                       current->name);
                return false;
            }
            if (previous->root_page_num == current->root_page_num) {
                printf("Ignoring schema catalog with duplicate root page %u for tables '%s' and '%s'.\n",
                       current->root_page_num,
                       previous->name,
                       current->name);
                return false;
            }
        }
    }

    for (uint32_t i = 0; i < table->catalog.num_views; i++) {
        const ViewSchema* current = &table->catalog.views[i];
        if (!name_present(current->name, sizeof(current->name))) {
            printf("Ignoring schema catalog with an empty or unterminated view name.\n");
            return false;
        }

        for (uint32_t j = 0; j < i; j++) {
            if (ci_equal(table->catalog.views[j].name, current->name)) {
                printf("Ignoring schema catalog with duplicate view name '%s'.\n",
                       current->name);
                return false;
            }
        }
        for (uint32_t j = 0; j < table->catalog.num_tables; j++) {
            if (ci_equal(table->catalog.schemas[j].name, current->name)) {
                printf("Ignoring schema catalog because table and view share name '%s'.\n",
                       current->name);
                return false;
            }
        }
    }

    return true;
}

bool multitable_catalog_load(Table* table, const char* database_filename) {
    if (!discard_committed_v2_wal_with_impossible_root(table,
                                                       database_filename)) {
        return false;
    }
    if (!multitable_catalog_load_checksums_base(table, database_filename)) {
        return false;
    }
    return catalog_identity_valid(table);
}
