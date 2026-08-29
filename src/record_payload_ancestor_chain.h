#ifndef TINYDB_RECORD_PAYLOAD_ANCESTOR_CHAIN_H
#define TINYDB_RECORD_PAYLOAD_ANCESTOR_CHAIN_H

#include "leaf_cursor_read.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t leaf_page_num;
    uint32_t* internal_pages;
    uint32_t count;
} TinyDBPayloadAncestorChain;

static void tinydb_payload_ancestor_set_message(char* message,
                                                 size_t message_size,
                                                 const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text != NULL ? text : "");
    }
}

static uint32_t tinydb_payload_ancestor_read_u32(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void tinydb_record_payload_ancestor_chain_release(
    TinyDBPayloadAncestorChain* chain) {
    if (chain == NULL) return;
    free(chain->internal_pages);
    chain->internal_pages = NULL;
    chain->leaf_page_num = INVALID_PAGE_NUM;
    chain->count = 0u;
}

static bool tinydb_payload_parent_references_child(
    const unsigned char parent[PAGE_SIZE],
    uint32_t child_page_num) {
    if (parent == NULL || !tinydb_parent_stage_validate(parent, PAGE_SIZE)) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        if (tinydb_parent_stage_child_at(parent, i) == child_page_num) {
            return true;
        }
    }
    return false;
}

/*
 * Re-traverse the payload INSERT target and collect the validated internal
 * ancestry from the selected leaf's parent through the catalog-stable root.
 * The returned parent-first page-number chain is deliberately detached from
 * Pager frames so later recursive split staging can reuse the exact topology
 * that was validated without retaining pointers across get_page() calls.
 *
 * Every child->parent link must be reciprocated by the parent's child list.
 * Foreign roots, cycles, invalid page numbers, and chains that do not reach
 * schema->root_page_num fail closed. The caller owns no memory on failure and
 * must release a successful chain with
 * tinydb_record_payload_ancestor_chain_release().
 */
static bool tinydb_record_payload_collect_ancestor_chain(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    TinyDBPayloadAncestorChain* chain,
    char* message,
    size_t message_size) {
    if (chain != NULL) {
        chain->leaf_page_num = INVALID_PAGE_NUM;
        chain->internal_pages = NULL;
        chain->count = 0u;
    }
    if (table == NULL || table->pager == NULL || schema == NULL ||
        chain == NULL || schema->root_page_num >= table->pager->num_pages) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "invalid payload ancestor validation input");
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    table->root_page_num = previous_root;
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        tinydb_payload_ancestor_set_message(
            message, message_size, "unable to locate payload overflow leaf while validating ancestry");
        return false;
    }

    uint32_t current_page_num = cursor->page_num;
    free(cursor);
    if (current_page_num == schema->root_page_num) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload overflow unexpectedly resolved to the root leaf");
        return false;
    }

    size_t capacity = (size_t)table->pager->num_pages;
    if (capacity > SIZE_MAX / sizeof(uint32_t)) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "payload overflow ancestry allocation is too large");
        return false;
    }
    uint32_t* internal_pages =
        (uint32_t*)malloc(capacity * sizeof(uint32_t));
    if (internal_pages == NULL) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "unable to allocate payload overflow ancestry chain");
        return false;
    }

    unsigned char child[PAGE_SIZE];
    memcpy(child, get_page(table->pager, current_page_num), PAGE_SIZE);
    if (child[IS_ROOT_OFFSET] != 0u) {
        free(internal_pages);
        tinydb_payload_ancestor_set_message(
            message, message_size, "non-root payload overflow leaf is incorrectly marked as root");
        return false;
    }

    uint32_t leaf_page_num = current_page_num;
    uint32_t parent_page_num = tinydb_payload_ancestor_read_u32(
        child + PARENT_POINTER_OFFSET);
    uint32_t count = 0u;
    for (uint32_t depth = 0u; depth < table->pager->num_pages; depth++) {
        if (parent_page_num >= table->pager->num_pages ||
            parent_page_num == current_page_num) {
            free(internal_pages);
            tinydb_payload_ancestor_set_message(
                message, message_size, "payload overflow ancestry contains an invalid parent pointer");
            return false;
        }

        unsigned char parent[PAGE_SIZE];
        memcpy(parent, get_page(table->pager, parent_page_num), PAGE_SIZE);
        if (get_node_type(parent) != NODE_INTERNAL ||
            !tinydb_parent_stage_validate(parent, PAGE_SIZE) ||
            !tinydb_payload_parent_references_child(parent, current_page_num)) {
            free(internal_pages);
            tinydb_payload_ancestor_set_message(
                message, message_size, "payload overflow ancestry has a non-reciprocal internal parent link");
            return false;
        }

        internal_pages[count++] = parent_page_num;
        if (parent_page_num == schema->root_page_num) {
            if (parent[IS_ROOT_OFFSET] == 0u) {
                free(internal_pages);
                tinydb_payload_ancestor_set_message(
                    message, message_size, "catalog payload root is not marked as root");
                return false;
            }
            chain->leaf_page_num = leaf_page_num;
            chain->internal_pages = internal_pages;
            chain->count = count;
            if (message != NULL && message_size > 0u) message[0] = '\0';
            return true;
        }

        if (parent[IS_ROOT_OFFSET] != 0u) {
            free(internal_pages);
            tinydb_payload_ancestor_set_message(
                message, message_size, "payload overflow ancestry reaches a foreign root before the catalog root");
            return false;
        }

        current_page_num = parent_page_num;
        parent_page_num = tinydb_payload_ancestor_read_u32(
            parent + PARENT_POINTER_OFFSET);
    }

    free(internal_pages);
    tinydb_payload_ancestor_set_message(
        message, message_size, "payload overflow ancestry is cyclic or exceeds the page bound");
    return false;
}

/* Read-only compatibility wrapper for callers that only need validation. */
static bool tinydb_record_payload_validate_ancestor_chain(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    char* message,
    size_t message_size) {
    TinyDBPayloadAncestorChain chain;
    if (!tinydb_record_payload_collect_ancestor_chain(
            table, schema, key, &chain, message, message_size)) {
        return false;
    }
    tinydb_record_payload_ancestor_chain_release(&chain);
    return true;
}

#endif
