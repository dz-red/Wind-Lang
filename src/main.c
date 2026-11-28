#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"

#ifndef WIND_RUNTIME_DIR
#define WIND_RUNTIME_DIR "runtime"
#endif

enum { MAX_BUILD_LINKS = WIND_MAX_LINKS + WIND_MAX_C_IMPORTS, LINK_FLAG_SIZE = 256 };

static int build_links(const Program *p, const char **links) {
    int n = p->nlinks;
    for (int i = 0; i < n; i++) links[i] = p->links[i];
    for (int i = 0; i < p->nc_imports; i++) {
        const char *library = p->c_imports[i].binding->library;
        if (!strcmp(library, "m") || ast_has_link(p, library)) continue;
        int j = p->nlinks;
        while (j < n && strcmp(links[j], library)) j++;
        if (j == n) links[n++] = library;
    }
    return n;
}

static int format_link(const char *name, char *flag) {
    size_t length = strlen(name);
    if (length >= 2 && name[0] == '"' && name[length - 1] == '"') {
        name++;
        length -= 2;
    }
    if (!length || length > LINK_FLAG_SIZE - 3 ||
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-:") < length) {
        fputs("[wind] invalid library name\n", stderr);
        return 0;
    }
    snprintf(flag, LINK_FLAG_SIZE, "-l%.*s", (int)length, name);
    return 1;
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef _WIN32
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

static int build_program(const Program *program, int gc_strings, int nlinks,
                         const char *const *links, FILE *log) {
    char *wgc[] = {
        "gcc", "-O2", "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
        "-Werror=return-type", "-I", WIND_RUNTIME_DIR, "-o", "app", "output_ast.c",
        WIND_RUNTIME_DIR "/gc.c", WIND_RUNTIME_DIR "/strings_gc.c",
        WIND_RUNTIME_DIR "/collections_gc.c", "-lm", NULL
    };
    char *boehm[] = {"gcc", "-O2", "-o", "app", "output_ast.c", "-lgc", "-lm", NULL};
    char **base = gc_strings ? wgc : boehm;
    char *args[sizeof wgc / sizeof *wgc + MAX_BUILD_LINKS + WIND_MAX_C_IMPORTS + 2];
    char flags[MAX_BUILD_LINKS][LINK_FLAG_SIZE];
    char runtime[WIND_MAX_C_IMPORTS][sizeof WIND_RUNTIME_DIR + 64];
    size_t argc = 0;
    while (base[argc]) { args[argc] = base[argc]; argc++; }
    int needs_include = !gc_strings;
    for (int i = 0; i < program->nc_imports; i++) {
        const char *source = program->c_imports[i].binding->runtime_source;
        if (!source) continue;
        if (needs_include) {
            args[argc++] = "-iquote";
            args[argc++] = WIND_RUNTIME_DIR;
            needs_include = 0;
        }
        int written = snprintf(runtime[i], sizeof runtime[i], "%s/%s", WIND_RUNTIME_DIR, source);
        if (written < 0 || (size_t)written >= sizeof runtime[i]) return 0;
        args[argc++] = runtime[i];
    }
    for (int i = 0; i < nlinks; i++) {
        if (!format_link(links[i], flags[i])) return 0;
        args[argc++] = flags[i];
    }
    args[argc] = NULL;
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 0; }
    if (child == 0) {
        if (log && dup2(fileno(log), STDERR_FILENO) < 0) _exit(127);
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
#else
static int build_program(const Program *program, int gc_strings, int nlinks,
                         const char *const *links, FILE *log) {
    (void)gc_strings;
    char cmd[1024] = "set \"LC_ALL=C\" && gcc -O2 -DGC_NOT_DLL -o app output_ast.c -static -lgc -lm -lws2_32";
    for (int i = 0; i < program->nc_imports; i++) {
        const char *source = program->c_imports[i].binding->runtime_source;
        if (!source) continue;
        size_t used = strlen(cmd);
        int written = snprintf(cmd + used, sizeof(cmd) - used, " -iquote \"%s\" \"%s/%s\"",
                               WIND_RUNTIME_DIR, WIND_RUNTIME_DIR, source);
        if (written < 0 || (size_t)written >= sizeof(cmd) - used) return 0;
    }
    for (int i = 0; i < nlinks; i++) {
        char flag[LINK_FLAG_SIZE];
        if (!format_link(links[i], flag)) return 0;
        size_t used = strlen(cmd);
        int written = snprintf(cmd + used, sizeof(cmd) - used, " %s", flag);
        if (written < 0 || (size_t)written >= sizeof(cmd) - used) return 0;
    }
    int saved = -1;
    if (log) {
        fflush(stderr);
        saved = _dup(_fileno(stderr));
        if (saved < 0) return 0;
        if (_dup2(_fileno(log), _fileno(stderr)) < 0) { _close(saved); return 0; }
    }
    int status = system(cmd);
    if (saved >= 0) { _dup2(saved, _fileno(stderr)); _close(saved); }
    return status == 0;
}
#endif

#include "errors.h"
#include "lexer.h"
#include "astparse.h"
#include "astcodegen.h"
#include "modules.h"

static int report_c_build_error(const Program *p, WindDiagnostics *diags, FILE *log) {
    char line[4096];
    while (fgets(line, sizeof line, log)) {
        int i = -1, fn = -1;
        WindDiagCode code;
        if (strstr(line, ": fatal error: ") && strstr(line, "No such file or directory") &&
            sscanf(line, "wind-ffi-header-%d:", &i) == 1) {
            code = WD_C_HEADER;
        } else if (strstr(line, ": error: ") &&
                   sscanf(line, "wind-ffi-signature-%d-%d:", &i, &fn) == 2) {
            code = WD_C_SIGNATURE;
        } else {
            const char *needle = "cannot find -l";
            const char *lib = strstr(line, needle);
            if (!lib) { needle = "unable to find library -l"; lib = strstr(line, needle); }
            if (!lib) continue;
            lib += strlen(needle);
            size_t len = strcspn(lib, " \t\r\n:");
            for (i = 0; i < p->nc_imports; i++) {
                const char *name = p->c_imports[i].binding->library;
                if (strlen(name) == len && !strncmp(name, lib, len)) break;
            }
            code = WD_C_LIBRARY;
        }
        if (i < 0 || i >= p->nc_imports) continue;
        const CImport *import = &p->c_imports[i];
        const WindCModule *m = import->binding;
        WindRange range = import->range;
        char message[512];
        if (code == WD_C_HEADER) {
            snprintf(message, sizeof message, "C header `%s` not found", m->header);
            if (m->source) range = m->header_range;
        } else if (code == WD_C_LIBRARY) {
            snprintf(message, sizeof message, "C library `%s` not found by the linker", m->library);
            if (m->source) range = m->library_range;
        } else {
            int j = 0;
            while (j < fn && m->functions[j].name) j++;
            if (fn < 0 || j != fn || !m->functions[j].name) continue;
            snprintf(message, sizeof message, "C signature for `%.180s` does not match binding `%.100s`",
                     m->functions[fn].symbol, m->name);
            if (m->source) range = m->functions[fn].range;
        }
        wind_diag_emit(diags, &(WindDiagnostic){
            .code = code, .source = m->source ? m->source : import->source,
            .primary = range, .message = message
        });
        return 1;
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
        "  %s --ast <file.wnd> print the AST and exit\n"
        "  %s --check <file.wnd> check Wind code without running GCC\n"
        "  %s --check --stdin --diag-json <file.wnd> check an editor buffer with its real path\n"
        "  %s --diag=compact|normal|learn <file.wnd> choose diagnostics (default: normal)\n"
        "  %s --explain W2001  explain a diagnostic code\n"
        "  %s --wgc <file.wnd> use WGC (the default on Linux)\n"
        "  %s --boehm <file.wnd> use the legacy Boehm collector\n"
        "  %s --help           this help\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int run_after = 0, mode_ast = 0, mode_check = 0, from_stdin = 0;
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
        } else if (strcmp(argv[i], "--stdin") == 0) {
            from_stdin = 1;
        } else if (strcmp(argv[i], "--diag-json") == 0) {
            diags.mode = DIAG_JSON;
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
    if ((from_stdin || diags.mode == DIAG_JSON) && (!mode_check || mode_ast || run_after)) {
        fputs("[wind] --stdin and --diag-json are only available with --check\n", stderr);
        return 1;
    }
#ifdef _WIN32
    if (gc_strings) {
        fprintf(stderr, "[wind] --wgc is not available on Windows\n");
        return 1;
    }
#endif
    current_file = input;

    Program prog = {0};
#ifdef _WIN32
    if (from_stdin) _setmode(_fileno(stdin), _O_BINARY);
#endif
    WindSource *src = from_stdin ? wind_source_read(&diags, input, stdin) : wind_source_open(&diags, input);
    if (!src) {
        wind_diag_emit(&diags, &(WindDiagnostic){.code = WD_IO, .message = "cannot read source file"});
        goto failed;
    }
    int ntok = 0;
    Token *toks = wind_lex(src->text, &ntok);
    int ok = wind_parse(toks, ntok, &prog, &diags, src);
    wind_tokens_free(toks, ntok);
    if (!ok || !wind_load_modules(&prog, src, &diags) ||
        !ast_resolve_c_imports(&prog, src, &diags)) goto failed;

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

    const char *links[MAX_BUILD_LINKS];
    int nlinks = build_links(&prog, links);
    FILE *gcc_log = prog.nc_imports ? tmpfile() : NULL;
    int built = build_program(&prog, gc_strings, nlinks, links, gcc_log);
    if (gcc_log) {
        rewind(gcc_log);
        int reported = !built && report_c_build_error(&prog, &diags, gcc_log);
        if (!reported) {
            rewind(gcc_log);
            char line[4096];
            while (fgets(line, sizeof line, gcc_log)) fputs(line, stderr);
        }
        fclose(gcc_log);
        if (reported) goto failed;
    }
    if (!built) goto gcc_failed;
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
