#include "gc.h"

#include <stdbool.h>
#include <stdlib.h>

/* Запись списка и память, которую получает вызывающий, — разные блоки. */
struct WindGcBlock {
    void *data;
    struct WindGcBlock *next;
    WindGcTrace trace;
    bool marked;
    struct WindGcBlock *pending_next; /* стек обхода поверх уже выделенных записей */
};

void *wind_gc_alloc(WindGc *gc, size_t size) {
    return wind_gc_alloc_traced(gc, size, NULL);
}

void *wind_gc_alloc_traced(WindGc *gc, size_t size, WindGcTrace trace) {
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
    block->trace = trace;
    block->marked = false;
    block->pending_next = NULL;
    gc->head = block;
    return block->data;
}

typedef struct {
    WindGc *gc;
    struct WindGcBlock *pending;
} MarkState;

static void mark(void *context, const void *ref) {
    MarkState *state = context;
    if (!ref) return;

    for (struct WindGcBlock *block = state->gc->head; block; block = block->next) {
        if (block->data != ref) continue;
        if (block->marked) return; /* повторный корень или цикл */

        block->marked = true;
        if (block->trace) {
            block->pending_next = state->pending;
            state->pending = block;
        }
        return;
    }
}

static void free_block(struct WindGcBlock *block) {
    free(block->data);
    free(block);
}

size_t wind_gc_collect(WindGc *gc, const void *const *roots, size_t count) {
    MarkState state = {gc, NULL};

    for (size_t i = 0; i < count; i++)
        mark(&state, roots[i]);

    /* Обход без рекурсии и дополнительных malloc во время сборки. */
    while (state.pending) {
        struct WindGcBlock *block = state.pending;
        state.pending = block->pending_next;
        block->pending_next = NULL;
        block->trace(block->data, mark, &state);
    }

    size_t released = 0;
    /* link указывает на саму ссылку на текущий блок: head или поле next. */
    struct WindGcBlock **link = &gc->head;
    while (*link) {
        struct WindGcBlock *block = *link;
        if (block->marked) {
            block->marked = false; /* следующая сборка начнёт с чистых меток */
            link = &block->next;
        } else {
            *link = block->next;
            free_block(block);
            released++;
        }
    }
    return released;
}

void wind_gc_destroy(WindGc *gc) {
    while (gc->head) {
        struct WindGcBlock *block = gc->head;
        gc->head = block->next;
        free_block(block);
    }
}
