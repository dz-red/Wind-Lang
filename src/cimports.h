#ifndef WIND_CIMPORTS_H
#define WIND_CIMPORTS_H

#include "lexer.h"

typedef struct {
    const char *name, *symbol;
    TokenKind result;
    int copy_string;
} WindCFunction;

typedef struct {
    const char *name, *header, *library;
    const WindCFunction *functions;
    int legacy_link;
} WindCModule;

extern const WindCModule wind_c_modules[];

const WindCModule *wind_c_module(const char *name);
const WindCFunction *wind_c_function(const WindCModule *module, const char *name);

#endif
