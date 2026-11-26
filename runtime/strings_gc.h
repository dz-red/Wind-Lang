#ifndef WIND_STRINGS_GC_H
#define WIND_STRINGS_GC_H

#include <stddef.h>
#include "gc.h"

typedef void (*WindRootTrace)(const volatile void *, size_t, WindGcVisit, void *);

typedef struct WindStrRoot {
    const volatile void *slot;
    size_t count;
    WindRootTrace trace;
    struct WindStrRoot *prev;
} WindStrRoot;

typedef struct WindStrTemps {
    struct WindStrTemps *prev;
    struct WindStrTemp *head;
    void *small[8];
    size_t used;
} WindStrTemps;

typedef struct {
    WindStrRoot *roots;
    WindStrTemps *temps;
} WindStrState;

void wind_str_root_enter(WindStrRoot *root, const volatile void *slot, size_t count, WindRootTrace trace);
void wind_str_root_leave(WindStrRoot *root);
void wind_str_trace_slots(const volatile void *slot, size_t count, WindGcVisit visit, void *context);
void wind_str_temps_enter(WindStrTemps *scope);
void wind_str_temps_leave(WindStrTemps *scope);
void *wind_str_keep(void *value);
void *wind_str_alloc(size_t size);
void *wind_str_alloc_traced(size_t size, WindGcTrace trace);
void *wind_str_grow(const void *old, size_t used, size_t size);
char *wind_str_at(const char *s, int index);
extern int wind_str_needs_collection;
void wind_str_collect(void);
void wind_str_destroy(void);
WindStrState wind_str_snapshot(void);
void wind_str_unwind(WindStrState state);

/* Кодоген GCC: cleanup срабатывает и при return, break и continue. */
#define WIND_ROOT(slot, id, count, trace) \
    WindStrRoot _wind_root_##id __attribute__((cleanup(wind_str_root_leave))); \
    wind_str_root_enter(&_wind_root_##id, (slot), (count), (trace))

#define WIND_STR_ROOT(name, id) WIND_ROOT(&(name), id, 1, wind_str_trace_slots)

/* После закрытия временной области результат сразу принимает C-инструкция. */
#define WIND_STR_EXPR(...) \
    ({ WindStrTemps _wind_temps __attribute__((cleanup(wind_str_temps_leave))); \
       wind_str_temps_enter(&_wind_temps); (__VA_ARGS__); })

/* GNU-блок не перемежается с вычислением соседнего аргумента. */
#define WIND_STR_KEEP(...) \
    ({ __auto_type _wind_value = (__VA_ARGS__); wind_str_keep(_wind_value); _wind_value; })

#define WIND_CHECKPOINT() do { if (wind_str_needs_collection) wind_str_collect(); } while (0)

#endif
