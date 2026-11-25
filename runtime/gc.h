#ifndef WIND_GC_H
#define WIND_GC_H

#include <stddef.h>

struct WindGcBlock;

/* Начальное состояние: WindGc gc = {0};. Не копируй непустую кучу. */
typedef struct {
    struct WindGcBlock *head;
} WindGc;

/* Память принадлежит куче: не передавай её в free или realloc.
   При нулевом размере или нехватке памяти возвращает NULL. */
void *wind_gc_alloc(WindGc *gc, size_t size);

typedef void (*WindGcVisit)(void *context, const void *ref);
typedef void (*WindGcTrace)(const void *data, WindGcVisit visit, void *context);

/* Как alloc, но при сборке trace перечисляет ссылки объекта через visit.
   Инициализируй поля-ссылки до сборки. В trace не меняй кучу и не запускай GC. */
void *wind_gc_alloc_traced(WindGc *gc, size_t size, WindGcTrace trace);

/* Возвращает число освобождённых блоков. roots может быть NULL при count == 0.
   Корни и ссылки должны указывать на начало блока; NULL и чужие блоки игнорируются. */
size_t wind_gc_collect(WindGc *gc, const void *const *roots, size_t count);

/* Освобождает все блоки и оставляет кучу пустой. */
void wind_gc_destroy(WindGc *gc);

#endif
