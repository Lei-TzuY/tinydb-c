#include "record_payload_nonroot_internal_split.h"
#include "record_payload_nonroot_split.h"
#include "record_payload_root_internal_split.h"

#include <stdio.h>
#include <string.h>

#define TINYDB_PAYLOAD_FULL_PARENT_ESCALATION \
    "payload-native INSERT reached a full internal parent; recursive parent overflow is not implemented yet"

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

static bool base_failure_allows_parent_escalation(bool applicable,
                                                  const char* message) {
    return applicable && message != NULL &&
           strcmp(message, TINYDB_PAYLOAD_FULL_PARENT_ESCALATION) == 0;
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

    /* Once the topology-neutral/non-full split path has positively matched a
     * request, preserve its fail-closed result unless the only reason it
     * stopped is the explicit full-parent capacity boundary. Corruption,
     * sibling-boundary failures, and other applicable errors must not be
     * masked by probing unrelated parent-growth handlers. */
    if (base_applicable &&
        !base_failure_allows_parent_escalation(base_applicable,
                                               base_message)) {
        if (applicable != NULL) *applicable = true;
        set_message(message,
                    message_size,
                    base_message[0] != '\0'
                        ? base_message
                        : "payload-native non-root split failed after matching the target topology");
        return false;
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

    /* A full-root handler only sets applicable after proving that the leaf is
     * actually beneath the full catalog-stable root. If it then fails, that
     * error is authoritative; trying the non-root-parent handler would both
     * repeat pager traversal and risk hiding a real root-topology failure. */
    if (root_applicable) {
        if (applicable != NULL) *applicable = true;
        set_message(message,
                    message_size,
                    root_message[0] != '\0'
                        ? root_message
                        : "payload-native full-root split failed after matching the target topology");
        return false;
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
