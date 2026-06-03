/*
 * errors.h — единая точка вывода ошибок компиляции Wind.
 *
 * current_file — имя текущего .wnd (для сообщений и проверки имён функций).
 * error_count  — сколько ошибок уже выведено (ненулевое => не компилируем дальше).
 */

#ifndef WIND_ERRORS_H
#define WIND_ERRORS_H

/* ANSI-цвета для сообщений компилятора. */
#define C_RST "\033[0m"
#define C_RED "\033[1;31m"
#define C_GRN "\033[1;32m"
#define C_YEL "\033[1;33m"
#define C_CYN "\033[1;36m"

extern const char *current_file;
extern int error_count;

/* Выводит ошибку: line_num — строка в .wnd, original — текст строки-источника,
 * msg — суть, hint — подсказка (может быть NULL). Инкрементит error_count. */
void wind_error(int line_num, const char *original, const char *msg, const char *hint);

#endif /* WIND_ERRORS_H */
