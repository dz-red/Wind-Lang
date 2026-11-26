#include "collections_gc.h"
#include "strings_gc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trace_list(const void *data, WindGcVisit visit, void *context) {
    const _wl *l = data;
    visit(context, l->data);
    if (l->strings) wind_str_trace_slots(l->data, (size_t)l->len, visit, context);
}

void _wl_trace_slots(const volatile void *slot, size_t count, WindGcVisit visit, void *context) {
    _wl *const volatile *lists = slot;
    for (size_t i = 0; i < count; i++) visit(context, lists[i]);
}

_wl *_wl_new(int esz, int strings) {
    _wl *l = wind_str_alloc_traced(sizeof *l, trace_list);
    *l = (_wl){.esz = esz, .strings = strings};
    return l;
}

static void grow_list(_wl *l) {
    if (l->len < l->cap) return;
    if (l->cap > INT_MAX / 2 || (size_t)l->esz > SIZE_MAX / (size_t)(l->cap ? l->cap * 2 : 4)) {
        fputs("Wind: list is too large\n", stderr);
        exit(EXIT_FAILURE);
    }
    int cap = l->cap ? l->cap * 2 : 4;
    void *data = wind_str_alloc((size_t)cap * l->esz);
    if (l->len) memcpy(data, l->data, (size_t)l->len * l->esz);
    l->data = data;
    l->cap = cap;
}

static void check_index(const _wl *l, int i) {
    if (i >= 0 && i < l->len) return;
    fprintf(stderr, "list index %d out of range (len=%d)\n", i, l->len);
    exit(EXIT_FAILURE);
}

#define LIST_ACCESSORS(suffix, type) \
    void _wl_push_##suffix(_wl *l, type v) { \
        grow_list(l); \
        ((type *)l->data)[l->len++] = v; \
    } \
    type _wl_get_##suffix(_wl *l, int i) { \
        check_index(l, i); \
        return ((type *)l->data)[i]; \
    } \
    void _wl_set_##suffix(_wl *l, int i, type v) { \
        check_index(l, i); \
        ((type *)l->data)[i] = v; \
    }

LIST_ACCESSORS(int, int)
LIST_ACCESSORS(frac, double)
LIST_ACCESSORS(str, char *)

int _wl_len(const _wl *l) { return l->len; }
void _wl_pop(_wl *l) { if (l->len) l->len--; }

static void trace_dict(const void *data, WindGcVisit visit, void *context) {
    const _wd *d = data;
    visit(context, d->b);
    for (int i = 0; i < d->nb; i++)
        for (_wde *e = d->b[i]; e; e = e->next) {
            visit(context, e);
            if (d->ks) visit(context, (void *)(uintptr_t)e->key);
            if (d->vs) visit(context, (void *)(uintptr_t)e->val);
        }
}

void _wd_trace_slots(const volatile void *slot, size_t count, WindGcVisit visit, void *context) {
    _wd *const volatile *dicts = slot;
    for (size_t i = 0; i < count; i++) visit(context, dicts[i]);
}

_wd *_wd_new(int string_keys, int string_values) {
    _wd *d = wind_str_alloc_traced(sizeof *d, trace_dict);
    *d = (_wd){.nb = 16, .ks = string_keys, .vs = string_values};
    d->b = wind_str_alloc((size_t)d->nb * sizeof *d->b);
    for (int i = 0; i < d->nb; i++) d->b[i] = NULL;
    return d;
}

uint64_t _wd_packf(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

double _wd_unpackf(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint64_t dict_hash(const _wd *d, uint64_t key) {
    if (!d->ks) return key * 1099511628211ULL + 1234567ULL;
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *s = (const unsigned char *)(uintptr_t)key; *s; s++)
        h = (h ^ *s) * 1099511628211ULL;
    return h;
}

static _wde **find_entry(_wd *d, uint64_t key) {
    _wde **entry = &d->b[dict_hash(d, key) % (uint64_t)d->nb];
    while (*entry) {
        uint64_t old = (*entry)->key;
        if (d->ks ? strcmp((const char *)(uintptr_t)key, (const char *)(uintptr_t)old) == 0 : key == old)
            break;
        entry = &(*entry)->next;
    }
    return entry;
}

void _wd_set(_wd *d, uint64_t key, uint64_t value) {
    _wde **entry = find_entry(d, key);
    if (*entry) {
        (*entry)->val = value;
        return;
    }
    _wde *item = wind_str_alloc(sizeof *item);
    size_t bucket = dict_hash(d, key) % (uint64_t)d->nb;
    *item = (_wde){.next = d->b[bucket], .key = key, .val = value};
    d->b[bucket] = item;
    d->count++;
}

uint64_t _wd_get(_wd *d, uint64_t key) {
    _wde *entry = *find_entry(d, key);
    if (entry) return entry->val;
    fputs("dict: key not found\n", stderr);
    exit(EXIT_FAILURE);
}

int _wd_has(_wd *d, uint64_t key) { return *find_entry(d, key) != NULL; }
int _wd_len(const _wd *d) { return d->count; }

char *wstr_dup(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = wind_str_alloc(size);
    memcpy(copy, s, size);
    return copy;
}
