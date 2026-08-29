#include "record_payload_nonroot_internal_split.h"
#include "record_payload_nonroot_split.h"
#include "record_payload_root_internal_split.h"

#include <stdio.h>

bool tinydb_record_payload_try_nonroot_split_nonfull_base(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size);

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text != NULL ? text : "");
    }
}

bool tinydb_record_payload_try_nonroot_split(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size) {
    if (applicable != NULL) *applicable = false;

    bool base_applicable = false;
    char base_message[TINYDB_RECORD_MESSAGE_MAX];
    base_message[0] = '\0';
    if (tinydb_record_payload_try_nonroot_split_nonfull_base(
            table,
            schema,
            key,
            envelope,
            envelope_length,
            &base_applicable,
            base_message,
            sizeof(base_message))) {
        if (applicable != NULL) *applicable = true;
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    bool root_applicable = false;
    char root_message[TINYDB_RECORD_MESSAGE_MAX];
    root_message[0] = '\0';
    if (tinydb_record_payload_try_full_root_parent_split(
            table,
            schema,
            key,
            envelope,
            envelope_length,
            &root_applicable,
            root_message,
            sizeof(root_message))) {
        if (applicable != NULL) *applicable = true;
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    bool nonroot_parent_applicable = false;
    char nonroot_parent_message[TINYDB_RECORD_MESSAGE_MAX];
    nonroot_parent_message[0] = '\0';
    if (tinydb_record_payload_try_full_nonroot_parent_split(
            table,
            schema,
            key,
            envelope,
            envelope_length,
            &nonroot_parent_applicable,
            nonroot_parent_message,
            sizeof(nonroot_parent_message))) {
        if (applicable != NULL) *applicable = true;
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (applicable != NULL) {
        *applicable = base_applicable || root_applicable ||
                      nonroot_parent_applicable;
    }
    if (nonroot_parent_applicable && nonroot_parent_message[0] != '\0') {
        set_message(message, message_size, nonroot_parent_message);
    } else if (root_applicable && root_message[0] != '\0') {
        set_message(message, message_size, root_message);
    } else if (base_message[0] != '\0') {
        set_message(message, message_size, base_message);
    } else if (nonroot_parent_message[0] != '\0') {
        set_message(message, message_size, nonroot_parent_message);
    } else if (root_message[0] != '\0') {
        set_message(message, message_size, root_message);
    } else {
        set_message(message,
                    message_size,
                    "payload-native non-root overflow is not applicable to the current topology");
    }
    return false;
}
