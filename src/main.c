#include "common.h"
#include "compiler.h"
#include "multitable.h"
#include "profile.h"
#include "vm.h"
#include "table.h"

#include <ctype.h>

typedef struct {
    char* buffer;
    size_t buffer_length;
    size_t input_length;
} InputBuffer;

InputBuffer* new_input_buffer() {
    InputBuffer* input_buffer = (InputBuffer*)malloc(sizeof(InputBuffer));
    input_buffer->buffer = NULL;
    input_buffer->buffer_length = 0;
    input_buffer->input_length = 0;
    return input_buffer;
}

void print_prompt() { printf("db > "); }

void read_input(InputBuffer* input_buffer) {
    size_t capacity = 1024;
    if (input_buffer->buffer == NULL) {
        input_buffer->buffer = (char*)malloc(capacity);
        input_buffer->buffer_length = capacity;
    }

    if (fgets(input_buffer->buffer, (int)input_buffer->buffer_length, stdin) == NULL) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }

    input_buffer->input_length = strlen(input_buffer->buffer);

    if (input_buffer->input_length > 0 && input_buffer->buffer[input_buffer->input_length - 1] == '\n') {
        input_buffer->buffer[input_buffer->input_length - 1] = '\0';
        input_buffer->input_length--;
    }
}

void close_input_buffer(InputBuffer* input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

static bool consume_ci_word(const char** input, const char* word) {
    const char* p = *input;
    const char* w = word;

    while (isspace((unsigned char)*p)) p++;
    while (*w != '\0' &&
           tolower((unsigned char)*p) == tolower((unsigned char)*w)) {
        p++;
        w++;
    }
    if (*w != '\0') return false;
    if (isalnum((unsigned char)*p) || *p == '_') return false;

    *input = p;
    return true;
}

static bool try_execute_explain_analyze(const char* input, Table* table) {
    const char* query = input;
    Statement statement;
    PrepareResult prepare_result;
    QueryProfile profile;
    MultiTableRouteScope route_scope;

    if (!consume_ci_word(&query, "explain") ||
        !consume_ci_word(&query, "analyze")) {
        return false;
    }

    while (isspace((unsigned char)*query)) query++;
    prepare_result = prepare_statement(query, &statement);
    if (prepare_result != PREPARE_SUCCESS || statement.type != STATEMENT_SELECT) {
        printf("Syntax error. EXPLAIN ANALYZE currently requires a SELECT statement.\n");
        return true;
    }

    MultiTableRouteResult route_result = multitable_begin_statement_scope(
        table, &statement, query, &route_scope);
    if (route_result != MULTITABLE_ROUTE_OK &&
        route_result != MULTITABLE_ROUTE_NOT_APPLICABLE) {
        printf("Error: %s.\n", multitable_route_error(route_result));
        return true;
    }

    bool profiled = query_profile_execute(&statement, table, &profile);
    multitable_end_statement_scope(table, &route_scope);

    if (!profiled) {
        printf("EXPLAIN ANALYZE could not profile this statement.\n");
        return true;
    }

    if (profile.plan_result != EXECUTE_SUCCESS) {
        printf("EXPLAIN ANALYZE failed while producing the plan.\n");
        return true;
    }

    printf("ANALYZE: execution_time_ms=%.3f cache_hits=%u cache_misses=%u evictions=%u page_accesses=%u\n",
           profile.execution_time_ms,
           profile.cache_hits,
           profile.cache_misses,
           profile.evictions,
           profile.page_accesses);

    if (profile.execute_result == EXECUTE_SUCCESS) {
        printf("Executed.\n");
    } else {
        printf("EXPLAIN ANALYZE query execution failed with code %d.\n",
               (int)profile.execute_result);
    }
    return true;
}

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table) {
    if (strcmp(input_buffer->buffer, ".exit") == 0) {
        close_input_buffer(input_buffer);
        db_close(table);
        exit(EXIT_SUCCESS);
    } else if (strcmp(input_buffer->buffer, ".btree") == 0) {
        print_tree(table->pager, table->root_page_num, 0);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".constants") == 0) {
        printf("PAGE_SIZE: %d\n", PAGE_SIZE);
        printf("ROW_SIZE: %d\n", (int)ROW_SIZE);
        printf("COMMON_NODE_HEADER_SIZE: %d\n", (int)COMMON_NODE_HEADER_SIZE);
        printf("LEAF_NODE_HEADER_SIZE: %d\n", (int)LEAF_NODE_HEADER_SIZE);
        printf("LEAF_NODE_CELL_SIZE: %d\n", (int)LEAF_NODE_CELL_SIZE);
        printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", (int)LEAF_NODE_SPACE_FOR_CELLS);
        printf("LEAF_NODE_MAX_CELLS: %d\n", (int)LEAF_NODE_MAX_CELLS);
        printf("INTERNAL_NODE_MAX_KEYS: %d\n", (int)INTERNAL_NODE_MAX_KEYS);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".stats") == 0) {
        TableStats stats;
        db_get_stats(table, &stats);
        printf("Total Pages: %u\n", stats.total_pages);
        printf("Leaf Pages: %u\n", stats.leaf_pages);
        printf("Internal Pages: %u\n", stats.internal_pages);
        printf("Free Pages: %u\n", stats.free_pages);
        printf("Total Rows: %u\n", stats.total_rows);
        printf("In Transaction: %s\n", table->in_transaction ? "Yes" : "No");
        printf("Secondary Index: %s\n", table->username_index_enabled ? "Enabled" : "Disabled");
        return META_COMMAND_SUCCESS;
    } else if (strncmp(input_buffer->buffer, ".page", 5) == 0) {
        uint32_t page_num = 0;
        if (sscanf(input_buffer->buffer, ".page %u", &page_num) == 1) {
            print_page(table, page_num);
            return META_COMMAND_SUCCESS;
        }
        printf("Usage: .page <page_num>\n");
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".schema") == 0) {
        printf("Table: users\n");
        printf("  id        INTEGER       PRIMARY KEY\n");
        printf("  username  VARCHAR(%d)\n", COLUMN_USERNAME_SIZE);
        printf("  email     VARCHAR(%d)\n", COLUMN_EMAIL_SIZE);
        if (table->username_index_enabled) {
            printf("Index: idx_users_username ON users(username)\n");
        }
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".tables") == 0) {
        table_print_tables(table);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".checkpoint") == 0) {
        db_checkpoint(table);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".check") == 0) {
        db_integrity_check(table);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".buffer_pool") == 0 ||
               strcmp(input_buffer->buffer, ".cache") == 0) {
        pager_print_buffer_pool_stats(table->pager);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".help") == 0) {
        printf("Meta commands:\n");
        printf("  .tables              List tables\n");
        printf("  .schema              Show table schema\n");
        printf("  .btree               Print B+ tree structure\n");
        printf("  .constants           Show database engine constants\n");
        printf("  .stats               Show database runtime statistics\n");
        printf("  .buffer_pool / .cache Display Buffer Pool Manager & LRU eviction stats\n");
        printf("  .page <n>            Inspect physical page <n> details\n");
        printf("  .checkpoint          Flush WAL frames to main database file\n");
        printf("  .check               Run B+ tree and pager integrity check\n");
        printf("  .help                Show this help\n");
        printf("  .exit                Exit\n");
        printf("\nSQL statements:\n");
        printf("  INSERT INTO users VALUES (id, 'username', 'email');\n");
        printf("  SELECT * FROM users;\n");
        printf("  SELECT * FROM users WHERE [id = N] [AND username LIKE 'p%%'] [AND email LIKE '%%s'];\n");
        printf("  SELECT COUNT(*)|MIN(id)|MAX(id)|SUM(id)|AVG(id) FROM users;\n");
        printf("  CREATE INDEX idx_users_username ON users(username);\n");
        printf("  DROP INDEX idx_users_username;\n");
        printf("  UPDATE users SET username = 'x' [, email = 'y'] WHERE id = N;\n");
        printf("  DELETE FROM users WHERE id = N;\n");
        printf("  DELETE FROM users;\n");
        printf("  VACUUM;\n");
        printf("  BEGIN;  COMMIT;  ROLLBACK;\n");
        printf("  SAVEPOINT sp1;  ROLLBACK TO sp1;  RELEASE sp1;\n");
        printf("  CHECKPOINT;\n");
        printf("  PRAGMA integrity_check;\n");
        printf("  PRAGMA table_info; / PRAGMA table_info(users);\n");
        printf("  PRAGMA index_list; / PRAGMA index_list(users);\n");
        printf("  PRAGMA user_version; / PRAGMA user_version = N;\n");
        printf("  EXPLAIN SELECT ...;\n");
        printf("  EXPLAIN ANALYZE SELECT ...;\n");
        return META_COMMAND_SUCCESS;
    } else {
        return META_COMMAND_UNRECOGNIZED_COMMAND;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    char* filename = argv[1];
    Table* table = db_open(filename);
    if (!multitable_catalog_load(table, filename)) {
        printf("Warning: schema catalog could not be loaded; using in-memory catalog state.\n");
    }
    InputBuffer* input_buffer = new_input_buffer();

    while (true) {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->input_length == 0) continue;

        if (input_buffer->buffer[0] == '.') {
            switch (do_meta_command(input_buffer, table)) {
                case (META_COMMAND_SUCCESS):
                    continue;
                case (META_COMMAND_UNRECOGNIZED_COMMAND):
                    printf("Unrecognized command '%s'\n", input_buffer->buffer);
                    continue;
            }
        }

        if (try_execute_explain_analyze(input_buffer->buffer, table)) {
            continue;
        }

        Statement statement;
        switch (prepare_statement(input_buffer->buffer, &statement)) {
            case (PREPARE_SUCCESS):
                break;
            case (PREPARE_SYNTAX_ERROR):
                printf("Syntax error. Could not parse statement.\n");
                continue;
            case (PREPARE_UNRECOGNIZED_STATEMENT):
                printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
                continue;
        }

        if (multitable_is_schema_ddl(statement.type) && table->in_transaction) {
            printf("Error: Schema DDL is not allowed inside a transaction.\n");
            continue;
        }

        if (table->catalog.num_tables > 1 && statement.type == STATEMENT_VACUUM) {
            printf("Error: VACUUM/VACUUM INTO is disabled for multi-table databases until compaction preserves every table root and schema sidecar.\n");
            continue;
        }

        if (table->catalog.num_tables > 1 && statement.type == STATEMENT_EXECUTE_PREPARED) {
            printf("Error: EXECUTE PREPARED is disabled for multi-table databases until bound SQL participates in root routing.\n");
            continue;
        }

        if (table->catalog.num_tables > 1 &&
            statement.type == STATEMENT_ALTER_TABLE &&
            statement.alter_table.is_add_column) {
            printf("Error: ALTER TABLE ADD COLUMN is disabled for multi-table fixed-Row storage until physical row migration is implemented.\n");
            continue;
        }

        if (statement.type == STATEMENT_CREATE_INDEX &&
            !multitable_index_target_supported(table, statement.create_index.table_name)) {
            printf("Error: Secondary indexes on non-primary table roots are not routed safely yet.\n");
            continue;
        }

        MultiTableRouteScope route_scope;
        MultiTableRouteResult route_result = multitable_begin_statement_scope(
            table, &statement, input_buffer->buffer, &route_scope);
        if (route_result != MULTITABLE_ROUTE_OK &&
            route_result != MULTITABLE_ROUTE_NOT_APPLICABLE) {
            printf("Error: %s.\n", multitable_route_error(route_result));
            continue;
        }

        ExecuteResult execute_result;
        if (statement.type == STATEMENT_DELETE &&
            statement.delete_all &&
            route_scope.active &&
            table->catalog.num_tables > 1) {
            execute_result = multitable_execute_delete_all(&statement, table, &route_scope);
        } else {
            execute_result = execute_statement(&statement, table);
        }

        multitable_end_statement_scope(table, &route_scope);

        if (execute_result == EXECUTE_SUCCESS && multitable_is_schema_ddl(statement.type)) {
            db_checkpoint(table);
            if (!multitable_catalog_save(table, filename)) {
                printf("Error: Schema catalog could not be persisted.\n");
            }
        }

        switch (execute_result) {
            case (EXECUTE_SUCCESS):
                printf("Executed.\n");
                break;
            case (EXECUTE_TABLE_FULL):
                printf("Error: Table full.\n");
                break;
            case (EXECUTE_DUPLICATE_KEY):
                printf("Error: Duplicate key.\n");
                break;
            case (EXECUTE_KEY_NOT_FOUND):
                printf("Error: Key not found.\n");
                break;
            case (EXECUTE_TRANSACTION_ALREADY_ACTIVE):
                printf("Error: Transaction already active.\n");
                break;
            case (EXECUTE_NO_ACTIVE_TRANSACTION):
                printf("Error: No active transaction.\n");
                break;
            case (EXECUTE_DDL_INSIDE_TRANSACTION):
                printf("Error: Index DDL is not allowed inside a transaction.\n");
                break;
            case (EXECUTE_SAVEPOINT_NOT_FOUND):
                printf("Error: Savepoint not found.\n");
                break;
            case (EXECUTE_SAVEPOINT_STACK_FULL):
                printf("Error: Savepoint stack full.\n");
                break;
        }
    }
    return 0;
}
