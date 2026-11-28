#ifndef WIND_CHEADER_H
#define WIND_CHEADER_H

#include "cimports.h"

typedef enum {
    C_HEADER_OK, C_HEADER_UNAVAILABLE, C_HEADER_NOT_FOUND, C_HEADER_INVALID,
    C_HEADER_NO_FUNCTION, C_HEADER_UNSUPPORTED
} WindCHeaderStatus;

WindCHeaderStatus wind_cheader_open(const char *header, WindCHeader **out, char *detail, size_t cap);
WindCHeaderStatus wind_cheader_function(WindCHeader *header, const char *name, WindCFunction *out);
void wind_cheader_free(WindCHeader *header);

#endif
