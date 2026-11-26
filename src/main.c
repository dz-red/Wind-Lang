#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

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

static int build_gc_strings(int nlinks, char *const *links) {
    char *args[64] = {
        "gcc", "-O2", "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
        "-Werror=return-type", "-I", WIND_RUNTIME_DIR, "-o", "app", "output_ast.c",
        WIND_RUNTIME_DIR "/gc.c", WIND_RUNTIME_DIR "/strings_gc.c",
        WIND_RUNTIME_DIR "/collections_gc.c", "-lm", NULL
    };
    char flags[32][256];
    size_t argc = 0;
    while (args[argc]) argc++;
    for (int i = 0; i < nlinks; i++) {
        const char *name = links[i];
        size_t length = strlen(name);
        if (length >= 2 && name[0] == '"' && name[length - 1] == '"') {
            name++;
            length -= 2;
        }
        if (!length || length > sizeof flags[i] - 3 ||
            strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-:") < length) {
            fputs("[wind] invalid library name\n", stderr);
            return 0;
        }
        snprintf(flags[i], sizeof flags[i], "-l%.*s", (int)length, name);
        args[argc++] = flags[i];
    }
    args[argc] = NULL;
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 0; }
    if (child == 0) {
        if (setenv("LC_ALL", "C", 1) != 0) {
            fputs("[wind] cannot set the locale for GCC\n", stderr);
            _exit(127);
        }
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
        "  %s --check <file.wnd> check Wind code without running GCC\n"
        "  %s --diag=compact|normal|learn <file.wnd> choose diagnostics (default: normal)\n"
        "  %s --explain W2001  explain a diagnostic code\n"
        "  %s --wgc <file.wnd> use WGC (the default on Linux)\n"
        "  %s --boehm <file.wnd> use the legacy Boehm collector\n"
        "  %s --help           this help\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int run_after = 0, mode_ast = 0, mode_check = 0;
    WindDiagnostics diags = {0};
    if (argc == 3 && !strcmp(argv[1], "--explain")) {
        if (wind_diag_explain(argv[2])) return 0;
        fprintf(stderr, "[wind] no explanation for %s\n", argv[2]);
        return 1;
    }
#ifdef _WIN32
    int gc_strings = 0;
#else
    int gc_strings = 1;
#endif
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--start") == 0) {
            run_after = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            mode_ast = 1;
        } else if (strcmp(argv[i], "--check") == 0) {
            mode_check = 1;
        } else if (strncmp(argv[i], "--diag=", 7) == 0) {
            if (!wind_diag_mode(argv[i] + 7, &diags.mode)) {
                fprintf(stderr, "[wind] unknown diagnostic mode: %s (use compact, normal or learn)\n", argv[i] + 7);
                return 1;
            }
        } else if (strcmp(argv[i], "--errors-simple") == 0) {
            diags.mode = DIAG_SIMPLE;
        } else if (strcmp(argv[i], "--wgc") == 0 || strcmp(argv[i], "--gc-strings") == 0) {
            gc_strings = 1;
        } else if (strcmp(argv[i], "--boehm") == 0) {
            gc_strings = 0;
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
        fprintf(stderr, "[wind] --wgc is not available on Windows\n");
        return 1;
    }
#endif
    current_file = input;

    Program prog = {0};
    WindSource *src = wind_source_open(&diags, input);
    if (!src) {
        wind_diag_emit(&diags, &(WindDiagnostic){.code = WD_IO, .message = "cannot read source file"});
        goto failed;
    }
    int ntok = 0;
    Token *toks = wind_lex(src->text, &ntok);
    int ok = wind_parse(toks, ntok, &prog, &diags, src);
    wind_tokens_free(toks, ntok);
    if (!ok || !wind_load_modules(&prog, src, &diags)) goto failed;

    if (mode_ast) {
        ast_dump_program(&prog, stdout);
        goto done;
    }

    FILE *o = tmpfile();
    if (!o) {
        wind_diag_emit(&diags, &(WindDiagnostic){.code = WD_IO, .message = "cannot open temporary file for generated code"});
        goto failed;
    }
    ok = wind_codegen(&prog, o, &diags, gc_strings);
    if (ok && !mode_check) {
        ok = fflush(o) == 0 && fseek(o, 0, SEEK_SET) == 0;
        FILE *generated = ok ? fopen("output_ast.c", "wb") : NULL;
        ok = generated != NULL;
        if (generated) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, o)) != 0) {
                if (fwrite(buf, 1, n, generated) != n) { ok = 0; break; }
            }
            if (fclose(generated) != 0 || ferror(o)) ok = 0;
        }
        if (!ok)
            wind_diag_emit(&diags, &(WindDiagnostic){.code = WD_IO, .message = "cannot write output_ast.c"});
    }
    fclose(o);
    if (!ok) goto failed;
    if (mode_check) goto done;

#ifndef _WIN32
    if (gc_strings) {
        if (!build_gc_strings(prog.nlinks, prog.links)) goto gcc_failed;
        goto compiled;
    }
#endif

    char gcc_cmd[1024];
#ifdef _WIN32
    /* -static + GC_NOT_DLL => самодостаточный .exe без libgc-1.dll/mingw-dll рядом */
    snprintf(gcc_cmd, sizeof(gcc_cmd), "set \"LC_ALL=C\" && gcc -O2 -DGC_NOT_DLL -o app output_ast.c -static -lgc -lm -lws2_32");
#else
    snprintf(gcc_cmd, sizeof(gcc_cmd), "LC_ALL=C gcc -O2 -o app output_ast.c -lgc -lm");
#endif

    for (int i = 0; i < prog.nlinks; i++) {
        char *lib = prog.links[i];
        strncat(gcc_cmd, " -l", sizeof(gcc_cmd) - strlen(gcc_cmd) - 1);
        if (lib[0] == '"') {
            size_t len = strlen(lib);
            if (len >= 2) strncat(gcc_cmd, lib + 1, len - 2);
        } else {
            strncat(gcc_cmd, lib, sizeof(gcc_cmd) - strlen(gcc_cmd) - 1);
        }
    }

    if (system(gcc_cmd) != 0) goto gcc_failed;
#ifndef _WIN32
compiled:
#endif
    if (diags.mode != DIAG_COMPACT && diags.mode != DIAG_SIMPLE)
        fprintf(stderr, C_CYN "Done: ./app" C_RST "\n");
    if (run_after) {
#ifdef _WIN32
        int r = system(".\\app");
#else
        int r = system("./app");
#endif
        (void)r;
    }
done:
    ast_free_program(&prog);
    wind_diag_free(&diags);
    return 0;

gcc_failed:
    wind_diag_emit(&diags, &(WindDiagnostic){.code = WD_GCC, .message = "GCC could not build the generated C code"});
failed:
    wind_diag_summary(&diags);
    ast_free_program(&prog);
    wind_diag_free(&diags);
    return 1;
}
