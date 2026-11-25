#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>

enum { SIZE = 8192 };
static int values[SIZE];

static void fill(void) {
    int seed = 42;
    for (int i = 0; i < SIZE; i++) {
        seed = (seed * 25173 + 13849) % 65536;
        values[i] = seed;
    }
}

static void insertion_sort(void) {
    for (int i = 1; i < SIZE; i++) {
        const int value = values[i];
        int j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

static int digest(void) {
    int hash = 0;
    for (int i = 0; i < SIZE; i++)
        hash = (hash * 31 + values[i]) % 65536;
    return hash;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    for (int i = 0; i < 2; i++) {
        fill();
        insertion_sort();
    }
    fill();
    const double started = now();
    insertion_sort();
    const double elapsed_ms = (now() - started) * 1000;
    printf("%.6f %d\n", elapsed_ms, digest());
}
