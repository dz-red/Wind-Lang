#include "strings_gc.h"
#include "gc.h"

#include <stdio.h>
#include <stdlib.h>

static WindGc strings;

void *wind_str_alloc(size_t size) {
    void *data = wind_gc_alloc(&strings, size);
    if (!data) {
        fputs("Wind: cannot allocate string\n", stderr);
        exit(EXIT_FAILURE);
    }
    return data;
}

void wind_str_collect(const void *const *roots, size_t count) {
    wind_gc_collect(&strings, roots, count);
}

void wind_str_destroy(void) {
    wind_gc_destroy(&strings);
}
