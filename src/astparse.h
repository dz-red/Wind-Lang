/*
 * astparse.h — рекурсивный парсер Wind: массив Token'ов (lexer.c) → AST (ast.h).
 * Готовое дерево можно посмотреть: `wind --ast file.wnd`.
 */

#ifndef WIND_ASTPARSE_H
#define WIND_ASTPARSE_H

#include "ast.h"
#include "lexer.h"

/*
 * Парсит toks[0..count) в *out.
 *   Успех:  возвращает 1, *out заполнен (вызывающий чистит ast_free_program).
 *   Ошибка: возвращает 0, errbuf содержит сообщение, *out_line/*out_col —
 *           позиция. *out не используется (внутренние частичные узлы
 *           утекают — процесс компилятора всё равно завершится).
 */
int wind_parse(Token *toks, int count, Program *out,
               char *errbuf, int errcap, int *out_line, int *out_col);

#endif /* WIND_ASTPARSE_H */
