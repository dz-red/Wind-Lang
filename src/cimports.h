#ifndef WIND_CIMPORTS_H
#define WIND_CIMPORTS_H

#include "lexer.h"
#include "errors.h"

enum { WIND_C_MAX_PARAMS = 16 };

typedef struct WindCHeader WindCHeader;
typedef struct WindCOwnership WindCOwnership;

typedef struct {
    TokenKind result;
    int nargs;
    TokenKind args[WIND_C_MAX_PARAMS];
} WindCCallback;

typedef struct {
    const char *name, *symbol;
    TokenKind result;
    int copy_string;
    int legacy_symbol;
    int nargs;
    TokenKind args[WIND_C_MAX_PARAMS];
    unsigned char buffer_lengths[WIND_C_MAX_PARAMS];
    WindCCallback callbacks[WIND_C_MAX_PARAMS];
    WindRange range;
} WindCFunction;

typedef struct {
    const char *name, *header, *library;
    const char *runtime_header, *runtime_source;
    const WindCFunction *functions;
    int legacy_link;
    const WindSource *source;
    WindRange header_range, library_range;
    int auto_header;
    WindCHeader *parsed_header;
    WindCOwnership *ownership;
} WindCModule;

extern const WindCModule wind_c_modules[];

const WindCModule *wind_c_module(const char *name);
const WindCFunction *wind_c_function(const WindCModule *module, const char *name);
TokenKind wind_c_buffer_element(TokenKind type);
int wind_c_has_callbacks(const WindCModule *module);
const WindCFunction *wind_c_resolve(const WindCModule *module, const char *name,
                                   WindDiagnostics *diags, const WindSource *source, WindRange range);
const WindCModule *wind_c_load(const char *name, const WindSource *entry,
                               const WindSource *source, WindRange range, WindDiagnostics *diags);
void wind_c_free(const WindCModule *module);
void wind_c_emit_header(FILE *out, const WindCModule *module, int import_index);
void wind_c_emit_checks(FILE *out, const WindCModule *module, int import_index);

#endif
