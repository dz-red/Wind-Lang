#ifndef WIND_MODULES_H
#define WIND_MODULES_H

#include "ast.h"

/* Модули ищутся рядом с base_path. Импортируются только функции и link. */
int wind_load_modules(Program *prog, const WindSource *source, WindDiagnostics *diags);

#endif /* WIND_MODULES_H */
