#include "../runtime/gc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#endif

static void test_heap(void) {
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
}

typedef struct Node {
    struct Node *child;
    const int *leaf;
} Node;

static void trace_node(const void *data, WindGcVisit visit, void *context) {
    const Node *node = data;
    visit(context, node->child);
    visit(context, node->leaf);
}

static Node *new_node(WindGc *gc) {
    Node *node = wind_gc_alloc_traced(gc, sizeof *node, trace_node);
    assert(node != NULL);
    *node = (Node){0};
    return node;
}

static void test_collect(void) {
    WindGc gc = {0};
    void *dead_tail = wind_gc_alloc(&gc, 1);
    Node *a = new_node(&gc);
    void *dead_middle = wind_gc_alloc(&gc, 1);
    Node *b = new_node(&gc);
    int *leaf = wind_gc_alloc(&gc, sizeof *leaf);
    void *dead_head = wind_gc_alloc(&gc, 1);
    assert(dead_head && dead_middle && dead_tail && leaf);

    a->child = b;
    b->child = a;
    a->leaf = leaf;
    *leaf = 42;
    int foreign = 0;
    const void *roots[] = {a, a, NULL, &foreign};

    size_t released = wind_gc_collect(&gc, roots, 4);
    assert(released == 3);
    assert(a->child == b && b->child == a && *a->leaf == 42);

    a->leaf = NULL;
    released = wind_gc_collect(&gc, roots, 1);
    assert(released == 1);
    assert(a->child == b && b->child == a);

    released = wind_gc_collect(&gc, NULL, 0);
    assert(released == 2);
    assert(gc.head == NULL);
    released = wind_gc_collect(&gc, NULL, 0);
    assert(released == 0);
    wind_gc_destroy(&gc);
}

static void test_long_chain(void) {
    WindGc gc = {0};
    Node *head = NULL;
    const size_t length = 2048;
    for (size_t i = 0; i < length; i++) {
        Node *node = new_node(&gc);
        node->child = head;
        head = node;
    }

    const void *roots[] = {head};
    size_t released = wind_gc_collect(&gc, roots, 1);
    assert(released == 0);
    size_t visited = 0;
    for (const Node *node = head; node; node = node->child)
        ++visited;
    assert(visited == length);

    released = wind_gc_collect(&gc, NULL, 0);
    assert(released == length);
    wind_gc_destroy(&gc);
}

static void test_small_pages(void) {
    enum { N = 1024 };
    WindGc gc = {0};
    const void *roots[N / 2];
    for (size_t i = 0; i < N; i++) {
        size_t *value = wind_gc_alloc(&gc, sizeof *value);
        assert(value != NULL);
        assert((uintptr_t)value % _Alignof(max_align_t) == 0);
        *value = i;
        if (i % 2 == 0) roots[i / 2] = value;
    }
    size_t freed = wind_gc_collect(&gc, roots, N / 2);
    assert(freed == N / 2);
    for (size_t i = 0; i < N / 2; i++) {
        char *reused = wind_gc_alloc(&gc, 13);
        assert(reused != NULL);
        memset(reused, 'x', 13);
        assert(*(const size_t *)roots[i] == i * 2);
#ifdef __SANITIZE_ADDRESS__
        assert(__asan_address_is_poisoned(reused + 13));
#endif
    }
    freed = wind_gc_collect(&gc, NULL, 0);
    assert(freed == N && gc.count == 0 && gc.head == NULL);
    void *again = wind_gc_alloc(&gc, 4);
    assert(again != NULL);
    wind_gc_destroy(&gc);
}

int main(void) {
    test_heap();
    test_collect();
    test_long_chain();
    test_small_pages();
    return 0;
}
