#include "record_payload.h"

#include <stdio.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

static bool validate_schema(const TableSchema* schema,
                            char* message,
                            size_t message_size) {
    if (!tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }
    if (schema->row_size > ROW_SIZE) {
        set_message(message,
                    message_size,
                    "logical record payload exceeds the current maximum payload size");
        return false;
    }
    return true;
}

bool tinydb_record_payload_from_record(const TableSchema* schema,
                                       const TinyDBRecord* record,
                                       TinyDBRecordPayload* payload,
                                       char* message,
                                       size_t message_size) {
    if (!validate_schema(schema, message, message_size)) return false;
    if (record == NULL || payload == NULL) {
        set_message(message, message_size, "record and payload are required");
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    memcpy(payload->bytes, record->bytes, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_to_record(const TableSchema* schema,
                                     const TinyDBRecordPayload* payload,
                                     TinyDBRecord* record,
                                     char* message,
                                     size_t message_size) {
    if (!validate_schema(schema, message, message_size)) return false;
    if (payload == NULL || record == NULL) {
        set_message(message, message_size, "payload and record are required");
        return false;
    }
    if (payload->length != schema->row_size || payload->length > ROW_SIZE) {
        set_message(message,
                    message_size,
                    "payload length does not match the schema row size");
        return false;
    }

    memset(record, 0, sizeof(*record));
    memcpy(record->bytes, payload->bytes, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_pack_fixed_slot(const TinyDBRecordPayload* payload,
                                           void* slot,
                                           size_t slot_size,
                                           char* message,
                                           size_t message_size) {
    if (payload == NULL || slot == NULL) {
        set_message(message, message_size, "payload and fixed slot are required");
        return false;
    }
    if (slot_size != ROW_SIZE) {
        set_message(message,
                    message_size,
                    "legacy fixed leaf slot must be exactly ROW_SIZE bytes");
        return false;
    }
    if (payload->length == 0u || payload->length > ROW_SIZE) {
        set_message(message, message_size, "payload length is outside fixed-slot bounds");
        return false;
    }

    memset(slot, 0, slot_size);
    memcpy(slot, payload->bytes, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_unpack_fixed_slot(const TableSchema* schema,
                                             const void* slot,
                                             size_t slot_size,
                                             TinyDBRecordPayload* payload,
                                             char* message,
                                             size_t message_size) {
    if (!validate_schema(schema, message, message_size)) return false;
    if (slot == NULL || payload == NULL) {
        set_message(message, message_size, "fixed slot and payload are required");
        return false;
    }
    if (slot_size != ROW_SIZE) {
        set_message(message,
                    message_size,
                    "legacy fixed leaf slot must be exactly ROW_SIZE bytes");
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    memcpy(payload->bytes, slot, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
