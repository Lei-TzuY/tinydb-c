#include "user_version.h"

#include "pager_try_pin.h"

#include <stdio.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

bool db_try_get_user_version(Table* table,
                             uint32_t* version,
                             char* message,
                             size_t message_size) {
    if (version != NULL) *version = 0u;
    set_message(message, message_size, "");

    if (version == NULL) {
        set_message(message, message_size, "invalid user_version output");
        return false;
    }
    if (table == NULL || table->pager == NULL) {
        set_message(message, message_size, "invalid database handle");
        return false;
    }
    if (table->pager->num_pages == 0u) return true;

    PagerPageHandle root_handle;
    PagerTryPinStatus pin_status = pager_try_pin_existing_page_handle(
        table->pager,
        table->root_page_num,
        &root_handle);
    if (pin_status != PAGER_TRY_PIN_OK) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "could not acquire root page: %s",
                     pager_try_pin_status_string(pin_status));
        }
        return false;
    }

    uint32_t value = *node_parent(root_handle.data);
    if (!pager_release_page_handle(&root_handle)) {
        set_message(message,
                    message_size,
                    "could not release root-page pin");
        return false;
    }

    *version = value;
    return true;
}
