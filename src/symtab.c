/*
 * symtab.c — реализация таблицы символов.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "symtab.h"
#include "errors.h"

#define MAX_VARS 256
#define MAX_FUNCS 64

Context current_ctx = CTX_MAIN;

typedef struct {
    char name[64];
    VarType type;
    int is_array;
    int array_size;
    int is_list;
    int is_dict;
    VarType key_type;
} VarEntry;

/* Два независимых скоупа — main и текущая функция.
 * static значит "наружу не торчат". Их видит только этот .c файл. */
static VarEntry main_vars[MAX_VARS];
static int main_vars_count = 0;
static VarEntry func_vars[MAX_VARS];
static int func_vars_count = 0;


void declare_list(const char *name, VarType elem_type);
int is_list_var(const char *name);
/* Таблица функций — глобальная, одна на программу. */
static FuncSig funcs_table[MAX_FUNCS];
static int funcs_count = 0;

static VarEntry *lookup_entry(const char *name) {
    VarEntry *table = (current_ctx == CTX_FUNC) ? func_vars : main_vars;
    int count = (current_ctx == CTX_FUNC) ? func_vars_count : main_vars_count;
    for (int i = 0; i < count; i++) {
        if (strcmp(table[i].name, name) == 0) return &table[i];
    }
    return NULL;
}

const char *vt_name(VarType t) {
    switch (t) {
        case VT_INT:  return "int";
        case VT_FRAC: return "frac";
        case VT_STR:  return "str";
        default:      return "?";
    }
}

VarType lookup_var(const char *name) {
    VarEntry *e = lookup_entry(name);
    return e ? e->type : VT_NONE;
}

void declare_var(const char *name, VarType type) {
    VarEntry *table = (current_ctx == CTX_FUNC) ? func_vars : main_vars;
    int *count    = (current_ctx == CTX_FUNC) ? &func_vars_count : &main_vars_count;
    if (*count >= MAX_VARS) return;
    strncpy(table[*count].name, name, 63);
    table[*count].name[63] = '\0';
    table[*count].type = type;
    table[*count].is_array = 0;
    table[*count].array_size = 0;
    table[*count].is_list = 0;
    table[*count].is_dict = 0;
    table[*count].key_type = VT_NONE;
    (*count)++;
}

void declare_array(const char *name, VarType type, int size) {
    VarEntry *table = (current_ctx == CTX_FUNC) ? func_vars : main_vars;
    int *count    = (current_ctx == CTX_FUNC) ? &func_vars_count : &main_vars_count;
    if (*count >= MAX_VARS) return;
    strncpy(table[*count].name, name, 63);
    table[*count].name[63] = '\0';
    table[*count].type = type;
    table[*count].is_array = 1;
    table[*count].array_size = size;
    table[*count].is_list = 0;
    table[*count].is_dict = 0;
    table[*count].key_type = VT_NONE;
    (*count)++;
}

void declare_list(const char *name, VarType elem_type) {
    VarEntry *table = (current_ctx == CTX_FUNC) ? func_vars : main_vars;
    int *count    = (current_ctx == CTX_FUNC) ? &func_vars_count : &main_vars_count;
    if (*count >= MAX_VARS) return;
    strncpy(table[*count].name, name, 63);
    table[*count].name[63] = '\0';
    table[*count].type = elem_type;
    table[*count].is_array = 0;
    table[*count].array_size = 0;
    table[*count].is_list = 1;
    table[*count].is_dict = 0;
    table[*count].key_type = VT_NONE;
    (*count)++;
}

void declare_dict(const char *name, VarType key_type, VarType val_type) {
    VarEntry *table = (current_ctx == CTX_FUNC) ? func_vars : main_vars;
    int *count    = (current_ctx == CTX_FUNC) ? &func_vars_count : &main_vars_count;
    if (*count >= MAX_VARS) return;
    strncpy(table[*count].name, name, 63);
    table[*count].name[63] = '\0';
    table[*count].type = val_type;
    table[*count].is_array = 0;
    table[*count].array_size = 0;
    table[*count].is_list = 0;
    table[*count].is_dict = 1;
    table[*count].key_type = key_type;
    (*count)++;
}

int lookup_array_size(const char *name) {
    VarEntry *e = lookup_entry(name);
    if (!e || !e->is_array) return 0;
    return e->array_size;
}

int is_list_var(const char *name) {
    VarEntry *e = lookup_entry(name);
    if (!e) return 0;
    return e->is_list;
}

int is_dict_var(const char *name) {
    VarEntry *e = lookup_entry(name);
    if (!e) return 0;
    return e->is_dict;
}

VarType dict_key_type(const char *name) {
    VarEntry *e = lookup_entry(name);
    if (!e || !e->is_dict) return VT_NONE;
    return e->key_type;
}

VarType dict_val_type(const char *name) {
    VarEntry *e = lookup_entry(name);
    if (!e || !e->is_dict) return VT_NONE;
    return e->type;
}

void symtab_reset_func_scope(void) {
    func_vars_count = 0;
}

void declare_func(const FuncSig *sig) {
    if (funcs_count >= MAX_FUNCS) return;
    funcs_table[funcs_count] = *sig;
    /* Если c_name не задан — копируем из name (нормальный случай, не импорт). */
    if (funcs_table[funcs_count].c_name[0] == '\0') {
        strncpy(funcs_table[funcs_count].c_name, sig->name,
                sizeof(funcs_table[funcs_count].c_name) - 1);
        funcs_table[funcs_count].c_name[sizeof(funcs_table[funcs_count].c_name) - 1] = '\0';
    }
    funcs_count++;
}

const FuncSig *lookup_func(const char *name) {
    for (int i = 0; i < funcs_count; i++) {
        if (strcmp(funcs_table[i].name, name) == 0) return &funcs_table[i];
    }
    return NULL;
}

int funcs_count_get(void) { return funcs_count; }
FuncSig *func_at(int idx) {
    if (idx < 0 || idx >= funcs_count) return NULL;
    return &funcs_table[idx];
}
void funcs_count_set(int n) { if (n >= 0 && n <= MAX_FUNCS) funcs_count = n; }

int is_valid_var_name(const char *name) {
    if (!name[0]) return 0;
    if (!isalpha((unsigned char)name[0]) && name[0] != '_') return 0;
    for (const char *p = name + 1; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    }
    return 1;
}

int is_c_reserved(const char *name) {
    static const char *kw[] = {
        /* Ключевые слова Си + потенциально опасные имена runtime */
        "auto", "break", "case", "char", "const", "continue", "default",
        "do", "double", "else", "enum", "extern", "float", "for", "goto",
        "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
        "union", "unsigned", "void", "volatile", "while",
        "_Bool", "_Complex", "_Imaginary",
        /* main — нельзя как имя пользовательской функции, иначе конфликт с main() */
        "main",
        /* Системные функции часто используемые в Wind runtime */
        "printf", "scanf", "fprintf", "malloc", "free", "exit",
        NULL
    };
    for (int i = 0; kw[i]; i++) {
        if (strcmp(kw[i], name) == 0) return 1;
    }
    return 0;
}

void check_var_ref(const char *name, VarType expected,
                   int line_num, const char *original) {
    if (!is_valid_var_name(name)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "invalid variable name: '%s'", name);
        wind_error(line_num, original, msg,
            "name: letters, digits, '_' only; must start with a letter or '_'");
        return;
    }
    VarType actual = lookup_var(name);
    if (actual == VT_NONE) {
        char msg[160];
        snprintf(msg, sizeof(msg), "variable '%s' is not declared", name);
        wind_error(line_num, original, msg,
            "declare it first: int.name = ... / frac.name = ... / str.name = ...");
        return;
    }
    if (expected != VT_NONE && actual != expected) {
        char msg[200];
        snprintf(msg, sizeof(msg),
            "variable '%s' has type '%s' but is used as '%s'",
            name, vt_name(actual), vt_name(expected));
        wind_error(line_num, original, msg,
            "check the type or the prefix (int. / frac. / str.)");
    }
}
