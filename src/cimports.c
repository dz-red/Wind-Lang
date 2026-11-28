#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cimports.h"
#include "cheader.h"
#include "../bindings/sqlite.inc"

struct WindCOwnership {
    char *symbol;
    unsigned borrowed;
    unsigned callbacks;
    unsigned char lengths[WIND_C_MAX_PARAMS];
    int copy;
    WindCOwnership *next;
};

const WindCModule wind_c_modules[] = {
    {.name = "sqlite", .header = "sqlite3.h", .library = "sqlite3",
     .runtime_header = "sqlite_ffi.h", .runtime_source = "sqlite_ffi.c",
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

TokenKind wind_c_buffer_element(TokenKind type) {
    switch (type) {
        case TK_C_INT_BUFFER: case TK_C_CONST_INT_BUFFER: return TK_KW_INT;
        case TK_C_FRAC_BUFFER: case TK_C_CONST_FRAC_BUFFER: return TK_KW_FRAC;
        default: return TK_BAD;
    }
}

int wind_c_has_callbacks(const WindCModule *module) {
    for (const WindCOwnership *rule = module->ownership; rule; rule = rule->next)
        if (rule->callbacks) return 1;
    return 0;
}

static const char *c_type(TokenKind type) {
    switch (type) {
        case TK_KW_INT: case TK_KW_BOOL: return "int";
        case TK_KW_FRAC: return "double";
        case TK_KW_STR: return "const char *";
        case TK_C_INT_BUFFER: return "int *";
        case TK_C_CONST_INT_BUFFER: return "const int *";
        case TK_C_FRAC_BUFFER: return "double *";
        case TK_C_CONST_FRAC_BUFFER: return "const double *";
        case TK_C_SQLITE_DB: return "WindSqliteDb";
        case TK_C_SQLITE_STMT: return "WindSqliteStmt";
        default: return "void";
    }
}

void wind_c_emit_header(FILE *out, const WindCModule *module, int import_index) {
    fprintf(out, "#line 1 \"wind-ffi-header-%d\"\n#include <%s>\n", import_index, module->header);
    if (module->runtime_header) fprintf(out, "#include \"%s\"\n", module->runtime_header);
    fputs("#line 1 \"output_ast.c\"\n", out);
}

void wind_c_emit_checks(FILE *out, const WindCModule *module, int import_index) {
    if (!module->functions[0].name) return;
    fputs("#pragma GCC diagnostic push\n"
          "#pragma GCC diagnostic error \"-Wincompatible-pointer-types\"\n", out);
    for (int i = 0; module->functions[i].name; i++) {
        const WindCFunction *f = &module->functions[i];
        fprintf(out, "#line 1 \"wind-ffi-signature-%d-%d\"\n"
                     "static inline void _wind_ffi_check_%d_%d(void) {\n"
                     "    %s (*prototype)(",
                import_index, i, import_index, i, c_type(f->result));
        if (!f->nargs) fputs("void", out);
        for (int j = 0; j < f->nargs; j++) {
            if (j) fputs(", ", out);
            if (f->args[j] != TK_C_CALLBACK) fputs(c_type(f->args[j]), out);
            else {
                const WindCCallback *callback = &f->callbacks[j];
                fprintf(out, "%s (*)(", c_type(callback->result));
                if (!callback->nargs) fputs("void", out);
                for (int k = 0; k < callback->nargs; k++)
                    fprintf(out, "%s%s", k ? ", " : "", c_type(callback->args[k]));
                fputc(')', out);
            }
        }
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

static const WindCFunction *add_function(WindCModule *module, WindCFunction f) {
    size_t n = 0;
    while (module->functions[n].name) n++;
    WindCFunction *functions = checked(realloc((void *)module->functions, (n + 2) * sizeof f));
    functions[n] = f;
    functions[n + 1] = (WindCFunction){0};
    module->functions = functions;
    return &functions[n];
}

static WindCOwnership *ownership(const WindCModule *module, const char *symbol) {
    for (WindCOwnership *rule = module->ownership; rule; rule = rule->next)
        if (!strcmp(rule->symbol, symbol)) return rule;
    return NULL;
}

const WindCFunction *wind_c_resolve(const WindCModule *module, const char *name,
                                   WindDiagnostics *diags, const WindSource *source, WindRange range) {
    const WindCFunction *declared = wind_c_function(module, name);
    if (declared || !module->auto_header) return declared;
    WindCModule *loaded = (WindCModule *)module;
    char message[384], suggestion[256], detail[384] = {0};
    if (!loaded->parsed_header) {
        WindCHeaderStatus status = wind_cheader_open(module->header, &loaded->parsed_header, detail, sizeof detail);
        if (status != C_HEADER_OK) {
            const char *reason = status == C_HEADER_UNAVAILABLE ? "automatic bindings require libclang" :
                status == C_HEADER_NOT_FOUND ? "C header not found" : "cannot read C declarations";
            snprintf(message, sizeof message, "%s for binding `%.100s`: <%s>", reason, module->name, module->header);
            wind_diag_emit(diags, &(WindDiagnostic){
                .code = status == C_HEADER_NOT_FOUND ? WD_C_HEADER : WD_C_HEADER_DISCOVERY,
                .source = module->source, .primary = module->header_range, .message = message,
                .note = *detail ? detail : NULL,
                .suggestions = status == C_HEADER_UNAVAILABLE ?
                    "Install libclang or provide explicit `func` declarations in the binding." : NULL
            });
            return NULL;
        }
    }
    WindCFunction f = {0};
    WindCHeaderStatus status = wind_cheader_function(loaded->parsed_header, name, &f);
    const WindCOwnership *rule = ownership(module, name);
    const char *hint = NULL;
    if (status == C_HEADER_NO_FUNCTION) {
        snprintf(message, sizeof message, "C header <%s> does not declare function `%.100s`", module->header, name);
    } else if (status != C_HEADER_OK) {
        snprintf(message, sizeof message, "C function `%.150s` has a signature Wind cannot import automatically",
                 name);
    } else if (f.result == TK_KW_STR && (!rule || !rule->copy)) {
        snprintf(message, sizeof message, "string result from C function `%.150s` needs `copy_cstr`", name);
        snprintf(suggestion, sizeof suggestion, "Add `copy_cstr %.150s` if it returns a borrowed C string.", name);
        hint = suggestion;
    } else {
        unsigned strings = 0, callbacks = 0, lengths = 0;
        int wrong_rule = 0;
        for (int i = 0; i < f.nargs; i++) {
            if (f.args[i] == TK_KW_STR) {
                strings |= 1u << i;
                if (rule && (rule->borrowed & (1u << i))) continue;
                snprintf(message, sizeof message, "argument %d of C function `%.120s` needs `borrow_cstr`", i + 1, name);
                snprintf(suggestion, sizeof suggestion, "Add `borrow_cstr %.120s %d` if C does not retain the pointer.",
                         name, i + 1);
                hint = suggestion;
                break;
            }
            if (f.args[i] == TK_C_CALLBACK) {
                callbacks |= 1u << i;
                if (rule && (rule->callbacks & (1u << i))) continue;
                snprintf(message, sizeof message, "argument %d of C function `%.120s` needs `borrow_callback`",
                         i + 1, name);
                snprintf(suggestion, sizeof suggestion, "Add `borrow_callback %.100s %d` for synchronous calls on the same thread.",
                         name, i + 1);
                hint = suggestion;
                break;
            }
            int length = rule ? rule->lengths[i] : 0;
            if (wind_c_buffer_element(f.args[i]) == TK_BAD) {
                if (length) wrong_rule = 1;
                continue;
            }
            if (!length) {
                snprintf(message, sizeof message, "argument %d of C function `%.120s` needs `borrow_buffer`",
                         i + 1, name);
                snprintf(suggestion, sizeof suggestion, "Add `borrow_buffer %.100s %d length_argument` if C does not retain the array.",
                         name, i + 1);
                hint = suggestion;
                break;
            }
            if (length > f.nargs || f.args[length - 1] != TK_KW_INT || (lengths & (1u << (length - 1)))) {
                wrong_rule = 1;
                continue;
            }
            lengths |= 1u << (length - 1);
            f.buffer_lengths[i] = (unsigned char)length;
        }
        if (rule) {
            for (int i = 0; i < WIND_C_MAX_PARAMS; i++)
                if (rule->lengths[i] && !f.buffer_lengths[i]) wrong_rule = 1;
            if ((rule->borrowed & ~strings) || (rule->callbacks & ~callbacks) ||
                (rule->copy && f.result != TK_KW_STR)) wrong_rule = 1;
        }
        if (!hint && wrong_rule) {
            snprintf(message, sizeof message, "ownership rule does not match C function `%.140s`", name);
        } else if (!hint) {
            f.name = copy_n(name, strlen(name));
            f.symbol = copy_n(name, strlen(name));
            f.copy_string = f.result == TK_KW_STR;
            f.range = module->header_range;
            return add_function(loaded, f);
        }
    }
    wind_diag_emit(diags, &(WindDiagnostic){
        .code = WD_C_HEADER_DISCOVERY, .source = source, .primary = range,
        .message = message, .suggestions = hint
    });
    return NULL;
}

void wind_c_free(const WindCModule *module) {
    if (!module || !module->source) return;
    wind_cheader_free(module->parsed_header);
    for (WindCOwnership *rule = module->ownership; rule;) {
        WindCOwnership *next = rule->next;
        free(rule->symbol);
        free(rule);
        rule = next;
    }
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

static int parse_function(BindingParser *p, WindCModule *m) {
    Token *name = expect(p, TK_IDENT, "expected binding function name");
    if (!name || !expect(p, TK_LPAREN, "expected '(' after binding function name")) return 0;
    if (wind_c_function(m, name->text)) return binding_error(p, name, "duplicate binding function");
    WindCFunction f = {0};
    const char *names[WIND_C_MAX_PARAMS];
    if (p->t->kind != TK_RPAREN) {
        for (;;) {
            Token *type = p->t++;
            if (type->kind != TK_KW_INT && type->kind != TK_KW_FRAC && type->kind != TK_KW_STR)
                return binding_error(p, type, "C parameters must have type `int`, `frac`, or `str`");
            if (f.nargs == WIND_C_MAX_PARAMS)
                return binding_error(p, type, "too many C parameters");
            if (!expect(p, TK_DOT, "expected '.' between parameter type and name")) return 0;
            Token *arg = expect(p, TK_IDENT, "expected parameter name");
            if (!arg) return 0;
            for (int i = 0; i < f.nargs; i++)
                if (!strcmp(names[i], arg->text)) return binding_error(p, arg, "duplicate parameter name");
            names[f.nargs] = arg->text;
            f.args[f.nargs++] = type->kind;
            if (type->kind == TK_KW_STR) {
                if (!word(p->t, "borrow_cstr"))
                    return binding_error(p, p->t, "`str` parameters require `borrow_cstr`");
                p->t++;
            }
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
    add_function(m, f);
    return 1;
}

typedef enum { COPY_STRING, BORROW_STRING, BORROW_BUFFER, BORROW_CALLBACK } OwnershipKind;

static int argument_index(BindingParser *p) {
    Token *index = expect(p, TK_INT_LIT, "expected a 1-based C argument number");
    if (!index) return 0;
    if (index->ival < 1 || index->ival > WIND_C_MAX_PARAMS)
        return binding_error(p, index, "argument number is out of range");
    return (int)index->ival;
}

static int parse_ownership(BindingParser *p, WindCModule *module, OwnershipKind kind) {
    Token *name = expect(p, TK_IDENT, "expected C function name for ownership rule");
    if (!name) return 0;
    int index = kind == COPY_STRING ? 1 : argument_index(p);
    if (!index) return 0;
    int length = kind == BORROW_BUFFER ? argument_index(p) : 0;
    if (kind == BORROW_BUFFER && !length) return 0;
    unsigned bit = 1u << (index - 1);
    if (!end_line(p)) return 0;
    WindCOwnership *rule = ownership(module, name->text);
    if (!rule) {
        rule = checked(calloc(1, sizeof *rule));
        rule->symbol = copy_n(name->text, strlen(name->text));
        rule->next = module->ownership;
        module->ownership = rule;
    }
    if ((kind == COPY_STRING && rule->copy) ||
        (kind == BORROW_STRING && (bit & rule->borrowed)) ||
        (kind == BORROW_CALLBACK && (bit & rule->callbacks)) ||
        (kind == BORROW_BUFFER && rule->lengths[index - 1]))
        return binding_error(p, name, "duplicate ownership rule");
    if (kind == COPY_STRING) rule->copy = 1;
    else if (kind == BORROW_STRING) rule->borrowed |= bit;
    else if (kind == BORROW_CALLBACK) rule->callbacks |= bit;
    else rule->lengths[index - 1] = (unsigned char)length;
    module->auto_header = 1;
    return 1;
}

static const WindCModule *parse_binding(const char *name, const WindSource *source, WindDiagnostics *diags) {
    WindCModule *m = checked(calloc(1, sizeof *m));
    m->name = name;
    m->source = source;
    m->functions = checked(calloc(1, sizeof *m->functions));
    int count, ok = 1;
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
            ok = parse_function(&p, m);
        } else if (word(first, "copy_cstr")) {
            ok = parse_ownership(&p, m, COPY_STRING);
        } else if (word(first, "borrow_cstr")) {
            ok = parse_ownership(&p, m, BORROW_STRING);
        } else if (word(first, "borrow_buffer")) {
            ok = parse_ownership(&p, m, BORROW_BUFFER);
        } else if (word(first, "borrow_callback")) {
            ok = parse_ownership(&p, m, BORROW_CALLBACK);
        } else if (word(first, "auto")) {
            m->auto_header = 1;
            ok = end_line(&p);
        } else {
            ok = binding_error(&p, first, "expected `header`, `link`, `func`, `auto`, or an ownership rule");
        }
    }
    if (ok && (!m->header || !m->library))
        ok = binding_error(&p, p.t, "a binding requires `header` and `link`");
    if (ok && !m->functions[0].name) m->auto_header = 1;
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
