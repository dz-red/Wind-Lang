#include "../runtime/gc.h"

#include <assert.h>
#include <string.h>

int main(void) {
    WindGc gc = {0};
    WindGc other = {0};

    wind_gc_destroy(&gc);
    void *empty = wind_gc_alloc(&gc, 0);
    assert(empty == NULL);
    assert(gc.head == NULL);

    int *number = wind_gc_alloc(&gc, sizeof *number);
    assert(number != NULL);
    *number = 42;

    char *word = wind_gc_alloc(&gc, sizeof "Wind");
    assert(word != NULL);
    memcpy(word, "Wind", sizeof "Wind");
    assert(*number == 42);
    assert(strcmp(word, "Wind") == 0);

    int *independent = wind_gc_alloc(&other, sizeof *independent);
    assert(independent != NULL);
    *independent = 7;

    wind_gc_destroy(&gc);
    assert(gc.head == NULL);
    assert(*independent == 7);
    wind_gc_destroy(&gc);

    void *after_destroy = wind_gc_alloc(&gc, 1);
    assert(after_destroy != NULL);
    wind_gc_destroy(&gc);
    wind_gc_destroy(&other);
    return 0;
}
