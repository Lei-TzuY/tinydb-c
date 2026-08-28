#ifndef TINYDB_INTERNAL_ROOT_COLLAPSE_STAGE_H
#define TINYDB_INTERNAL_ROOT_COLLAPSE_STAGE_H

#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Stage the height-two -> height-one collapse of a V2 tree without touching
 * Pager allocation state.
 *
 * The input root must be a valid one-key internal root with exactly two leaf
 * children. One child has already become empty and is being removed by the
 * caller. The surviving child image must already describe the only remaining
 * leaf in the chain (prev = next = 0), still be non-root, and still point at
 * the stable root page as its parent.
 *
 * TinyDB keeps catalog root page numbers stable, so collapse copies the
 * surviving V2 leaf image into the existing root page rather than changing the
 * schema's root_page_num. The surviving child page itself is deliberately not
 * modified; after an atomic Pager/WAL publication the caller may reclaim both
 * the removed empty page and the promoted child page.
 *
 * Only PAGE_USABLE_SIZE is copied on success. Pager-owned checksum trailer
 * bytes in the stable root page are preserved. Every validation happens in
 * scratch memory, so failure leaves both caller images byte-for-byte unchanged.
 */
static inline bool tinydb_stage_internal_root_collapse_to_v2_leaf(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
    const void* surviving_leaf_page,
    size_t surviving_capacity,
    uint32_t surviving_leaf_page_num,
    uint32_t removed_child_page_num,
    uint32_t removed_child_max,
    uint32_t* promoted_page_num_out) {
    if (promoted_page_num_out != NULL) {
        *promoted_page_num_out = INVALID_PAGE_NUM;
    }
    if (root_page == NULL || surviving_leaf_page == NULL ||
        root_capacity < PAGE_SIZE || surviving_capacity < PAGE_SIZE ||
        surviving_leaf_page_num == 0u ||
        surviving_leaf_page_num == INVALID_PAGE_NUM ||
        removed_child_page_num == 0u ||
        removed_child_page_num == INVALID_PAGE_NUM ||
        surviving_leaf_page_num == removed_child_page_num ||
        surviving_leaf_page_num == root_page_num ||
        removed_child_page_num == root_page_num) {
        return false;
    }

    const unsigned char* original_root = (const unsigned char*)root_page;
    const unsigned char* surviving =
        (const unsigned char*)surviving_leaf_page;
    if (original_root[NODE_TYPE_OFFSET] != (unsigned char)NODE_INTERNAL ||
        original_root[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(original_root + PARENT_POINTER_OFFSET) !=
            0u ||
        !tinydb_parent_stage_validate(original_root, root_capacity) ||
        tinydb_parent_stage_read_u32(
            original_root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_slotted_leaf_v2_validate(surviving, surviving_capacity) ||
        surviving[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(surviving + PARENT_POINTER_OFFSET) !=
            root_page_num) {
        return false;
    }

    uint32_t previous_page_num = INVALID_PAGE_NUM;
    uint32_t next_page_num = INVALID_PAGE_NUM;
    uint32_t survivor_count = 0u;
    if (!tinydb_leaf_page_prev(surviving,
                               surviving_capacity,
                               &previous_page_num) ||
        !tinydb_leaf_page_next(surviving,
                               surviving_capacity,
                               &next_page_num) ||
        previous_page_num != 0u || next_page_num != 0u ||
        !tinydb_leaf_page_count(surviving,
                                surviving_capacity,
                                &survivor_count) ||
        survivor_count == 0u) {
        return false;
    }

    uint32_t survivor_min = 0u;
    uint32_t survivor_max = 0u;
    if (!tinydb_leaf_page_key_at(surviving,
                                 surviving_capacity,
                                 0u,
                                 &survivor_min) ||
        !tinydb_leaf_page_key_at(surviving,
                                 surviving_capacity,
                                 survivor_count - 1u,
                                 &survivor_max) ||
        survivor_min > survivor_max) {
        return false;
    }

    uint32_t left_child = tinydb_parent_stage_child_at(original_root, 0u);
    uint32_t right_child = tinydb_parent_stage_child_at(original_root, 1u);
    uint32_t separator = tinydb_parent_stage_key_at(original_root, 0u);
    bool removed_left = left_child == removed_child_page_num &&
                        right_child == surviving_leaf_page_num;
    bool removed_right = right_child == removed_child_page_num &&
                         left_child == surviving_leaf_page_num;
    if (removed_left == removed_right) return false;

    if (removed_left) {
        if (separator != removed_child_max ||
            removed_child_max >= survivor_min) {
            return false;
        }
    } else {
        if (separator != survivor_max || removed_child_max <= survivor_max) {
            return false;
        }
    }

    unsigned char scratch[PAGE_SIZE];
    memcpy(scratch, original_root, PAGE_SIZE);
    memcpy(scratch, surviving, PAGE_USABLE_SIZE);
    scratch[IS_ROOT_OFFSET] = 1u;
    tinydb_parent_stage_write_u32(scratch + PARENT_POINTER_OFFSET, 0u);

    if (!tinydb_slotted_leaf_v2_validate(scratch, PAGE_SIZE) ||
        scratch[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(scratch + PARENT_POINTER_OFFSET) != 0u) {
        return false;
    }

    uint32_t staged_previous = INVALID_PAGE_NUM;
    uint32_t staged_next = INVALID_PAGE_NUM;
    uint32_t staged_count = 0u;
    uint32_t staged_min = 0u;
    uint32_t staged_max = 0u;
    if (!tinydb_leaf_page_prev(scratch, PAGE_SIZE, &staged_previous) ||
        !tinydb_leaf_page_next(scratch, PAGE_SIZE, &staged_next) ||
        staged_previous != 0u || staged_next != 0u ||
        !tinydb_leaf_page_count(scratch, PAGE_SIZE, &staged_count) ||
        staged_count != survivor_count ||
        !tinydb_leaf_page_key_at(scratch, PAGE_SIZE, 0u, &staged_min) ||
        !tinydb_leaf_page_key_at(scratch,
                                 PAGE_SIZE,
                                 staged_count - 1u,
                                 &staged_max) ||
        staged_min != survivor_min || staged_max != survivor_max) {
        return false;
    }

    memcpy(root_page, scratch, PAGE_USABLE_SIZE);
    if (promoted_page_num_out != NULL) {
        *promoted_page_num_out = surviving_leaf_page_num;
    }
    return true;
}

#endif /* TINYDB_INTERNAL_ROOT_COLLAPSE_STAGE_H */
