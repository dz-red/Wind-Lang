/*
 * parser.c — главный парсер строк Wind.
 *
 * Внутреннее состояние — приватное (static): счётчики открытых блоков,
 * файловые буферы для main/функций/предобъявлений, имя текущей функции.
 *
 * Наружу торчат: parser_init(), translate_line(), parser_check_eof().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "parser.h"
#include "errors.h"
#include "util.h"
#include "symtab.h"
#include "expr.h"

#define MAX_EXPR 1024
#define STR_BUF_SIZE 256

/* Приватное состояние парсера. */
static int open_if_blocks = 0;
static int open_repeat_blocks = 0;
static int open_while_blocks = 0;
static FILE *decls_out = NULL;
static FILE *funcs_out = NULL;
static FILE *main_out  = NULL;
static char current_func_name[64] = "";
/* Тип возврата текущей открытой функции. VT_NONE = void. */
static VarType current_func_return = VT_NONE;
static int current_func_is_void = 1;

/* === Import state ===
 * При обработке `st.import` мы рекурсивно гоним соседний .wnd через тот же
 * translate_line. Чтобы функции из импортируемого файла не пересекались с
 * текущими, мы:
 *   1) задаём префикс mangling'а для C-имён (math__square)
 *   2) после импорта переименовываем temp-функции в "math.square" (если
 *      whole-namespace) либо оставляем под коротким именем (если selective).
 */
int wind_import_mode = 0;
static char import_ns_name[64] = "";
const char *wind_import_ns_get(void) { return import_ns_name; }
#define import_mode wind_import_mode
/* Список уже импортированных namespace'ов — чтобы не импортировать дважды. */
static char already_imported[32][64];
static int already_imported_n = 0;

static int is_namespace_already_imported(const char *ns) {
    for (int i = 0; i < already_imported_n; i++) {
        if (strcmp(already_imported[i], ns) == 0) return 1;
    }
    return 0;
}
static void mark_namespace_imported(const char *ns) {
    if (already_imported_n >= 32) return;
    snprintf(already_imported[already_imported_n],
             sizeof(already_imported[already_imported_n]), "%s", ns);
    already_imported_n++;
}

/* Парсит "int.a", "frac.b", "str.c" → возвращает тип и кладёт чистое имя в name.
 * Возвращает VT_NONE если префикс невалиден. */
static VarType parse_typed_name(const char *src, char *name, size_t name_sz) {
    VarType t = VT_NONE;
    int plen = 0;
    if (strncmp(src, "int.", 4) == 0)       { t = VT_INT;  plen = 4; }
    else if (strncmp(src, "frac.", 5) == 0) { t = VT_FRAC; plen = 5; }
    else if (strncmp(src, "str.", 4) == 0)  { t = VT_STR;  plen = 4; }
    if (t == VT_NONE) return VT_NONE;
    const char *q = src + plen;
    size_t i = 0;
    while (*q && (isalnum((unsigned char)*q) || *q == '_') && i < name_sz - 1) {
        name[i++] = *q++;
    }
    name[i] = '\0';
    return name[0] ? t : VT_NONE;
}

/* Имя C-типа для генерации заголовков и объявлений. */
static const char *c_type_for(VarType t) {
    switch (t) {
        case VT_INT:  return "int";
        case VT_FRAC: return "double";
        case VT_STR:  return "char *";
        default:      return "void";
    }
}

static FILE *cur_out(void) {
    return current_ctx == CTX_FUNC ? funcs_out : main_out;
}

/* Эмитит ОДНУ простую проверку "LHS OP RHS" из строки chunk.
 * Использует find_compare_op и translate_expr.
 * Если хотя бы одна сторона — строка, оборачивает в strcmp(...) op 0. */
static void emit_single_compare(const char *chunk, int line_num, const char *original) {
    char buf[MAX_EXPR];
    strncpy(buf, chunk, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char op[4];
    int op_len = 0;
    const char *op_pos = find_compare_op(buf, op, &op_len);
    if (!op_pos) {
        wind_error(line_num, original,
            "no comparison operator in condition piece",
            "available: >=  <=  ==  !=  >  <");
        fprintf(cur_out(), "0");
        return;
    }
    char lhs[MAX_EXPR], rhs[MAX_EXPR];
    size_t lhs_len = (size_t)(op_pos - buf);
    if (lhs_len >= sizeof(lhs)) lhs_len = sizeof(lhs) - 1;
    memcpy(lhs, buf, lhs_len);
    lhs[lhs_len] = '\0';
    rtrim(lhs);
    strncpy(rhs, op_pos + op_len, sizeof(rhs) - 1);
    rhs[sizeof(rhs) - 1] = '\0';
    char *rhs_trim = ltrim(rhs);
    rtrim(rhs_trim);

    if (lhs[0] == '\0' || rhs_trim[0] == '\0') {
        wind_error(line_num, original, "one side of the condition is empty", NULL);
        fprintf(cur_out(), "0");
        return;
    }

    VarType lt = guess_expr_type(lhs);
    VarType rt = guess_expr_type(rhs_trim);
    int string_compare = (lt == VT_STR || rt == VT_STR);

    if (string_compare) {
        if (strcmp(op, "==") != 0 && strcmp(op, "!=") != 0) {
            wind_error(line_num, original,
                "strings can only be compared with '==' and '!='", NULL);
            fprintf(cur_out(), "0");
            return;
        }
        fprintf(cur_out(), "strcmp(");
        translate_expr(lhs, cur_out(), line_num, original);
        fprintf(cur_out(), ", ");
        translate_expr(rhs_trim, cur_out(), line_num, original);
        fprintf(cur_out(), ") %s 0", op);
    } else {
        translate_expr(lhs, cur_out(), line_num, original);
        fprintf(cur_out(), " %s ", op);
        translate_expr(rhs_trim, cur_out(), line_num, original);
    }
}

/* Эмитит составное условие. Разбивает по && и || (вне кавычек),
 * каждый кусок пропускает через emit_single_compare. C сам разрулит приоритеты:
 * && связывает крепче чем || (стандарт во всех Си-подобных языках). */
static void emit_compound_condition(const char *expr,
                                    int line_num, const char *original) {
    const char *p = expr;
    const char *chunk_start = p;
    int in_q = 0;

    while (1) {
        int is_end = (*p == '\0');
        int is_and = (!in_q && p[0] == '&' && p[1] == '&');
        int is_or  = (!in_q && p[0] == '|' && p[1] == '|');

        if (*p == '"') in_q = !in_q;

        if (is_end || is_and || is_or) {
            /* Извлекаем кусок [chunk_start .. p), триммим, эмитим как сравнение. */
            char chunk[MAX_EXPR];
            size_t len = (size_t)(p - chunk_start);
            if (len >= sizeof(chunk)) len = sizeof(chunk) - 1;
            memcpy(chunk, chunk_start, len);
            chunk[len] = '\0';
            char *trimmed = ltrim(chunk);
            rtrim(trimmed);

            if (trimmed[0]) {
                fprintf(cur_out(), "(");
                emit_single_compare(trimmed, line_num, original);
                fprintf(cur_out(), ")");
            } else if (!is_end) {
                wind_error(line_num, original,
                    "empty operand around '&&' or '||'", NULL);
                fprintf(cur_out(), "0");
            }

            if (is_end) break;

            fprintf(cur_out(), is_and ? " && " : " || ");
            p += 2;
            chunk_start = p;
            continue;
        }
        p++;
    }
}

void parser_init(FILE *decls, FILE *funcs, FILE *main_buf) {
    decls_out = decls;
    funcs_out = funcs;
    main_out  = main_buf;
}

void translate_line(char *p, const char *original, int line_num) {
    if (*p == '\0') return;

    /* В import-режиме на верхнем уровне (вне функции) разрешаем только
     * wnd.func / end wnd.func / комментарии — модуль не должен иметь top-level
     * исполняемого кода. Всё остальное тихо скипаем. */
    if (import_mode && current_ctx == CTX_MAIN) {
        if (strncmp(p, "wnd.func", 8) != 0 && strncmp(p, "end wnd.func", 12) != 0
                && strncmp(p, "st.import", 9) != 0) {
            return;
        }
    }

    /* === wnd.func NAME(params) -> ret ===
     * Универсальная сигнатура. Возврат всегда явный — даже void.
     * Примеры:
     *   wnd.func greet() -> void
     *   wnd.func max(int.a, int.b) -> int
     *   wnd.func avg(frac.x, frac.y) -> frac */
    if (strncmp(p, "wnd.func ", 9) == 0) {
        if (current_ctx == CTX_FUNC) {
            wind_error(line_num, original,
                "functions cannot be declared inside another function",
                "close the current one with 'end wnd.func' first");
            return;
        }

        const char *cur = p + 9;
        while (*cur && isspace((unsigned char)*cur)) cur++;

        /* Имя — до '(' или пробела. */
        char name[64];
        size_t ni = 0;
        while (*cur && (isalnum((unsigned char)*cur) || *cur == '_')
                && ni < sizeof(name) - 1) {
            name[ni++] = *cur++;
        }
        name[ni] = '\0';
        if (!name[0] || !is_valid_var_name(name)) {
            wind_error(line_num, original, "invalid function name",
                "example: wnd.func max(int.a, int.b) -> int");
            return;
        }
        if (is_c_reserved(name)) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                "function name '%s' is a reserved C keyword", name);
            wind_error(line_num, original, msg,
                "pick a different name — reserved: int, long, void, return, if, for, while, etc.");
            return;
        }
        if (func_name_conflicts_with_file(name)) {
            wind_error(line_num, original,
                "function name conflicts with the file name",
                "rename the function — it cannot have the same name as the .wnd file");
            return;
        }
        if (lookup_func(name)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "function '%s' is already declared", name);
            wind_error(line_num, original, msg, NULL);
            return;
        }

        while (*cur && isspace((unsigned char)*cur)) cur++;
        if (*cur != '(') {
            wind_error(line_num, original,
                "function signature must include '(...)'",
                "example: wnd.func greet() -> void");
            return;
        }
        cur++;
        const char *params_end = strchr(cur, ')');
        if (!params_end) {
            wind_error(line_num, original,
                "missing ')' in function signature", NULL);
            return;
        }

        /* Разбор параметров через запятую. */
        FuncSig sig;
        memset(&sig, 0, sizeof(sig));
        strncpy(sig.name, name, sizeof(sig.name) - 1);

        char params_buf[256];
        size_t plen = (size_t)(params_end - cur);
        if (plen >= sizeof(params_buf)) plen = sizeof(params_buf) - 1;
        memcpy(params_buf, cur, plen);
        params_buf[plen] = '\0';

        char *pp = ltrim(params_buf);
        rtrim(pp);
        if (*pp != '\0') {
            char *tok = pp;
            while (tok && *tok) {
                char *comma = strchr(tok, ',');
                if (comma) *comma = '\0';
                char *one = ltrim(tok);
                rtrim(one);
                if (*one == '\0') {
                    wind_error(line_num, original,
                        "empty parameter in signature", NULL);
                    return;
                }
                if (sig.param_count >= WIND_MAX_PARAMS) {
                    wind_error(line_num, original,
                        "too many parameters (max 8)", NULL);
                    return;
                }
                char pname[64];
                VarType pt = parse_typed_name(one, pname, sizeof(pname));
                if (pt == VT_NONE) {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                        "bad parameter '%s' — expected int.name / frac.name / str.name", one);
                    wind_error(line_num, original, msg, NULL);
                    return;
                }
                if (is_c_reserved(pname)) {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                        "parameter name '%s' is a reserved C keyword", pname);
                    wind_error(line_num, original, msg,
                        "pick a different name");
                    return;
                }
                sig.param_types[sig.param_count] = pt;
                snprintf(sig.param_names[sig.param_count],
                         sizeof(sig.param_names[sig.param_count]), "%s", pname);
                sig.param_count++;
                tok = comma ? comma + 1 : NULL;
            }
        }

        /* После ')' ищем '->' и тип возврата. */
        const char *after = params_end + 1;
        while (*after && isspace((unsigned char)*after)) after++;
        if (after[0] != '-' || after[1] != '>') {
            wind_error(line_num, original,
                "function signature must end with '-> TYPE'",
                "use '-> void' if the function returns nothing");
            return;
        }
        after += 2;
        while (*after && isspace((unsigned char)*after)) after++;
        if (strncmp(after, "void", 4) == 0)      sig.return_type = VT_NONE;
        else if (strncmp(after, "int", 3) == 0)  sig.return_type = VT_INT;
        else if (strncmp(after, "frac", 4) == 0) sig.return_type = VT_FRAC;
        else if (strncmp(after, "str", 3) == 0) sig.return_type = VT_STR;
        else {
            wind_error(line_num, original,
                "unknown return type — use int / frac / void", NULL);
            return;
        }

        /* Импортированная функция регистрируется СРАЗУ под "ns.X" с c_name "ns__X".
         * Так повторный импорт того же файла не плодит дубликатов. */
        if (import_mode) {
            snprintf(sig.name, sizeof(sig.name), "%s.%s", import_ns_name, name);
            snprintf(sig.c_name, sizeof(sig.c_name), "%s__%s", import_ns_name, name);
        } else {
            strncpy(sig.c_name, name, sizeof(sig.c_name) - 1);
            sig.c_name[sizeof(sig.c_name) - 1] = '\0';
        }

        /* Регистрируем функцию и переключаем контекст. */
        declare_func(&sig);
        current_ctx = CTX_FUNC;
        strncpy(current_func_name, name, sizeof(current_func_name) - 1);
        current_func_name[sizeof(current_func_name) - 1] = '\0';
        current_func_return = sig.return_type;
        current_func_is_void = (sig.return_type == VT_NONE);
        symtab_reset_func_scope();

        /* Параметры — в скоуп функции. */
        for (int k = 0; k < sig.param_count; k++) {
            declare_var(sig.param_names[k], sig.param_types[k]);
        }

        /* Генерация прототипа и заголовка тела — используем mangled c_name. */
        const char *ret_c = c_type_for(sig.return_type);
        fprintf(decls_out, "%s %s(", ret_c, sig.c_name);
        fprintf(funcs_out, "%s %s(", ret_c, sig.c_name);
        if (sig.param_count == 0) {
            fprintf(decls_out, "void");
            fprintf(funcs_out, "void");
        } else {
            for (int k = 0; k < sig.param_count; k++) {
                if (k) { fprintf(decls_out, ", "); fprintf(funcs_out, ", "); }
                const char *pc = c_type_for(sig.param_types[k]);
                fprintf(decls_out, "%s %s", pc, sig.param_names[k]);
                fprintf(funcs_out, "%s %s", pc, sig.param_names[k]);
            }
        }
        fprintf(decls_out, ");\n");
        fprintf(funcs_out, ") {\n");
        return;
    }

    /* === end wnd.func === */
    if (strncmp(p, "end wnd.func", 12) == 0) {
        if (current_ctx != CTX_FUNC) {
            wind_error(line_num, original,
                "'end wnd.func' without an open function", NULL);
            return;
        }
        if (open_if_blocks > 0) {
            wind_error(line_num, original,
                "an if-block is still open inside the function",
                "add 'end if' before 'end wnd.func'");
            return;
        }
        if (open_repeat_blocks > 0) {
            wind_error(line_num, original,
                "a repeat-block is still open inside the function",
                "add 'end repeat' before 'end wnd.func'");
            return;
        }
        if (open_while_blocks > 0) {
            wind_error(line_num, original,
                "a while-block is still open inside the function",
                "add 'end while' before 'end wnd.func'");
            return;
        }
        fprintf(funcs_out, "}\n\n");
        current_ctx = CTX_MAIN;
        current_func_name[0] = '\0';
        current_func_return = VT_NONE;
        current_func_is_void = 1;
        return;
    }

    /* === return <expr>? ===
     * Только внутри функции. Голый 'return' допустим только для void.
     * Для не-void требуется выражение. */
    if (strncmp(p, "return", 6) == 0 && (p[6] == '\0' || isspace((unsigned char)p[6]))) {
        if (current_ctx != CTX_FUNC) {
            wind_error(line_num, original,
                "'return' outside of a function",
                "return can only appear between 'wnd.func' and 'end wnd.func'");
            return;
        }
        char *rhs = ltrim(p + 6);
        rtrim(rhs);
        if (*rhs == '\0') {
            if (!current_func_is_void) {
                wind_error(line_num, original,
                    "non-void function must return a value",
                    "example: return int.x");
                return;
            }
            fprintf(cur_out(), "    return;\n");
            return;
        }
        if (current_func_is_void) {
            wind_error(line_num, original,
                "void function cannot return a value",
                "either remove the value or change return type from 'void'");
            return;
        }
        if (current_func_return == VT_STR) {
            /* Возврат строки — обработка повторяет str= ветку:
             *   concat   → _wind_str_concat_n(...)  (уже malloc, передаём как есть)
             *   литерал  → _wind_str_new("...")
             *   str.X    → _wind_str_new(X)
             *   выражение (вызов функции) → как есть, ждём malloc */
            int has_concat = 0;
            int in_q = 0;
            for (const char *q = rhs; *q; q++) {
                if (*q == '"') in_q = !in_q;
                else if (!in_q && *q == '+') { has_concat = 1; break; }
            }
            if (has_concat) {
                int pieces = 1;
                {
                    int q_in = 0;
                    for (const char *q = rhs; *q; q++) {
                        if (*q == '"') q_in = !q_in;
                        else if (!q_in && *q == '+') pieces++;
                    }
                }
                fprintf(cur_out(), "    return _wind_str_concat_n(%d", pieces);
                const char *piece_start = rhs;
                while (1) {
                    const char *next_plus = NULL;
                    int q_in = 0;
                    for (const char *q = piece_start; *q; q++) {
                        if (*q == '"') q_in = !q_in;
                        else if (!q_in && *q == '+') { next_plus = q; break; }
                    }
                    const char *piece_end = next_plus ? next_plus : piece_start + strlen(piece_start);
                    char piece[MAX_EXPR];
                    size_t plen = (size_t)(piece_end - piece_start);
                    if (plen >= sizeof(piece)) plen = sizeof(piece) - 1;
                    memcpy(piece, piece_start, plen);
                    piece[plen] = '\0';
                    char *t = ltrim(piece);
                    rtrim(t);
                    fputs(", ", cur_out());
                    if (*t == '"')                       fputs(t, cur_out());
                    else if (strncmp(t, "str.", 4) == 0) fputs(t + 4, cur_out());
                    else {
                        wind_error(line_num, original,
                            "in return concatenation each piece must be a str variable or literal",
                            NULL);
                        fputs("\"\"", cur_out());
                    }
                    if (!next_plus) break;
                    piece_start = next_plus + 1;
                }
                fprintf(cur_out(), ");\n");
            } else if (*rhs == '"' || strncmp(rhs, "str.", 4) == 0) {
                fprintf(cur_out(), "    return _wind_str_new(");
                translate_expr(rhs, cur_out(), line_num, original);
                fprintf(cur_out(), ");\n");
            } else {
                /* Выражение — например вызов другой str-функции. Доверяем что вернётся malloc. */
                fprintf(cur_out(), "    return ");
                translate_expr(rhs, cur_out(), line_num, original);
                fprintf(cur_out(), ";\n");
            }
        } else {
            fprintf(cur_out(), "    return ");
            translate_expr(rhs, cur_out(), line_num, original);
            fprintf(cur_out(), ";\n");
        }
        return;
    }

    /* === end if === */
    if (strncmp(p, "end if", 6) == 0) {
        if (open_if_blocks == 0) {
            wind_error(line_num, original,
                "'end if' without an open if-block", NULL);
            return;
        }
        fprintf(cur_out(), "    }\n");
        open_if_blocks--;
        return;
    }

    /* === wnd.run NAME ===
     * Имя может быть дотированным: wnd.run math.banner */
    if (strncmp(p, "wnd.run ", 8) == 0) {
        char name[96];
        if (sscanf(p, "wnd.run %95s", name) != 1) {
            wind_error(line_num, original,
                "'wnd.run' must be followed by a function name", NULL);
            return;
        }
        /* Проверка валидности — буквы, цифры, _, и точка для namespace. */
        for (const char *q = name; *q; q++) {
            if (!isalnum((unsigned char)*q) && *q != '_' && *q != '.') {
                wind_error(line_num, original, "invalid function name in wnd.run", NULL);
                return;
            }
        }
        const FuncSig *fn = lookup_func(name);
        if (!fn) {
            char msg[200];
            snprintf(msg, sizeof(msg), "unknown function '%s'", name);
            wind_error(line_num, original, msg,
                "declare it with 'wnd.func' or import its namespace");
            return;
        }
        fprintf(cur_out(), "    %s();\n", fn->c_name);
        return;
    }

    /* === st.import ===
     * Три формы:
     *   st.import math                — весь файл math.wnd → namespace 'math'
     *                                   Использование: math.square(5)
     *   st.import math.square         — одна функция → в текущий скоуп
     *                                   Использование: square(5)
     *   st.import math.{square, cube} — несколько функций → в текущий скоуп */
    if (strncmp(p, "st.import ", 10) == 0 || strcmp(p, "st.import") == 0) {
        static int import_depth = 0;
        if (import_depth >= 8) {
            wind_error(line_num, original,
                "import depth exceeded (max 8)",
                "possible circular import");
            return;
        }

        const char *cur = p + 9;
        while (*cur && isspace((unsigned char)*cur)) cur++;
        if (*cur == '\0') {
            wind_error(line_num, original,
                "'st.import' must be followed by a module name",
                "example: st.import math");
            return;
        }

        /* Парсим namespace + опционально .X или .{X, Y, ...} */
        char ns[64];
        int ni = 0;
        while (*cur && (isalnum((unsigned char)*cur) || *cur == '_')
                && ni < (int)sizeof(ns) - 1) {
            ns[ni++] = *cur++;
        }
        ns[ni] = '\0';
        if (!ns[0]) {
            wind_error(line_num, original, "empty module name in st.import", NULL);
            return;
        }

        /* Собираем selective list если нужно. */
        char selective[16][64];
        int selective_n = 0;
        if (*cur == '.') {
            cur++;
            if (*cur == '{') {
                /* Множественный: .{a, b, c} */
                cur++;
                while (1) {
                    while (*cur && isspace((unsigned char)*cur)) cur++;
                    char one[64];
                    int oi = 0;
                    while (*cur && (isalnum((unsigned char)*cur) || *cur == '_')
                            && oi < (int)sizeof(one) - 1) {
                        one[oi++] = *cur++;
                    }
                    one[oi] = '\0';
                    if (!one[0]) {
                        wind_error(line_num, original,
                            "empty name inside {} of import", NULL);
                        return;
                    }
                    if (selective_n >= 16) {
                        wind_error(line_num, original,
                            "too many items in import list (max 16)", NULL);
                        return;
                    }
                    snprintf(selective[selective_n], sizeof(selective[selective_n]), "%s", one);
                    selective_n++;
                    while (*cur && isspace((unsigned char)*cur)) cur++;
                    if (*cur == ',') { cur++; continue; }
                    if (*cur == '}') { cur++; break; }
                    wind_error(line_num, original,
                        "expected ',' or '}' in import list", NULL);
                    return;
                }
            } else {
                /* Единичный: .name */
                char one[64];
                int oi = 0;
                while (*cur && (isalnum((unsigned char)*cur) || *cur == '_')
                        && oi < (int)sizeof(one) - 1) {
                    one[oi++] = *cur++;
                }
                one[oi] = '\0';
                if (!one[0]) {
                    wind_error(line_num, original,
                        "expected function name after '.'", NULL);
                    return;
                }
                strncpy(selective[0], one, 63);
                selective[0][63] = '\0';
                selective_n = 1;
            }
        }

        /* Парсим файл ровно один раз — при первом обращении любой формы.
         * Все функции регистрируются под "ns.X" с c_name "ns__X" сразу.
         * Повторный импорт = только добавить aliases для selective. */
        int need_parse = !is_namespace_already_imported(ns);

        if (need_parse) {
            char path[512];
            const char *slash = strrchr(current_file, '/');
            if (slash) {
                int dirlen = (int)(slash - current_file + 1);
                snprintf(path, sizeof(path), "%.*s%s.wnd", dirlen, current_file, ns);
            } else {
                snprintf(path, sizeof(path), "%s.wnd", ns);
            }
            FILE *imp = fopen(path, "r");
            if (!imp) {
                char msg[200];
                snprintf(msg, sizeof(msg), "cannot find import '%s'", ns);
                char hint[600];
                snprintf(hint, sizeof(hint), "expected file: %s", path);
                wind_error(line_num, original, msg, hint);
                return;
            }
            const char *saved_file = current_file;
            char *path_dup = strdup(path);
            current_file = path_dup ? path_dup : path;
            import_depth++;

            int saved_import_mode = import_mode;
            char saved_ns[64];
            strncpy(saved_ns, import_ns_name, sizeof(saved_ns));
            import_mode = 1;
            strncpy(import_ns_name, ns, sizeof(import_ns_name) - 1);
            import_ns_name[sizeof(import_ns_name) - 1] = '\0';

            char sub_line[1024], sub_original[1024];
            int sub_lineno = 0;
            while (fgets(sub_line, sizeof(sub_line), imp)) {
                sub_lineno++;
                rtrim(sub_line);
                strncpy(sub_original, sub_line, sizeof(sub_original) - 1);
                sub_original[sizeof(sub_original) - 1] = '\0';
                strip_comments(sub_line);
                char *q = ltrim(sub_line);
                translate_line(q, sub_original, sub_lineno);
            }
            fclose(imp);

            import_mode = saved_import_mode;
            strncpy(import_ns_name, saved_ns, sizeof(import_ns_name) - 1);
            import_ns_name[sizeof(import_ns_name) - 1] = '\0';
            import_depth--;
            current_file = saved_file;
            free(path_dup);

            mark_namespace_imported(ns);
        }

        /* Selective: для каждой запрошенной функции добавляем alias short → c_name. */
        for (int j = 0; j < selective_n; j++) {
            char qualified[160];
            /* %.63s — Wind-имена ограничены 64 символами, явно говорим gcc. */
            snprintf(qualified, sizeof(qualified), "%.63s.%.63s", ns, selective[j]);
            const FuncSig *src = lookup_func(qualified);
            if (!src) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "function '%.63s' not found in module '%.63s'", selective[j], ns);
                wind_error(line_num, original, msg, NULL);
                continue;
            }
            /* Если alias уже есть — пропускаем. */
            if (lookup_func(selective[j])) continue;
            FuncSig alias = *src;
            strncpy(alias.name, selective[j], sizeof(alias.name) - 1);
            alias.name[sizeof(alias.name) - 1] = '\0';
            /* c_name остаётся mangled — указывает на ту же C-функцию. */
            declare_func(&alias);
        }
        return;
    }

    /* ===== Объявление list: int.list.NAME = [...] ===== */
    {
        VarType list_elem_type = VT_NONE;
        int prefix_len = 0;
        if (strncmp(p, "int.list.", 9) == 0)        { list_elem_type = VT_INT;  prefix_len = 9; }
        else if (strncmp(p, "frac.list.", 10) == 0) { list_elem_type = VT_FRAC; prefix_len = 10; }
        else if (strncmp(p, "str.list.", 9) == 0)   { list_elem_type = VT_STR;  prefix_len = 9; }

        if (list_elem_type != VT_NONE) {
            char *name_start = p + prefix_len;
            char name[64];
            size_t i = 0;
            char *q = name_start;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')
                    && i < sizeof(name) - 1) {
                name[i++] = *q++;
            }
            name[i] = '\0';
            if (name[0] == '\0') {
                wind_error(line_num, original,
                    "list prefix must be followed by a variable name",
                    "example: int.list.nums = [1, 2, 3]");
                return;
            }
            if (!is_valid_var_name(name)) {
                char msg[160];
                snprintf(msg, sizeof(msg), "invalid list name: '%s'", name);
                wind_error(line_num, original, msg, NULL);
                return;
            }
            if (is_c_reserved(name)) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "list name '%s' is a reserved C keyword", name);
                wind_error(line_num, original, msg, NULL);
                return;
            }
            if (lookup_var(name) != VT_NONE) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "name '%s' is already declared", name);
                wind_error(line_num, original, msg, NULL);
                return;
            }
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q != '=') {
                wind_error(line_num, original,
                    "expected '=' after list name",
                    "example: int.list.nums = [1, 2, 3]");
                return;
            }
            q++;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q != '[') {
                wind_error(line_num, original,
                    "list literal must start with '['",
                    "example: int.list.nums = [1, 2, 3]");
                return;
            }
            q++;
            char *rb = strrchr(q, ']');
            if (!rb) {
                wind_error(line_num, original,
                    "missing ']' at end of list literal", NULL);
                return;
            }
            *rb = '\0';
            char *items = ltrim(q);
            rtrim(items);

        }
    }


    /* ===== Объявление переменной: <prefix>.NAME = ... ===== */
    {
        VarType decl_type = VT_NONE;
        int prefix_len = 0;
        if (strncmp(p, "int.", 4) == 0)  { decl_type = VT_INT;  prefix_len = 4; }
        else if (strncmp(p, "frac.", 5) == 0) { decl_type = VT_FRAC; prefix_len = 5; }
        else if (strncmp(p, "str.", 4) == 0)  { decl_type = VT_STR;  prefix_len = 4; }

        if (decl_type != VT_NONE) {
            char *name_start = p + prefix_len;
            char name[64];
            size_t i = 0;
            char *q = name_start;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')
                    && i < sizeof(name) - 1) {
                name[i++] = *q++;
            }
            name[i] = '\0';
            if (name[0] == '\0') {
                wind_error(line_num, original,
                    "type prefix must be followed by a variable name", NULL);
                return;
            }
            if (!is_valid_var_name(name)) {
                char msg[160];
                snprintf(msg, sizeof(msg), "invalid variable name: '%s'", name);
                wind_error(line_num, original, msg,
                    "name: letters, digits, '_' only; must start with a letter or '_'");
                return;
            }
            if (is_c_reserved(name)) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "variable name '%s' is a reserved C keyword", name);
                wind_error(line_num, original, msg,
                    "pick a different name — reserved: int, long, void, return, if, for, while, etc.");
                return;
            }
            while (*q && isspace((unsigned char)*q)) q++;

            /* === Массивы: int.nums[10] объявление, int.nums[i] = val присваивание === */
            if (*q == '[') {
                if (decl_type == VT_STR) {
                    wind_error(line_num, original,
                        "string arrays are not supported yet",
                        "use int[] or frac[]");
                    return;
                }
                char *rb = strchr(q + 1, ']');
                if (!rb) {
                    wind_error(line_num, original, "missing ']' in array brackets", NULL);
                    return;
                }
                char idx_buf[128];
                size_t idx_len = (size_t)(rb - q - 1);
                if (idx_len >= sizeof(idx_buf)) idx_len = sizeof(idx_buf) - 1;
                memcpy(idx_buf, q + 1, idx_len);
                idx_buf[idx_len] = '\0';
                char *idx = ltrim(idx_buf);
                rtrim(idx);

                char *after_rb = rb + 1;
                while (*after_rb && isspace((unsigned char)*after_rb)) after_rb++;

                if (*after_rb == '\0') {
                    /* Объявление массива: int.nums[10] */
                    if (lookup_var(name) != VT_NONE) {
                        char msg[200];
                        snprintf(msg, sizeof(msg),
                            "name '%s' is already declared", name);
                        wind_error(line_num, original, msg, NULL);
                        return;
                    }
                    int size = 0;
                    if (sscanf(idx, "%d", &size) != 1 || size <= 0) {
                        wind_error(line_num, original,
                            "array size must be a positive integer literal",
                            "example: int.nums[10]");
                        return;
                    }
                    const char *ctype = (decl_type == VT_FRAC) ? "double" : "int";
                    fprintf(cur_out(), "    %s %s[%d] = {0};\n", ctype, name, size);
                    declare_array(name, decl_type, size);
                    return;
                }

                /* Присваивание элементу: int.nums[i] = val */
                if (*after_rb != '=') {
                    wind_error(line_num, original,
                        "expected '=' after array index, or no '=' for declaration",
                        "examples: int.nums[10]   or   int.nums[i] = 5");
                    return;
                }
                int arr_size = lookup_array_size(name);
                if (arr_size == 0) {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                        "'%s' is not a declared array", name);
                    wind_error(line_num, original, msg,
                        "declare it first: int.NAME[SIZE]");
                    return;
                }
                if (lookup_var(name) != decl_type) {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                        "array '%s' has element type '%s' but assigned through '%s.'",
                        name, vt_name(lookup_var(name)), vt_name(decl_type));
                    wind_error(line_num, original, msg, NULL);
                    return;
                }
                char *arhs = ltrim(after_rb + 1);
                if (*arhs == '\0') {
                    wind_error(line_num, original,
                        "nothing after '=' — value or expression expected", NULL);
                    return;
                }
                fprintf(cur_out(), "    %s[", name);
                translate_expr(idx, cur_out(), line_num, original);
                fprintf(cur_out(), "] = ");
                translate_expr(arhs, cur_out(), line_num, original);
                fprintf(cur_out(), ";\n");
                return;
            }

            if (*q != '=') {
                wind_error(line_num, original,
                    "variable name must be followed by '='", NULL);
                return;
            }
            char *rhs = ltrim(q + 1);

            VarType existing = lookup_var(name);
            if (lookup_array_size(name) > 0) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "'%s' is an array — assign elements with index, not as a scalar", name);
                wind_error(line_num, original, msg,
                    "example: int.nums[0] = 5");
                return;
            }
            if (existing != VT_NONE && existing != decl_type) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "variable '%s' is already declared with type '%s', cannot redeclare as '%s'",
                    name, vt_name(existing), vt_name(decl_type));
                wind_error(line_num, original, msg,
                    "use the original type prefix or pick a different name");
                return;
            }
            int is_decl = (existing == VT_NONE);

            /* === Ввод с приглашением: str.write {"..."} === */
            if (strncmp(rhs, "str.write", 9) == 0) {
                char *brace = strchr(rhs, '{');
                char *fq = brace ? strchr(brace, '"') : NULL;
                char *lq = fq ? strrchr(fq + 1, '"') : NULL;
                if (!brace || !fq || !lq || lq <= fq) {
                    wind_error(line_num, original,
                        "prompt must be in braces and quotes",
                        "example: str.write {\"Enter: \"}");
                    return;
                }
                size_t len = (size_t)(lq - fq - 1);
                char *prompt = (char *)malloc(len + 1);
                if (!prompt) { fprintf(stderr, "malloc failed\n"); exit(1); }
                memcpy(prompt, fq + 1, len);
                prompt[len] = '\0';

                if (decl_type == VT_INT) {
                    if (is_decl) fprintf(cur_out(), "    int %s; {\n", name);
                    else         fprintf(cur_out(), "    {\n");
                    fprintf(cur_out(), "        char _buf[64];\n");
                    fprintf(cur_out(), "        printf(\"%s\"); fflush(stdout);\n", prompt);
                    fprintf(cur_out(), "        if (fgets(_buf, sizeof(_buf), stdin)) {\n");
                    fprintf(cur_out(), "            if (sscanf(_buf, \"%%d\", &%s) != 1) {\n", name);
                    fprintf(cur_out(), "                fprintf(stderr, \"[Wind runtime] expected an integer, got: %%s\", _buf);\n");
                    fprintf(cur_out(), "                exit(1);\n");
                    fprintf(cur_out(), "            }\n");
                    fprintf(cur_out(), "        }\n");
                    fprintf(cur_out(), "    }\n");
                } else if (decl_type == VT_FRAC) {
                    if (is_decl) fprintf(cur_out(), "    double %s; {\n", name);
                    else         fprintf(cur_out(), "    {\n");
                    fprintf(cur_out(), "        char _buf[64];\n");
                    fprintf(cur_out(), "        printf(\"%s\"); fflush(stdout);\n", prompt);
                    fprintf(cur_out(), "        if (fgets(_buf, sizeof(_buf), stdin)) {\n");
                    fprintf(cur_out(), "            for (char *q = _buf; *q; q++) if (*q == ',') *q = '.';\n");
                    fprintf(cur_out(), "            if (sscanf(_buf, \"%%lf\", &%s) != 1) {\n", name);
                    fprintf(cur_out(), "                fprintf(stderr, \"[Wind runtime] expected a number, got: %%s\", _buf);\n");
                    fprintf(cur_out(), "                exit(1);\n");
                    fprintf(cur_out(), "            }\n");
                    fprintf(cur_out(), "        }\n");
                    fprintf(cur_out(), "    }\n");
                } else { /* VT_STR — динамическая строка */
                    if (is_decl) fprintf(cur_out(), "    char *%s = NULL;\n", name);
                    fprintf(cur_out(), "    printf(\"%s\"); fflush(stdout);\n", prompt);
                    fprintf(cur_out(), "    _wind_str_take(&%s, _wind_str_input());\n", name);
                }
                free(prompt);
                if (is_decl) declare_var(name, decl_type);
                return;
            }

            /* === Ввод без приглашения: write() === */
            if (strstr(rhs, "write()") != NULL) {
                if (decl_type == VT_INT) {
                    if (is_decl) fprintf(cur_out(), "    int %s; {\n", name);
                    else         fprintf(cur_out(), "    {\n");
                    fprintf(cur_out(), "        char _buf[64];\n");
                    fprintf(cur_out(), "        if (fgets(_buf, sizeof(_buf), stdin)) {\n");
                    fprintf(cur_out(), "            if (sscanf(_buf, \"%%d\", &%s) != 1) {\n", name);
                    fprintf(cur_out(), "                fprintf(stderr, \"[Wind runtime] expected an integer, got: %%s\", _buf);\n");
                    fprintf(cur_out(), "                exit(1);\n");
                    fprintf(cur_out(), "            }\n");
                    fprintf(cur_out(), "        }\n");
                    fprintf(cur_out(), "    }\n");
                } else if (decl_type == VT_FRAC) {
                    if (is_decl) fprintf(cur_out(), "    double %s; {\n", name);
                    else         fprintf(cur_out(), "    {\n");
                    fprintf(cur_out(), "        char _buf[64];\n");
                    fprintf(cur_out(), "        if (fgets(_buf, sizeof(_buf), stdin)) {\n");
                    fprintf(cur_out(), "            for (char *q = _buf; *q; q++) if (*q == ',') *q = '.';\n");
                    fprintf(cur_out(), "            if (sscanf(_buf, \"%%lf\", &%s) != 1) {\n", name);
                    fprintf(cur_out(), "                fprintf(stderr, \"[Wind runtime] expected a number, got: %%s\", _buf);\n");
                    fprintf(cur_out(), "                exit(1);\n");
                    fprintf(cur_out(), "            }\n");
                    fprintf(cur_out(), "        }\n");
                    fprintf(cur_out(), "    }\n");
                } else { /* VT_STR — динамическая строка через write() */
                    if (is_decl) fprintf(cur_out(), "    char *%s = NULL;\n", name);
                    fprintf(cur_out(), "    _wind_str_take(&%s, _wind_str_input());\n", name);
                }
                if (is_decl) declare_var(name, decl_type);
                return;
            }

            if (*rhs == '\0') {
                wind_error(line_num, original,
                    "nothing after '=' — value or expression expected", NULL);
                return;
            }

            /* === Строки — все варианты через динамические буферы ===
             * - Литерал:        str.X = "hello"
             * - Из другой str:  str.X = str.Y
             * - Конкатенация:   str.X = str.A + str.B + "!"
             * - Из выражения:   str.X = some_func(...)
             *
             * Генерация:
             *   объявление  →  char *name = NULL;
             *   присваивание литерала/var  →  _wind_str_set(&name, src);
             *   присваивание concat        →  _wind_str_take(&name, _wind_str_concat_n(...));
             *   присваивание выражения     →  _wind_str_take(&name, EXPR);  // ждём malloc */
            if (decl_type == VT_STR) {
                int has_concat = 0;
                {
                    int in_q = 0;
                    for (const char *q = rhs; *q; q++) {
                        if (*q == '"') in_q = !in_q;
                        else if (!in_q && *q == '+') { has_concat = 1; break; }
                    }
                }

                if (is_decl) fprintf(cur_out(), "    char *%s = NULL;\n", name);

                if (has_concat) {
                    /* Считаем количество кусков. */
                    int pieces = 1;
                    {
                        int in_q = 0;
                        for (const char *q = rhs; *q; q++) {
                            if (*q == '"') in_q = !in_q;
                            else if (!in_q && *q == '+') pieces++;
                        }
                    }
                    fprintf(cur_out(), "    _wind_str_take(&%s, _wind_str_concat_n(%d", name, pieces);
                    const char *piece_start = rhs;
                    while (1) {
                        const char *next_plus = NULL;
                        int in_q = 0;
                        for (const char *q = piece_start; *q; q++) {
                            if (*q == '"') in_q = !in_q;
                            else if (!in_q && *q == '+') { next_plus = q; break; }
                        }
                        const char *piece_end = next_plus ? next_plus : piece_start + strlen(piece_start);
                        char piece[MAX_EXPR];
                        size_t plen = (size_t)(piece_end - piece_start);
                        if (plen >= sizeof(piece)) plen = sizeof(piece) - 1;
                        memcpy(piece, piece_start, plen);
                        piece[plen] = '\0';
                        char *t = ltrim(piece);
                        rtrim(t);

                        fputs(", ", cur_out());
                        if (*t == '"') {
                            /* Строковый литерал — кавычки оставляем как есть. */
                            fputs(t, cur_out());
                        } else if (strncmp(t, "str.", 4) == 0) {
                            fputs(t + 4, cur_out());
                        } else {
                            char msg[200];
                            snprintf(msg, sizeof(msg),
                                "in concatenation each piece must be a string variable (str.X) or a literal: %s", t);
                            wind_error(line_num, original, msg,
                                "example: str.full = str.first + \" \" + str.last");
                            fputs("\"\"", cur_out()); /* чтобы вывод остался валидным C */
                        }
                        if (!next_plus) break;
                        piece_start = next_plus + 1;
                    }
                    fprintf(cur_out(), "));\n");
                    if (is_decl) declare_var(name, decl_type);
                    return;
                }

                if (*rhs == '"') {
                    /* Литерал. */
                    char *lq = strrchr(rhs, '"');
                    if (lq == rhs) {
                        wind_error(line_num, original, "unclosed quote in string", NULL);
                        return;
                    }
                    size_t len = (size_t)(lq - rhs - 1);
                    char *text = (char *)malloc(len + 1);
                    if (!text) { fprintf(stderr, "malloc failed\n"); exit(1); }
                    memcpy(text, rhs + 1, len);
                    text[len] = '\0';
                    fprintf(cur_out(), "    _wind_str_set(&%s, \"%s\");\n", name, text);
                    free(text);
                    if (is_decl) declare_var(name, decl_type);
                    return;
                }

                if (strncmp(rhs, "str.", 4) == 0) {
                    /* Простое str.X = str.Y — копируем содержимое Y. */
                    /* Берём имя Y до конца строки (или пробела) — без слежения за индексами,
                     * потому что str-переменные пока не имеют индексации. */
                    char yname[64];
                    int yi = 0;
                    const char *q = rhs + 4;
                    while (*q && (isalnum((unsigned char)*q) || *q == '_') && yi < 63) {
                        yname[yi++] = *q++;
                    }
                    yname[yi] = '\0';
                    if (!yi) {
                        wind_error(line_num, original,
                            "empty variable name after 'str.'", NULL);
                        return;
                    }
                    check_var_ref(yname, VT_STR, line_num, original);
                    fprintf(cur_out(), "    _wind_str_set(&%s, %s);\n", name, yname);
                    if (is_decl) declare_var(name, decl_type);
                    return;
                }

                /* Иначе — выражение (вызов функции возвращающей str). Ждём malloc'нутый указатель. */
                fprintf(cur_out(), "    _wind_str_take(&%s, ", name);
                translate_expr(rhs, cur_out(), line_num, original);
                fprintf(cur_out(), ");\n");
                if (is_decl) declare_var(name, decl_type);
                return;
            }

            /* === Числовая переменная: объявление или присваивание === */
            if (is_decl) {
                const char *ctype = (decl_type == VT_FRAC) ? "double" : "int";
                fprintf(cur_out(), "    %s %s = ", ctype, name);
            } else {
                fprintf(cur_out(), "    %s = ", name);
            }
            translate_expr(rhs, cur_out(), line_num, original);
            fprintf(cur_out(), ";\n");
            if (is_decl) declare_var(name, decl_type);
            return;
        }
    }

    /* === end repeat === */
    if (strncmp(p, "end repeat", 10) == 0) {
        if (open_repeat_blocks == 0) {
            wind_error(line_num, original,
                "'end repeat' без открытого repeat",
                "перед ним должно быть 'repeat N'");
            return;
        }
        fprintf(cur_out(), "    }\n");
        open_repeat_blocks--;
        return;
    }

    /* === continue === */
    if (strcmp(p, "continue") == 0) {
        if (open_repeat_blocks == 0 && open_while_blocks == 0) {
            wind_error(line_num, original,
                "'continue' outside of a loop", NULL);
            return;
        }
        fprintf(cur_out(), "    continue;\n");
        return;
    }

    /* === break === */
    if (strcmp(p, "break") == 0) {
        if (open_repeat_blocks == 0 && open_while_blocks == 0) {
            wind_error(line_num, original,
                "'break' outside of a loop",
                "'break' can only appear inside a 'repeat' or 'while'");
            return;
        }
        fprintf(cur_out(), "    break;\n");
        return;
    }

 
    /* === repeat N === */
    if (strncmp(p, "repeat ", 7) == 0) {
        int n;
        if (sscanf(p, "repeat %d", &n) != 1) {
            wind_error(line_num, original,
                "'repeat' must be followed by a number",
                "example: repeat 5");
            return;
        }
        fprintf(cur_out(), "    for (int _r = 0; _r < %d; _r++) {\n", n);
        open_repeat_blocks++;
        return;
    }

    /* === end while === */
    if (strncmp(p, "end while", 9) == 0) {
        if (open_while_blocks == 0) {
            wind_error(line_num, original,
                "'end while' without an open while-block",
                "must be preceded by 'while ...'");
            return;
        }
        fprintf(cur_out(), "    }\n");
        open_while_blocks--;
        return;
    }

    /* === while <condition> === */
    if (strncmp(p, "while ", 6) == 0 || strcmp(p, "while") == 0) {
        char *expr_part = ltrim(p + 5);
        if (*expr_part == '\0') {
            wind_error(line_num, original,
                "'while' must be followed by a condition",
                "example: while int.i < 10");
            return;
        }
        fprintf(cur_out(), "    while (");
        emit_compound_condition(expr_part, line_num, original);
        fprintf(cur_out(), ") {\n");
        open_while_blocks++;
        return;
    }

    /* === if <condition> === */
    if (strncmp(p, "if ", 3) == 0 || strcmp(p, "if") == 0) {
        char *expr_part = ltrim(p + 2);
        if (*expr_part == '\0') {
            wind_error(line_num, original,
                "'if' must be followed by a condition", NULL);
            return;
        }
        fprintf(cur_out(), "    if (");
        emit_compound_condition(expr_part, line_num, original);
        fprintf(cur_out(), ") {\n");
        open_if_blocks++;
        return;
    }

    /* === else === */
    if (strncmp(p, "else", 4) == 0) {
        if (open_if_blocks > 0) {
            fprintf(cur_out(), "    } else {\n");
            return;
        }
        wind_error(line_num, original, "'else' without a preceding 'if'", NULL);
        return;
    }

    /* === terminal.paste -> "...{TYPE.NAME}..." === */
    if (strstr(p, "terminal.paste") != NULL) {
        char *arrow = strstr(p, "->");
        if (!arrow) {
            wind_error(line_num, original,
                "'terminal.paste' must be followed by the arrow '->'", NULL);
            return;
        }
        char *fq = strchr(arrow, '"');
        char *lq = fq ? strrchr(fq + 1, '"') : NULL;
        if (!fq || !lq || lq <= fq) {
            wind_error(line_num, original,
                "text for terminal.paste must be in double quotes", NULL);
            return;
        }
        size_t text_len = (size_t)(lq - fq - 1);
        char *fmt  = (char *)malloc(text_len * 4 + 16);
        char *args = (char *)malloc(text_len * 4 + 16);
        if (!fmt || !args) { free(fmt); free(args); fprintf(stderr, "malloc failed\n"); exit(1); }
        size_t fi = 0, ai = 0;
        args[0] = '\0';
        const char *s = fq + 1;
        const char *end = lq;
        int bad = 0;
        while (s < end) {
            if (*s == '{') {
                const char *close = NULL;
                for (const char *t = s + 1; t < end; t++) {
                    if (*t == '}') { close = t; break; }
                }
                if (!close) {
                    wind_error(line_num, original,
                        "unclosed brace in string (missing '}')", NULL);
                    bad = 1; break;
                }
                size_t inner_len = (size_t)(close - s - 1);
                const char *inner = s + 1;
                if (inner_len == 0) {
                    wind_error(line_num, original, "empty braces {} in string", NULL);
                    bad = 1; break;
                }
                VarType ref = VT_NONE;
                int plen = 0;
                if (inner_len >= 5 && strncmp(inner, "int.", 4) == 0)   { ref = VT_INT;  plen = 4; }
                else if (inner_len >= 6 && strncmp(inner, "frac.", 5) == 0) { ref = VT_FRAC; plen = 5; }
                else if (inner_len >= 5 && strncmp(inner, "str.", 4) == 0)  { ref = VT_STR;  plen = 4; }
                if (ref == VT_NONE) {
                    wind_error(line_num, original,
                        "inside {} expected a variable: {int.name}, {frac.name} or {str.name}",
                        "example: terminal.paste -> \"x = {int.x}\"");
                    bad = 1; break;
                }
                char vname[96];
                size_t nlen = inner_len - (size_t)plen;
                if (nlen >= sizeof(vname)) nlen = sizeof(vname) - 1;
                memcpy(vname, inner + plen, nlen);
                vname[nlen] = '\0';

                /* Поддержка элемента массива: {int.nums[0]} или {int.nums[int.i]} */
                char *lbr = strchr(vname, '[');
                const char *spec = (ref == VT_INT)  ? "%d"
                                 : (ref == VT_FRAC) ? "%g"
                                 :                    "%s";
                if (lbr) {
                    char *rbr = strchr(lbr, ']');
                    if (!rbr || rbr[1] != '\0') {
                        wind_error(line_num, original,
                            "bad array indexing in {...}",
                            "example: {int.nums[0]} or {int.nums[int.i]}");
                        bad = 1; break;
                    }
                    *lbr = '\0';
                    *rbr = '\0';
                    char *idx = lbr + 1;
                    if (lookup_array_size(vname) == 0) {
                        char msg[200];
                        snprintf(msg, sizeof(msg), "'%s' is not an array", vname);
                        wind_error(line_num, original, msg,
                            "declare it: int.NAME[SIZE]");
                        bad = 1; break;
                    }
                    if (lookup_var(vname) != ref) {
                        char msg[200];
                        snprintf(msg, sizeof(msg),
                            "array '%s' has element type '%s' but used with '%s.'",
                            vname, vt_name(lookup_var(vname)), vt_name(ref));
                        wind_error(line_num, original, msg, NULL);
                        bad = 1; break;
                    }
                    /* Индекс: число или int.var — снимаем префикс. */
                    const char *idx_clean = idx;
                    if (strncmp(idx, "int.", 4) == 0) idx_clean = idx + 4;
                    ai += (size_t)snprintf(args + ai, text_len * 4 + 16 - ai,
                                           ", %s[%s]", vname, idx_clean);
                    fmt[fi++] = spec[0];
                    fmt[fi++] = spec[1];
                    s = close + 1;
                    continue;
                }

                check_var_ref(vname, ref, line_num, original);
                ai += (size_t)snprintf(args + ai, text_len * 4 + 16 - ai,
                                       ", %s", vname);
                fmt[fi++] = spec[0];
                fmt[fi++] = spec[1];
                s = close + 1;
                continue;
            }
            if (*s == '"' || *s == '\\') fmt[fi++] = '\\';
            fmt[fi++] = *s++;
        }
        if (!bad) {
            fmt[fi] = '\0';
            fprintf(cur_out(), "    printf(\"%s\\n\"%s);\n", fmt, args);
        }
        free(fmt); free(args);
        return;
    }

    /* === terminal.clear === */
    if (strncmp(p, "terminal.clear", 14) == 0) {
        fprintf(cur_out(), "    printf(\"\\033[2J\\033[H\");\n");
        return;
    }

    /* Нераспознанная строка — пытаемся угадать ближайшее ключевое слово. */
    char first[64];
    int fw = 0;
    while (p[fw] && !isspace((unsigned char)p[fw]) && fw < 63) {
        first[fw] = p[fw];
        fw++;
    }
    first[fw] = '\0';

    const char *guess = suggest_keyword(first);
    if (guess) {
        char hint[160];
        snprintf(hint, sizeof(hint), "did you mean '%s'?", guess);
        wind_error(line_num, original, "cannot recognize this line", hint);
    } else {
        wind_error(line_num, original,
            "cannot recognize this line",
            "keywords: int. frac. str. if else end if terminal.paste terminal.clear st.import wnd.func end wnd.func wnd.run return repeat end repeat while end while break continue");
    }
}

void parser_check_eof(int line_num) {
    if (in_comment) {
        wind_error(line_num, "<EOF>",
            "unclosed comment — missing closing //", NULL);
    }
    if (current_ctx == CTX_FUNC) {
        char hint[256];
        snprintf(hint, sizeof(hint),
            "function '%s' is open but not closed — add 'end wnd.func'",
            current_func_name);
        wind_error(line_num, "<EOF>", "function not closed", hint);
    }
    if (open_if_blocks > 0) {
        char hint[256];
        snprintf(hint, sizeof(hint),
            "unclosed if-blocks remaining: %d — add 'end if' that many times",
            open_if_blocks);
        wind_error(line_num, "<EOF>", "if-block not closed", hint);
    }
    if (open_repeat_blocks > 0) {
        char hint[256];
        snprintf(hint, sizeof(hint),
            "unclosed repeat-blocks remaining: %d — add 'end repeat' that many times",
            open_repeat_blocks);
        wind_error(line_num, "<EOF>", "repeat-block not closed", hint);
    }
    if (open_while_blocks > 0) {
        char hint[256];
        snprintf(hint, sizeof(hint),
            "unclosed while-blocks remaining: %d — add 'end while' that many times",
            open_while_blocks);
        wind_error(line_num, "<EOF>", "while-block not closed", hint);
    }
}
