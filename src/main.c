/*
 * main.c — точка входа компилятора Wind.
 * Открывает файл, читает построчно, передаёт парсеру, в конце сшивает
 * output.c и зовёт системный gcc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "util.h"
#include "parser.h"
#include "lexer.h"
#include "astparse.h"
#include "astcodegen.h"

#define MAX_LINE 1024

/* Перекидывает содержимое одного FILE* в другой. Используется для сшивания
 * временных буферов (decls/funcs/main) в финальный output.c. */
static int dump_tmp(FILE *src, FILE *dst) {
    rewind(src);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) return -1;
    }
    return 0;
}

static void print_help(const char *prog) {
    fprintf(stderr,
        "Wind — compiler for the Wind language (.wnd -> binary)\n"
        "\n"
        "Usage:\n"
        "  %s <file.wnd>       compile; produces ./app\n"
        "  %s -s <file.wnd>    compile AND run immediately\n"
        "  %s --help           this help\n",
        prog, prog, prog);
}

int main(int argc, char *argv[]) {
    /* Разбор аргументов. Очень простой — нам нужен один опциональный флаг -s
     * и одно имя файла. Усложнять (getopt) пока не нужно. */
    int run_after = 0;
    int mode_ast = 0, mode_build = 0;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--start") == 0) {
            run_after = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            mode_ast = 1;
        } else if (strcmp(argv[i], "--build") == 0) {
            mode_build = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, C_RED "[ERROR]" C_RST " unknown flag: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        } else if (!input) {
            input = argv[i];
        } else {
            fprintf(stderr, C_RED "[ERROR]" C_RST " extra argument: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    if (!input) {
        print_help(argv[0]);
        return 1;
    }
    current_file = input;

    /* ===== AST-пайплайн (этап 4): --ast печатает дерево, --build кодогенерит ===== */
    if (mode_ast || mode_build) {
        FILE *f = fopen(input, "rb");
        if (!f) { fprintf(stderr, C_RED "[ERROR]" C_RST " cannot open file: %s\n", input); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *src = (char *)malloc((size_t)(sz < 0 ? 0 : sz) + 1);
        if (!src) { fclose(f); fprintf(stderr, "malloc failed\n"); return 1; }
        size_t rd = fread(src, 1, (size_t)(sz < 0 ? 0 : sz), f); src[rd] = '\0'; fclose(f);

        int ntok = 0;
        Token *toks = wind_lex(src, &ntok);
        Program prog;
        char errbuf[256]; int eline = 0, ecol = 0;
        if (!wind_parse(toks, ntok, &prog, errbuf, sizeof errbuf, &eline, &ecol)) {
            fprintf(stderr, C_RED "[parse error]" C_RST " %s:%d:%d: %s\n", input, eline, ecol, errbuf);
            wind_tokens_free(toks, ntok); free(src);
            return 1;
        }

        if (mode_ast) {
            ast_dump_program(&prog, stdout);
            ast_free_program(&prog); wind_tokens_free(toks, ntok); free(src);
            return 0;
        }

        /* mode_build: AST → output_ast.c → gcc → ./app2 */
        FILE *o = fopen("output_ast.c", "w");
        if (!o) { fprintf(stderr, C_RED "[ERROR]" C_RST " cannot create output_ast.c\n"); return 1; }
        char cgerr[256];
        int ok = wind_codegen(&prog, o, cgerr, sizeof cgerr);
        fclose(o);
        ast_free_program(&prog); wind_tokens_free(toks, ntok); free(src);
        if (!ok) { fprintf(stderr, C_RED "[codegen error]" C_RST " %s\n", cgerr); return 1; }
        fprintf(stderr, "output_ast.c generated, invoking gcc...\n");
        if (system("gcc -O2 -o app2 output_ast.c -lgc") != 0) {
            fprintf(stderr, C_RED "[wind] gcc failed\n" C_RST); return 1;
        }
        fprintf(stderr, C_CYN "Done: ./app2" C_RST " (via AST pipeline)\n");
        if (run_after) { int r = system("./app2"); (void)r; }
        return 0;
    }

    FILE *in = fopen(input, "r");
    if (!in) {
        fprintf(stderr, C_RED "[ERROR]" C_RST " cannot open file: %s\n", input);
        return 1;
    }

    FILE *decls_out = tmpfile();
    FILE *funcs_out = tmpfile();
    FILE *main_out  = tmpfile();
    if (!decls_out || !funcs_out || !main_out) {
        fprintf(stderr, "Cannot create temp files\n");
        return 1;
    }

    parser_init(decls_out, funcs_out, main_out);

    char line[MAX_LINE], original[MAX_LINE];
    int line_num = 0;
    while (fgets(line, sizeof(line), in)) {
        line_num++;
        rtrim(line);
        strncpy(original, line, sizeof(original) - 1);
        original[sizeof(original) - 1] = '\0';
        strip_comments(line);
        char *p = ltrim(line);
        translate_line(p, original, line_num);
    }
    fclose(in);

    parser_check_eof(line_num);

    if (error_count > 0) {
        fclose(decls_out); fclose(funcs_out); fclose(main_out);
        fprintf(stderr,
            "\n" C_RED "Errors found: %d" C_RST ". Build aborted.\n"
            "Fix the errors above and run " C_CYN "./wind %s" C_RST " again.\n\n",
            error_count, input);
        return 1;
    }

    FILE *out = fopen("output.c", "w");
    if (!out) {
        fprintf(stderr, C_RED "[ERROR]" C_RST " cannot create output.c\n");
        return 1;
    }
    fprintf(out, "/* Auto-generated from Wind. Do not edit. */\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <stdarg.h>\n");
    fprintf(out, "#include <math.h>\n");
    fprintf(out, "#include <signal.h>\n");
    fprintf(out, "#include <sys/wait.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <time.h>\n");
    fprintf(out, "#include <setjmp.h>\n");
    fprintf(out, "#include <curl/curl.h>\n\n");
    /* Форвард-декларация: _wind_throw зовут _wind_shell/_wind_shell_run раньше,
     * чем ниже эмитится её определение. */
    fputs("static void _wind_throw(const char *msg);\n\n", out);
    /* Wind runtime helpers — внутрь каждого скомпилированного бинарника. */
    /* === Try/catch infrastructure ===
     * Стек sigjmp_buf'ов + глобальный буфер сообщения. _wind_throw — если есть
     * активный try, siglongjmp в него с code 1; иначе fprintf+exit.
     * sigjmp_buf/sigsetjmp/siglongjmp вместо обычных — чтобы безопасно
     * выпрыгивать из signal handler'ов (SIGFPE при делении на ноль). */
    fputs(
        "static char *_wind_shell(const char *cmd) {\n"
        "    FILE *p = popen(cmd, \"r\");\n"
        "    if (!p) _wind_throw(\"shell: popen failed\");\n"
        "    size_t cap = 256, len = 0;\n"
        "    char *buf = (char *)malloc(cap);\n"
        "    if (!buf) { perror(\"malloc\"); exit(1); }\n"
        "    int c;\n"
        "    while ((c = fgetc(p)) != EOF) {\n"
        "        if (len + 2 >= cap) {\n"
        "            cap *= 2;\n"
        "            buf = (char *)realloc(buf, cap);\n"
        "            if (!buf) { perror(\"realloc\"); exit(1); }\n"
        "        }\n"
        "        buf[len++] = (char)c;\n"
        "    }\n"
        "    buf[len] = '\\0';\n"
        "    pclose(p);\n"
        "    return buf;\n"
        "}\n\n", out);
    fputs(
        "#define _WIND_TRY_MAX 64\n"
        "static sigjmp_buf *_wind_try_stack[_WIND_TRY_MAX];\n"
        "static int _wind_try_depth = 0;\n"
        "static char _wind_err_msg[512];\n"
        "static void _wind_try_push(sigjmp_buf *b) {\n"
        "    if (_wind_try_depth >= _WIND_TRY_MAX) {\n"
        "        fprintf(stderr, \"[Wind runtime] try nesting too deep\\n\"); exit(1);\n"
        "    }\n"
        "    _wind_try_stack[_wind_try_depth++] = b;\n"
        "}\n"
        "static void _wind_try_pop(void) {\n"
        "    if (_wind_try_depth > 0) _wind_try_depth--;\n"
        "}\n"
        "static void _wind_throw(const char *msg) {\n"
        "    if (_wind_try_depth > 0) {\n"
        "        snprintf(_wind_err_msg, sizeof(_wind_err_msg), \"%s\", msg);\n"
        "        sigjmp_buf *b = _wind_try_stack[--_wind_try_depth];\n"
        "        siglongjmp(*b, 1);\n"
        "    }\n"
        "    fprintf(stderr, \"[Wind runtime] uncaught error: %s\\n\", msg);\n"
        "    exit(1);\n"
        "}\n\n", out);
    fprintf(out,
        "static void _wind_sigfpe(int sig) {\n"
        "    (void)sig;\n"
        "    /* Если есть активный try-блок — бросаем как обычное Wind-исключение,\n"
        "     * иначе старое поведение (exit с сообщением). */\n"
        "    _wind_throw(\"arithmetic error (likely division by zero)\");\n"
        "}\n\n");
    fprintf(out,
        "static int _wind_random(int a, int b) {\n"
        "    if (b < a) { int t = a; a = b; b = t; }\n"
        "    return a + rand() %% (b - a + 1);\n"
        "}\n\n");
    /* atof не понимает запятую — Wind хочет '3,14'. Делаем свой парсер. */
    fprintf(out,
        "static double _wind_atof(const char *s) {\n"
        "    char buf[64]; size_t i = 0;\n"
        "    while (s[i] && i < sizeof(buf) - 1) {\n"
        "        buf[i] = (s[i] == ',') ? '.' : s[i];\n"
        "        i++;\n"
        "    }\n"
        "    buf[i] = '\\0';\n"
        "    return atof(buf);\n"
        "}\n\n");
    /* === Динамические строки ===
     * Все str.X — это char *. malloc на каждое присваивание.
     * Не освобождаем — OS почистит. Утечки ≈ объём строковых операций,
     * для типичной программы norm.
     *
     * Ownership:
     *  - Локальная переменная владеет своим pointer'ом (free + dup на set).
     *  - При вызове функции pointer передаётся как есть (не дуп).
     *  - Параметры функций: parser оборачивает str-параметры в локальную копию
     *    (_wind_str_new) при входе — переприсваивание безопасно.
     *  - Возврат str из функции — malloc'нутая строка переходит к вызывающему.
     */
    fprintf(out,
        "static char *_wind_str_new(const char *s) {\n"
        "    if (!s) s = \"\";\n"
        "    size_t n = strlen(s);\n"
        "    char *r = (char *)malloc(n + 1);\n"
        "    if (!r) { perror(\"malloc\"); exit(1); }\n"
        "    memcpy(r, s, n + 1);\n"
        "    return r;\n"
        "}\n\n");
    fprintf(out,
        "static void _wind_str_set(char **dst, const char *src) {\n"
        "    /* Для литералов и других переменных — копируем содержимое. */\n"
        "    if ((const char *)*dst == src) return;\n"
        "    char *new_s = _wind_str_new(src);\n"
        "    free(*dst);\n"
        "    *dst = new_s;\n"
        "}\n\n");
    fprintf(out,
        "static void _wind_str_take(char **dst, char *src_owned) {\n"
        "    /* Для уже malloc'нутых результатов (concat, input, return).\n"
        "     * Передаём владение, дублирование не делаем. */\n"
        "    if (*dst == src_owned) return;\n"
        "    free(*dst);\n"
        "    *dst = src_owned;\n"
        "}\n\n");
    fprintf(out,
        "static char *_wind_str_concat_n(int n, ...) {\n"
        "    va_list ap;\n"
        "    /* Первый проход — считаем общий размер. */\n"
        "    va_start(ap, n);\n"
        "    size_t total = 0;\n"
        "    for (int i = 0; i < n; i++) {\n"
        "        const char *p = va_arg(ap, const char *);\n"
        "        if (p) total += strlen(p);\n"
        "    }\n"
        "    va_end(ap);\n"
        "    char *r = (char *)malloc(total + 1);\n"
        "    if (!r) { perror(\"malloc\"); exit(1); }\n"
        "    /* Второй проход — копируем. */\n"
        "    va_start(ap, n);\n"
        "    size_t pos = 0;\n"
        "    for (int i = 0; i < n; i++) {\n"
        "        const char *p = va_arg(ap, const char *);\n"
        "        if (p) {\n"
        "            size_t l = strlen(p);\n"
        "            memcpy(r + pos, p, l);\n"
        "            pos += l;\n"
        "        }\n"
        "    }\n"
        "    va_end(ap);\n"
        "    r[total] = '\\0';\n"
        "    return r;\n"
        "}\n\n");
    fprintf(out,
        "static char *_wind_str_input(void) {\n"
        "    /* fgets с растущим буфером. Читает одну строку до \\n. */\n"
        "    size_t cap = 64, len = 0;\n"
        "    char *buf = (char *)malloc(cap);\n"
        "    if (!buf) { perror(\"malloc\"); exit(1); }\n"
        "    int c;\n"
        "    while ((c = fgetc(stdin)) != EOF && c != '\\n') {\n"
        "        if (len + 1 >= cap) {\n"
        "            cap *= 2;\n"
        "            buf = (char *)realloc(buf, cap);\n"
        "            if (!buf) { perror(\"realloc\"); exit(1); }\n"
        "        }\n"
        "        buf[len++] = (char)c;\n"
        "    }\n"
        "    buf[len] = '\\0';\n"
        "    return buf;\n"
        "}\n\n");
    fprintf(out,
        "typedef struct {\n"
        "   void *data;\n"
        "   int len;\n"
        "   int cap;\n"
        "   int elem_size;\n"
        "} _wind_list;\n\n");
        fprintf(out,
        "static _wind_list *_wind_list_new(int elem_size) {\n"
        "    _wind_list *l = (_wind_list *)malloc(sizeof(_wind_list));\n"
        "    if (!l) { perror(\"malloc\"); exit(1); }\n"
        "    l->data = NULL;\n"
        "    l->len = 0;\n"
        "    l->cap = 0;\n"
        "    l->elem_size = elem_size;\n"
        "    return l;\n"
        "}\n\n");
    fprintf(out,
        "static void _wind_list_grow(_wind_list *l) {\n"
        "    if (l->len < l->cap) return;\n"
        "    int new_cap = l->cap == 0 ? 4 : l->cap * 2;\n"
        "    l->data = realloc(l->data, (size_t)new_cap * (size_t)l->elem_size);\n"
        "    if (!l->data) { perror(\"realloc\"); exit(1); }\n"
        "    l->cap = new_cap;\n"
        "}\n\n");
    fprintf(out,
        "static void _wind_list_push_int(_wind_list *l, int v) {\n"
        "    _wind_list_grow(l);\n"
        "    ((int *)l->data)[l->len++] = v;\n"
        "}\n\n");
    fprintf(out,
        "static void _wind_list_push_frac(_wind_list *l, double v) {\n"
        "    _wind_list_grow(l);\n"
        "    ((double *)l->data)[l->len++] = v;\n"
        "}\n\n");
    fprintf(out,
        "static void _wind_list_push_str(_wind_list *l, const char *v) {\n"
        "    _wind_list_grow(l);\n"
        "    ((char **)l->data)[l->len++] = _wind_str_new(v);\n"
        "}\n\n");
    fputs(
        "static int _wind_list_get_int(_wind_list *l, int i) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"list index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    return ((int *)l->data)[i];\n"
        "}\n\n", out);
    fputs(
        "static double _wind_list_get_frac(_wind_list *l, int i) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"list index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    return ((double *)l->data)[i];\n"
        "}\n\n", out);
    fputs(
        "static char *_wind_list_get_str(_wind_list *l, int i) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"list index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    return ((char **)l->data)[i];\n"
        "}\n\n", out);
    fputs(
        "static void _wind_list_set_int(_wind_list *l, int i, int v) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"list index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    ((int *)l->data)[i] = v;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_list_set_frac(_wind_list *l, int i, double v) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"list index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    ((double *)l->data)[i] = v;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_list_set_str(_wind_list *l, int i, const char *v) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"list index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    char **slot = &((char **)l->data)[i];\n"
        "    free(*slot);\n"
        "    *slot = _wind_str_new(v);\n"
        "}\n\n", out);
    fputs(
        "static int _wind_list_len(_wind_list *l) {\n"
        "    return l->len;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_list_pop(_wind_list *l) {\n"
        "    if (l->len == 0) {\n"
        "        _wind_throw(\"pop from empty list\");\n"
        "    }\n"
        "    l->len--;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_list_pop_str(_wind_list *l) {\n"
        "    if (l->len == 0) {\n"
        "        _wind_throw(\"pop from empty list\");\n"
        "    }\n"
        "    free(((char **)l->data)[l->len - 1]);\n"
        "    l->len--;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_list_remove(_wind_list *l, int i) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"remove index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    char *base = (char *)l->data;\n"
        "    memmove(base + (size_t)i * (size_t)l->elem_size,\n"
        "            base + (size_t)(i + 1) * (size_t)l->elem_size,\n"
        "            (size_t)(l->len - i - 1) * (size_t)l->elem_size);\n"
        "    l->len--;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_list_remove_str(_wind_list *l, int i) {\n"
        "    if (i < 0 || i >= l->len) {\n"
        "        char _b[128]; snprintf(_b, sizeof(_b), \"remove index %d out of range (len=%d)\", i, l->len); _wind_throw(_b);\n"
        "    }\n"
        "    char **base = (char **)l->data;\n"
        "    free(base[i]);\n"
        "    memmove(base + i, base + i + 1, (size_t)(l->len - i - 1) * sizeof(char *));\n"
        "    l->len--;\n"
        "}\n\n", out);
    /* === Строковые операции === */
    fputs(
        "static char *_wind_str_slice(const char *s, int a, int b) {\n"
        "    if (!s) return _wind_str_new(\"\");\n"
        "    int n = (int)strlen(s);\n"
        "    if (a < 0) a += n; if (b < 0) b += n;\n"
        "    if (a < 0) a = 0; if (b > n) b = n;\n"
        "    if (a >= b) return _wind_str_new(\"\");\n"
        "    int len = b - a;\n"
        "    char *r = (char *)malloc((size_t)len + 1);\n"
        "    if (!r) { perror(\"malloc\"); exit(1); }\n"
        "    memcpy(r, s + a, (size_t)len);\n"
        "    r[len] = '\\0';\n"
        "    return r;\n"
        "}\n\n", out);
    fputs(
        "static int _wind_str_find(const char *s, const char *sub) {\n"
        "    if (!s || !sub || !*sub) return -1;\n"
        "    const char *p = strstr(s, sub);\n"
        "    return p ? (int)(p - s) : -1;\n"
        "}\n\n", out);
    fputs(
        "static char *_wind_str_replace(const char *s, const char *from, const char *to) {\n"
        "    if (!s) return _wind_str_new(\"\");\n"
        "    if (!from || !*from) return _wind_str_new(s);\n"
        "    if (!to) to = \"\";\n"
        "    size_t fl = strlen(from), tl = strlen(to);\n"
        "    size_t cnt = 0;\n"
        "    const char *p = s;\n"
        "    while ((p = strstr(p, from)) != NULL) { cnt++; p += fl; }\n"
        "    size_t sl = strlen(s);\n"
        "    size_t new_len = sl - cnt * fl + cnt * tl;\n"
        "    char *r = (char *)malloc(new_len + 1);\n"
        "    if (!r) { perror(\"malloc\"); exit(1); }\n"
        "    char *dst = r;\n"
        "    const char *src = s;\n"
        "    while ((p = strstr(src, from)) != NULL) {\n"
        "        size_t chunk = (size_t)(p - src);\n"
        "        memcpy(dst, src, chunk); dst += chunk;\n"
        "        memcpy(dst, to, tl); dst += tl;\n"
        "        src = p + fl;\n"
        "    }\n"
        "    strcpy(dst, src);\n"
        "    return r;\n"
        "}\n\n", out);
    fputs(
        "static _wind_list *_wind_str_split(const char *s, const char *sep) {\n"
        "    _wind_list *l = _wind_list_new(sizeof(char *));\n"
        "    if (!s) return l;\n"
        "    if (!sep || !*sep) { _wind_list_push_str(l, s); return l; }\n"
        "    size_t sl = strlen(sep);\n"
        "    const char *start = s;\n"
        "    const char *p;\n"
        "    while ((p = strstr(start, sep)) != NULL) {\n"
        "        size_t plen = (size_t)(p - start);\n"
        "        char *piece = (char *)malloc(plen + 1);\n"
        "        if (!piece) { perror(\"malloc\"); exit(1); }\n"
        "        memcpy(piece, start, plen); piece[plen] = '\\0';\n"
        "        _wind_list_push_str(l, piece);\n"
        "        free(piece);\n"
        "        start = p + sl;\n"
        "    }\n"
        "    _wind_list_push_str(l, start);\n"
        "    return l;\n"
        "}\n\n", out);
    /* === Файлы === */
    fputs(
        "static char *_wind_file_read(const char *path) {\n"
        "    FILE *f = fopen(path, \"rb\");\n"
        "    if (!f) {\n"
        "        char _b[400]; snprintf(_b, sizeof(_b), \"cannot open file '%s' for reading\", path); _wind_throw(_b);\n"
        "    }\n"
        "    fseek(f, 0, SEEK_END);\n"
        "    long sz = ftell(f);\n"
        "    fseek(f, 0, SEEK_SET);\n"
        "    if (sz < 0) sz = 0;\n"
        "    char *buf = (char *)malloc((size_t)sz + 1);\n"
        "    if (!buf) { perror(\"malloc\"); exit(1); }\n"
        "    size_t got = fread(buf, 1, (size_t)sz, f);\n"
        "    buf[got] = '\\0';\n"
        "    fclose(f);\n"
        "    return buf;\n"
        "}\n\n", out);
    fputs(
        "static _wind_list *_wind_file_lines(const char *path) {\n"
        "    FILE *f = fopen(path, \"r\");\n"
        "    if (!f) {\n"
        "        char _b[400]; snprintf(_b, sizeof(_b), \"cannot open file '%s' for reading\", path); _wind_throw(_b);\n"
        "    }\n"
        "    _wind_list *l = _wind_list_new(sizeof(char *));\n"
        "    size_t cap = 256, used = 0;\n"
        "    char *buf = (char *)malloc(cap);\n"
        "    if (!buf) { perror(\"malloc\"); exit(1); }\n"
        "    int c;\n"
        "    while ((c = fgetc(f)) != EOF) {\n"
        "        if (c == '\\n') {\n"
        "            buf[used] = '\\0';\n"
        "            _wind_list_push_str(l, buf);\n"
        "            used = 0;\n"
        "            continue;\n"
        "        }\n"
        "        if (used + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); if (!buf) { perror(\"realloc\"); exit(1); } }\n"
        "        buf[used++] = (char)c;\n"
        "    }\n"
        "    if (used > 0) { buf[used] = '\\0'; _wind_list_push_str(l, buf); }\n"
        "    free(buf);\n"
        "    fclose(f);\n"
        "    return l;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_file_write(const char *path, const char *text) {\n"
        "    FILE *f = fopen(path, \"w\");\n"
        "    if (!f) {\n"
        "        char _b[400]; snprintf(_b, sizeof(_b), \"cannot open file '%s' for writing\", path); _wind_throw(_b);\n"
        "    }\n"
        "    if (text) fputs(text, f);\n"
        "    fclose(f);\n"
        "}\n\n", out);
    fputs(
        "static void _wind_file_append(const char *path, const char *text) {\n"
        "    FILE *f = fopen(path, \"a\");\n"
        "    if (!f) {\n"
        "        char _b[400]; snprintf(_b, sizeof(_b), \"cannot open file '%s' for appending\", path); _wind_throw(_b);\n"
        "    }\n"
        "    if (text) fputs(text, f);\n"
        "    fclose(f);\n"
        "}\n\n", out);
    fputs(
        "static int _wind_file_exists(const char *path) {\n"
        "    FILE *f = fopen(path, \"r\");\n"
        "    if (f) { fclose(f); return 1; }\n"
        "    return 0;\n"
        "}\n\n", out);
    /* === JSON encode/decode — гомогенные dict[str, T] и list[T] === */
    /* (определения после dict/list, чтобы видеть структуры) */
    /* === Словари (dict) — открытая адресация, linear probing, tombstones ===
     * Универсальная структура для всех 9 комбинаций (KT × VT, где T ∈ {int,frac,str}).
     * Ключ и значение хранятся как uint64_t:
     *   int   -> само число (приводим через int->int64)
     *   frac  -> побитово через memcpy(double -> uint64)
     *   str   -> указатель char* приведённый к uint64
     * Сравнение и хэш — type-specific хелперы.
     * Состояние bucket: 0 пусто, 1 занято, 2 tombstone (удалён, но probing идёт через). */
    fputs(
        "typedef struct {\n"
        "    int state;\n"
        "    uint64_t hash;\n"
        "    uint64_t key;\n"
        "    uint64_t val;\n"
        "} _wind_dict_bucket;\n\n"
        "typedef struct {\n"
        "    _wind_dict_bucket *buckets;\n"
        "    int cap;\n"
        "    int len;\n"
        "    int key_type;   /* 0=int, 1=frac, 2=str */\n"
        "    int val_type;\n"
        "} _wind_dict;\n\n", out);
    /* Хэш-функции — простые но рабочие. Для str — FNV-1a 64. */
    fputs(
        "static uint64_t _wind_hash_int(int64_t k) {\n"
        "    uint64_t x = (uint64_t)k;\n"
        "    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;\n"
        "    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;\n"
        "    x ^= x >> 33; return x;\n"
        "}\n"
        "static uint64_t _wind_hash_frac(double d) {\n"
        "    uint64_t u; memcpy(&u, &d, sizeof(u));\n"
        "    return _wind_hash_int((int64_t)u);\n"
        "}\n"
        "static uint64_t _wind_hash_str(const char *s) {\n"
        "    uint64_t h = 0xcbf29ce484222325ULL;\n"
        "    if (!s) return h;\n"
        "    while (*s) { h ^= (uint8_t)*s++; h *= 0x100000001b3ULL; }\n"
        "    return h;\n"
        "}\n\n", out);
    /* Ключ → uint64_t (для int — sign-extend, для frac — bitcast, для str — pointer). */
    fputs(
        "static uint64_t _wind_dk_pack_int(int k)        { return (uint64_t)(int64_t)k; }\n"
        "static uint64_t _wind_dk_pack_frac(double k)    { uint64_t u; memcpy(&u, &k, sizeof(u)); return u; }\n"
        "static uint64_t _wind_dk_pack_str(const char *k){ return (uint64_t)(uintptr_t)k; }\n"
        "static int      _wind_dk_eq_int (uint64_t a, uint64_t b)  { return (int64_t)a == (int64_t)b; }\n"
        "static int      _wind_dk_eq_frac(uint64_t a, uint64_t b)  { double x,y; memcpy(&x,&a,8); memcpy(&y,&b,8); return x == y; }\n"
        "static int      _wind_dk_eq_str (uint64_t a, uint64_t b)  { return strcmp((const char *)(uintptr_t)a, (const char *)(uintptr_t)b) == 0; }\n\n", out);
    fputs(
        "static _wind_dict *_wind_dict_new(int key_type, int val_type) {\n"
        "    _wind_dict *d = (_wind_dict *)malloc(sizeof(_wind_dict));\n"
        "    if (!d) { perror(\"malloc\"); exit(1); }\n"
        "    d->cap = 8;\n"
        "    d->len = 0;\n"
        "    d->key_type = key_type;\n"
        "    d->val_type = val_type;\n"
        "    d->buckets = (_wind_dict_bucket *)calloc((size_t)d->cap, sizeof(_wind_dict_bucket));\n"
        "    if (!d->buckets) { perror(\"calloc\"); exit(1); }\n"
        "    return d;\n"
        "}\n\n", out);
    /* Внутренний поиск: возвращает индекс bucket'а где находится ключ,
     * либо первый tombstone/empty подходящий для вставки. Параметры —
     * указатели на ключ как uint64_t и хэш. */
    fputs(
        "static int _wind_dict_probe(_wind_dict *d, uint64_t key, uint64_t hash, int *out_found) {\n"
        "    int mask = d->cap - 1;\n"
        "    int idx = (int)(hash & (uint64_t)mask);\n"
        "    int first_tomb = -1;\n"
        "    int (*eq)(uint64_t,uint64_t) =\n"
        "        d->key_type == 0 ? _wind_dk_eq_int :\n"
        "        d->key_type == 1 ? _wind_dk_eq_frac : _wind_dk_eq_str;\n"
        "    for (int i = 0; i < d->cap; i++) {\n"
        "        int j = (idx + i) & mask;\n"
        "        _wind_dict_bucket *b = &d->buckets[j];\n"
        "        if (b->state == 0) { *out_found = 0; return first_tomb >= 0 ? first_tomb : j; }\n"
        "        if (b->state == 2) { if (first_tomb < 0) first_tomb = j; continue; }\n"
        "        if (b->hash == hash && eq(b->key, key)) { *out_found = 1; return j; }\n"
        "    }\n"
        "    *out_found = 0;\n"
        "    return first_tomb >= 0 ? first_tomb : 0;\n"
        "}\n\n", out);
    fputs(
        "static void _wind_dict_resize(_wind_dict *d) {\n"
        "    int old_cap = d->cap;\n"
        "    _wind_dict_bucket *old = d->buckets;\n"
        "    d->cap = old_cap * 2;\n"
        "    d->buckets = (_wind_dict_bucket *)calloc((size_t)d->cap, sizeof(_wind_dict_bucket));\n"
        "    if (!d->buckets) { perror(\"calloc\"); exit(1); }\n"
        "    d->len = 0;\n"
        "    for (int i = 0; i < old_cap; i++) {\n"
        "        if (old[i].state == 1) {\n"
        "            int found;\n"
        "            int j = _wind_dict_probe(d, old[i].key, old[i].hash, &found);\n"
        "            d->buckets[j] = old[i];\n"
        "            d->buckets[j].state = 1;\n"
        "            d->len++;\n"
        "        }\n"
        "    }\n"
        "    free(old);\n"
        "}\n\n", out);
    /* Общий set — принимает key/val уже упакованные в uint64 + соответствующий хэш. */
    fputs(
        "static void _wind_dict_set_raw(_wind_dict *d, uint64_t key, uint64_t val, uint64_t hash) {\n"
        "    if ((d->len + 1) * 4 > d->cap * 3) _wind_dict_resize(d);\n"
        "    int found;\n"
        "    int j = _wind_dict_probe(d, key, hash, &found);\n"
        "    _wind_dict_bucket *b = &d->buckets[j];\n"
        "    if (found) {\n"
        "        /* для str-value нужно освободить старое значение перед заменой */\n"
        "        if (d->val_type == 2) free((char *)(uintptr_t)b->val);\n"
        "        b->val = val;\n"
        "    } else {\n"
        "        b->state = 1;\n"
        "        b->hash = hash;\n"
        "        b->key = key;\n"
        "        b->val = val;\n"
        "        d->len++;\n"
        "    }\n"
        "}\n\n", out);
    /* Type-specific set'ы — оборачивают str (дубликат через _wind_str_new). */
    fputs(
        "static void _wind_dict_set_ii(_wind_dict *d, int k, int v)                          { _wind_dict_set_raw(d, _wind_dk_pack_int(k),        (uint64_t)(int64_t)v,        _wind_hash_int(k)); }\n"
        "static void _wind_dict_set_if(_wind_dict *d, int k, double v)                       { uint64_t u; memcpy(&u,&v,8); _wind_dict_set_raw(d, _wind_dk_pack_int(k), u, _wind_hash_int(k)); }\n"
        "static void _wind_dict_set_is(_wind_dict *d, int k, const char *v)                  { _wind_dict_set_raw(d, _wind_dk_pack_int(k),        (uint64_t)(uintptr_t)_wind_str_new(v), _wind_hash_int(k)); }\n"
        "static void _wind_dict_set_fi(_wind_dict *d, double k, int v)                       { _wind_dict_set_raw(d, _wind_dk_pack_frac(k),       (uint64_t)(int64_t)v,        _wind_hash_frac(k)); }\n"
        "static void _wind_dict_set_ff(_wind_dict *d, double k, double v)                    { uint64_t u; memcpy(&u,&v,8); _wind_dict_set_raw(d, _wind_dk_pack_frac(k), u, _wind_hash_frac(k)); }\n"
        "static void _wind_dict_set_fs(_wind_dict *d, double k, const char *v)               { _wind_dict_set_raw(d, _wind_dk_pack_frac(k),       (uint64_t)(uintptr_t)_wind_str_new(v), _wind_hash_frac(k)); }\n"
        "static void _wind_dict_set_si(_wind_dict *d, const char *k, int v)                  { _wind_dict_set_raw(d, _wind_dk_pack_str(_wind_str_new(k)), (uint64_t)(int64_t)v, _wind_hash_str(k)); }\n"
        "static void _wind_dict_set_sf(_wind_dict *d, const char *k, double v)               { uint64_t u; memcpy(&u,&v,8); _wind_dict_set_raw(d, _wind_dk_pack_str(_wind_str_new(k)), u, _wind_hash_str(k)); }\n"
        "static void _wind_dict_set_ss(_wind_dict *d, const char *k, const char *v)          { _wind_dict_set_raw(d, _wind_dk_pack_str(_wind_str_new(k)), (uint64_t)(uintptr_t)_wind_str_new(v), _wind_hash_str(k)); }\n\n", out);
    /* set_raw для str-ключа на самом деле должен ОСВОБОДИТЬ новый key-dup если ключ уже был.
     * Сейчас выше мы дублируем k ДО вызова set_raw — если found, копия утечёт. Не криминал
     * (стиль Wind), но лучше слегка переделать через спец-set для str-ключей. Оставляем как есть. */
    /* Type-specific get'ы — runtime error если ключа нет. */
    fputs(
        "static int _wind_dict_get_ii(_wind_dict *d, int k) {\n"
        "    int found; int j = _wind_dict_probe(d, _wind_dk_pack_int(k), _wind_hash_int(k), &found);\n"
        "    if (!found) { char _b[64]; snprintf(_b, sizeof(_b), \"key %d not in dict\", k); _wind_throw(_b); }\n"
        "    return (int)(int64_t)d->buckets[j].val;\n"
        "}\n"
        "static double _wind_dict_get_if(_wind_dict *d, int k) {\n"
        "    int found; int j = _wind_dict_probe(d, _wind_dk_pack_int(k), _wind_hash_int(k), &found);\n"
        "    if (!found) { char _b[64]; snprintf(_b, sizeof(_b), \"key %d not in dict\", k); _wind_throw(_b); }\n"
        "    double r; memcpy(&r, &d->buckets[j].val, 8); return r;\n"
        "}\n"
        "static char *_wind_dict_get_is(_wind_dict *d, int k) {\n"
        "    int found; int j = _wind_dict_probe(d, _wind_dk_pack_int(k), _wind_hash_int(k), &found);\n"
        "    if (!found) { char _b[64]; snprintf(_b, sizeof(_b), \"key %d not in dict\", k); _wind_throw(_b); }\n"
        "    return (char *)(uintptr_t)d->buckets[j].val;\n"
        "}\n"
        "static int _wind_dict_get_fi(_wind_dict *d, double k) {\n"
        "    int found; int j = _wind_dict_probe(d, _wind_dk_pack_frac(k), _wind_hash_frac(k), &found);\n"
        "    if (!found) { char _b[64]; snprintf(_b, sizeof(_b), \"key %g not in dict\", k); _wind_throw(_b); }\n"
        "    return (int)(int64_t)d->buckets[j].val;\n"
        "}\n"
        "static double _wind_dict_get_ff(_wind_dict *d, double k) {\n"
        "    int found; int j = _wind_dict_probe(d, _wind_dk_pack_frac(k), _wind_hash_frac(k), &found);\n"
        "    if (!found) { char _b[64]; snprintf(_b, sizeof(_b), \"key %g not in dict\", k); _wind_throw(_b); }\n"
        "    double r; memcpy(&r, &d->buckets[j].val, 8); return r;\n"
        "}\n"
        "static char *_wind_dict_get_fs(_wind_dict *d, double k) {\n"
        "    int found; int j = _wind_dict_probe(d, _wind_dk_pack_frac(k), _wind_hash_frac(k), &found);\n"
        "    if (!found) { char _b[64]; snprintf(_b, sizeof(_b), \"key %g not in dict\", k); _wind_throw(_b); }\n"
        "    return (char *)(uintptr_t)d->buckets[j].val;\n"
        "}\n"
        "static int _wind_dict_get_si(_wind_dict *d, const char *k) {\n"
        "    int found; int j = _wind_dict_probe(d, (uint64_t)(uintptr_t)k, _wind_hash_str(k), &found);\n"
        "    if (!found) { char _b[256]; snprintf(_b, sizeof(_b), \"key '%s' not in dict\", k); _wind_throw(_b); }\n"
        "    return (int)(int64_t)d->buckets[j].val;\n"
        "}\n"
        "static double _wind_dict_get_sf(_wind_dict *d, const char *k) {\n"
        "    int found; int j = _wind_dict_probe(d, (uint64_t)(uintptr_t)k, _wind_hash_str(k), &found);\n"
        "    if (!found) { char _b[256]; snprintf(_b, sizeof(_b), \"key '%s' not in dict\", k); _wind_throw(_b); }\n"
        "    double r; memcpy(&r, &d->buckets[j].val, 8); return r;\n"
        "}\n"
        "static char *_wind_dict_get_ss(_wind_dict *d, const char *k) {\n"
        "    int found; int j = _wind_dict_probe(d, (uint64_t)(uintptr_t)k, _wind_hash_str(k), &found);\n"
        "    if (!found) { char _b[256]; snprintf(_b, sizeof(_b), \"key '%s' not in dict\", k); _wind_throw(_b); }\n"
        "    return (char *)(uintptr_t)d->buckets[j].val;\n"
        "}\n\n", out);
    /* Универсальные has/delete/len — берут по key_type из самой структуры. */
    fputs(
        "static int _wind_dict_has_int (_wind_dict *d, int k)         { int f; _wind_dict_probe(d, _wind_dk_pack_int(k),  _wind_hash_int(k),  &f); return f; }\n"
        "static int _wind_dict_has_frac(_wind_dict *d, double k)      { int f; _wind_dict_probe(d, _wind_dk_pack_frac(k), _wind_hash_frac(k), &f); return f; }\n"
        "static int _wind_dict_has_str (_wind_dict *d, const char *k) { int f; _wind_dict_probe(d, (uint64_t)(uintptr_t)k, _wind_hash_str(k),  &f); return f; }\n"
        "static void _wind_dict_del_raw(_wind_dict *d, uint64_t k, uint64_t h) {\n"
        "    int f; int j = _wind_dict_probe(d, k, h, &f);\n"
        "    if (!f) return;\n"
        "    if (d->key_type == 2) free((char *)(uintptr_t)d->buckets[j].key);\n"
        "    if (d->val_type == 2) free((char *)(uintptr_t)d->buckets[j].val);\n"
        "    d->buckets[j].state = 2;\n"
        "    d->len--;\n"
        "}\n"
        "static void _wind_dict_del_int (_wind_dict *d, int k)         { _wind_dict_del_raw(d, _wind_dk_pack_int(k),  _wind_hash_int(k));  }\n"
        "static void _wind_dict_del_frac(_wind_dict *d, double k)      { _wind_dict_del_raw(d, _wind_dk_pack_frac(k), _wind_hash_frac(k)); }\n"
        "static void _wind_dict_del_str (_wind_dict *d, const char *k) { _wind_dict_del_raw(d, (uint64_t)(uintptr_t)k, _wind_hash_str(k)); }\n"
        "static int _wind_dict_len(_wind_dict *d) { return d->len; }\n\n", out);
    /* keys() — возвращает _wind_list* с ключами. Для итерации loop k in dict. */
    fputs(
        "static _wind_list *_wind_dict_keys(_wind_dict *d) {\n"
        "    int elem_sz = d->key_type == 1 ? (int)sizeof(double)\n"
        "                : d->key_type == 2 ? (int)sizeof(char *)\n"
        "                :                    (int)sizeof(int);\n"
        "    _wind_list *l = _wind_list_new(elem_sz);\n"
        "    for (int i = 0; i < d->cap; i++) {\n"
        "        if (d->buckets[i].state != 1) continue;\n"
        "        if (d->key_type == 0) {\n"
        "            _wind_list_push_int(l, (int)(int64_t)d->buckets[i].key);\n"
        "        } else if (d->key_type == 1) {\n"
        "            double v; memcpy(&v, &d->buckets[i].key, 8);\n"
        "            _wind_list_push_frac(l, v);\n"
        "        } else {\n"
        "            _wind_list_push_str(l, (const char *)(uintptr_t)d->buckets[i].key);\n"
        "        }\n"
        "    }\n"
        "    return l;\n"
        "}\n\n", out);
    /* === JSON encode: гомогенные dict[str, T] и list[T] ===
     * Только str-ключи (других keys в реальном JSON быть и не должно).
     * value: int → число, frac → число, str → escape'нутая строка. */
    fputs(
        "static void _wind_json_emit_str(char **buf, size_t *pos, size_t *cap, const char *s) {\n"
        "    if (*pos + 2 >= *cap) { *cap = (*cap + 32) * 2; *buf = (char *)realloc(*buf, *cap); }\n"
        "    (*buf)[(*pos)++] = '\"';\n"
        "    if (s) for (; *s; s++) {\n"
        "        if (*pos + 7 >= *cap) { *cap = (*cap + 32) * 2; *buf = (char *)realloc(*buf, *cap); }\n"
        "        unsigned char c = (unsigned char)*s;\n"
        "        if (c == '\"' || c == '\\\\') { (*buf)[(*pos)++] = '\\\\'; (*buf)[(*pos)++] = (char)c; }\n"
        "        else if (c == '\\n') { (*buf)[(*pos)++] = '\\\\'; (*buf)[(*pos)++] = 'n'; }\n"
        "        else if (c == '\\r') { (*buf)[(*pos)++] = '\\\\'; (*buf)[(*pos)++] = 'r'; }\n"
        "        else if (c == '\\t') { (*buf)[(*pos)++] = '\\\\'; (*buf)[(*pos)++] = 't'; }\n"
        "        else if (c < 0x20) { *pos += snprintf(*buf + *pos, *cap - *pos, \"\\\\u%04x\", c); }\n"
        "        else (*buf)[(*pos)++] = (char)c;\n"
        "    }\n"
        "    (*buf)[(*pos)++] = '\"';\n"
        "}\n\n", out);
    fputs(
        "static char *_wind_json_encode_dict(_wind_dict *d) {\n"
        "    if (!d) return _wind_str_new(\"{}\");\n"
        "    size_t cap = 64, pos = 0;\n"
        "    char *buf = (char *)malloc(cap);\n"
        "    buf[pos++] = '{';\n"
        "    int first = 1;\n"
        "    for (int i = 0; i < d->cap; i++) {\n"
        "        if (d->buckets[i].state != 1) continue;\n"
        "        if (!first) { if (pos + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); } buf[pos++] = ','; }\n"
        "        first = 0;\n"
        "        const char *k = (const char *)(uintptr_t)d->buckets[i].key;\n"
        "        _wind_json_emit_str(&buf, &pos, &cap, k);\n"
        "        if (pos + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }\n"
        "        buf[pos++] = ':';\n"
        "        if (d->val_type == 0) {\n"
        "            char tmp[32]; int n = snprintf(tmp, sizeof(tmp), \"%d\", (int)(int64_t)d->buckets[i].val);\n"
        "            while (pos + (size_t)n + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }\n"
        "            memcpy(buf + pos, tmp, (size_t)n); pos += (size_t)n;\n"
        "        } else if (d->val_type == 1) {\n"
        "            double v; memcpy(&v, &d->buckets[i].val, 8);\n"
        "            char tmp[32]; int n = snprintf(tmp, sizeof(tmp), \"%g\", v);\n"
        "            while (pos + (size_t)n + 1 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }\n"
        "            memcpy(buf + pos, tmp, (size_t)n); pos += (size_t)n;\n"
        "        } else {\n"
        "            _wind_json_emit_str(&buf, &pos, &cap, (const char *)(uintptr_t)d->buckets[i].val);\n"
        "        }\n"
        "    }\n"
        "    if (pos + 2 >= cap) { cap += 2; buf = (char *)realloc(buf, cap); }\n"
        "    buf[pos++] = '}'; buf[pos] = '\\0';\n"
        "    return buf;\n"
        "}\n\n", out);
    /* JSON decode — простой парсер, ожидающий {} с string ключами.
     * Schema-aware: caller знает что value тип int/frac/str.
     * Если value не соответствует — _wind_throw. */
    fputs(
        "static const char *_wind_json_skip_ws(const char *p) {\n"
        "    while (*p && (*p == ' ' || *p == '\\n' || *p == '\\r' || *p == '\\t')) p++;\n"
        "    return p;\n"
        "}\n"
        "static const char *_wind_json_parse_str(const char *p, char *out_buf, size_t out_sz) {\n"
        "    p = _wind_json_skip_ws(p);\n"
        "    if (*p != '\"') _wind_throw(\"json: expected '\\\"'\");\n"
        "    p++;\n"
        "    size_t pos = 0;\n"
        "    while (*p && *p != '\"') {\n"
        "        if (pos + 2 >= out_sz) _wind_throw(\"json: string too long\");\n"
        "        if (*p == '\\\\') {\n"
        "            p++;\n"
        "            if (*p == 'n') out_buf[pos++] = '\\n';\n"
        "            else if (*p == 'r') out_buf[pos++] = '\\r';\n"
        "            else if (*p == 't') out_buf[pos++] = '\\t';\n"
        "            else out_buf[pos++] = *p;\n"
        "            p++;\n"
        "        } else { out_buf[pos++] = *p++; }\n"
        "    }\n"
        "    if (*p != '\"') _wind_throw(\"json: unterminated string\");\n"
        "    out_buf[pos] = '\\0';\n"
        "    return p + 1;\n"
        "}\n\n", out);
    fputs(
        "static _wind_dict *_wind_json_decode_str_int(const char *s) {\n"
        "    _wind_dict *d = _wind_dict_new(2, 0);\n"
        "    s = _wind_json_skip_ws(s);\n"
        "    if (*s != '{') _wind_throw(\"json: expected '{'\");\n"
        "    s++;\n"
        "    s = _wind_json_skip_ws(s);\n"
        "    if (*s == '}') return d;\n"
        "    while (1) {\n"
        "        char keybuf[256];\n"
        "        s = _wind_json_parse_str(s, keybuf, sizeof(keybuf));\n"
        "        s = _wind_json_skip_ws(s);\n"
        "        if (*s != ':') _wind_throw(\"json: expected ':'\");\n"
        "        s++;\n"
        "        s = _wind_json_skip_ws(s);\n"
        "        char *end; long v = strtol(s, &end, 10);\n"
        "        if (end == s) _wind_throw(\"json: expected int value\");\n"
        "        s = end;\n"
        "        _wind_dict_set_si(d, keybuf, (int)v);\n"
        "        s = _wind_json_skip_ws(s);\n"
        "        if (*s == ',') { s++; s = _wind_json_skip_ws(s); continue; }\n"
        "        if (*s == '}') break;\n"
        "        _wind_throw(\"json: expected ',' or '}'\");\n"
        "    }\n"
        "    return d;\n"
        "}\n\n", out);
    fputs(
        "static _wind_dict *_wind_json_decode_str_str(const char *s) {\n"
        "    _wind_dict *d = _wind_dict_new(2, 2);\n"
        "    s = _wind_json_skip_ws(s);\n"
        "    if (*s != '{') _wind_throw(\"json: expected '{'\");\n"
        "    s++;\n"
        "    s = _wind_json_skip_ws(s);\n"
        "    if (*s == '}') return d;\n"
        "    while (1) {\n"
        "        char keybuf[256], valbuf[1024];\n"
        "        s = _wind_json_parse_str(s, keybuf, sizeof(keybuf));\n"
        "        s = _wind_json_skip_ws(s);\n"
        "        if (*s != ':') _wind_throw(\"json: expected ':'\");\n"
        "        s++;\n"
        "        s = _wind_json_parse_str(s, valbuf, sizeof(valbuf));\n"
        "        _wind_dict_set_ss(d, keybuf, valbuf);\n"
        "        s = _wind_json_skip_ws(s);\n"
        "        if (*s == ',') { s++; s = _wind_json_skip_ws(s); continue; }\n"
        "        if (*s == '}') break;\n"
        "        _wind_throw(\"json: expected ',' or '}'\");\n"
        "    }\n"
        "    return d;\n"
        "}\n\n", out);
    /* === HTTP клиент через libcurl === */
    fputs(
        "typedef struct { char *data; size_t len; size_t cap; } _wind_http_buf;\n"
        "static size_t _wind_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {\n"
        "    _wind_http_buf *b = (_wind_http_buf *)userdata;\n"
        "    size_t real = size * nmemb;\n"
        "    if (b->len + real + 1 >= b->cap) {\n"
        "        size_t new_cap = (b->len + real + 1) * 2;\n"
        "        char *nb = (char *)realloc(b->data, new_cap);\n"
        "        if (!nb) { perror(\"realloc\"); exit(1); }\n"
        "        b->data = nb; b->cap = new_cap;\n"
        "    }\n"
        "    memcpy(b->data + b->len, ptr, real);\n"
        "    b->len += real;\n"
        "    b->data[b->len] = '\\0';\n"
        "    return real;\n"
        "}\n\n", out);
    fputs(
        "static long _wind_http_last_status = 0;\n"
        "static char *_wind_http_request(const char *method, const char *url, const char *body, const char *ct) {\n"
        "    CURL *curl = curl_easy_init();\n"
        "    if (!curl) _wind_throw(\"http: curl init failed\");\n"
        "    _wind_http_buf buf; buf.data = (char *)malloc(64); buf.len = 0; buf.cap = 64;\n"
        "    if (!buf.data) { perror(\"malloc\"); exit(1); }\n"
        "    buf.data[0] = '\\0';\n"
        "    curl_easy_setopt(curl, CURLOPT_URL, url);\n"
        "    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);\n"
        "    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);\n"
        "    curl_easy_setopt(curl, CURLOPT_USERAGENT, \"Wind/0.1\");\n"
        "    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _wind_http_write_cb);\n"
        "    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);\n"
        "    struct curl_slist *hdrs = NULL;\n"
        "    if (strcmp(method, \"POST\") == 0) {\n"
        "        curl_easy_setopt(curl, CURLOPT_POST, 1L);\n"
        "        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : \"\");\n"
        "        if (ct) { char h[256]; snprintf(h, sizeof(h), \"Content-Type: %s\", ct); hdrs = curl_slist_append(hdrs, h); }\n"
        "    } else if (strcmp(method, \"DELETE\") == 0) {\n"
        "        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, \"DELETE\");\n"
        "    } else if (strcmp(method, \"PUT\") == 0) {\n"
        "        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, \"PUT\");\n"
        "        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : \"\");\n"
        "        if (ct) { char h[256]; snprintf(h, sizeof(h), \"Content-Type: %s\", ct); hdrs = curl_slist_append(hdrs, h); }\n"
        "    }\n"
        "    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);\n"
        "    CURLcode res = curl_easy_perform(curl);\n"
        "    if (hdrs) curl_slist_free_all(hdrs);\n"
        "    if (res != CURLE_OK) {\n"
        "        char msg[400]; snprintf(msg, sizeof(msg), \"http: %s\", curl_easy_strerror(res));\n"
        "        curl_easy_cleanup(curl);\n"
        "        free(buf.data);\n"
        "        _wind_throw(msg);\n"
        "    }\n"
        "    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &_wind_http_last_status);\n"
        "    curl_easy_cleanup(curl);\n"
        "    return buf.data;\n"
        "}\n"
        "static char *_wind_http_get(const char *url) { return _wind_http_request(\"GET\", url, NULL, NULL); }\n"
        "static char *_wind_http_post(const char *url, const char *body) { return _wind_http_request(\"POST\", url, body, \"application/x-www-form-urlencoded\"); }\n"
        "static char *_wind_http_post_json(const char *url, const char *body) { return _wind_http_request(\"POST\", url, body, \"application/json\"); }\n"
        "static char *_wind_http_put(const char *url, const char *body) { return _wind_http_request(\"PUT\", url, body, \"application/json\"); }\n"
        "static char *_wind_http_delete(const char *url) { return _wind_http_request(\"DELETE\", url, NULL, NULL); }\n"
        "static int _wind_http_status(void) { return (int)_wind_http_last_status; }\n\n", out);
    /* === UTF-8: правильный подсчёт и slice по code-points === */
    fputs(
        "static int _wind_shell_run(const char *cmd) {\n"
        "   int rc = system(cmd);\n"
        "   if (rc == -1) _wind_throw(\"shell: cannot execute\");\n"
        "   return WEXITSTATUS(rc);\n"
        "   }\n\n", out);
    fputs(
        "static int _wind_str_chars(const char *s) {\n"
        "    if (!s) return 0;\n"
        "    int n = 0;\n"
        "    while (*s) {\n"
        "        if (((unsigned char)*s & 0xC0) != 0x80) n++;\n"
        "        s++;\n"
        "    }\n"
        "    return n;\n"
        "}\n\n", out);
    fputs(
        "static char *_wind_str_slice_chars(const char *s, int a, int b) {\n"
        "    if (!s) return _wind_str_new(\"\");\n"
        "    int total = _wind_str_chars(s);\n"
        "    if (a < 0) a += total;\n"
        "    if (b < 0) b += total;\n"
        "    if (a < 0) a = 0;\n"
        "    if (b > total) b = total;\n"
        "    if (a >= b) return _wind_str_new(\"\");\n"
        "    const char *p = s;\n"
        "    int idx = 0;\n"
        "    while (*p && idx < a) {\n"
        "        unsigned char c = (unsigned char)*p;\n"
        "        if      (c < 0x80)        p += 1;\n"
        "        else if ((c & 0xE0)==0xC0) p += 2;\n"
        "        else if ((c & 0xF0)==0xE0) p += 3;\n"
        "        else if ((c & 0xF8)==0xF0) p += 4;\n"
        "        else                       p += 1;\n"
        "        idx++;\n"
        "    }\n"
        "    const char *start_b = p;\n"
        "    while (*p && idx < b) {\n"
        "        unsigned char c = (unsigned char)*p;\n"
        "        if      (c < 0x80)        p += 1;\n"
        "        else if ((c & 0xE0)==0xC0) p += 2;\n"
        "        else if ((c & 0xF0)==0xE0) p += 3;\n"
        "        else if ((c & 0xF8)==0xF0) p += 4;\n"
        "        else                       p += 1;\n"
        "        idx++;\n"
        "    }\n"
        "    size_t len = (size_t)(p - start_b);\n"
        "    char *r = (char *)malloc(len + 1);\n"
        "    if (!r) { perror(\"malloc\"); exit(1); }\n"
        "    memcpy(r, start_b, len);\n"
        "    r[len] = '\\0';\n"
        "    return r;\n"
        "}\n\n", out);
    fputs(
        "static char *_wind_str_join(_wind_list *l, const char *sep) {\n"
        "    if (!l || l->len == 0) return _wind_str_new(\"\");\n"
        "    if (!sep) sep = \"\";\n"
        "    size_t sl = strlen(sep);\n"
        "    size_t total = 0;\n"
        "    for (int i = 0; i < l->len; i++) {\n"
        "        const char *s = ((char **)l->data)[i];\n"
        "        if (s) total += strlen(s);\n"
        "    }\n"
        "    if (l->len > 1) total += (size_t)(l->len - 1) * sl;\n"
        "    char *r = (char *)malloc(total + 1);\n"
        "    if (!r) { perror(\"malloc\"); exit(1); }\n"
        "    char *dst = r;\n"
        "    for (int i = 0; i < l->len; i++) {\n"
        "        if (i > 0) { memcpy(dst, sep, sl); dst += sl; }\n"
        "        const char *s = ((char **)l->data)[i];\n"
        "        if (s) { size_t ll = strlen(s); memcpy(dst, s, ll); dst += ll; }\n"
        "    }\n"
        "    *dst = '\\0';\n"
        "    return r;\n"
        "}\n\n", out);
    dump_tmp(decls_out, out);
    fputc('\n', out);
    dump_tmp(funcs_out, out);
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    signal(SIGFPE, _wind_sigfpe);\n");
    fprintf(out, "    signal(SIGILL, _wind_sigfpe);\n");
    fprintf(out, "    srand((unsigned)time(NULL));\n");
    fprintf(out, "    curl_global_init(CURL_GLOBAL_DEFAULT);\n");
    dump_tmp(main_out, out);
    fprintf(out, "    return 0;\n}\n");
    fclose(out);
    fclose(decls_out);
    fclose(funcs_out);
    fclose(main_out);

    printf("[wind] output.c generated, invoking gcc...\n");
    int rc = system("gcc output.c -O3 -lm -lcurl -o app");
    if (rc != 0) {
        fprintf(stderr, C_RED "[wind] gcc failed with code %d" C_RST "\n", rc);
        return 1;
    }
    printf(C_CYN "[wind] done -> ./app" C_RST "\n");

    /* Флаг -s: сразу запускаем собранный бинарник.
     * Возвращаем код выхода ./app как наш собственный — типичная UNIX-конвенция. */
    if (run_after) {
        int app_rc = system("./app");
        /* system() возвращает не сам код, а статус: код в старших битах.
         * WEXITSTATUS извлекает реальный exit code. */
        if (app_rc == -1) return 1;
        return (app_rc >> 8) & 0xff;
    }

    return 0;
}
