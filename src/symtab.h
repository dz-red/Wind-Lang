/*
 * symtab.h — таблица символов компилятора Wind: типы переменных, скоупы
 * (main/функция), сигнатуры функций, валидация имён.
 */

#ifndef WIND_SYMTAB_H
#define WIND_SYMTAB_H

#define WIND_MAX_PARAMS 32

/* Типы значений Wind. VT_NONE — «не объявлена/не найдена». */
typedef enum { VT_NONE, VT_INT, VT_FRAC, VT_STR } VarType;

/* Текущий скоуп объявлений. */
typedef enum { CTX_MAIN, CTX_FUNC } Context;
extern Context current_ctx;

/* Сигнатура пользовательской функции. */
typedef struct {
    char    name[64];                  /* имя в Wind                    */
    char    c_name[64];                /* имя в C (для импортов/конфликтов) */
    VarType return_type;
    int     param_count;
    char    param_names[WIND_MAX_PARAMS][64];
    VarType param_types[WIND_MAX_PARAMS];
} FuncSig;

/* --- переменные --- */
const char *vt_name(VarType t);
VarType lookup_var(const char *name);
void declare_var(const char *name, VarType type);
void declare_array(const char *name, VarType type, int size);
void declare_list(const char *name, VarType elem_type);
void declare_dict(const char *name, VarType key_type, VarType val_type);
int  lookup_array_size(const char *name);
int  is_list_var(const char *name);
int  is_dict_var(const char *name);
VarType dict_key_type(const char *name);
VarType dict_val_type(const char *name);
void symtab_reset_func_scope(void);

/* --- функции --- */
void declare_func(const FuncSig *sig);
const FuncSig *lookup_func(const char *name);
int  funcs_count_get(void);
FuncSig *func_at(int idx);
void funcs_count_set(int n);

/* --- валидация --- */
int  is_valid_var_name(const char *name);
int  is_c_reserved(const char *name);
void check_var_ref(const char *name, VarType expected, int line_num, const char *original);

#endif /* WIND_SYMTAB_H */
