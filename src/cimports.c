#include <string.h>

#include "cimports.h"
#include "../bindings/sqlite.inc"

const WindCModule wind_c_modules[] = {
    {.name = "sqlite", .header = "sqlite3.h", .library = "sqlite3",
     .functions = sqlite_functions, .legacy_link = 1},
    {0}
};

const WindCModule *wind_c_module(const char *name) {
    for (const WindCModule *m = wind_c_modules; m->name; m++)
        if (!strcmp(m->name, name)) return m;
    return NULL;
}

const WindCFunction *wind_c_function(const WindCModule *module, const char *name) {
    for (const WindCFunction *f = module->functions; f->name; f++)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}
