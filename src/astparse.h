#ifndef WIND_ASTPARSE_H
#define WIND_ASTPARSE_H

#include "ast.h"
#include "lexer.h"
int wind_parse(Token *toks, int count, Program *out,
               WindDiagnostics *diags, const WindSource *source);

#endif /* WIND_ASTPARSE_H */
