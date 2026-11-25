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

/* Освобождает все блоки и оставляет кучу пустой. */
void wind_gc_destroy(WindGc *gc);

#endif
