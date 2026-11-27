#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cimports.h"
#include "../bindings/sqlite.inc"

const WindCModule wind_c_modules[] = {
    {.name = "sqlite", .header = "sqlite3.h", .library = "sqlite3",
     .functions = sqlite_functions, .legacy_link = 1},
    {0}
};

const WindCModule *wind_c_module(const char *name) {
    for (const WindCModule *m = wind_c_modules; m->name; m++)
        if (!strcmp(m->name, name)) return m;
    return NULL;
}

const WindCFunction *wind_c_function(const WindCModule *module, const char *name) {
    for (const WindCFunction *f = module->functions; f->name; f++)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}

static const char *c_type(TokenKind type) {
    switch (type) {
        case TK_KW_INT: return "int";
        case TK_KW_FRAC: return "double";
        case TK_KW_STR: return "const char *";
        default: return "void";
    }
}

void wind_c_emit_checks(FILE *out, const WindCModule *module, int import_index) {
    fprintf(out, "#line 1 \"wind-ffi-header-%d\"\n#include <%s>\n", import_index, module->header);
    fputs("#pragma GCC diagnostic push\n"
          "#pragma GCC diagnostic error \"-Wincompatible-pointer-types\"\n", out);
    for (int i = 0; module->functions[i].name; i++) {
        const WindCFunction *f = &module->functions[i];
        fprintf(out, "#line 1 \"wind-ffi-signature-%d-%d\"\n"
                     "static inline void _wind_ffi_check_%d_%d(void) {\n"
                     "    %s (*prototype)(",
                import_index, i, import_index, i, c_type(f->result));
        if (!f->nargs) fputs("void", out);
        for (int j = 0; j < f->nargs; j++)
            fprintf(out, "%s%s", j ? ", " : "", c_type(f->args[j]));
        fprintf(out, ") = &%s;\n    (void)prototype;\n}\n", f->symbol);
    }
    fputs("#pragma GCC diagnostic pop\n#line 1 \"output_ast.c\"\n", out);
}

static void *checked(void *p) {
    if (!p) { perror("malloc"); exit(1); }
    return p;
}

static char *copy_n(const char *s, size_t n) {
    char *r = checked(malloc(n + 1));
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

void wind_c_free(const WindCModule *module) {
    if (!module || !module->source) return;
    for (const WindCFunction *f = module->functions; f->name; f++) {
        free((void *)f->name);
        free((void *)f->symbol);
    }
    free((void *)module->functions);
    free((void *)module->header);
    free((void *)module->library);
    free((void *)module);
}

typedef struct {
    Token *t;
    const WindSource *source;
    WindDiagnostics *diags;
} BindingParser;

static WindRange token_range(const Token *t) {
    return (WindRange){t->line, t->col, t->end_line, t->end_col};
}

static int binding_error(BindingParser *p, const Token *at, const char *message) {
    wind_diag_emit(p->diags, &(WindDiagnostic){
        .code = WD_C_BINDING, .source = p->source, .primary = token_range(at),
        .message = at->problem ? at->problem : message
    });
    return 0;
}

static Token *expect(BindingParser *p, TokenKind token, const char *message) {
    if (p->t->kind == token) return p->t++;
    binding_error(p, p->t, message);
    return NULL;
}

static int word(const Token *t, const char *s) {
    return t->kind == TK_IDENT && !strcmp(t->text, s);
}

static int end_line(BindingParser *p) {
    if (p->t->kind == TK_EOF) return 1;
    return expect(p, TK_NEWLINE, "expected end of binding declaration") != NULL;
}

static int safe_header(const char *s, size_t n) {
    if (!n || n > 240 || strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-") < n)
        return 0;
    for (size_t from = 0; from <= n; ) {
        size_t to = from;
        while (to < n && s[to] != '/') to++;
        if (to == from || (s[from] == '.' && (to - from == 1 ||
            (to - from == 2 && s[from + 1] == '.')))) return 0;
        from = to + 1;
    }
    return 1;
}

static int parse_header(BindingParser *p, WindCModule *m) {
    Token *start = expect(p, TK_LT, "expected '<' after 'header'");
    if (!start) return 0;
    while (p->t->line == start->line && p->t->kind != TK_GT &&
           p->t->kind != TK_NEWLINE && p->t->kind != TK_EOF) p->t++;
    if (p->t->line != start->line) return binding_error(p, start, "expected a header on one line");
    Token *end = expect(p, TK_GT, "expected '>' after C header");
    if (!end) return 0;
    const char *line = p->source->text;
    for (int i = 1; i < start->line; i++) line = strchr(line, '\n') + 1;
    const char *from = line + start->end_col - 1;
    const char *to = line + end->col - 1;
    while (from < to && (*from == ' ' || *from == '\t')) from++;
    while (to > from && (to[-1] == ' ' || to[-1] == '\t')) to--;
    if (!safe_header(from, (size_t)(to - from)))
        return binding_error(p, start, "expected a relative C header name such as `zlib.h`");
    m->header = copy_n(from, (size_t)(to - from));
    m->header_range = (WindRange){start->line, (int)(from - line) + 1, start->line, (int)(to - line) + 1};
    return end_line(p);
}

static int parse_function(BindingParser *p, WindCModule *m, int *nfunc) {
    Token *name = expect(p, TK_IDENT, "expected binding function name");
    if (!name || !expect(p, TK_LPAREN, "expected '(' after binding function name")) return 0;
    if (wind_c_function(m, name->text)) return binding_error(p, name, "duplicate binding function");
    WindCFunction f = {0};
    const char *names[WIND_C_MAX_PARAMS];
    if (p->t->kind != TK_RPAREN) {
        for (;;) {
            Token *type = p->t++;
            if (type->kind != TK_KW_INT && type->kind != TK_KW_FRAC)
                return binding_error(p, type, "C parameters must have type `int` or `frac`");
            if (f.nargs == WIND_C_MAX_PARAMS)
                return binding_error(p, type, "too many C parameters");
            if (!expect(p, TK_DOT, "expected '.' between parameter type and name")) return 0;
            Token *arg = expect(p, TK_IDENT, "expected parameter name");
            if (!arg) return 0;
            for (int i = 0; i < f.nargs; i++)
                if (!strcmp(names[i], arg->text)) return binding_error(p, arg, "duplicate parameter name");
            names[f.nargs] = arg->text;
            f.args[f.nargs++] = type->kind;
            if (p->t->kind != TK_COMMA) break;
            p->t++;
        }
    }
    if (!expect(p, TK_RPAREN, "expected ')' after C parameters") ||
        !expect(p, TK_ARROW, "expected '->' before C return type")) return 0;
    Token *type = p->t++;
    f.result = type->kind;
    if (f.result != TK_KW_INT && f.result != TK_KW_FRAC &&
        f.result != TK_KW_STR && f.result != TK_KW_VOID)
        return binding_error(p, type, "C return type must be `int`, `frac`, `str`, or `void`");
    if (!expect(p, TK_ASSIGN, "expected '=' before C symbol")) return 0;
    Token *symbol = expect(p, TK_IDENT, "expected C function symbol");
    if (!symbol) return 0;
    if (f.result == TK_KW_STR) {
        if (!word(p->t, "copy_cstr")) return binding_error(p, p->t, "`str` returns require `copy_cstr`");
        f.copy_string = 1;
        p->t++;
    }
    if (!end_line(p)) return 0;
    f.name = copy_n(name->text, strlen(name->text));
    f.symbol = copy_n(symbol->text, strlen(symbol->text));
    f.range = token_range(symbol);
    WindCFunction *functions = checked(realloc((void *)m->functions, ((size_t)*nfunc + 2) * sizeof f));
    functions[(*nfunc)++] = f;
    functions[*nfunc] = (WindCFunction){0};
    m->functions = functions;
    return 1;
}

static const WindCModule *parse_binding(const char *name, const WindSource *source, WindDiagnostics *diags) {
    WindCModule *m = checked(calloc(1, sizeof *m));
    m->name = name;
    m->source = source;
    m->functions = checked(calloc(1, sizeof *m->functions));
    int count, nfunc = 0, ok = 1;
    Token *tokens = wind_lex(source->text, &count);
    BindingParser p = {tokens, source, diags};
    while (ok && p.t->kind != TK_EOF) {
        if (p.t->kind == TK_NEWLINE) { p.t++; continue; }
        Token *first = p.t++;
        if (word(first, "header")) {
            ok = m->header ? binding_error(&p, first, "duplicate C header declaration") : parse_header(&p, m);
        } else if (first->kind == TK_KW_LINK) {
            if (m->library) { ok = binding_error(&p, first, "duplicate C library declaration"); continue; }
            Token *lib = expect(&p, TK_STR_LIT, "expected library name in quotes");
            if (!lib) { ok = 0; continue; }
            if (!*lib->text || strlen(lib->text) > 240 ||
                lib->text[strspn(lib->text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")]) {
                ok = binding_error(&p, lib, "expected a library name, not a path or linker flags");
                continue;
            }
            m->library = copy_n(lib->text, strlen(lib->text));
            m->library_range = token_range(lib);
            ok = end_line(&p);
        } else if (first->kind == TK_KW_FUNC) {
            ok = parse_function(&p, m, &nfunc);
        } else {
            ok = binding_error(&p, first, "expected `header`, `link`, or `func`");
        }
    }
    if (ok && (!m->header || !m->library || !nfunc))
        ok = binding_error(&p, p.t, "a binding requires `header`, `link`, and at least one `func`");
    wind_tokens_free(tokens, count);
    if (ok) return m;
    wind_c_free(m);
    return NULL;
}

static char *binding_path(const char *root, size_t n, const char *dir, const char *name) {
    size_t size = n + strlen(dir) + strlen(name) + sizeof ".windbind";
    char *path = checked(malloc(size));
    snprintf(path, size, "%.*s%s%s.windbind", (int)n, root, dir, name);
    return path;
}

const WindCModule *wind_c_load(const char *name, const WindSource *entry,
                               const WindSource *source, WindRange range, WindDiagnostics *diags) {
    const WindCModule *module = wind_c_module(name);
    if (module) return module;

    char *paths[3];
    size_t n = 0;
    const char *slash = strrchr(entry->path, '/');
#ifdef _WIN32
    const char *back = strrchr(entry->path, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    paths[n++] = binding_path(entry->path, slash ? (size_t)(slash - entry->path + 1) : 0, "bindings/", name);
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') paths[n++] = binding_path(xdg, strlen(xdg), "/wind/bindings/", name);
    else if (home && *home) paths[n++] = binding_path(home, strlen(home), "/.local/share/wind/bindings/", name);
    paths[n++] = binding_path("", 0, "/usr/share/wind/bindings/", name);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f && errno == ENOENT) continue;
        found = 1;
        WindSource *binding = f ? wind_source_read(diags, paths[i], f) : NULL;
        if (f) fclose(f);
        if (binding) module = parse_binding(name, binding, diags);
        else wind_diag_emit(diags, &(WindDiagnostic){
            .code = WD_C_BINDING, .source = source, .primary = range,
            .message = "cannot read C binding", .note = paths[i]
        });
        break;
    }
    if (!found) {
        char message[256];
        snprintf(message, sizeof message, "binding `%.160s` not found", name);
        size_t size = 1;
        for (size_t i = 0; i < n; i++) size += strlen(paths[i]) + 3;
        char *searched = checked(calloc(size, 1));
        for (size_t i = 0; i < n; i++) {
            if (i) strcat(searched, "\n  ");
            strcat(searched, paths[i]);
        }
        wind_diag_emit(diags, &(WindDiagnostic){
            .code = WD_C_NOT_FOUND, .source = source, .primary = range,
            .message = message, .search_paths = searched
        });
        free(searched);
    }
    for (size_t i = 0; i < n; i++) free(paths[i]);
    return module;
}
