#ifndef WIND_CIMPORTS_H
#define WIND_CIMPORTS_H

#include "lexer.h"
#include "errors.h"

enum { WIND_C_MAX_PARAMS = 16 };

typedef struct {
    const char *name, *symbol;
    TokenKind result;
    int copy_string;
    int nargs;
    TokenKind args[WIND_C_MAX_PARAMS];
    WindRange range;
} WindCFunction;

typedef struct {
    const char *name, *header, *library;
    const WindCFunction *functions;
    int legacy_link;
    const WindSource *source;
    WindRange header_range, library_range;
} WindCModule;

extern const WindCModule wind_c_modules[];

const WindCModule *wind_c_module(const char *name);
const WindCFunction *wind_c_function(const WindCModule *module, const char *name);
const WindCModule *wind_c_load(const char *name, const WindSource *entry,
                               const WindSource *source, WindRange range, WindDiagnostics *diags);
void wind_c_free(const WindCModule *module);
void wind_c_emit_checks(FILE *out, const WindCModule *module, int import_index);

#endif
