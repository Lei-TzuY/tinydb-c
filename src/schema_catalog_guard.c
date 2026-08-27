#include "multitable.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool multitable_catalog_load_checksums_base(Table* table,
                                            const char* database_filename);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (ci_char(*left) != ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool name_present(const char* name, size_t capacity) {
    if (name == NULL || capacity == 0 || name[0] == '\0') return false;
    return memchr(name, '\0', capacity) != NULL;
}

static bool catalog_identity_valid(const Table* table) {
    if (table == NULL || table->catalog.num_tables == 0 ||
        table->catalog.num_tables > MAX_TABLES ||
        table->catalog.num_views > MAX_VIEWS) {
        printf("Ignoring schema catalog with invalid object counts.\n");
        return false;
    }

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        const TableSchema* current = &table->catalog.schemas[i];
        if (!name_present(current->name, sizeof(current->name))) {
            printf("Ignoring schema catalog with an empty or unterminated table name.\n");
            return false;
        }

        for (uint32_t j = 0; j < i; j++) {
            const TableSchema* previous = &table->catalog.schemas[j];
            if (ci_equal(previous->name, current->name)) {
                printf("Ignoring schema catalog with duplicate table name '%s'.\n",
                       current->name);
                return false;
            }
            if (previous->root_page_num == current->root_page_num) {
                printf("Ignoring schema catalog with duplicate root page %u for tables '%s' and '%s'.\n",
                       current->root_page_num,
                       previous->name,
                       current->name);
                return false;
            }
        }
    }

    for (uint32_t i = 0; i < table->catalog.num_views; i++) {
        const ViewSchema* current = &table->catalog.views[i];
        if (!name_present(current->name, sizeof(current->name))) {
            printf("Ignoring schema catalog with an empty or unterminated view name.\n");
            return false;
        }

        for (uint32_t j = 0; j < i; j++) {
            if (ci_equal(table->catalog.views[j].name, current->name)) {
                printf("Ignoring schema catalog with duplicate view name '%s'.\n",
                       current->name);
                return false;
            }
        }
        for (uint32_t j = 0; j < table->catalog.num_tables; j++) {
            if (ci_equal(table->catalog.schemas[j].name, current->name)) {
                printf("Ignoring schema catalog because table and view share name '%s'.\n",
                       current->name);
                return false;
            }
        }
    }

    return true;
}

bool multitable_catalog_load(Table* table, const char* database_filename) {
    if (!multitable_catalog_load_checksums_base(table, database_filename)) {
        return false;
    }
    return catalog_identity_valid(table);
}
