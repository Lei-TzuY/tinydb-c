#include "slotted_v2_publish_batch.h"

#include <stdio.h>
#include <string.h>

static int page_equals(const unsigned char* page,
                       unsigned char value,
                       unsigned char checksum_value) {
    for (uint32_t i = 0u; i < PAGE_USABLE_SIZE; i++) {
        if (page[i] != value) return 0;
    }
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != checksum_value) return 0;
    }
    return 1;
}

int main(void) {
    unsigned char page_a[PAGE_SIZE];
    unsigned char page_b[PAGE_SIZE];
    unsigned char page_c[PAGE_SIZE];
    unsigned char staged_a[PAGE_SIZE];
    unsigned char staged_b[PAGE_SIZE];
    unsigned char staged_c[PAGE_SIZE];

    memset(page_a, 0x11, PAGE_USABLE_SIZE);
    memset(page_b, 0x22, PAGE_USABLE_SIZE);
    memset(page_c, 0x33, PAGE_USABLE_SIZE);
    memset(page_a + PAGE_USABLE_SIZE, 0xA1, PAGE_CHECKSUM_SIZE);
    memset(page_b + PAGE_USABLE_SIZE, 0xB2, PAGE_CHECKSUM_SIZE);
    memset(page_c + PAGE_USABLE_SIZE, 0xC3, PAGE_CHECKSUM_SIZE);

    memset(staged_a, 0x44, sizeof(staged_a));
    memset(staged_b, 0x55, sizeof(staged_b));
    memset(staged_c, 0x66, sizeof(staged_c));

    TinyDBV2PublishEntry entries[3] = {
        {11u, page_a, staged_a},
        {12u, page_b, staged_b},
        {13u, page_c, staged_c},
    };

    if (tinydb_v2_publish_batch(entries, 3u, 2u)) {
        fprintf(stderr, "injected partial publication unexpectedly succeeded\n");
        return 1;
    }
    if (!page_equals(page_a, 0x11, 0xA1) ||
        !page_equals(page_b, 0x22, 0xB2) ||
        !page_equals(page_c, 0x33, 0xC3)) {
        fprintf(stderr, "failure did not restore every page image\n");
        return 2;
    }

    if (!tinydb_v2_publish_batch(entries, 3u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "atomic publication failed\n");
        return 3;
    }
    if (!page_equals(page_a, 0x44, 0xA1) ||
        !page_equals(page_b, 0x55, 0xB2) ||
        !page_equals(page_c, 0x66, 0xC3)) {
        fprintf(stderr, "publication or checksum isolation mismatch\n");
        return 4;
    }

    TinyDBV2PublishEntry duplicate[2] = {
        {21u, page_a, staged_a},
        {21u, page_b, staged_b},
    };
    if (tinydb_v2_publish_batch(duplicate, 2u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "duplicate page identity was accepted\n");
        return 5;
    }

    TinyDBV2PublishEntry alias[2] = {
        {31u, page_a, staged_a},
        {32u, page_a, staged_b},
    };
    if (tinydb_v2_publish_batch(alias, 2u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "aliased target pointer was accepted\n");
        return 6;
    }

    if (tinydb_v2_publish_batch(entries, 0u, TINYDB_V2_PUBLISH_NO_FAIL) ||
        tinydb_v2_publish_batch(NULL, 1u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "invalid batch shape was accepted\n");
        return 7;
    }

    printf("SLOTTED_V2_PUBLISH_BATCH_OK atomic=yes rollback=yes checksum_isolated=yes duplicate_guard=yes\n");
    return 0;
}
