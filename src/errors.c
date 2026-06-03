/*
 * errors.c — вывод ошибок компиляции с цветом, позицией и подсказкой.
 */

#include <stdio.h>
#include "errors.h"

const char *current_file = "<stdin>";
int error_count = 0;

void wind_error(int line_num, const char *original, const char *msg, const char *hint) {
    error_count++;
    fprintf(stderr, "\033[1;31m[ошибка]\033[0m %s:%d: %s\n",
            current_file ? current_file : "?", line_num, msg);
    if (original && *original) {
        /* пропускаем ведущие пробелы для аккуратного вывода строки-источника */
        const char *o = original;
        while (*o == ' ' || *o == '\t') o++;
        fprintf(stderr, "    \033[2m%s\033[0m\n", o);
    }
    if (hint && *hint)
        fprintf(stderr, "    \033[1;36mподсказка:\033[0m %s\n", hint);
}
