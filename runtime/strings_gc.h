#ifndef WIND_STRINGS_GC_H
#define WIND_STRINGS_GC_H

#include <stddef.h>

void *wind_str_alloc(size_t size);
void wind_str_collect(const void *const *roots, size_t count);
void wind_str_destroy(void);

#endif
