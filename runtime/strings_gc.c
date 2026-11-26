#include "strings_gc.h"
#include "gc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static WindGc strings;
static WindStrRoot *roots;
static WindStrTemps *temps;
static size_t new_blocks;
static size_t next_gc = 32;
static int stress = -1;
int wind_str_needs_collection;

struct WindStrTemp {
    void *value;
    struct WindStrTemp *next;
};

void wind_str_root_enter(WindStrRoot *root, const volatile void *slot, size_t count, WindRootTrace trace) {
    root->slot = slot;
    root->count = count;
    root->trace = trace;
    root->prev = roots;
    roots = root;
}

void wind_str_trace_slots(const volatile void *slot, size_t count, WindGcVisit visit, void *context) {
    char *const volatile *strings = slot;
    for (size_t i = 0; i < count; i++) visit(context, strings[i]);
}

void wind_str_root_leave(WindStrRoot *root) {
    assert(roots == root);
    roots = root->prev;
}

void wind_str_temps_enter(WindStrTemps *scope) {
    scope->prev = temps;
    scope->head = NULL;
    scope->used = 0;
    temps = scope;
}

void wind_str_temps_leave(WindStrTemps *scope) {
    assert(temps == scope);
    while (scope->head) {
        struct WindStrTemp *temp = scope->head;
        scope->head = temp->next;
        free(temp); /* только запись о корне, не сама строка */
    }
    temps = scope->prev;
}

void *wind_str_keep(void *value) {
    if (!temps || !value) return value;
    if (temps->used < sizeof temps->small / sizeof *temps->small) {
        temps->small[temps->used++] = value;
        return value;
    }
    struct WindStrTemp *temp = malloc(sizeof *temp);
    if (!temp) {
        fputs("Wind: cannot protect temporary value\n", stderr);
        exit(EXIT_FAILURE);
    }
    temp->value = value;
    temp->next = temps->head;
    temps->head = temp;
    return value;
}

void *wind_str_alloc_traced(size_t size, WindGcTrace trace) {
    void *data = trace ? wind_gc_alloc_traced(&strings, size, trace) : wind_gc_alloc(&strings, size);
    if (!data) {
        fputs("Wind: cannot allocate managed memory\n", stderr);
        exit(EXIT_FAILURE);
    }
    new_blocks++;
    if (stress < 0) stress = getenv("WGC_STRESS") != NULL;
    if (stress || new_blocks >= next_gc) wind_str_needs_collection = 1;
    return wind_str_keep(data);
}

void *wind_str_alloc(size_t size) {
    return wind_str_alloc_traced(size, NULL);
}

void *wind_str_grow(const void *old, size_t used, size_t size) {
    assert(used <= size);
    void *data = wind_str_alloc(size);
    if (old && used) memcpy(data, old, used);
    return data;
}

char *wind_str_at(const char *s, int index) {
    if (!s || index < 0 || (size_t)index >= strlen(s)) {
        fprintf(stderr, "str index %d out of range\n", index);
        exit(EXIT_FAILURE);
    }
    char *part = wind_str_alloc(2);
    part[0] = s[index];
    part[1] = '\0';
    return part;
}

static void trace_roots(const void *data, WindGcVisit visit, void *context) {
    (void)data;
    for (WindStrRoot *root = roots; root; root = root->prev)
        root->trace(root->slot, root->count, visit, context);
    for (WindStrTemps *scope = temps; scope; scope = scope->prev) {
        for (size_t i = 0; i < scope->used; i++) visit(context, scope->small[i]);
        for (struct WindStrTemp *temp = scope->head; temp; temp = temp->next)
            visit(context, temp->value);
    }
}

void wind_str_collect(void) {
    if (!strings.head) return;
    wind_gc_collect_with(&strings, NULL, trace_roots);
    new_blocks = 0;
    next_gc = 32 + strings.count / 2;
    wind_str_needs_collection = stress > 0 && strings.head != NULL;
}

void wind_str_destroy(void) {
    wind_gc_destroy(&strings);
    new_blocks = 0;
    next_gc = 32;
    wind_str_needs_collection = 0;
}

WindStrState wind_str_snapshot(void) {
    return (WindStrState){roots, temps};
}

void wind_str_unwind(WindStrState state) {
    while (temps != state.temps) wind_str_temps_leave(temps);
    roots = state.roots;
}
