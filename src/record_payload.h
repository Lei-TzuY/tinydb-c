#ifndef RECORD_PAYLOAD_H
#define RECORD_PAYLOAD_H

#include "record.h"

#include <stddef.h>

/* A logical generic row is no longer sized by the historical users Row slot.
 * The largest schema currently expressible by TinyDBValue is sixteen VARCHAR
 * fields (including their terminating NUL); INT fields consume less space.
 * Physical leaf formats remain free to impose tighter per-value limits. */
#define TINYDB_RECORD_PAYLOAD_MAX \
    (MAX_COLUMNS_PER_TABLE * (TINYDB_RECORD_TEXT_MAX + 1u))

typedef struct {
    uint32_t length;
    unsigned char bytes[TINYDB_RECORD_PAYLOAD_MAX];
} TinyDBRecordPayload;

/* Schema-aware logical codecs. These operate directly on the sized payload
 * carrier so generic rows can be wider than the legacy ROW_SIZE value slot.
 * They do not imply that every physical leaf format can store every payload. */
bool tinydb_record_payload_schema_supported(const TableSchema* schema,
                                            char* message,
                                            size_t message_size);

bool tinydb_record_payload_encode_values(const TableSchema* schema,
                                         const TinyDBValue* values,
                                         uint32_t value_count,
                                         TinyDBRecordPayload* payload,
                                         char* message,
                                         size_t message_size);

bool tinydb_record_payload_decode_values(const TableSchema* schema,
                                         const TinyDBRecordPayload* payload,
                                         TinyDBValue* values,
                                         uint32_t value_capacity,
                                         uint32_t* value_count,
                                         char* message,
                                         size_t message_size);

/* Compatibility adapters for callers that still use the historical fixed
 * TinyDBRecord carrier. They intentionally reject rows wider than ROW_SIZE. */
bool tinydb_record_payload_from_record(const TableSchema* schema,
                                       const TinyDBRecord* record,
                                       TinyDBRecordPayload* payload,
                                       char* message,
                                       size_t message_size);

bool tinydb_record_payload_to_record(const TableSchema* schema,
                                     const TinyDBRecordPayload* payload,
                                     TinyDBRecord* record,
                                     char* message,
                                     size_t message_size);

/* Legacy fixed leaf slots remain exactly ROW_SIZE bytes. These helpers make
 * padding an explicit physical-format concern instead of part of logical row
 * encoding. */
bool tinydb_record_payload_pack_fixed_slot(const TinyDBRecordPayload* payload,
                                           void* slot,
                                           size_t slot_size,
                                           char* message,
                                           size_t message_size);

bool tinydb_record_payload_unpack_fixed_slot(const TableSchema* schema,
                                             const void* slot,
                                             size_t slot_size,
                                             TinyDBRecordPayload* payload,
                                             char* message,
                                             size_t message_size);

#endif /* RECORD_PAYLOAD_H */
