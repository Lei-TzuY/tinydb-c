#include "multitable.h"

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
#define SCHEMA_CATALOG_V2_MAX_PAYLOAD 32768u
#define SCHEMA_CATALOG_FNV64_OFFSET 1469598103934665603ULL
#define SCHEMA_CATALOG_FNV64_PRIME 1099511628211ULL

typedef struct {
    uint32_t num_tables;
    uint32_t num_views;
    TableSchema schemas[MAX_TABLES];
    ViewSchema views[MAX_VIEWS];
} SchemaCatalogSnapshot;

typedef struct {
    unsigned char* data;
    size_t capacity;
    size_t position;
    bool ok;
} ByteWriter;

typedef struct {
    const unsigned char* data;
    size_t size;
    size_t position;
    bool ok;
} ByteReader;

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

static void writer_bytes(ByteWriter* writer, const void* data, size_t size) {
    if (!writer->ok || size > writer->capacity - writer->position) {
        writer->ok = false;
        return;
    }
    memcpy(writer->data + writer->position, data, size);
    writer->position += size;
}

static void writer_u8(ByteWriter* writer, uint8_t value) {
    writer_bytes(writer, &value, sizeof(value));
}

static void writer_u32(ByteWriter* writer, uint32_t value) {
    unsigned char encoded[4];
    encode_u32(value, encoded);
    writer_bytes(writer, encoded, sizeof(encoded));
}

static void reader_bytes(ByteReader* reader, void* output, size_t size) {
    if (!reader->ok || size > reader->size - reader->position) {
        reader->ok = false;
        return;
    }
    memcpy(output, reader->data + reader->position, size);
    reader->position += size;
}

static uint8_t reader_u8(ByteReader* reader) {
    uint8_t value = 0;
    reader_bytes(reader, &value, sizeof(value));
    return value;
}

static uint32_t reader_u32(ByteReader* reader) {
    unsigned char encoded[4] = {0};
    reader_bytes(reader, encoded, sizeof(encoded));
    return reader->ok ? decode_u32(encoded) : 0;
}

static bool fixed_string_valid(const char* value, size_t capacity) {
    return memchr(value, '\0', capacity) != NULL;
}

static bool schema_shape_valid(const SchemaCatalogSnapshot* snapshot) {
    if (snapshot == NULL || snapshot->num_tables == 0 ||
        snapshot->num_tables > MAX_TABLES || snapshot->num_views > MAX_VIEWS) {
        return false;
    }

    for (uint32_t i = 0; i < snapshot->num_tables; i++) {
        const TableSchema* schema = &snapshot->schemas[i];
        if (!fixed_string_valid(schema->name, sizeof(schema->name)) ||
            schema->num_columns == 0 || schema->num_columns > MAX_COLUMNS_PER_TABLE) {
            return false;
        }
        for (uint32_t j = 0; j < schema->num_columns; j++) {
            const TableColumn* column = &schema->columns[j];
            if (!fixed_string_valid(column->name, sizeof(column->name)) ||
                (column->type != COL_TYPE_INT && column->type != COL_TYPE_VARCHAR)) {
                return false;
            }
            if (column->offset > schema->row_size ||
                column->size > schema->row_size - column->offset) {
                return false;
            }
        }
        if (!fixed_string_valid(schema->fk_col, sizeof(schema->fk_col)) ||
            !fixed_string_valid(schema->fk_parent_table,
                                sizeof(schema->fk_parent_table)) ||
            !fixed_string_valid(schema->fk_parent_col,
                                sizeof(schema->fk_parent_col))) {
            return false;
        }
    }

    for (uint32_t i = 0; i < snapshot->num_views; i++) {
        const ViewSchema* view = &snapshot->views[i];
        if (!fixed_string_valid(view->name, sizeof(view->name)) ||
            !fixed_string_valid(view->select_sql, sizeof(view->select_sql))) {
            return false;
        }
    }
    return true;
}

static bool schema_roots_valid(const Table* table,
                               const SchemaCatalogSnapshot* snapshot) {
    if (table == NULL || table->pager == NULL) return false;
    for (uint32_t i = 0; i < snapshot->num_tables; i++) {
        if (snapshot->schemas[i].root_page_num >= table->pager->num_pages) {
            printf("Ignoring schema catalog with invalid root page %u for table '%s'.\n",
                   snapshot->schemas[i].root_page_num,
                   snapshot->schemas[i].name);
            return false;
        }
    }
    return true;
}

static void fill_snapshot(const Table* table, SchemaCatalogSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->num_tables = table->catalog.num_tables;
    snapshot->num_views = table->catalog.num_views;
    memcpy(snapshot->schemas,
           table->catalog.schemas,
           sizeof(TableSchema) * table->catalog.num_tables);
    memcpy(snapshot->views,
           table->catalog.views,
           sizeof(ViewSchema) * table->catalog.num_views);
}

static void install_snapshot(Table* table, const SchemaCatalogSnapshot* snapshot) {
    table->catalog.num_tables = snapshot->num_tables;
    memset(table->catalog.schemas, 0, sizeof(table->catalog.schemas));
    memcpy(table->catalog.schemas,
           snapshot->schemas,
           sizeof(TableSchema) * snapshot->num_tables);
    table->catalog.num_views = snapshot->num_views;
    memset(table->catalog.views, 0, sizeof(table->catalog.views));
    memcpy(table->catalog.views,
           snapshot->views,
           sizeof(ViewSchema) * snapshot->num_views);
}

static bool encode_snapshot(const SchemaCatalogSnapshot* snapshot,
                            unsigned char* payload,
                            size_t payload_capacity,
                            size_t* payload_size) {
    if (!schema_shape_valid(snapshot)) return false;

    ByteWriter writer;
    writer.data = payload;
    writer.capacity = payload_capacity;
    writer.position = 0;
    writer.ok = true;

    writer_u32(&writer, snapshot->num_tables);
    writer_u32(&writer, snapshot->num_views);
    for (uint32_t i = 0; i < snapshot->num_tables; i++) {
        const TableSchema* schema = &snapshot->schemas[i];
        writer_bytes(&writer, schema->name, sizeof(schema->name));
        writer_u32(&writer, schema->root_page_num);
        writer_u32(&writer, schema->num_columns);
        for (uint32_t j = 0; j < schema->num_columns; j++) {
            const TableColumn* column = &schema->columns[j];
            writer_bytes(&writer, column->name, sizeof(column->name));
            writer_u32(&writer, (uint32_t)column->type);
            writer_u32(&writer, column->size);
            writer_u32(&writer, column->offset);
        }
        writer_u32(&writer, schema->row_size);
        writer_u8(&writer, schema->has_fk ? 1u : 0u);
        writer_bytes(&writer, schema->fk_col, sizeof(schema->fk_col));
        writer_bytes(&writer,
                     schema->fk_parent_table,
                     sizeof(schema->fk_parent_table));
        writer_bytes(&writer,
                     schema->fk_parent_col,
                     sizeof(schema->fk_parent_col));
        writer_u8(&writer, schema->fk_on_delete_cascade ? 1u : 0u);
    }

    for (uint32_t i = 0; i < snapshot->num_views; i++) {
        writer_bytes(&writer,
                     snapshot->views[i].name,
                     sizeof(snapshot->views[i].name));
        writer_bytes(&writer,
                     snapshot->views[i].select_sql,
                     sizeof(snapshot->views[i].select_sql));
    }

    if (!writer.ok) return false;
    *payload_size = writer.position;
    return true;
}

static bool decode_snapshot(const unsigned char* payload,
                            size_t payload_size,
                            SchemaCatalogSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    ByteReader reader;
    reader.data = payload;
    reader.size = payload_size;
    reader.position = 0;
    reader.ok = true;

    snapshot->num_tables = reader_u32(&reader);
    snapshot->num_views = reader_u32(&reader);
    if (!reader.ok || snapshot->num_tables == 0 ||
        snapshot->num_tables > MAX_TABLES || snapshot->num_views > MAX_VIEWS) {
        return false;
    }

    for (uint32_t i = 0; i < snapshot->num_tables; i++) {
        TableSchema* schema = &snapshot->schemas[i];
        reader_bytes(&reader, schema->name, sizeof(schema->name));
        schema->root_page_num = reader_u32(&reader);
        schema->num_columns = reader_u32(&reader);
        if (!reader.ok || schema->num_columns == 0 ||
            schema->num_columns > MAX_COLUMNS_PER_TABLE) {
            return false;
        }
        for (uint32_t j = 0; j < schema->num_columns; j++) {
            TableColumn* column = &schema->columns[j];
            reader_bytes(&reader, column->name, sizeof(column->name));
            column->type = (ColumnType)reader_u32(&reader);
            column->size = reader_u32(&reader);
            column->offset = reader_u32(&reader);
        }
        schema->row_size = reader_u32(&reader);
        uint8_t has_fk = reader_u8(&reader);
        reader_bytes(&reader, schema->fk_col, sizeof(schema->fk_col));
        reader_bytes(&reader,
                     schema->fk_parent_table,
                     sizeof(schema->fk_parent_table));
        reader_bytes(&reader,
                     schema->fk_parent_col,
                     sizeof(schema->fk_parent_col));
        uint8_t cascade = reader_u8(&reader);
        if (!reader.ok || has_fk > 1u || cascade > 1u) return false;
        schema->has_fk = has_fk != 0;
        schema->fk_on_delete_cascade = cascade != 0;
    }

    for (uint32_t i = 0; i < snapshot->num_views; i++) {
        reader_bytes(&reader,
                     snapshot->views[i].name,
                     sizeof(snapshot->views[i].name));
        reader_bytes(&reader,
                     snapshot->views[i].select_sql,
                     sizeof(snapshot->views[i].select_sql));
    }

    return reader.ok && reader.position == reader.size &&
           schema_shape_valid(snapshot);
}

static bool write_v2_stream(FILE* file,
                            const SchemaCatalogSnapshot* snapshot,
                            bool include_commit_marker) {
    unsigned char payload[SCHEMA_CATALOG_V2_MAX_PAYLOAD];
    size_t payload_size = 0;
    if (!encode_snapshot(snapshot,
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
                                         SchemaCatalogSnapshot* snapshot,
                                         bool require_commit_marker) {
    unsigned char prefix[8];
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
        !decode_snapshot(payload, payload_size, snapshot)) {
        return SCHEMA_V2_INVALID;
    }

    if (require_commit_marker) {
        unsigned char commit[4];
        if (fread(commit, 1, sizeof(commit), file) != sizeof(commit) ||
            decode_u32(commit) != SCHEMA_CATALOG_WAL_COMMIT_MAGIC) {
            return SCHEMA_V2_INVALID;
        }
    }

    return fgetc(file) == EOF ? SCHEMA_V2_VALID : SCHEMA_V2_INVALID;
}

static bool write_v2_file(const char* path,
                          const SchemaCatalogSnapshot* snapshot,
                          bool include_commit_marker) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = write_v2_stream(file, snapshot, include_commit_marker);
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

    SchemaCatalogSnapshot snapshot;
    SchemaV2ReadStatus status = read_v2_stream(wal, &snapshot, true);
    fclose(wal);
    if (status == SCHEMA_V2_NOT_V2) return true;

    *handled = true;
    if (status == SCHEMA_V2_INVALID) {
        (void)remove(wal_path);
        printf("Ignoring invalid checksummed schema catalog WAL.\n");
        return true;
    }

    if (!write_v2_file(main_path, &snapshot, false)) {
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

    SchemaCatalogSnapshot snapshot;
    SchemaV2ReadStatus status = read_v2_stream(file, &snapshot, false);
    fclose(file);
    if (status == SCHEMA_V2_NOT_V2) {
        return multitable_catalog_load_v1_base(table, database_filename);
    }
    if (status != SCHEMA_V2_VALID) {
        printf("Ignoring invalid checksummed schema catalog.\n");
        return false;
    }
    if (!schema_roots_valid(table, &snapshot)) return false;

    install_snapshot(table, &snapshot);
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

    SchemaCatalogSnapshot snapshot;
    fill_snapshot(table, &snapshot);
    if (!schema_shape_valid(&snapshot)) {
        printf("Refusing to persist invalid schema catalog state.\n");
        return false;
    }

    if (!write_v2_file(wal_path, &snapshot, true)) {
        printf("Unable to write checksummed schema catalog WAL.\n");
        return false;
    }
    if (!write_v2_file(main_path, &snapshot, false)) {
        printf("Unable to write checksummed schema catalog.\n");
        return false;
    }
    if (remove(wal_path) != 0 && errno != ENOENT) {
        printf("Unable to remove committed checksummed schema catalog WAL.\n");
        return false;
    }
    return true;
}
