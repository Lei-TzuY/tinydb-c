#include "diagnostics.h"

#include <stdio.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

bool tinydb_check_database(Table* table,
                           TinyDBPageOwnershipStats* ownership_stats,
                           char* message,
                           size_t message_size) {
    if (table == NULL || table->pager == NULL || ownership_stats == NULL) {
        set_message(message, message_size, "invalid database diagnostic input");
        return false;
    }

    memset(ownership_stats, 0, sizeof(*ownership_stats));
    if (message != NULL && message_size > 0) message[0] = '\0';

    if (table->catalog.num_tables == 0) {
        set_message(message, message_size, "catalog contains no table roots");
        return false;
    }

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        const TableSchema* schema = &table->catalog.schemas[i];
        for (uint32_t j = 0; j < i; j++) {
            const TableSchema* previous = &table->catalog.schemas[j];
            if (previous->root_page_num == schema->root_page_num) {
                if (message != NULL && message_size > 0) {
                    snprintf(message,
                             message_size,
                             "tables '%s' and '%s' share root page %u",
                             previous->name,
                             schema->name,
                             schema->root_page_num);
                }
                return false;
            }
        }

        TinyDBTreeStats tree_stats;
        if (!tinydb_get_tree_stats(table, schema->name, &tree_stats)) {
            if (message != NULL && message_size > 0) {
                snprintf(message,
                         message_size,
                         "table '%s' B+ tree structural validation failed",
                         schema->name);
            }
            return false;
        }
    }

    char ownership_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table,
                                     ownership_stats,
                                     ownership_message,
                                     sizeof(ownership_message))) {
        if (message != NULL && message_size > 0) {
            snprintf(message,
                     message_size,
                     "page ownership: %s",
                     ownership_message);
        }
        return false;
    }

    if (message != NULL && message_size > 0) {
        snprintf(message,
                 message_size,
                 "ok: tables=%u total=%u owned=%u free=%u orphan=%u shared=%u",
                 table->catalog.num_tables,
                 ownership_stats->total_pages,
                 ownership_stats->owned_pages,
                 ownership_stats->free_pages,
                 ownership_stats->orphan_pages,
                 ownership_stats->shared_pages);
    }
    return true;
}
