/*
 * astcodegen.h — кодогенерация C из AST (этап 4 миграции).
 *
 * Срез 1: императивное ядро — скаляры int/frac, арифметика/сравнения/логика,
 * функции с рекурсией, if/elif/else, while, repeat, break/continue, return,
 * terminal.paste (числа + строковые литералы), var с выводом типа.
 *
 * Срез 2 (строки): str-переменные, конкатенация (+), интерполяция "{expr}"
 * (внутри скобок — полноценное выражение), сравнение строк (==/!=), касты
 * str/to_str/int/frac. Рантайм: wstr_cat/build/from_int/from_frac/to_int/to_frac.
 *
 * Срез 3a (массивы): int.nums[5] — объявление+zero-init, чтение/запись nums[i],
 * индексация в интерполяции {int.nums[i]}. Тип переменной — VType (скаляр/массив).
 *
 * Срез 3b (списки): int.list.nums [= литерал], индексация get/set через рантайм
 * (_wl_*), методы nums.add(v)/.pop()/.len, len(nums). Рантайм _wl в преамбуле.
 *
 * Срез 3c (словари): dict[K,V].name [= литерал k:v], cfg[key] чтение/запись,
 * .len/.has(k), loop по ключам. Хеш-таблица _wd в преамбуле.
 *
 * Срез 3d (loop..in): итерация по спискам и словарям (for + _wl_get / _wde).
 *
 * НЕ покрыто (следующие срезы): try/catch, http.serve, json/file, импорт.
 *
 * Это ОТДЕЛЬНЫЙ пайплайн от легаси parser.c. Легаси остаётся рабочим
 * компилятором, пока AST-кодоген не дорастёт до паритета.
 */

#ifndef WIND_ASTCODEGEN_H
#define WIND_ASTCODEGEN_H

#include <stdio.h>
#include "ast.h"

/*
 * Генерирует C-код для program в out.
 * Возвращает 1 при успехе, 0 при ошибке (в errbuf — сообщение).
 * Встретив узел вне поддержанного среза — это ошибка с понятным текстом.
 */
int wind_codegen(const Program *program, FILE *out, char *errbuf, int errcap);

#endif /* WIND_ASTCODEGEN_H */
