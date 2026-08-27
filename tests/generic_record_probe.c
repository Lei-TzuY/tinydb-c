#include "engine.h"
#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool exec_ok(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    TinyDBSqlStatus status = tinydb_execute_sql(db, sql, &result);
    if (status != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "SQL failed: %s (%s)\n", sql, result.message);
        return false;
    }
    return true;
}

static TinyDBValue int_value(uint32_t value) {
    TinyDBValue result;
    memset(&result, 0, sizeof(result));
    result.type = COL_TYPE_INT;
    result.int_value = value;
    return result;
}

static TinyDBValue text_value(const char* value) {
    TinyDBValue result;
    memset(&result, 0, sizeof(result));
    result.type = COL_TYPE_VARCHAR;
    snprintf(result.text, sizeof(result.text), "%s", value);
    return result;
}

static bool expect_product(Table* table,
                           TableSchema* schema,
                           uint32_t id,
                           const char* name,
                           uint32_t price) {
    TinyDBRecord record;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!tinydb_record_find(table, schema, id, &record)) {
        fprintf(stderr, "product %u not found\n", id);
        return false;
    }
    if (!tinydb_record_decode(schema,
                              &record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &count,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "product decode failed: %s\n", message);
        return false;
    }
    if (count != 3 ||
        values[0].int_value != id ||
        strcmp(values[1].text, name) != 0 ||
        values[2].int_value != price) {
        fprintf(stderr, "product %u decoded values do not match\n", id);
        return false;
    }

    uint32_t raw_price = 0;
    memcpy(&raw_price,
           record.bytes + schema->columns[2].offset,
           sizeof(raw_price));
    if (raw_price != price) {
        fprintf(stderr, "product %u raw price offset is incorrect\n", id);
        return false;
    }
    for (uint32_t i = schema->row_size; i < ROW_SIZE; i++) {
        if (record.bytes[i] != 0) {
            fprintf(stderr, "product %u has nonzero fixed-slot tail at byte %u\n", id, i);
            return false;
        }
    }
    return true;
}

static bool expect_order(Table* table,
                         TableSchema* schema,
                         uint32_t id,
                         uint32_t product_id,
                         uint32_t quantity) {
    TinyDBRecord record;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!tinydb_record_find(table, schema, id, &record)) {
        fprintf(stderr, "order %u not found\n", id);
        return false;
    }
    if (!tinydb_record_decode(schema,
                              &record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &count,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "order decode failed: %s\n", message);
        return false;
    }
    if (count != 3 ||
        values[0].int_value != id ||
        values[1].int_value != product_id ||
        values[2].int_value != quantity) {
        fprintf(stderr, "order %u decoded values do not match\n", id);
        return false;
    }
    for (uint32_t i = schema->row_size; i < ROW_SIZE; i++) {
        if (record.bytes[i] != 0) {
            fprintf(stderr, "order %u has nonzero fixed-slot tail at byte %u\n", id, i);
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s DATABASE\n", argv[0]);
        return EXIT_FAILURE;
    }

    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;

    if (!exec_ok(db, "CREATE TABLE products (id INT, name VARCHAR, price INT);") ||
        !exec_ok(db, "CREATE TABLE orders (id INT, product_id INT, quantity INT);")) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    Table* table = tinydb_table(db);
    TableSchema* products = table_get_schema(table, "products");
    TableSchema* orders = table_get_schema(table, "orders");
    if (products == NULL || orders == NULL || products == orders) {
        fprintf(stderr, "generic schemas were not created\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    if (products->row_size != 264u || orders->row_size != 12u ||
        products->columns[2].offset != 260u || orders->columns[2].offset != 8u) {
        fprintf(stderr,
                "unexpected layouts: products=%u price_offset=%u orders=%u qty_offset=%u\n",
                products->row_size,
                products->columns[2].offset,
                orders->row_size,
                orders->columns[2].offset);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    for (uint32_t i = 1; i <= 30; i++) {
        char name[64];
        snprintf(name, sizeof(name), "product_%u", i);
        TinyDBValue values[3];
        values[0] = int_value(i);
        values[1] = text_value(name);
        values[2] = int_value(i * 100u);
        if (!tinydb_record_insert(table, products, values, 3, message, sizeof(message))) {
            fprintf(stderr, "product insert %u failed: %s\n", i, message);
            tinydb_close(db);
            return EXIT_FAILURE;
        }
    }

    for (uint32_t i = 1; i <= 20; i++) {
        TinyDBValue values[3];
        values[0] = int_value(i);
        values[1] = int_value((i % 30u) + 1u);
        values[2] = int_value((i % 5u) + 1u);
        if (!tinydb_record_insert(table, orders, values, 3, message, sizeof(message))) {
            fprintf(stderr, "order insert %u failed: %s\n", i, message);
            tinydb_close(db);
            return EXIT_FAILURE;
        }
    }

    if (tinydb_record_scan(table, products, NULL, NULL) != 30u ||
        tinydb_record_scan(table, orders, NULL, NULL) != 20u ||
        !expect_product(table, products, 29u, "product_29", 2900u) ||
        !expect_order(table, orders, 19u, 20u, 5u)) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("GENERIC_LAYOUT products_row_size=%u price_offset=%u orders_row_size=%u quantity_offset=%u\n",
           products->row_size,
           products->columns[2].offset,
           orders->row_size,
           orders->columns[2].offset);

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    products = table_get_schema(table, "products");
    orders = table_get_schema(table, "orders");

    if (products == NULL || orders == NULL ||
        tinydb_record_scan(table, products, NULL, NULL) != 30u ||
        tinydb_record_scan(table, orders, NULL, NULL) != 20u ||
        !expect_product(table, products, 29u, "product_29", 2900u) ||
        !expect_order(table, orders, 19u, 20u, 5u) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("GENERIC_RECORD_OK products=30 orders=20 reopen=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}
