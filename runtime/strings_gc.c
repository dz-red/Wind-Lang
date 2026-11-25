#include "strings_gc.h"
#include "gc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static WindGc strings;
static WindStrRoot *roots;
static WindStrTemps *temps;

struct WindStrTemp {
    char *value;
    struct WindStrTemp *next;
};

void wind_str_root_enter(WindStrRoot *root, char *const *slot) {
    root->slot = slot;
    root->prev = roots;
    roots = root;
}

void wind_str_root_leave(WindStrRoot *root) {
    assert(roots == root);
    roots = root->prev;
}

void wind_str_temps_enter(WindStrTemps *scope) {
    scope->prev = temps;
    scope->head = NULL;
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

char *wind_str_keep(char *value) {
    if (!temps || !value) return value;
    struct WindStrTemp *temp = malloc(sizeof *temp);
    if (!temp) {
        fputs("Wind: cannot protect temporary string\n", stderr);
        exit(EXIT_FAILURE);
    }
    temp->value = value;
    temp->next = temps->head;
    temps->head = temp;
    return value;
}

void *wind_str_alloc(size_t size) {
    void *data = wind_gc_alloc(&strings, size);
    if (!data) {
        fputs("Wind: cannot allocate string\n", stderr);
        exit(EXIT_FAILURE);
    }
    return wind_str_keep(data);
}

static void trace_roots(const void *data, WindGcVisit visit, void *context) {
    (void)data;
    for (WindStrRoot *root = roots; root; root = root->prev)
        visit(context, *root->slot);
    for (WindStrTemps *scope = temps; scope; scope = scope->prev)
        for (struct WindStrTemp *temp = scope->head; temp; temp = temp->next)
            visit(context, temp->value);
}

void wind_str_collect(void) {
    if (strings.head) wind_gc_collect_with(&strings, NULL, trace_roots);
}

void wind_str_destroy(void) {
    wind_gc_destroy(&strings);
}
