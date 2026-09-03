#ifndef WIND_ASTPARSE_H
#define WIND_ASTPARSE_H

#include "ast.h"
#include "lexer.h"
int wind_parse(Token *toks, int count, Program *out,
               char *errbuf, int errcap, int *out_line, int *out_col);

#endif /* WIND_ASTPARSE_H */
