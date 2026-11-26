#ifndef WIND_COLLECTIONS_GC_H
#define WIND_COLLECTIONS_GC_H

#include "gc.h"
#include <stdint.h>

typedef struct { void *data; int len, cap, esz, strings; } _wl;
typedef struct _wde { struct _wde *next; uint64_t key, val; } _wde;
typedef struct { _wde **b; int nb, count, ks, vs; } _wd;

_wl *_wl_new(int esz, int strings);
void _wl_push_int(_wl *l, int v);
void _wl_push_frac(_wl *l, double v);
void _wl_push_str(_wl *l, char *v);
int _wl_get_int(_wl *l, int i);
double _wl_get_frac(_wl *l, int i);
char *_wl_get_str(_wl *l, int i);
void _wl_set_int(_wl *l, int i, int v);
void _wl_set_frac(_wl *l, int i, double v);
void _wl_set_str(_wl *l, int i, char *v);
int _wl_len(const _wl *l);
void _wl_pop(_wl *l);
void _wl_trace_slots(const volatile void *slot, size_t count, WindGcVisit visit, void *context);

_wd *_wd_new(int string_keys, int string_values);
uint64_t _wd_packf(double value);
double _wd_unpackf(uint64_t value);
void _wd_set(_wd *d, uint64_t key, uint64_t value);
uint64_t _wd_get(_wd *d, uint64_t key);
int _wd_has(_wd *d, uint64_t key);
int _wd_len(const _wd *d);
void _wd_trace_slots(const volatile void *slot, size_t count, WindGcVisit visit, void *context);
char *wstr_dup(const char *s);

#endif
