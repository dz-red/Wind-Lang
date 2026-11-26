#ifndef WIND_ERRORS_H
#define WIND_ERRORS_H

#include <stddef.h>

#define C_RST "\033[0m"
#define C_RED "\033[1;31m"
#define C_GRN "\033[1;32m"
#define C_YEL "\033[1;33m"
#define C_CYN "\033[1;36m"

extern const char *current_file;

typedef enum { DIAG_NORMAL, DIAG_COMPACT, DIAG_LEARN, DIAG_SIMPLE } WindDiagMode;
typedef enum {
    WD_LEX = 1000, WD_TERMINAL = 1001, WD_PARSE = 1002,
    WD_RETURN = 2001, WD_CODEGEN = 2002, WD_MODULE = 3001, WD_IO = 3002, WD_GCC = 3003
} WindDiagCode;

typedef struct {
    int line, col, end_line, end_col;
} WindRange;

typedef struct WindSource {
    char *path, *text;
    struct WindSource *next;
} WindSource;

typedef struct {
    WindDiagMode mode;
    unsigned count;
    WindSource *sources;
} WindDiagnostics;

typedef struct {
    WindDiagCode code;
    const WindSource *source;
    WindRange primary, secondary;
    const char *message, *title, *label, *secondary_label;
    const char *note, *suggestions;
} WindDiagnostic;

WindSource *wind_source_open(WindDiagnostics *diags, const char *path);
void wind_diag_emit(WindDiagnostics *diags, const WindDiagnostic *diag);
void wind_diag_summary(const WindDiagnostics *diags);
void wind_diag_free(WindDiagnostics *diags);
int wind_diag_mode(const char *value, WindDiagMode *mode);
int wind_diag_explain(const char *code);

#endif /* WIND_ERRORS_H */
