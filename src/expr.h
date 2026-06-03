/*
 * expr.h — трансляция выражений Wind в C (легаси-пайплайн).
 */

#ifndef WIND_EXPR_H
#define WIND_EXPR_H

#include <stdio.h>
#include "symtab.h"   /* VarType */

/* Транслирует выражение src в C-код, пишет в out. */
void translate_expr(const char *src, FILE *out, int line_num, const char *original);

/* Ищет оператор сравнения в s; возвращает позицию, заполняет op/op_len. */
const char *find_compare_op(const char *s, char *op, int *op_len);

/* Грубо угадывает тип выражения (int/frac/str) для проверок. */
VarType guess_expr_type(const char *src);

#endif /* WIND_EXPR_H */
