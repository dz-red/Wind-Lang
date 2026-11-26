#include "gc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct WindGcBlock {
    _Alignas(max_align_t) void *data;
    struct WindGcBlock *next;
    struct WindGcBlock *indexed_next;
    WindGcTrace trace;
    bool marked;
    struct WindGcBlock *pending_next; /* стек обхода поверх уже выделенных записей */
};

static size_t bucket_of(const void *p, size_t capacity) {
    uintptr_t bits = (uintptr_t)p >> 3;
    bits ^= bits >> 9;
    bits *= UINT32_C(2654435761);
    return (bits ^ (bits >> 16)) & (capacity - 1);
}

static void index_add(WindGc *gc, struct WindGcBlock *block) {
    size_t i = bucket_of(block->data, gc->capacity);
    block->indexed_next = gc->index[i];
    gc->index[i] = block;
}

static bool grow_index(WindGc *gc) {
    if (gc->capacity && gc->count < gc->capacity / 4 * 3) return true;
    if (gc->capacity > SIZE_MAX / 2 / sizeof *gc->index) return false;
    size_t capacity = gc->capacity ? gc->capacity * 2 : 64;
    struct WindGcBlock **index = calloc(capacity, sizeof *index);
    if (!index) return false;
    free(gc->index);
    gc->index = index;
    gc->capacity = capacity;
    for (struct WindGcBlock *block = gc->head; block; block = block->next)
        index_add(gc, block);
    return true;
}

void *wind_gc_alloc(WindGc *gc, size_t size) {
    return wind_gc_alloc_traced(gc, size, NULL);
}

void *wind_gc_alloc_traced(WindGc *gc, size_t size, WindGcTrace trace) {
    if (size == 0 || size > SIZE_MAX - sizeof(struct WindGcBlock) || !grow_index(gc)) return NULL;

    struct WindGcBlock *block = malloc(sizeof *block + size);
    if (!block) return NULL;
    block->data = block + 1;
    block->next = gc->head;
    block->trace = trace;
    block->marked = false;
    block->pending_next = NULL;
    gc->head = block;
    index_add(gc, block);
    gc->count++;
    return block->data;
}

typedef struct {
    WindGc *gc;
    struct WindGcBlock *pending;
} MarkState;

static void mark(void *context, const void *ref) {
    MarkState *state = context;
    if (!ref || !state->gc->capacity) return;

    size_t i = bucket_of(ref, state->gc->capacity);
    for (struct WindGcBlock *block = state->gc->index[i]; block; block = block->indexed_next) {
        if (block->data != ref) continue;
        if (block->marked) return;

        block->marked = true;
        if (block->trace) {
            block->pending_next = state->pending;
            state->pending = block;
        }
        return;
    }
}

static void free_block(struct WindGcBlock *block) {
    free(block);
}

static size_t finish_collection(MarkState *state) {
    /* Обход без рекурсии и дополнительных malloc во время сборки. */
    while (state->pending) {
        struct WindGcBlock *block = state->pending;
        state->pending = block->pending_next;
        block->pending_next = NULL;
        block->trace(block->data, mark, state);
    }

    size_t released = 0;
    if (state->gc->capacity)
        memset(state->gc->index, 0, state->gc->capacity * sizeof *state->gc->index);
    /* link указывает на саму ссылку на текущий блок: head или поле next. */
    struct WindGcBlock **link = &state->gc->head;
    while (*link) {
        struct WindGcBlock *block = *link;
        if (block->marked) {
            block->marked = false;
            index_add(state->gc, block);
            link = &block->next;
        } else {
            *link = block->next;
            free_block(block);
            released++;
        }
    }
    state->gc->count -= released;
    if (!state->gc->count) {
        free(state->gc->index);
        state->gc->index = NULL;
        state->gc->capacity = 0;
    }
    return released;
}

size_t wind_gc_collect(WindGc *gc, const void *const *roots, size_t count) {
    MarkState state = {gc, NULL};
    for (size_t i = 0; i < count; i++)
        mark(&state, roots[i]);
    return finish_collection(&state);
}

size_t wind_gc_collect_with(WindGc *gc, const void *data, WindGcTrace trace_roots) {
    MarkState state = {gc, NULL};
    trace_roots(data, mark, &state);
    return finish_collection(&state);
}

void wind_gc_destroy(WindGc *gc) {
    while (gc->head) {
        struct WindGcBlock *block = gc->head;
        gc->head = block->next;
        free_block(block);
    }
    free(gc->index);
    *gc = (WindGc){0};
}
