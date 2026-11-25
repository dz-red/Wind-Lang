#include "gc.h"

#include <stdlib.h>

/* Запись списка и память, которую получает вызывающий, — разные блоки. */
struct WindGcBlock {
    void *data;
    struct WindGcBlock *next;
};

void *wind_gc_alloc(WindGc *gc, size_t size) {
    if (size == 0) return NULL;

    struct WindGcBlock *block = malloc(sizeof *block);
    if (!block) return NULL;

    block->data = malloc(size);
    if (!block->data) {
        free(block);
        return NULL;
    }

    /* Новая запись становится головой; старая остаётся следующей. */
    block->next = gc->head;
    gc->head = block;
    return block->data;
}

void wind_gc_destroy(WindGc *gc) {
    while (gc->head) {
        struct WindGcBlock *block = gc->head;
        gc->head = block->next;
        free(block->data);
        free(block);
    }
}
