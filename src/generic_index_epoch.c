#include "generic_index_epoch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define GENERIC_INDEX_EPOCH_MAGIC 0x47494550u
#define GENERIC_INDEX_EPOCH_VERSION 1u
#define FNV64_OFFSET 1469598103934665603ULL
#define FNV64_PRIME 1099511628211ULL

static uint64_t fnv64_update(uint64_t hash, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= FNV64_PRIME;
    }
    return hash;
}

static void encode_u32(uint32_t value, unsigned char out[4]) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void encode_u64(uint64_t value, unsigned char out[8]) {
    for (uint32_t i = 0; i < 8; i++) {
        out[i] = (unsigned char)((value >> (i * 8u)) & 0xffu);
    }
}

static uint32_t decode_u32(const unsigned char in[4]) {
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static uint64_t decode_u64(const unsigned char in[8]) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++) {
        value |= ((uint64_t)in[i]) << (i * 8u);
    }
    return value;
}

static bool write_bytes(FILE* file, const void* data, size_t size) {
    return fwrite(data, 1, size, file) == size;
}

static bool read_bytes(FILE* file, void* data, size_t size) {
    return fread(data, 1, size, file) == size;
}

static bool sync_file(FILE* file) {
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool epoch_filename(const Table* table, char* output, size_t output_size) {
    if (table == NULL || table->pager == NULL) return false;
    int written = snprintf(output,
                           output_size,
                           "%s.gidx.epoch",
                           table->pager->filename);
    return written >= 0 && (size_t)written < output_size;
}

static uint64_t epoch_checksum(uint64_t epoch) {
    unsigned char u32[4];
    unsigned char u64[8];
    uint64_t hash = FNV64_OFFSET;

    encode_u32(GENERIC_INDEX_EPOCH_MAGIC, u32);
    hash = fnv64_update(hash, u32, sizeof(u32));
    encode_u32(GENERIC_INDEX_EPOCH_VERSION, u32);
    hash = fnv64_update(hash, u32, sizeof(u32));
    encode_u64(epoch, u64);
    hash = fnv64_update(hash, u64, sizeof(u64));
    return hash;
}

static bool write_epoch_file(const char* filename, uint64_t epoch) {
    FILE* file = fopen(filename, "wb");
    if (file == NULL) return false;

    unsigned char u32[4];
    unsigned char u64[8];
    bool ok = true;

    encode_u32(GENERIC_INDEX_EPOCH_MAGIC, u32);
    ok = ok && write_bytes(file, u32, sizeof(u32));
    encode_u32(GENERIC_INDEX_EPOCH_VERSION, u32);
    ok = ok && write_bytes(file, u32, sizeof(u32));
    encode_u64(epoch, u64);
    ok = ok && write_bytes(file, u64, sizeof(u64));
    encode_u64(epoch_checksum(epoch), u64);
    ok = ok && write_bytes(file, u64, sizeof(u64));

    if (ok) ok = sync_file(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool read_epoch_file(const char* filename, uint64_t* epoch) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;

    unsigned char u32[4];
    unsigned char u64[8];
    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t value = 0;
    uint64_t checksum = 0;

    bool ok = read_bytes(file, u32, sizeof(u32));
    if (ok) magic = decode_u32(u32);
    ok = ok && read_bytes(file, u32, sizeof(u32));
    if (ok) version = decode_u32(u32);
    ok = ok && read_bytes(file, u64, sizeof(u64));
    if (ok) value = decode_u64(u64);
    ok = ok && read_bytes(file, u64, sizeof(u64));
    if (ok) checksum = decode_u64(u64);
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    fclose(file);

    if (!ok || magic != GENERIC_INDEX_EPOCH_MAGIC ||
        version != GENERIC_INDEX_EPOCH_VERSION ||
        checksum != epoch_checksum(value)) {
        return false;
    }
    *epoch = value;
    return true;
}

static uint64_t fresh_epoch_seed(void) {
    uint64_t now = (uint64_t)time(NULL);
    uint64_t seed = (now << 32) ^ 0x9e3779b97f4a7c15ULL;
    if (seed == 0 || seed == UINT64_MAX) seed = 1;
    return seed;
}

static bool has_generic_index_for_schema(Table* table,
                                         const TableSchema* schema) {
    if (table == NULL || schema == NULL) return false;
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1 &&
            strcmp(index->table_name, schema->name) == 0 &&
            strcmp(index->column_name, "id") != 0) {
            return true;
        }
    }
    return false;
}

bool tinydb_generic_index_epoch_current(Table* table, uint64_t* epoch) {
    if (table == NULL || epoch == NULL) return false;

    char filename[600];
    if (!epoch_filename(table, filename, sizeof(filename))) return false;

    uint64_t value = 0;
    if (read_epoch_file(filename, &value)) {
        *epoch = value;
        return true;
    }

    value = fresh_epoch_seed();
    if (!write_epoch_file(filename, value)) return false;
    *epoch = value;
    return true;
}

bool tinydb_generic_index_epoch_before_mutation(Table* table,
                                                const TableSchema* schema) {
    if (!has_generic_index_for_schema(table, schema)) return true;

    char filename[600];
    if (!epoch_filename(table, filename, sizeof(filename))) return false;

    uint64_t epoch = 0;
    if (!read_epoch_file(filename, &epoch)) {
        epoch = fresh_epoch_seed();
    }
    if (epoch == UINT64_MAX) {
        epoch = fresh_epoch_seed();
    } else {
        epoch++;
    }
    return write_epoch_file(filename, epoch);
}
