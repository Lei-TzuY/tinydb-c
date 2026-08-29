#include "record_payload_ancestor_chain.h"
#include "record_payload_nonroot_internal_split.h"
#include "record_payload_nonroot_split.h"
#include "record_payload_overflow_reservation.h"
#include "record_payload_root_internal_split.h"

#include <stdio.h>
#include <string.h>

#define TINYDB_PAYLOAD_FULL_PARENT_ESCALATION \
    "payload-native INSERT reached a full internal parent; recursive parent overflow is not implemented yet"
#define TINYDB_PAYLOAD_FULL_GRANDPARENT_ESCALATION \
    "payload-native recursive internal overflow beyond the grandparent remains fail-closed"

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

static bool nonroot_failure_is_recursive_capacity(bool applicable,
                                                  const char* message) {
    return applicable && message != NULL &&
           strcmp(message, TINYDB_PAYLOAD_FULL_GRANDPARENT_ESCALATION) == 0;
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

    /* A full grandparent is the first still-unimplemented payload-native
     * recursive capacity boundary. Before reporting that limitation, collect
     * the selected leaf's reciprocal ancestry, convert it into a read-only
     * cascade preflight, and size the complete allocator reservation. This
     * proves both how far the separator must propagate and how many pages the
     * eventual mutation must claim before durable publication begins. */
    if (nonroot_failure_is_recursive_capacity(nonroot_parent_applicable,
                                              nonroot_parent_message)) {
        TinyDBPayloadAncestorChain chain;
        TinyDBPayloadOverflowPlan plan;
        TinyDBPayloadOverflowReservation reservation = {0};
        char ancestry_message[TINYDB_RECORD_MESSAGE_MAX];
        ancestry_message[0] = '\0';
        if (!tinydb_record_payload_collect_ancestor_chain(
                table,
                schema,
                key,
                &chain,
                ancestry_message,
                sizeof(ancestry_message))) {
            if (applicable != NULL) *applicable = true;
            set_message(message,
                        message_size,
                        ancestry_message[0] != '\0'
                            ? ancestry_message
                            : "payload-native recursive overflow ancestry collection failed");
            return false;
        }
        bool planned = tinydb_record_payload_plan_overflow_chain(
            table,
            schema,
            &chain,
            &plan,
            ancestry_message,
            sizeof(ancestry_message));
        bool sized = planned && tinydb_record_payload_size_overflow_reservation(
            &chain,
            &plan,
            &reservation,
            ancestry_message,
            sizeof(ancestry_message));
        tinydb_record_payload_ancestor_chain_release(&chain);
        if (!planned || !sized) {
            if (applicable != NULL) *applicable = true;
            set_message(message,
                        message_size,
                        ancestry_message[0] != '\0'
                            ? ancestry_message
                            : "payload-native recursive overflow preflight failed");
            return false;
        }
        if (plan.full_internal_levels < 2u ||
            reservation.new_leaf_pages != 1u ||
            reservation.new_internal_pages < 2u ||
            reservation.total_pages < 3u) {
            if (applicable != NULL) *applicable = true;
            set_message(message,
                        message_size,
                        "payload-native recursive overflow preflight disagrees with the full-grandparent boundary");
            return false;
        }
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
