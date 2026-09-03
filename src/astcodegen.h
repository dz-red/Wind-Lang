#ifndef WIND_ASTCODEGEN_H
#define WIND_ASTCODEGEN_H

#include <stdio.h>
#include "ast.h"
int wind_codegen(const Program *program, FILE *out, char *errbuf, int errcap);

#endif /* WIND_ASTCODEGEN_H */
