/*
 * util.h — утилиты общего назначения (строки, расстояние Левенштейна,
 * подсказки по ключевым словам).
 */

#ifndef WIND_UTIL_H
#define WIND_UTIL_H

/* Флаг «внутри блочного комментария» — общий для лексинга строк. */
extern int in_comment;

void strip_comments(char *line);
char *ltrim(char *s);
void rtrim(char *s);
int  levenshtein(const char *a, const char *b);
const char *suggest_keyword(const char *word);
int  func_name_conflicts_with_file(const char *func_name);

#endif /* WIND_UTIL_H */
