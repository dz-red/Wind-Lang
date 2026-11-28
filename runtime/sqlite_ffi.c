#include "sqlite_ffi.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FREE_SLOT, DB_SLOT, STMT_SLOT } Kind;
typedef enum { READY, ROW, DONE } State;

typedef struct {
    void *ptr;
    uint32_t generation, next;
    Kind kind;
    State state;
} Slot;

static Slot *slots;
static uint32_t used, free_slot;
static size_t capacity;

static _Noreturn void fail(const char *operation, const char *message) {
    fprintf(stderr, "sqlite.%s: %s\n", operation, message);
    exit(EXIT_FAILURE);
}

static void dispose(void) {
    for (uint32_t i = 0; i < used; i++)
        if (slots[i].kind == STMT_SLOT) sqlite3_finalize(slots[i].ptr);
    for (uint32_t i = 0; i < used; i++)
        if (slots[i].kind == DB_SLOT) sqlite3_close(slots[i].ptr);
    free(slots);
}

static void reserve_slot(const char *operation) {
    if (free_slot || used < capacity) return;
    if (capacity >= UINT32_MAX / 2 || capacity > SIZE_MAX / sizeof *slots / 2)
        fail(operation, "handle table is full");
    if (!capacity && atexit(dispose) != 0) fail(operation, "cannot register cleanup");
    size_t next = capacity ? capacity * 2 : 16;
    Slot *grown = realloc(slots, next * sizeof *slots);
    if (!grown) fail(operation, "cannot allocate a handle");
    slots = grown;
    capacity = next;
}

static uint64_t new_slot(void *ptr, Kind kind) {
    uint32_t index = free_slot ? free_slot : ++used;
    Slot *slot = &slots[index - 1];
    uint32_t generation = 1;
    if (free_slot) {
        free_slot = slot->next;
        generation = slot->generation + 1;
    }
    *slot = (Slot){.ptr = ptr, .kind = kind, .generation = generation, .state = READY};
    return ((uint64_t)generation << 32) | index;
}

static Slot *lookup(uint64_t id, Kind kind, const char *operation) {
    uint32_t index = (uint32_t)id;
    if (index && index <= used) {
        Slot *slot = &slots[index - 1];
        if (slot->kind == kind && slot->generation == (uint32_t)(id >> 32)) return slot;
    }
    fail(operation, kind == DB_SLOT ? "connection is closed or invalid" : "statement is finalized or invalid");
}

static void release(uint64_t id, Slot *slot) {
    slot->ptr = NULL;
    slot->kind = FREE_SLOT;
    if (slot->generation == UINT32_MAX) return;
    slot->next = free_slot;
    free_slot = (uint32_t)id;
}

WindSqliteDb wind_sqlite_open(const char *path) {
    reserve_slot("open");
    sqlite3 *db = NULL;
    int result = sqlite3_open(path, &db);
    if (result != SQLITE_OK) {
        fprintf(stderr, "sqlite.open: %s\n", db ? sqlite3_errmsg(db) : sqlite3_errstr(result));
        if (db) sqlite3_close(db);
        exit(EXIT_FAILURE);
    }
    return (WindSqliteDb){new_slot(db, DB_SLOT)};
}

void wind_sqlite_close(WindSqliteDb db) {
    Slot *slot = lookup(db.id, DB_SLOT, "close");
    int result = sqlite3_close(slot->ptr);
    if (result == SQLITE_BUSY) fail("close", "finalize all statements before closing the connection");
    if (result != SQLITE_OK) fail("close", sqlite3_errstr(result));
    release(db.id, slot);
}

void wind_sqlite_exec(WindSqliteDb db, const char *sql) {
    sqlite3 *handle = lookup(db.id, DB_SLOT, "exec")->ptr;
    if (sqlite3_exec(handle, sql, NULL, NULL, NULL) != SQLITE_OK) fail("exec", sqlite3_errmsg(handle));
}

WindSqliteStmt wind_sqlite_prepare(WindSqliteDb db, const char *sql) {
    sqlite3 *handle = lookup(db.id, DB_SLOT, "prepare")->ptr;
    reserve_slot("prepare");
    sqlite3_stmt *stmt = NULL;
    const char *tail;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, &tail) != SQLITE_OK)
        fail("prepare", sqlite3_errmsg(handle));
    if (!stmt) fail("prepare", "expected a SQL statement");
    while (isspace((unsigned char)*tail)) tail++;
    if (*tail) {
        sqlite3_finalize(stmt);
        fail("prepare", "expected a single SQL statement");
    }
    return (WindSqliteStmt){new_slot(stmt, STMT_SLOT)};
}

void wind_sqlite_finalize(WindSqliteStmt stmt) {
    Slot *slot = lookup(stmt.id, STMT_SLOT, "finalize");
    int result = sqlite3_finalize(slot->ptr);
    release(stmt.id, slot);
    if (result != SQLITE_OK) fail("finalize", sqlite3_errstr(result));
}

void wind_sqlite_reset(WindSqliteStmt stmt) {
    Slot *slot = lookup(stmt.id, STMT_SLOT, "reset");
    int result = sqlite3_reset(slot->ptr);
    if (result != SQLITE_OK) fail("reset", sqlite3_errstr(result));
    slot->state = READY;
}

static sqlite3_stmt *bindable(WindSqliteStmt stmt, const char *operation) {
    Slot *slot = lookup(stmt.id, STMT_SLOT, operation);
    if (slot->state != READY) fail(operation, "reset the statement before binding again");
    return slot->ptr;
}

void wind_sqlite_bind_int(WindSqliteStmt stmt, int index, int value) {
    sqlite3_stmt *handle = bindable(stmt, "bind_int");
    int result = sqlite3_bind_int(handle, index, value);
    if (result != SQLITE_OK) fail("bind_int", sqlite3_errstr(result));
}

void wind_sqlite_bind_text(WindSqliteStmt stmt, int index, const char *value) {
    sqlite3_stmt *handle = bindable(stmt, "bind_text");
    int result = sqlite3_bind_text(handle, index, value, -1, SQLITE_TRANSIENT);
    if (result != SQLITE_OK) fail("bind_text", sqlite3_errstr(result));
}

int wind_sqlite_step(WindSqliteStmt stmt) {
    Slot *slot = lookup(stmt.id, STMT_SLOT, "step");
    if (slot->state == DONE) return 0;
    sqlite3_stmt *handle = slot->ptr;
    int result = sqlite3_step(handle);
    if (result == SQLITE_ROW) { slot->state = ROW; return 1; }
    if (result == SQLITE_DONE) { slot->state = DONE; return 0; }
    fail("step", sqlite3_errmsg(sqlite3_db_handle(handle)));
}

static sqlite3_stmt *get_column(WindSqliteStmt stmt, int column, const char *operation) {
    Slot *slot = lookup(stmt.id, STMT_SLOT, operation);
    if (slot->state != ROW) fail(operation, "no current row; call step first");
    sqlite3_stmt *handle = slot->ptr;
    if (column < 0 || column >= sqlite3_column_count(handle)) fail(operation, "column index out of range");
    return handle;
}

int wind_sqlite_column_int(WindSqliteStmt stmt, int column) {
    sqlite3_stmt *handle = get_column(stmt, column, "column_int");
    if (sqlite3_column_type(handle, column) != SQLITE_INTEGER)
        fail("column_int", "expected a SQL integer");
    sqlite3_int64 value = sqlite3_column_int64(handle, column);
    if (value < INT32_MIN || value > INT32_MAX) fail("column_int", "integer does not fit in Wind int");
    return (int)value;
}

const char *wind_sqlite_column_text(WindSqliteStmt stmt, int column) {
    sqlite3_stmt *handle = get_column(stmt, column, "column_text");
    if (sqlite3_column_type(handle, column) != SQLITE_TEXT)
        fail("column_text", "expected SQL text; use is_null to check for NULL");
    const unsigned char *text = sqlite3_column_text(handle, column);
    if (!text) fail("column_text", "cannot read text from SQLite");
    if ((size_t)sqlite3_column_bytes(handle, column) != strlen((const char *)text))
        fail("column_text", "text contains an embedded NUL");
    return (const char *)text;
}

int wind_sqlite_is_null(WindSqliteStmt stmt, int column) {
    sqlite3_stmt *handle = get_column(stmt, column, "is_null");
    return sqlite3_column_type(handle, column) == SQLITE_NULL;
}
