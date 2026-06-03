/*
 * expr.c — реализация работы с выражениями.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "expr.h"
#include "symtab.h"
#include "errors.h"
#include "parser.h"
#include "util.h"

void translate_expr(const char *src, FILE *out,
                    int line_num, const char *original) {
    const char *p = src;
    while (*p) {
        /* === Встроенная random(a, b) ===
         * Принимаем и круглые, и квадратные скобки — Wind-стиль допускает оба. */
        if (strncmp(p, "random(", 7) == 0 || strncmp(p, "random[", 7) == 0) {
            fputs("_wind_random(", out);
            p += 7;
            continue;
        }

        /* === Строковые builtin'ы ===
         * slice(s, a, b)        -> подстрока [a..b)
         * find(s, sub)          -> позиция или -1
         * replace(s, from, to)  -> копия с заменой всех вхождений
         * split(s, sep)         -> str.list
         * join(list, sep)       -> str
         * Просто подменяем имя — основной цикл переводит аргументы дальше. */
        if (strncmp(p, "slice(", 6) == 0)         { fputs("_wind_str_slice(", out);        p += 6;  continue; }
        if (strncmp(p, "slice_chars(", 12) == 0)  { fputs("_wind_str_slice_chars(", out);  p += 12; continue; }
        if (strncmp(p, "chars(", 6) == 0)         { fputs("_wind_str_chars(", out);        p += 6;  continue; }
        if (strncmp(p, "find(", 5) == 0)    { fputs("_wind_str_find(", out);    p += 5;  continue; }
        if (strncmp(p, "replace(", 8) == 0) { fputs("_wind_str_replace(", out); p += 8;  continue; }
        if (strncmp(p, "split(", 6) == 0)   { fputs("_wind_str_split(", out);   p += 6;  continue; }
        if (strncmp(p, "join(", 5) == 0)    { fputs("_wind_str_join(", out);    p += 5;  continue; }

        /* === Файловые builtin'ы (только read-side, write — statement в parser.c) === */
        if (strncmp(p, "file.read(", 10) == 0)   { fputs("_wind_file_read(", out);   p += 10; continue; }
        if (strncmp(p, "file.lines(", 11) == 0)  { fputs("_wind_file_lines(", out);  p += 11; continue; }
        if (strncmp(p, "file.exists(", 12) == 0) { fputs("_wind_file_exists(", out); p += 12; continue; }

        /* === JSON encode/decode === */
        if (strncmp(p, "json.encode(", 12) == 0)         { fputs("_wind_json_encode_dict(", out);   p += 12; continue; }
        if (strncmp(p, "json.decode_str_int(", 20) == 0) { fputs("_wind_json_decode_str_int(", out); p += 20; continue; }
        if (strncmp(p, "json.decode_str_str(", 20) == 0) { fputs("_wind_json_decode_str_str(", out); p += 20; continue; }

        /* === Subprocess === */
        if (strncmp(p, "shell_run(", 10) == 0) { fputs("_wind_shell_run(", out); p += 10; continue; }
        if (strncmp(p, "shell(", 6) == 0)      { fputs("_wind_shell(", out);     p += 6;  continue; }


        /* === HTTP клиент === */
        if (strncmp(p, "http.get(", 9) == 0)        { fputs("_wind_http_get(", out);       p += 9;  continue; }
        if (strncmp(p, "http.post(", 10) == 0)      { fputs("_wind_http_post(", out);      p += 10; continue; }
        if (strncmp(p, "http.post_json(", 15) == 0) { fputs("_wind_http_post_json(", out); p += 15; continue; }
        if (strncmp(p, "http.put(", 9) == 0)        { fputs("_wind_http_put(", out);       p += 9;  continue; }
        if (strncmp(p, "http.delete(", 12) == 0)    { fputs("_wind_http_delete(", out);    p += 12; continue; }
        if (strncmp(p, "http.status()", 13) == 0)   { fputs("_wind_http_status()", out);   p += 13; continue; }

        /* === math builtin'ы (обёртки над <math.h>) === */
        if (strncmp(p, "sqrt(", 5) == 0)  { fputs("sqrt(", out);    p += 5;  continue; }
        if (strncmp(p, "pow(", 4) == 0)   { fputs("pow(", out);     p += 4;  continue; }
        if (strncmp(p, "sin(", 4) == 0)   { fputs("sin(", out);     p += 4;  continue; }
        if (strncmp(p, "cos(", 4) == 0)   { fputs("cos(", out);     p += 4;  continue; }
        if (strncmp(p, "tan(", 4) == 0)   { fputs("tan(", out);     p += 4;  continue; }
        if (strncmp(p, "log(", 4) == 0)   { fputs("log(", out);     p += 4;  continue; }
        if (strncmp(p, "exp(", 4) == 0)   { fputs("exp(", out);     p += 4;  continue; }
        if (strncmp(p, "floor(", 6) == 0) { fputs("floor(", out);   p += 6;  continue; }
        if (strncmp(p, "ceil(", 5) == 0)  { fputs("ceil(", out);    p += 5;  continue; }
        if (strncmp(p, "round(", 6) == 0) { fputs("round(", out);   p += 6;  continue; }
        if (strncmp(p, "abs(", 4) == 0)   { fputs("fabs(", out);    p += 4;  continue; }
        if (strncmp(p, "min(", 4) == 0)   { fputs("fmin(", out);    p += 4;  continue; }
        if (strncmp(p, "max(", 4) == 0)   { fputs("fmax(", out);    p += 4;  continue; }
        /* Константы — на месте подменяем имена. */
        if (strncmp(p, "PI", 2) == 0 && !isalnum((unsigned char)p[2]) && p[2] != '_') {
            fputs("3.14159265358979323846", out); p += 2; continue;
        }
        if (strncmp(p, "E", 1) == 0 && !isalnum((unsigned char)p[1]) && p[1] != '_') {
            fputs("2.71828182845904523536", out); p += 1; continue;
        }

        /* === Встроенная функция len(NAME) ===
         * list  -> _wind_list_len(NAME)
         * str   -> (int)strlen(NAME)
         * array -> размер как литерал */
        if (strncmp(p, "len(", 4) == 0) {
            const char *q = p + 4;
            while (*q && isspace((unsigned char)*q)) q++;
            char arg[64];
            size_t ai = 0;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')
                    && ai < sizeof(arg) - 1) {
                arg[ai++] = *q++;
            }
            arg[ai] = '\0';
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q != ')') {
                wind_error(line_num, original,
                    "len() expects a single variable name",
                    "example: len(nums)");
                fputs("0", out);
                while (*p && *p != ')') p++;
                if (*p == ')') p++;
                continue;
            }
            if (!arg[0]) {
                wind_error(line_num, original,
                    "len() needs a name inside",
                    "example: len(nums)");
                fputs("0", out);
                p = q + 1;
                continue;
            }
            VarType vt = lookup_var(arg);
            if (is_dict_var(arg)) {
                fprintf(out, "_wind_dict_len(%s)", arg);
            } else if (vt == VT_NONE && !is_list_var(arg) && lookup_array_size(arg) == 0) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "len(%s): variable '%s' is not declared", arg, arg);
                wind_error(line_num, original, msg, NULL);
                fputs("0", out);
            } else if (is_list_var(arg)) {
                fprintf(out, "_wind_list_len(%s)", arg);
            } else if (vt == VT_STR) {
                fprintf(out, "(int)strlen(%s)", arg);
            } else if (lookup_array_size(arg) > 0) {
                fprintf(out, "%d", lookup_array_size(arg));
            } else {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "len(%s): only list, str, or array supported", arg);
                wind_error(line_num, original, msg, NULL);
                fputs("0", out);
            }
            p = q + 1;
            continue;
        }

        /* TYPE.NAME — пытаемся распознать любой из трёх типов.
         * Сначала проверяем длинные префиксы (int.list.), потом короткие (int.) —
         * иначе int.list.nums разберётся как int. + имя "list" и обломится. */
        VarType ref_type = VT_NONE;
        int prefix_len = 0;
        if (strncmp(p, "int.list.", 9) == 0)        { ref_type = VT_INT;  prefix_len = 9; }
        else if (strncmp(p, "frac.list.", 10) == 0) { ref_type = VT_FRAC; prefix_len = 10; }
        else if (strncmp(p, "str.list.", 9) == 0)   { ref_type = VT_STR;  prefix_len = 9; }
        else if (strncmp(p, "int.", 4) == 0)        { ref_type = VT_INT;  prefix_len = 4; }
        else if (strncmp(p, "frac.", 5) == 0)       { ref_type = VT_FRAC; prefix_len = 5; }
        else if (strncmp(p, "str.", 4) == 0)        { ref_type = VT_STR;  prefix_len = 4; }

        if (ref_type != VT_NONE) {
            p += prefix_len;
            char name[64];
            int i = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < 63) {
                name[i++] = *p++;
            }
            name[i] = '\0';
            check_var_ref(name, ref_type, line_num, original);
            if (is_list_var(name)) {
                if (*p != '[') {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                        "'%s' is a list — index it with [...]", name);
                    wind_error(line_num, original, msg,
                        "example: int.list.nums[0]");
                    fputs(name, out);
                    continue;
                }
                p++;
                int depth = 1;
                const char *idx_start = p;
                while (*p && depth > 0) {
                    if (*p == '[') depth++;
                    else if (*p == ']') { depth--; if (depth == 0) break; }
                    p++;
                }
                if (*p != ']') {
                    wind_error(line_num, original,
                        "missing ']' in list index", NULL);
                    fputs(name, out);
                    continue;
                }
                char idx_buf[256];
                size_t ilen = (size_t)(p - idx_start);
                if (ilen >= sizeof(idx_buf)) ilen = sizeof(idx_buf) - 1;
                memcpy(idx_buf, idx_start, ilen);
                idx_buf[ilen] = '\0';
                const char *get_fn = (ref_type == VT_INT)  ? "_wind_list_get_int"
                                   : (ref_type == VT_FRAC) ? "_wind_list_get_frac"
                                                           : "_wind_list_get_str";
                /* Для str list оборачиваем в _wind_str_new — get_str возвращает
                 * указатель внутрь list, owner'ить его нельзя (use-after-free
                 * при последующем str_take/str_set). Свежий malloc безопасен. */
                if (ref_type == VT_STR) fputs("_wind_str_new(", out);
                fputs(get_fn, out);
                fputc('(', out);
                fputs(name, out);
                fputs(", ", out);
                translate_expr(idx_buf, out, line_num, original);
                fputc(')', out);
                if (ref_type == VT_STR) fputc(')', out);
                p++;
                continue;
            }
            if (lookup_array_size(name) > 0) {
                if (*p != '[') {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                        "'%s' is an array — index it with [...]", name);
                    wind_error(line_num, original, msg, "example: int.nums[0]");
                    fputs(name, out);
                    continue;
                }
                /* Берём кусок между совпадающими [ и ], отдаём в рекурсивный
                 * translate_expr. Это нужно потому что внутри индекса
                 * группировочные [..] должны остаться обычными. */
                p++;
                int depth = 1;
                const char *idx_start = p;
                while (*p && depth > 0) {
                    if (*p == '[') depth++;
                    else if (*p == ']') { depth--; if (depth == 0) break; }
                    p++;
                }
                if (*p != ']') {
                    wind_error(line_num, original,
                        "missing ']' in array index", NULL);
                    fputs(name, out);
                    continue;
                }
                char idx_buf[256];
                size_t ilen = (size_t)(p - idx_start);
                if (ilen >= sizeof(idx_buf)) ilen = sizeof(idx_buf) - 1;
                memcpy(idx_buf, idx_start, ilen);
                idx_buf[ilen] = '\0';
                fputs(name, out);
                fputc('[', out);
                translate_expr(idx_buf, out, line_num, original);
                fputc(']', out);
                p++;
                continue;
            }
            fputs(name, out);
            continue;
        }

        /* Вызов функции: IDENT(...) или NAMESPACE.IDENT(...). */
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            char ident[128];
            int i = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < 63) {
                ident[i++] = *p++;
            }
            ident[i] = '\0';

            /* Возможно дотированное имя: namespace.func — пробуем расширить. */
            int dotted = 0;
            if (*p == '.' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
                /* Только если "ident.tail" даст известную функцию.
                 * Без этой проверки мы бы съели '.' там где это не намерения. */
                const char *save_p = p;
                int save_i = i;
                ident[i++] = *p++;  /* съели '.' */
                while (*p && (isalnum((unsigned char)*p) || *p == '_')
                        && i < (int)sizeof(ident) - 1) {
                    ident[i++] = *p++;
                }
                ident[i] = '\0';
                if (!lookup_func(ident)) {
                    /* Откатываем — это была не функция, оставляем только первое имя. */
                    ident[save_i] = '\0';
                    p = save_p;
                    i = save_i;
                } else {
                    dotted = 1;
                }
            }

            /* Bare list / array / dict access: nums[0], words[i], cfg["port"].
             * Wind = убийца Python, юзер ожидает не писать int.list. каждый раз. */
            if (!dotted && *p == '[' && (is_list_var(ident) || lookup_array_size(ident) > 0 || is_dict_var(ident))) {
                int is_list = is_list_var(ident);
                int is_dict = is_dict_var(ident);
                VarType et   = is_dict ? dict_val_type(ident) : lookup_var(ident);
                VarType kt   = is_dict ? dict_key_type(ident) : VT_NONE;
                p++;
                int depth = 1;
                int in_q = 0;
                const char *idx_start = p;
                while (*p && depth > 0) {
                    if (*p == '"') in_q = !in_q;
                    else if (!in_q && *p == '[') depth++;
                    else if (!in_q && *p == ']') { depth--; if (depth == 0) break; }
                    p++;
                }
                if (*p != ']') {
                    wind_error(line_num, original,
                        "missing ']' in index", NULL);
                    fputs(ident, out);
                    continue;
                }
                char idx_buf[256];
                size_t ilen = (size_t)(p - idx_start);
                if (ilen >= sizeof(idx_buf)) ilen = sizeof(idx_buf) - 1;
                memcpy(idx_buf, idx_start, ilen);
                idx_buf[ilen] = '\0';
                if (is_dict) {
                    char kc = (kt == VT_INT) ? 'i' : (kt == VT_FRAC) ? 'f' : 's';
                    char vc = (et == VT_INT) ? 'i' : (et == VT_FRAC) ? 'f' : 's';
                    if (et == VT_STR) fputs("_wind_str_new(", out);
                    fprintf(out, "_wind_dict_get_%c%c(%s, ", kc, vc, ident);
                    /* Триммим ключ */
                    char *kk = ltrim(idx_buf); rtrim(kk);
                    if (kt == VT_STR) {
                        if (*kk == '"') fputs(kk, out);
                        else if (strncmp(kk, "str.", 4) == 0) fputs(kk + 4, out);
                        else translate_expr(kk, out, line_num, original);
                    } else {
                        translate_expr(kk, out, line_num, original);
                    }
                    fputc(')', out);
                    if (et == VT_STR) fputc(')', out);
                } else if (is_list) {
                    const char *get_fn = (et == VT_INT)  ? "_wind_list_get_int"
                                       : (et == VT_FRAC) ? "_wind_list_get_frac"
                                                         : "_wind_list_get_str";
                    if (et == VT_STR) fputs("_wind_str_new(", out);
                    fputs(get_fn, out);
                    fputc('(', out);
                    fputs(ident, out);
                    fputs(", ", out);
                    translate_expr(idx_buf, out, line_num, original);
                    fputc(')', out);
                    if (et == VT_STR) fputc(')', out);
                } else {
                    fputs(ident, out);
                    fputc('[', out);
                    translate_expr(idx_buf, out, line_num, original);
                    fputc(']', out);
                }
                p++;
                continue;
            }

            /* Bare method access: cfg.has("port") — для dict, возвращает int.
             * Парсим аргумент как обычную expr. */
            if (!dotted && *p == '.' && is_dict_var(ident)) {
                VarType kt = dict_key_type(ident);
                const char *qm = p + 1;
                char method[32];
                size_t mi = 0;
                while (*qm && (isalnum((unsigned char)*qm) || *qm == '_')
                        && mi < sizeof(method) - 1) {
                    method[mi++] = *qm++;
                }
                method[mi] = '\0';
                if (*qm == '(' && strcmp(method, "has") == 0) {
                    const char *fn = (kt == VT_INT) ? "_wind_dict_has_int"
                                  : (kt == VT_FRAC) ? "_wind_dict_has_frac"
                                                    : "_wind_dict_has_str";
                    fprintf(out, "%s(%s, ", fn, ident);
                    p = qm + 1;  /* после ( */
                    /* Дочитываем до балансной ) с учётом кавычек, и эмитим аргумент. */
                    int dpth = 1;
                    int inq = 0;
                    const char *as = p;
                    while (*p && dpth > 0) {
                        if (*p == '"') inq = !inq;
                        else if (!inq && *p == '(') dpth++;
                        else if (!inq && *p == ')') { dpth--; if (dpth == 0) break; }
                        p++;
                    }
                    char ab[256];
                    size_t al = (size_t)(p - as);
                    if (al >= sizeof(ab)) al = sizeof(ab) - 1;
                    memcpy(ab, as, al); ab[al] = '\0';
                    char *kk = ltrim(ab); rtrim(kk);
                    if (kt == VT_STR) {
                        if (*kk == '"') fputs(kk, out);
                        else if (strncmp(kk, "str.", 4) == 0) fputs(kk + 4, out);
                        else translate_expr(kk, out, line_num, original);
                    } else {
                        translate_expr(kk, out, line_num, original);
                    }
                    fputc(')', out);
                    if (*p == ')') p++;
                    continue;
                }
            }

            if (*p == '(') {
                const FuncSig *fn = lookup_func(ident);
                if (!fn && wind_import_mode) {
                    /* Fallback: внутри импорта intra-module вызовы.
                     * `helper(x)` в math.wnd ищется как `math.helper`. */
                    char qualified[160];
                    snprintf(qualified, sizeof(qualified), "%s.%s",
                             wind_import_ns_get(), ident);
                    fn = lookup_func(qualified);
                }
                if (!fn) {
                    char msg[200];
                    snprintf(msg, sizeof(msg), "unknown function '%s'", ident);
                    wind_error(line_num, original, msg,
                        "declare it with 'wnd.func' or import its namespace");
                    fputs(ident, out);  /* всё равно эмитим, чтоб C не падал нелепо */
                } else if (fn->return_type == VT_NONE) {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                        "function '%s' returns void and cannot be used in an expression", ident);
                    wind_error(line_num, original, msg,
                        "call it on its own line with 'wnd.run'");
                    fputs(fn->c_name, out);
                } else {
                    fputs(fn->c_name, out);  /* mangled C-имя */
                }
                continue;
            }
            /* Не вызов — пишем как есть (это может быть C-builtin типа rand). */
            (void)dotted;
            fwrite(start, 1, (size_t)(p - start), out);
            continue;
        }

        /* Запятая между цифрами → десятичная точка. */
        if (*p == ',' && p > src
                && isdigit((unsigned char)p[-1])
                && isdigit((unsigned char)p[1])) {
            fputc('.', out); p++;
            continue;
        }

        if (p[0] == '/' && p[1] == '%') { fputc('%', out); p += 2; continue; }
        if (*p == '[') { fputc('(', out); p++; continue; }
        if (*p == ']') { fputc(')', out); p++; continue; }

        /* Касты типов: int(...) / frac(...).
         * Если внутри str.X — используем atoi/atof.
         * Иначе обычный C-каст (int)/(double). */
        if (strncmp(p, "int(", 4) == 0) {
            const char *q = p + 4;
            while (*q && isspace((unsigned char)*q)) q++;
            if (strncmp(q, "str.", 4) == 0) fputs("atoi(", out);
            else                            fputs("(int)(", out);
            p += 4;
            continue;
        }
        if (strncmp(p, "frac(", 5) == 0) {
            const char *q = p + 5;
            while (*q && isspace((unsigned char)*q)) q++;
            /* Для str используем _wind_atof — он умеет запятую как разделитель. */
            if (strncmp(q, "str.", 4) == 0) fputs("_wind_atof(", out);
            else                            fputs("(double)(", out);
            p += 5;
            continue;
        }

        fputc(*p++, out);
    }
}

const char *find_compare_op(const char *s, char *op, int *op_len) {
    for (const char *p = s; *p; p++) {
        if ((p[0]=='>'||p[0]=='<'||p[0]=='='||p[0]=='!') && p[1]=='=') {
            op[0] = p[0]; op[1] = '='; op[2] = '\0';
            *op_len = 2; return p;
        }
        if (p[0] == '>' || p[0] == '<') {
            op[0] = p[0]; op[1] = '\0';
            *op_len = 1; return p;
        }
    }
    return NULL;
}

VarType guess_expr_type(const char *src) {
    if (strstr(src, "str.")) return VT_STR;
    if (strstr(src, "frac.")) return VT_FRAC;
    for (const char *p = src; *p && p[1]; p++) {
        if (*p == ',' && p > src
                && isdigit((unsigned char)p[-1])
                && isdigit((unsigned char)p[1])) {
            return VT_FRAC;
        }
    }
    return VT_INT;
}
