#ifndef ROW_ENVELOPE_H
#define ROW_ENVELOPE_H

#include "record_payload.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TINYDB_ROW_ENVELOPE_MAGIC 0x31575254u /* TRW1 */
#define TINYDB_ROW_ENVELOPE_VERSION 1u
#define TINYDB_ROW_ENVELOPE_HEADER_SIZE 20u
#define TINYDB_ROW_ENVELOPE_MAGIC_OFFSET 0u
#define TINYDB_ROW_ENVELOPE_VERSION_OFFSET 4u
#define TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET 6u
#define TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET 8u
#define TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET 12u

static inline uint16_t tinydb_row_envelope_read_u16_le(
    const unsigned char* bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static inline uint32_t tinydb_row_envelope_read_u32_le(
    const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static inline uint64_t tinydb_row_envelope_read_u64_le(
    const unsigned char* bytes) {
    uint64_t value = 0u;
    for (uint32_t i = 0u; i < 8u; i++) {
        value |= (uint64_t)bytes[i] << (8u * i);
    }
    return value;
}

static inline void tinydb_row_envelope_write_u16_le(unsigned char* bytes,
                                                     uint16_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
}

static inline void tinydb_row_envelope_write_u32_le(unsigned char* bytes,
                                                     uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
}

static inline void tinydb_row_envelope_write_u64_le(unsigned char* bytes,
                                                     uint64_t value) {
    for (uint32_t i = 0u; i < 8u; i++) {
        bytes[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
}

static inline uint64_t tinydb_row_envelope_hash_byte(uint64_t hash,
                                                      unsigned char byte) {
    hash ^= (uint64_t)byte;
    return hash * UINT64_C(1099511628211);
}

static inline uint64_t tinydb_row_envelope_hash_u32(uint64_t hash,
                                                     uint32_t value) {
    for (uint32_t i = 0u; i < 4u; i++) {
        hash = tinydb_row_envelope_hash_byte(
            hash, (unsigned char)((value >> (8u * i)) & 0xffu));
    }
    return hash;
}

static inline uint64_t tinydb_row_envelope_hash_text(uint64_t hash,
                                                      const char* text) {
    if (text == NULL) return tinydb_row_envelope_hash_byte(hash, 0u);
    while (*text != '\0') {
        hash = tinydb_row_envelope_hash_byte(hash, (unsigned char)*text++);
    }
    return tinydb_row_envelope_hash_byte(hash, 0u);
}

static inline uint64_t tinydb_row_envelope_schema_fingerprint(
    const TableSchema* schema) {
    if (schema == NULL) return 0u;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = tinydb_row_envelope_hash_text(hash, schema->name);
    hash = tinydb_row_envelope_hash_u32(hash, schema->num_columns);
    hash = tinydb_row_envelope_hash_u32(hash, schema->row_size);
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        hash = tinydb_row_envelope_hash_text(hash, column->name);
        hash = tinydb_row_envelope_hash_u32(hash, (uint32_t)column->type);
        hash = tinydb_row_envelope_hash_u32(hash, column->offset);
        hash = tinydb_row_envelope_hash_u32(hash, column->size);
    }
    return hash;
}

static inline bool tinydb_row_envelope_encode(
    const TableSchema* schema,
    const TinyDBRecordPayload* payload,
    void* output,
    size_t output_capacity,
    uint32_t* stored_length) {
    if (schema == NULL || payload == NULL || output == NULL ||
        payload->length == 0u || payload->length != schema->row_size) {
        return false;
    }
    uint32_t total = TINYDB_ROW_ENVELOPE_HEADER_SIZE + payload->length;
    if (total > output_capacity || total > UINT16_MAX) return false;

    unsigned char* bytes = (unsigned char*)output;
    memset(bytes, 0, total);
    tinydb_row_envelope_write_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_MAGIC_OFFSET,
        TINYDB_ROW_ENVELOPE_MAGIC);
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_VERSION_OFFSET,
        TINYDB_ROW_ENVELOPE_VERSION);
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET,
        TINYDB_ROW_ENVELOPE_HEADER_SIZE);
    tinydb_row_envelope_write_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET,
        payload->length);
    tinydb_row_envelope_write_u64_le(
        bytes + TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET,
        tinydb_row_envelope_schema_fingerprint(schema));
    memcpy(bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE,
           payload->bytes,
           payload->length);
    if (stored_length != NULL) *stored_length = total;
    return true;
}

static inline bool tinydb_row_envelope_decode(
    const TableSchema* schema,
    const void* stored_value,
    uint32_t stored_length,
    TinyDBRecordPayload* payload) {
    if (schema == NULL || stored_value == NULL || payload == NULL ||
        stored_length < TINYDB_ROW_ENVELOPE_HEADER_SIZE) {
        return false;
    }
    const unsigned char* bytes = (const unsigned char*)stored_value;
    if (tinydb_row_envelope_read_u32_le(
            bytes + TINYDB_ROW_ENVELOPE_MAGIC_OFFSET) !=
            TINYDB_ROW_ENVELOPE_MAGIC ||
        tinydb_row_envelope_read_u16_le(
            bytes + TINYDB_ROW_ENVELOPE_VERSION_OFFSET) !=
            TINYDB_ROW_ENVELOPE_VERSION ||
        tinydb_row_envelope_read_u16_le(
            bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET) !=
            TINYDB_ROW_ENVELOPE_HEADER_SIZE) {
        return false;
    }

    uint32_t logical_length = tinydb_row_envelope_read_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET);
    if (logical_length == 0u || logical_length != schema->row_size ||
        logical_length > ROW_SIZE ||
        stored_length != TINYDB_ROW_ENVELOPE_HEADER_SIZE + logical_length ||
        tinydb_row_envelope_read_u64_le(
            bytes + TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET) !=
            tinydb_row_envelope_schema_fingerprint(schema)) {
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = logical_length;
    memcpy(payload->bytes,
           bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE,
           logical_length);
    return true;
}

#endif /* ROW_ENVELOPE_H */
