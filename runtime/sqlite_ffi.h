#ifndef WIND_SQLITE_FFI_H
#define WIND_SQLITE_FFI_H

#include <stdint.h>

typedef struct { uint64_t id; } WindSqliteDb;
typedef struct { uint64_t id; } WindSqliteStmt;

WindSqliteDb wind_sqlite_open(const char *path);
void wind_sqlite_close(WindSqliteDb db);
void wind_sqlite_exec(WindSqliteDb db, const char *sql);
WindSqliteStmt wind_sqlite_prepare(WindSqliteDb db, const char *sql);
void wind_sqlite_finalize(WindSqliteStmt stmt);
void wind_sqlite_reset(WindSqliteStmt stmt);
void wind_sqlite_bind_int(WindSqliteStmt stmt, int index, int value);
void wind_sqlite_bind_text(WindSqliteStmt stmt, int index, const char *value);
int wind_sqlite_step(WindSqliteStmt stmt);
int wind_sqlite_column_int(WindSqliteStmt stmt, int column);
const char *wind_sqlite_column_text(WindSqliteStmt stmt, int column);
int wind_sqlite_is_null(WindSqliteStmt stmt, int column);

#endif
