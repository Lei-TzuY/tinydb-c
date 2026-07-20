#include "common.h"
#include "compiler.h"
#include "vm.h"
#include "table.h"

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
    size_t capacity = 1024; // Simple fixed capacity for now
    if (input_buffer->buffer == NULL) {
        input_buffer->buffer = (char*)malloc(capacity);
        input_buffer->buffer_length = capacity;
    }
    
    if (fgets(input_buffer->buffer, (int)input_buffer->buffer_length, stdin) == NULL) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }
    
    input_buffer->input_length = strlen(input_buffer->buffer);
    
    // Strip trailing newline
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

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table) {
    if (strcmp(input_buffer->buffer, ".exit") == 0) {
        close_input_buffer(input_buffer);
        db_close(table);
        exit(EXIT_SUCCESS);
    } else if (strcmp(input_buffer->buffer, ".btree") == 0) {
        print_tree(table->pager, table->root_page_num, 0);
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
        printf("users\n");
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".help") == 0) {
        printf("Meta commands:\n");
        printf("  .tables              List tables\n");
        printf("  .schema              Show table schema\n");
        printf("  .btree               Print B+ tree structure\n");
        printf("  .help                Show this help\n");
        printf("  .exit                Exit\n");
        printf("\nSQL statements:\n");
        printf("  INSERT INTO users VALUES (id, 'username', 'email');\n");
        printf("  SELECT * FROM users;\n");
        printf("  SELECT * FROM users WHERE id = N;\n");
        printf("  SELECT * FROM users WHERE username = 'x';\n");
        printf("  CREATE INDEX idx_users_username ON users(username);\n");
        printf("  DROP INDEX idx_users_username;\n");
        printf("  UPDATE users SET username = 'x' [, email = 'y'] WHERE id = N;\n");
        printf("  DELETE FROM users WHERE id = N;\n");
        printf("  DELETE FROM users;\n");
        printf("  BEGIN;  COMMIT;  ROLLBACK;\n");
        printf("  EXPLAIN SELECT ...;\n");
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

        switch (execute_statement(&statement, table)) {
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
        }
    }
    return 0;
}
