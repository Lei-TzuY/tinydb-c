#include "multitable.h"
#include "schema_catalog_shape_codec.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define SCHEMA_CATALOG_MAGIC 0x4d435354u /* TSCM */
#define SCHEMA_CATALOG_V2_VERSION 2u
#define SCHEMA_CATALOG_WAL_COMMIT_MAGIC 0x57435354u /* TSCW */
#define SCHEMA_CATALOG_V2_HEADER_SIZE 20u
#define SCHEMA_CATALOG_V2_MAX_PAYLOAD TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE
#define SCHEMA_CATALOG_FNV64_OFFSET 1469598103934665603ULL
#define SCHEMA_CATALOG_FNV64_PRIME 1099511628211ULL

typedef enum {
    SCHEMA_V2_NOT_V2 = 0,
    SCHEMA_V2_VALID,
    SCHEMA_V2_INVALID
} SchemaV2ReadStatus;

/* Legacy V1 implementation retained inside multitable.c via CMake symbol rename. */
bool multitable_catalog_load_v1_base(Table* table, const char* database_filename);

static bool sync_catalog_file(FILE* file) {
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool build_catalog_path(char* output,
                               size_t output_size,
                               const char* database_filename,
                               const char* suffix) {
    int written = snprintf(output, output_size, "%s%s", database_filename, suffix);
    return written >= 0 && (size_t)written < output_size;
}

static void encode_u32(uint32_t value, unsigned char output[4]) {
    output[0] = (unsigned char)(value & 0xffu);
    output[1] = (unsigned char)((value >> 8) & 0xffu);
    output[2] = (unsigned char)((value >> 16) & 0xffu);
    output[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void encode_u64(uint64_t value, unsigned char output[8]) {
    for (uint32_t i = 0; i < 8; i++) {
        output[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
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

static bool schema_roots_valid(const Table* table, const Catalog* catalog) {
    if (table == NULL || table->pager == NULL || catalog == NULL) return false;
    for (uint32_t i = 0; i < catalog->num_tables; i++) {
        if (catalog->schemas[i].root_page_num >= table->pager->num_pages) {
            printf("Ignoring schema catalog with invalid root page %u for table '%s'.\n",
                   catalog->schemas[i].root_page_num,
                   catalog->schemas[i].name);
            return false;
        }
    }
    return true;
}

static bool write_v2_stream(FILE* file,
                            const Catalog* catalog,
                            bool include_commit_marker) {
    unsigned char payload[SCHEMA_CATALOG_V2_MAX_PAYLOAD];
    size_t payload_size = 0;
    if (!tinydb_schema_catalog_shape_encode(catalog,
                                            payload,
                                            sizeof(payload),
                                            &payload_size) ||
        payload_size > UINT32_MAX) {
        return false;
    }

    unsigned char header[SCHEMA_CATALOG_V2_HEADER_SIZE];
    encode_u32(SCHEMA_CATALOG_MAGIC, header);
    encode_u32(SCHEMA_CATALOG_V2_VERSION, header + 4);
    encode_u32((uint32_t)payload_size, header + 8);
    encode_u64(fnv64(payload, payload_size), header + 12);

    if (fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
        fwrite(payload, 1, payload_size, file) != payload_size) {
        return false;
    }

    if (include_commit_marker) {
        unsigned char commit[4];
        encode_u32(SCHEMA_CATALOG_WAL_COMMIT_MAGIC, commit);
        if (fwrite(commit, 1, sizeof(commit), file) != sizeof(commit)) return false;
    }
    return true;
}

static SchemaV2ReadStatus read_v2_stream(FILE* file,
                                         Catalog* catalog,
                                         bool require_commit_marker) {
    unsigned char prefix[8];
    if (catalog == NULL) return SCHEMA_V2_INVALID;
    memset(catalog, 0, sizeof(*catalog));
    if (fread(prefix, 1, sizeof(prefix), file) != sizeof(prefix)) {
        return SCHEMA_V2_INVALID;
    }
    uint32_t magic = decode_u32(prefix);
    uint32_t version = decode_u32(prefix + 4);
    if (magic != SCHEMA_CATALOG_MAGIC || version != SCHEMA_CATALOG_V2_VERSION) {
        return SCHEMA_V2_NOT_V2;
    }

    unsigned char rest[12];
    if (fread(rest, 1, sizeof(rest), file) != sizeof(rest)) {
        return SCHEMA_V2_INVALID;
    }
    uint32_t payload_size = decode_u32(rest);
    uint64_t expected_checksum = decode_u64(rest + 4);
    if (payload_size == 0 || payload_size > SCHEMA_CATALOG_V2_MAX_PAYLOAD) {
        return SCHEMA_V2_INVALID;
    }

    unsigned char payload[SCHEMA_CATALOG_V2_MAX_PAYLOAD];
    if (fread(payload, 1, payload_size, file) != payload_size ||
        fnv64(payload, payload_size) != expected_checksum ||
        !tinydb_schema_catalog_shape_decode(payload, payload_size, catalog)) {
        memset(catalog, 0, sizeof(*catalog));
        return SCHEMA_V2_INVALID;
    }

    if (require_commit_marker) {
        unsigned char commit[4];
        if (fread(commit, 1, sizeof(commit), file) != sizeof(commit) ||
            decode_u32(commit) != SCHEMA_CATALOG_WAL_COMMIT_MAGIC) {
            memset(catalog, 0, sizeof(*catalog));
            return SCHEMA_V2_INVALID;
        }
    }

    if (fgetc(file) != EOF) {
        memset(catalog, 0, sizeof(*catalog));
        return SCHEMA_V2_INVALID;
    }
    return SCHEMA_V2_VALID;
}

static bool write_v2_file(const char* path,
                          const Catalog* catalog,
                          bool include_commit_marker) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = write_v2_stream(file, catalog, include_commit_marker);
    if (ok) ok = sync_catalog_file(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool recover_v2_wal(const char* main_path,
                           const char* wal_path,
                           bool* handled) {
    *handled = false;
    FILE* wal = fopen(wal_path, "rb");
    if (wal == NULL) return true;

    Catalog catalog;
    SchemaV2ReadStatus status = read_v2_stream(wal, &catalog, true);
    fclose(wal);
    if (status == SCHEMA_V2_NOT_V2) return true;

    *handled = true;
    if (status == SCHEMA_V2_INVALID) {
        (void)remove(wal_path);
        printf("Ignoring invalid checksummed schema catalog WAL.\n");
        return true;
    }

    if (!write_v2_file(main_path, &catalog, false)) {
        printf("Unable to recover checksummed schema catalog.\n");
        return false;
    }
    if (remove(wal_path) != 0 && errno != ENOENT) {
        printf("Unable to remove recovered checksummed schema catalog WAL.\n");
        return false;
    }
    printf("Schema catalog recovery complete.\n");
    return true;
}

bool multitable_catalog_load(Table* table, const char* database_filename) {
    char main_path[768];
    char wal_path[768];
    if (table == NULL || database_filename == NULL ||
        !build_catalog_path(main_path,
                            sizeof(main_path),
                            database_filename,
                            ".schema") ||
        !build_catalog_path(wal_path,
                            sizeof(wal_path),
                            database_filename,
                            ".schema.wal")) {
        printf("Database filename too long for schema catalog.\n");
        return false;
    }

    bool wal_handled = false;
    if (!recover_v2_wal(main_path, wal_path, &wal_handled)) return false;
    if (!wal_handled) {
        FILE* wal = fopen(wal_path, "rb");
        if (wal != NULL) {
            fclose(wal);
            return multitable_catalog_load_v1_base(table, database_filename);
        }
    }

    FILE* file = fopen(main_path, "rb");
    if (file == NULL) return errno == ENOENT;

    Catalog catalog;
    SchemaV2ReadStatus status = read_v2_stream(file, &catalog, false);
    fclose(file);
    if (status == SCHEMA_V2_NOT_V2) {
        return multitable_catalog_load_v1_base(table, database_filename);
    }
    if (status != SCHEMA_V2_VALID) {
        printf("Ignoring invalid checksummed schema catalog.\n");
        return false;
    }
    if (!schema_roots_valid(table, &catalog)) return false;

    table->catalog = catalog;
    return true;
}

bool multitable_catalog_save(Table* table, const char* database_filename) {
    char main_path[768];
    char wal_path[768];
    if (table == NULL || database_filename == NULL ||
        !build_catalog_path(main_path,
                            sizeof(main_path),
                            database_filename,
                            ".schema") ||
        !build_catalog_path(wal_path,
                            sizeof(wal_path),
                            database_filename,
                            ".schema.wal")) {
        printf("Database filename too long for schema catalog.\n");
        return false;
    }

    if (!tinydb_schema_catalog_shape_valid(&table->catalog)) {
        printf("Refusing to persist invalid schema catalog state.\n");
        return false;
    }

    if (!write_v2_file(wal_path, &table->catalog, true)) {
        printf("Unable to write checksummed schema catalog WAL.\n");
        return false;
    }
    if (!write_v2_file(main_path, &table->catalog, false)) {
        printf("Unable to write checksummed schema catalog.\n");
        return false;
    }
    if (remove(wal_path) != 0 && errno != ENOENT) {
        printf("Unable to remove committed checksummed schema catalog WAL.\n");
        return false;
    }
    return true;
}
