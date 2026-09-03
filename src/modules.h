#ifndef WIND_MODULES_H
#define WIND_MODULES_H

#include "ast.h"

/*
 * Проход загрузки модулей. Работает МЕЖДУ парсером и кодогеном.
 *
 * Берёт уже разобранную Program, смотрит на её imports[], для каждого
 * находит рядом файл <модуль>.wnd, парсит его тем же wind_parse и
 * вливает его функции в prog->body.
 *
 * Из модуля берутся ТОЛЬКО функции и директивы link. Код верхнего уровня
 * (вызовы, объявления переменных) игнорируется — иначе импорт исполнял бы
 * чужую программу.
 *
 * base_path — путь к главному .wnd; модули ищутся в его каталоге.
 * Возвращает 1 при успехе, 0 при ошибке (текст в errbuf).
 */
int wind_load_modules(Program *prog, const char *base_path,
                      char *errbuf, int errcap);

#endif /* WIND_MODULES_H */
