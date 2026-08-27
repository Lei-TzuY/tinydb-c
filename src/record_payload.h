#ifndef RECORD_PAYLOAD_H
#define RECORD_PAYLOAD_H

#include "record.h"

#include <stddef.h>

/* Logical record bytes are schema-sized even though today's B+ tree leaf
 * carrier reserves ROW_SIZE bytes per value. Keeping the logical length
 * explicit is the compatibility seam for a future slotted/variable-size leaf
 * format without changing the schema-aware SQL encoder. */
typedef struct {
    uint32_t length;
    unsigned char bytes[ROW_SIZE];
} TinyDBRecordPayload;

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
