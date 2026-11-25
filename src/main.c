#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef WIND_RUNTIME_DIR
#define WIND_RUNTIME_DIR "runtime"
#endif

static int build_gc_strings(void) {
    char *args[] = {
        "gcc", "-O2", "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
        "-Werror=return-type", "-I", WIND_RUNTIME_DIR, "-o", "app", "output_ast.c",
        WIND_RUNTIME_DIR "/gc.c", WIND_RUNTIME_DIR "/strings_gc.c", "-lm", NULL
    };
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 0; }
    if (child == 0) {
        execvp(args[0], args);
        perror("gcc");
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

#include "errors.h"
#include "lexer.h"
#include "ast.h"
#include "astparse.h"
#include "astcodegen.h"
#include "modules.h"

static void print_help(const char *prog) {
    fprintf(stderr,
        "Wind — compiler for the Wind language (.wnd -> binary)\n"
        "\n"
        "Usage:\n"
        "  %s <file.wnd>       compile; produces ./app\n"
        "  %s -s <file.wnd>    compile AND run immediately\n"
        "  %s --ast <file.wnd> print the AST and exit\n"
        "  %s --gc-strings <file.wnd> use the experimental Wind string GC (Linux)\n"
        "  %s --help           this help\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int run_after = 0, mode_ast = 0, gc_strings = 0;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--start") == 0) {
            run_after = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            mode_ast = 1;
        } else if (strcmp(argv[i], "--gc-strings") == 0) {
            gc_strings = 1;
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
#ifdef _WIN32
    if (gc_strings) {
        fprintf(stderr, "[wind] --gc-strings is not available on Windows\n");
        return 1;
    }
#endif
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

    char moderr[256];
    if (!wind_load_modules(&prog, input, moderr, sizeof moderr)) {
        fprintf(stderr, C_RED "[module error]" C_RST " %s\n", moderr);
        ast_free_program(&prog); wind_tokens_free(toks, ntok); free(src);
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
    int ok = wind_codegen(&prog, o, cgerr, sizeof cgerr, gc_strings);
    fclose(o);
    ast_free_program(&prog); wind_tokens_free(toks, ntok); free(src);
    if (!ok) { fprintf(stderr, C_RED "[codegen error]" C_RST " %s\n", cgerr); return 1; }

#ifndef _WIN32
    if (gc_strings) {
        fprintf(stderr, C_CYN "[wind] gcc -O2 (experimental Wind string GC)" C_RST "\n");
        if (!build_gc_strings()) {
            fprintf(stderr, C_RED "[wind] gcc failed\n" C_RST);
            return 1;
        }
        goto compiled;
    }
#endif

    fprintf(stderr, "output_ast.c generated, assembling gcc command...\n");

    char gcc_cmd[1024];
#ifdef _WIN32
    /* -static + GC_NOT_DLL => самодостаточный .exe без libgc-1.dll/mingw-dll рядом */
    snprintf(gcc_cmd, sizeof(gcc_cmd), "gcc -O2 -DGC_NOT_DLL -o app output_ast.c -static -lgc -lm -lws2_32");
#else
    snprintf(gcc_cmd, sizeof(gcc_cmd), "gcc -O2 -o app output_ast.c -lgc -lm");
#endif

    /* append a -l flag for each `link "lib"` directive */
    for (int i = 0; i < prog.nlinks; i++) {
        char *lib = prog.links[i];
        strncat(gcc_cmd, " -l", sizeof(gcc_cmd) - strlen(gcc_cmd) - 1);
        if (lib[0] == '"') {                 /* strip the surrounding quotes */
            size_t len = strlen(lib);
            if (len >= 2) strncat(gcc_cmd, lib + 1, len - 2);
        } else {
            strncat(gcc_cmd, lib, sizeof(gcc_cmd) - strlen(gcc_cmd) - 1);
        }
    }

    fprintf(stderr, C_CYN "[wind] %s" C_RST "\n", gcc_cmd);
    if (system(gcc_cmd) != 0) {
        fprintf(stderr, C_RED "[wind] gcc failed\n" C_RST); return 1;
    }
#ifndef _WIN32
compiled:
#endif
    fprintf(stderr, C_CYN "Done: ./app" C_RST "\n");
    if (run_after) {
#ifdef _WIN32
        int r = system(".\\app");
#else
        int r = system("./app");
#endif
        (void)r;
    }
    return 0;
}
