#include "../runtime/gc.h"

#include <assert.h>
#include <string.h>

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

int main(void) {
    test_heap();
    test_collect();
    test_long_chain();
    return 0;
}
