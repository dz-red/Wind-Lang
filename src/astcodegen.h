/*
 * astcodegen.h — генерация C из AST.
 *
 * Обходит дерево и печатает готовый C-файл: объявления, рантайм-преамбулу
 * и тело main. Дальше его забирает gcc.
 *
 * Поддержано: скаляры int/frac/str/bool, арифметика и логика, интерполяция
 * строк, функции с рекурсией, global, if/elif/else, while, repeat, for,
 * loop..in, break/continue/return, массивы, списки, словари, terminal.paste,
 * http.serve, рантайм json и time, работа с файлами, link внешних библиотек.
 *
 * НЕ поддержано: try/catch и throw (узлы ST_TRY/ST_THROW парсер строит,
 * но кодоген их не обрабатывает), импорт модулей.
 *
 * Рантайм печатается в преамбулу: _wstr (строки), _wl (списки), _wd
 * (словари, хеш-таблица), _wjb (буфер json), сокеты для http.serve.
 */

#ifndef WIND_ASTCODEGEN_H
#define WIND_ASTCODEGEN_H

#include <stdio.h>
#include "ast.h"

/*
 * Генерирует C-код для program в out.
 * Возвращает 1 при успехе, 0 при ошибке (в errbuf — сообщение).
 * Неподдержанный узел — это ошибка с понятным текстом.
 */
int wind_codegen(const Program *program, FILE *out, char *errbuf, int errcap);

#endif /* WIND_ASTCODEGEN_H */
