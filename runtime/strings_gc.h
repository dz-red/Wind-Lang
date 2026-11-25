#ifndef WIND_STRINGS_GC_H
#define WIND_STRINGS_GC_H

#include <stddef.h>

typedef struct WindStrRoot {
    char *const *slot;
    struct WindStrRoot *prev;
} WindStrRoot;

typedef struct WindStrTemps {
    struct WindStrTemps *prev;
    struct WindStrTemp *head;
} WindStrTemps;

void wind_str_root_enter(WindStrRoot *root, char *const *slot);
void wind_str_root_leave(WindStrRoot *root);
void wind_str_temps_enter(WindStrTemps *scope);
void wind_str_temps_leave(WindStrTemps *scope);
char *wind_str_keep(char *value);
void *wind_str_alloc(size_t size);
void wind_str_collect(void);
void wind_str_destroy(void);

/* Кодоген GCC: cleanup срабатывает и при return, break и continue. */
#define WIND_STR_ROOT(name, id) \
    WindStrRoot _wind_root_##id __attribute__((cleanup(wind_str_root_leave))); \
    wind_str_root_enter(&_wind_root_##id, &(name))

/* После закрытия временной области результат сразу принимает C-инструкция. */
#define WIND_STR_EXPR(...) \
    ({ WindStrTemps _wind_temps __attribute__((cleanup(wind_str_temps_leave))); \
       wind_str_temps_enter(&_wind_temps); (__VA_ARGS__); })

/* GNU-блок не перемежается с вычислением соседнего аргумента. */
#define WIND_STR_KEEP(...) \
    ({ char *_wind_value = (__VA_ARGS__); wind_str_keep(_wind_value); })

#endif
