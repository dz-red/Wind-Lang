#if defined(WIND_GC_PROFILE) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "gc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(p, size) ((void)0)
#define ASAN_UNPOISON_MEMORY_REGION(p, size) ((void)0)
#endif

#ifdef WIND_GC_PROFILE
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static struct {
    uint64_t allocations, allocation_samples, alloc_ns;
    uint64_t collections, released, mark_ns, sweep_ns, destroy_ns;
} profile;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void print_profile(void) {
    fprintf(stderr,
            "WGC_PROFILE {\"allocations\":%" PRIu64 ",\"allocation_samples\":%" PRIu64
            ",\"alloc_sample_ns\":%" PRIu64 ",\"collections\":%" PRIu64
            ",\"released\":%" PRIu64 ",\"mark_ns\":%" PRIu64 ",\"sweep_ns\":%" PRIu64
            ",\"destroy_ns\":%" PRIu64 "}\n",
            profile.allocations, profile.allocation_samples, profile.alloc_ns,
            profile.collections, profile.released, profile.mark_ns, profile.sweep_ns, profile.destroy_ns);
}
#endif

struct WindGcBlock {
    _Alignas(max_align_t) struct WindGcBlock *next;
    struct WindGcBlock *indexed_next;
    WindGcTrace trace;
    bool marked;
    struct WindGcBlock *pending_next; /* стек обхода поверх уже выделенных записей */
    struct WindGcPage *page;
};

enum { SMALL_SIZE = 16, SLOTS_PER_PAGE = 256 };
#define SLOT_STRIDE (sizeof(struct WindGcBlock) + \
    (SMALL_SIZE + _Alignof(max_align_t) - 1) / _Alignof(max_align_t) * _Alignof(max_align_t))

struct WindGcPage {
    _Alignas(max_align_t) struct WindGcPage *next;
    struct WindGcPage **link;
    struct WindGcBlock *free_slot;
    size_t live, used;
};

static void page_add(WindGc *gc, struct WindGcPage *page) {
    page->next = gc->partial_page;
    page->link = &gc->partial_page;
    if (page->next) page->next->link = &page->next;
    gc->partial_page = page;
}

static void page_remove(struct WindGcPage *page) {
    *page->link = page->next;
    if (page->next) page->next->link = page->link;
    page->link = NULL;
}

static struct WindGcBlock *alloc_small(WindGc *gc, size_t size) {
    struct WindGcPage *page = gc->partial_page;
    if (!page) {
        page = gc->empty_page;
        if (page) gc->empty_page = NULL;
        else page = malloc(sizeof *page + SLOT_STRIDE * SLOTS_PER_PAGE);
        if (!page) return NULL;
        *page = (struct WindGcPage){0};
        page_add(gc, page);
    }
    struct WindGcBlock *block = page->free_slot;
    if (block) {
        page->free_slot = block->next;
    } else {
        unsigned char *payload = (unsigned char *)(page + 1);
        block = (struct WindGcBlock *)(payload + SLOT_STRIDE * page->used++);
    }
    if (++page->live == SLOTS_PER_PAGE) page_remove(page);
    block->page = page;
    ASAN_POISON_MEMORY_REGION(block + 1, SMALL_SIZE);
    ASAN_UNPOISON_MEMORY_REGION(block + 1, size);
    (void)size;
    return block;
}

static void free_block(WindGc *gc, struct WindGcBlock *block) {
    struct WindGcPage *page = block->page;
    if (!page) {
        free(block);
        return;
    }
    ASAN_POISON_MEMORY_REGION(block + 1, SMALL_SIZE);
    if (page->live-- == SLOTS_PER_PAGE) page_add(gc, page);
    if (!page->live) {
        page_remove(page);
        if (gc->empty_page) free(page);
        else gc->empty_page = page;
        return;
    }
    block->next = page->free_slot;
    page->free_slot = block;
}

static size_t bucket_of(const void *p, size_t capacity) {
    uintptr_t bits = (uintptr_t)p >> 3;
    bits ^= bits >> 9;
    bits *= UINT32_C(2654435761);
    return (bits ^ (bits >> 16)) & (capacity - 1);
}

static void index_add(WindGc *gc, struct WindGcBlock *block) {
    size_t i = bucket_of(block + 1, gc->capacity);
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

static void *alloc_block(WindGc *gc, size_t size, WindGcTrace trace) {
    if (size == 0 || size > SIZE_MAX - sizeof(struct WindGcBlock) || !grow_index(gc)) return NULL;

    struct WindGcBlock *block;
    if (!trace && size <= SMALL_SIZE) {
        block = alloc_small(gc, size);
        if (!block) return NULL;
    } else {
        block = malloc(sizeof *block + size);
        if (!block) return NULL;
        block->page = NULL;
    }
    block->next = gc->head;
    block->trace = trace;
    block->marked = false;
    block->pending_next = NULL;
    gc->head = block;
    index_add(gc, block);
    gc->count++;
    return block + 1;
}

void *wind_gc_alloc_traced(WindGc *gc, size_t size, WindGcTrace trace) {
#ifdef WIND_GC_PROFILE
    if (!profile.allocations) atexit(print_profile);
    bool sample = (profile.allocations++ % 64) == 0;
    uint64_t start = sample ? now_ns() : 0;
#endif
    void *data = alloc_block(gc, size, trace);
#ifdef WIND_GC_PROFILE
    if (sample) {
        profile.alloc_ns += now_ns() - start;
        profile.allocation_samples++;
    }
#endif
    return data;
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
        if (block + 1 != ref) continue;
        if (block->marked) return;

        block->marked = true;
        if (block->trace) {
            block->pending_next = state->pending;
            state->pending = block;
        }
        return;
    }
}

static size_t finish_collection(MarkState *state, uint64_t start) {
    /* Обход без рекурсии и дополнительных malloc во время сборки. */
    while (state->pending) {
        struct WindGcBlock *block = state->pending;
        state->pending = block->pending_next;
        block->pending_next = NULL;
        block->trace(block + 1, mark, state);
    }

#ifdef WIND_GC_PROFILE
    uint64_t start_sweep = now_ns();
    profile.mark_ns += start_sweep - start;
#else
    (void)start;
#endif
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
            free_block(state->gc, block);
            released++;
        }
    }
    state->gc->count -= released;
    if (!state->gc->count) {
        free(state->gc->index);
        state->gc->index = NULL;
        state->gc->capacity = 0;
    }
#ifdef WIND_GC_PROFILE
    profile.sweep_ns += now_ns() - start_sweep;
    profile.collections++;
    profile.released += released;
#endif
    return released;
}

size_t wind_gc_collect(WindGc *gc, const void *const *roots, size_t count) {
    uint64_t start = 0;
#ifdef WIND_GC_PROFILE
    start = now_ns();
#endif
    MarkState state = {gc, NULL};
    for (size_t i = 0; i < count; i++)
        mark(&state, roots[i]);
    return finish_collection(&state, start);
}

size_t wind_gc_collect_with(WindGc *gc, const void *data, WindGcTrace trace_roots) {
    uint64_t start = 0;
#ifdef WIND_GC_PROFILE
    start = now_ns();
#endif
    MarkState state = {gc, NULL};
    trace_roots(data, mark, &state);
    return finish_collection(&state, start);
}

void wind_gc_destroy(WindGc *gc) {
#ifdef WIND_GC_PROFILE
    uint64_t start = now_ns();
#endif
    while (gc->head) {
        struct WindGcBlock *block = gc->head;
        gc->head = block->next;
        free_block(gc, block);
    }
    free(gc->index);
    free(gc->empty_page);
    *gc = (WindGc){0};
#ifdef WIND_GC_PROFILE
    profile.destroy_ns += now_ns() - start;
#endif
}
