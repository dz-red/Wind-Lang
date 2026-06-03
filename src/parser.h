/*
 * parser.h — построчный транслятор Wind → C (легаси-пайплайн).
 *
 * parser_init   — задаёт буферы вывода (декларации / функции / main).
 * translate_line — обрабатывает одну строку исходника.
 * parser_check_eof — финальная проверка незакрытых блоков.
 */

#ifndef WIND_PARSER_H
#define WIND_PARSER_H

#include <stdio.h>

extern int wind_import_mode;   /* режим разбора импортируемого модуля */

const char *wind_import_ns_get(void);
void parser_init(FILE *decls, FILE *funcs, FILE *main_buf);
void translate_line(char *p, const char *original, int line_num);
void parser_check_eof(int line_num);

#endif /* WIND_PARSER_H */
