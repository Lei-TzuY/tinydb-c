#include "record_payload.h"

#include <stdio.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

bool tinydb_record_payload_schema_supported(const TableSchema* schema,
                                            char* message,
                                            size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (schema == NULL) {
        set_message(message, message_size, "schema is required");
        return false;
    }
    if (schema->num_columns == 0u || schema->num_columns > MAX_COLUMNS_PER_TABLE) {
        set_message(message, message_size, "schema has an invalid column count");
        return false;
    }
    if (schema->row_size == 0u || schema->row_size > TINYDB_RECORD_PAYLOAD_MAX) {
        set_message(message,
                    message_size,
                    "schema row exceeds the schema-aware logical payload capacity");
        return false;
    }
    if (schema->columns[0].type != COL_TYPE_INT ||
        schema->columns[0].offset != 0u ||
        schema->columns[0].size != sizeof(uint32_t)) {
        set_message(message,
                    message_size,
                    "generic payload records require an INT primary key as the first column");
        return false;
    }

    uint32_t expected_offset = 0u;
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        if (column->size == 0u || column->offset != expected_offset ||
            column->offset > schema->row_size ||
            column->size > schema->row_size - column->offset) {
            set_message(message,
                        message_size,
                        "schema columns must form a contiguous serialized payload layout");
            return false;
        }
        if (column->type == COL_TYPE_INT) {
            if (column->size != sizeof(uint32_t)) {
                set_message(message,
                            message_size,
                            "INT columns must use a 4-byte serialized representation");
                return false;
            }
        } else if (column->type != COL_TYPE_VARCHAR) {
            set_message(message, message_size, "schema contains an unsupported column type");
            return false;
        }
        if (expected_offset > UINT32_MAX - column->size) {
            set_message(message, message_size, "schema serialized offsets overflow");
            return false;
        }
        expected_offset += column->size;
    }
    if (expected_offset != schema->row_size) {
        set_message(message,
                    message_size,
                    "schema row size must match the serialized payload layout");
        return false;
    }
    return true;
}

bool tinydb_record_payload_encode_values(const TableSchema* schema,
                                         const TinyDBValue* values,
                                         uint32_t value_count,
                                         TinyDBRecordPayload* payload,
                                         char* message,
                                         size_t message_size) {
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (values == NULL || payload == NULL || value_count != schema->num_columns) {
        set_message(message,
                    message_size,
                    "record value count does not match the payload schema");
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        const TinyDBValue* value = &values[i];
        unsigned char* destination = payload->bytes + column->offset;
        if (value->type != column->type) {
            set_message(message,
                        message_size,
                        "record value type does not match payload column schema");
            memset(payload, 0, sizeof(*payload));
            return false;
        }
        if (column->type == COL_TYPE_INT) {
            memcpy(destination, &value->int_value, sizeof(value->int_value));
        } else {
            size_t length = strlen(value->text);
            if (length >= column->size) {
                set_message(message,
                            message_size,
                            "VARCHAR value exceeds its serialized column capacity");
                memset(payload, 0, sizeof(*payload));
                return false;
            }
            memcpy(destination, value->text, length);
            destination[length] = '\0';
        }
    }
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_decode_values(const TableSchema* schema,
                                         const TinyDBRecordPayload* payload,
                                         TinyDBValue* values,
                                         uint32_t value_capacity,
                                         uint32_t* value_count,
                                         char* message,
                                         size_t message_size) {
    if (value_count != NULL) *value_count = 0u;
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (payload == NULL || values == NULL || value_capacity < schema->num_columns ||
        payload->length != schema->row_size) {
        set_message(message, message_size, "payload decode buffer or length is invalid");
        return false;
    }

    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        const unsigned char* source = payload->bytes + column->offset;
        TinyDBValue* value = &values[i];
        memset(value, 0, sizeof(*value));
        value->type = column->type;
        if (column->type == COL_TYPE_INT) {
            memcpy(&value->int_value, source, sizeof(value->int_value));
        } else {
            uint32_t length = 0u;
            while (length < column->size && source[length] != '\0') length++;
            if (length == column->size || length > TINYDB_RECORD_TEXT_MAX) {
                set_message(message,
                            message_size,
                            "serialized VARCHAR does not fit the logical value carrier");
                return false;
            }
            memcpy(value->text, source, length);
            value->text[length] = '\0';
        }
    }
    if (value_count != NULL) *value_count = schema->num_columns;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

static bool validate_legacy_schema(const TableSchema* schema,
                                   char* message,
                                   size_t message_size) {
    if (!tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }
    if (schema->row_size > ROW_SIZE) {
        set_message(message,
                    message_size,
                    "logical record does not fit the legacy fixed record carrier");
        return false;
    }
    return true;
}

bool tinydb_record_payload_from_record(const TableSchema* schema,
                                       const TinyDBRecord* record,
                                       TinyDBRecordPayload* payload,
                                       char* message,
                                       size_t message_size) {
    if (!validate_legacy_schema(schema, message, message_size)) return false;
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
    if (!validate_legacy_schema(schema, message, message_size)) return false;
    if (payload == NULL || record == NULL) {
        set_message(message, message_size, "payload and record are required");
        return false;
    }
    if (payload->length != schema->row_size || payload->length > ROW_SIZE) {
        set_message(message,
                    message_size,
                    "payload length does not match the legacy schema row size");
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
    if (!validate_legacy_schema(schema, message, message_size)) return false;
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
