/*
 * main.c — точка входа компилятора Wind.
 * Читает .wnd, лексер -> AST-парсер -> кодоген в output_ast.c -> gcc -> ./app.
 * (Срез 5: легаси построчный parser.c удалён, AST-пайплайн теперь дефолт.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "lexer.h"
#include "ast.h"
#include "astparse.h"
#include "astcodegen.h"

static void print_help(const char *prog) {
    fprintf(stderr,
        "Wind — compiler for the Wind language (.wnd -> binary)\n"
        "\n"
        "Usage:\n"
        "  %s <file.wnd>       compile; produces ./app\n"
        "  %s -s <file.wnd>    compile AND run immediately\n"
        "  %s --ast <file.wnd> print the AST and exit\n"
        "  %s --help           this help\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int run_after = 0, mode_ast = 0;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--start") == 0) {
            run_after = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            mode_ast = 1;
        } else if (strcmp(argv[i], "--build") == 0) {
            /* build — теперь поведение по умолчанию; флаг принимаем для совместимости */
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

    if (!input) { print_help(argv[0]); return 1; }
    current_file = input;

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

    FILE *o = fopen("output_ast.c", "w");
    if (!o) { fprintf(stderr, C_RED "[ERROR]" C_RST " cannot create output_ast.c\n"); return 1; }
    char cgerr[256];
    int ok = wind_codegen(&prog, o, cgerr, sizeof cgerr);
    fclose(o);
    ast_free_program(&prog); wind_tokens_free(toks, ntok); free(src);
    if (!ok) { fprintf(stderr, C_RED "[codegen error]" C_RST " %s\n", cgerr); return 1; }

    fprintf(stderr, "output_ast.c generated, invoking gcc...\n");
    if (system("gcc -O2 -o app output_ast.c -lgc") != 0) {
        fprintf(stderr, C_RED "[wind] gcc failed\n" C_RST); return 1;
    }
    fprintf(stderr, C_CYN "Done: ./app" C_RST "\n");
    if (run_after) { int r = system("./app"); (void)r; }
    return 0;
}
