#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astcodegen.h"

typedef enum {
    WT_INT, WT_FRAC, WT_STR, WT_VOID, WT_BOOL, WT_JSON, WT_SQLITE_DB, WT_SQLITE_STMT, WT_UNKNOWN
} WType;

typedef enum { CK_SCALAR, CK_ARRAY, CK_LIST, CK_DICT } CKind;
typedef struct { CKind ck; WType elem; WType key; } VType;  /* elem: тип элемента/значения; key: ключ dict */
static VType vt_scalar(WType t) { VType v; v.ck = CK_SCALAR; v.elem = t; v.key = WT_UNKNOWN; return v; }

typedef struct { char name[64]; VType vt; } Sym;
typedef struct { char name[64]; WType ret;  } FuncSig;

static jmp_buf  cg_jb;
static WindDiagnostics *cg_diags;
static const Stmt *cg_stmt, *cg_func;
static const WindSource *cg_source;
static const Program *cg_program;
static WindRange cg_interp_range;
static int cg_has_handles;
static int cg_has_callbacks;

static Sym      g_glob[256]; static int g_nglob;
static Sym      g_loc[256];  static int g_nloc;     /* сбрасывается на каждую функцию/main */
static FuncSig  g_func[256]; static int g_nfunc;
static int      g_rep_id;                            /* уникальные счётчики repeat-циклов */
static int      g_gc_strings, g_in_main, g_gc_id, g_volatile;

static void cg_fail(const char *fmt, ...) {
    char message[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    wind_diag_emit(cg_diags, &(WindDiagnostic){
        .code = WD_CODEGEN, .source = cg_source, .message = message,
        .primary = cg_interp_range.line ? cg_interp_range :
                   cg_stmt ? (WindRange){.line = cg_stmt->line, .col = cg_stmt->col} : (WindRange){0}
    });
    longjmp(cg_jb, 1);
}

static void cg_at(const Stmt *s) {
    cg_stmt = s;
    if (s->source) cg_source = s->source;
}

static void sym_add(Sym *arr, int *n, const char *name, VType vt) {
    if (*n >= 256) cg_fail("too many variables");
    snprintf(arr[*n].name, sizeof arr[*n].name, "%s", name);
    arr[*n].vt = vt; (*n)++;
}
static int local_declared(const char *name) {
    for (int i = g_nloc - 1; i >= 0; i--) if (!strcmp(g_loc[i].name, name)) return 1;
    return 0;
}
static VType var_vtype(const char *name) {
    for (int i = g_nloc - 1; i >= 0; i--) if (!strcmp(g_loc[i].name, name)) return g_loc[i].vt;
    for (int i = g_nglob - 1; i >= 0; i--) if (!strcmp(g_glob[i].name, name)) return g_glob[i].vt;
    return vt_scalar(WT_UNKNOWN);
}
static WType var_type(const char *name) {
    VType v = var_vtype(name);
    return v.ck == CK_SCALAR ? v.elem : WT_UNKNOWN;
}
static WType func_ret(const char *name) {
    for (int i = 0; i < g_nfunc; i++) if (!strcmp(g_func[i].name, name)) return g_func[i].ret;
    return WT_UNKNOWN;
}

static WType native_ret(const char *name) {
    if (!g_gc_strings) return WT_UNKNOWN;
    static const char *names[] = {
        "acos", "asin", "atan", "atan2", "ceil", "cos", "exp", "fabs", "floor",
        "fmod", "hypot", "log", "log10", "pow", "round", "sin", "sqrt", "tan"
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(names[i], name)) return WT_FRAC;
    return WT_UNKNOWN;
}

static WType wt_base(TokenKind t) {
    switch (t) {
        case TK_KW_INT:  return WT_INT;
        case TK_KW_FRAC: return WT_FRAC;
        case TK_KW_STR:  return WT_STR;
        case TK_KW_BOOL: return WT_BOOL;
        case TK_KW_VOID: return WT_VOID;
        case TK_C_SQLITE_DB: return WT_SQLITE_DB;
        case TK_C_SQLITE_STMT: return WT_SQLITE_STMT;
        default:         return WT_UNKNOWN;
    }
}
static const char *wind_type_name(WType t) {
    switch (t) {
        case WT_INT:  return "int";
        case WT_FRAC: return "frac";
        case WT_STR:  return "str";
        case WT_BOOL: return "bool";
        case WT_JSON: return "json";
        case WT_VOID: return "void";
        case WT_SQLITE_DB: return "sqlite.connection";
        case WT_SQLITE_STMT: return "sqlite.statement";
        default:      return "unknown";
    }
}

/* Префикс проверяет тип из таблицы имён, а не заменяет его. */
static VType typed_ref(const Expr *e) {
    const char *name = e->as.typed.name;
    WType prefix = wt_base(e->as.typed.type);
    VType v = var_vtype(name);
    if (v.elem == WT_UNKNOWN)
        cg_fail("typed reference '%s.%s': variable '%s' is not declared",
                wind_type_name(prefix), name, name);
    if (v.elem != prefix)
        cg_fail("typed reference '%s.%s': '%s' has %stype '%s'",
                wind_type_name(prefix), name, name,
                v.ck == CK_SCALAR ? "" : "element ", wind_type_name(v.elem));
    return v;
}

static VType coll_vtype(const Expr *coll) {
    if (coll->kind == EX_IDENT) return var_vtype(coll->as.ident);
    if (coll->kind == EX_TYPED) return typed_ref(coll);
    return vt_scalar(WT_UNKNOWN);
}

static const char *ctype(WType t) {
    switch (t) {
        case WT_INT: case WT_BOOL: return "int";
        case WT_FRAC: return "double";
        case WT_STR:  return "char*";
        case WT_JSON: return "_wj*";
        case WT_VOID: return "void";
        case WT_SQLITE_DB: return "WindSqliteDb";
        case WT_SQLITE_STMT: return "WindSqliteStmt";
        default: cg_fail("unknown type in codegen"); return "?";
    }
}
static const char *list_suffix(WType t) {
    switch (t) {
        case WT_INT: case WT_BOOL: return "int";
        case WT_FRAC: return "frac";
        case WT_STR:  return "str";
        default: cg_fail("list: unsupported element type"); return "?";
    }
}

static WType infer(const Expr *e);

static int handle_type(WType type) {
    return type == WT_SQLITE_DB || type == WT_SQLITE_STMT;
}

static void reject_handle(const Expr *e, const char *context) {
    if (cg_has_handles && handle_type(infer(e))) cg_fail("C handles cannot be used %s", context);
}

static _Noreturn void c_call_error(const Expr *callee, const char *message, const char *suggestions) {
    WindRange range = {.line = callee->line, .col = callee->col};
    wind_diag_emit(cg_diags, &(WindDiagnostic){
        .code = WD_C_IMPORT, .source = cg_source,
        .primary = cg_interp_range.line ? cg_interp_range : range,
        .message = message, .suggestions = suggestions
    });
    longjmp(cg_jb, 1);
}

static const WindCFunction *imported_c_function(const Expr *callee) {
    const Expr *receiver = callee->as.dot.obj;
    if (receiver->kind != EX_IDENT) return NULL;
    const WindCModule *module = NULL;
    for (int i = 0; i < cg_program->nc_imports; i++)
        if (!strcmp(cg_program->c_imports[i].name, receiver->as.ident)) {
            module = cg_program->c_imports[i].binding;
            break;
        }
    if (!module) module = wind_c_module(receiver->as.ident);
    if (!module) return NULL;
    char message[256], suggestion[128];
    if (!ast_has_c_import(cg_program, module)) {
        if (var_vtype(receiver->as.ident).elem != WT_UNKNOWN) return NULL;
        snprintf(message, sizeof message, "C binding `%s` is not imported", module->name);
        snprintf(suggestion, sizeof suggestion, "Add `c.import <%s>` at the top level.", module->name);
        c_call_error(callee, message, suggestion);
    }
    WindRange range = cg_interp_range.line ? cg_interp_range :
        (WindRange){.line = callee->line, .col = callee->col};
    unsigned before = cg_diags->count;
    const WindCFunction *function = wind_c_resolve(module, callee->as.dot.field, cg_diags, cg_source, range);
    if (function) return function;
    if (cg_diags->count != before) longjmp(cg_jb, 1);
    snprintf(message, sizeof message, "C binding `%s` has no function `%.140s`",
             module->name, callee->as.dot.field);
    snprintf(suggestion, sizeof suggestion, "This binding provides `%s.%s()`.",
             module->name, module->functions[0].name);
    c_call_error(callee, message, suggestion);
}

static int legacy_c_link(const WindCModule *module) {
    return module->legacy_link && ast_has_link(cg_program, module->library);
}

static const WindCFunction *legacy_c_function(const char *name) {
    for (const WindCModule *m = wind_c_modules; m->name; m++) {
        if (!legacy_c_link(m)) continue;
        for (const WindCFunction *f = m->functions; f->name; f++)
            if (f->legacy_symbol && !strcmp(f->symbol, name)) return f;
    }
    return NULL;
}

static WType infer_binary(const Expr *e) {
    TokenKind op = e->as.binary.op;
    switch (op) {
        case TK_EQ: case TK_NEQ: case TK_LT: case TK_GT:
        case TK_LE: case TK_GE: case TK_AND: case TK_OR:
            return WT_BOOL;
        default: break;
    }
    WType l = infer(e->as.binary.l), r = infer(e->as.binary.r);
    if (op == TK_PLUS && (l == WT_STR || r == WT_STR)) return WT_STR;
    if (l == WT_FRAC || r == WT_FRAC) return WT_FRAC;
    return WT_INT;
}

static WType infer(const Expr *e) {
    if (!e) return WT_UNKNOWN;
    switch (e->kind) {
        case EX_INT:    return WT_INT;
        case EX_FRAC:   return WT_FRAC;
        case EX_STR:    return WT_STR;
        case EX_INTERP: return WT_STR;
        case EX_IDENT:  return var_type(e->as.ident);
        case EX_TYPED: {
            VType v = typed_ref(e);
            return v.ck == CK_SCALAR ? v.elem : WT_UNKNOWN;
        }
        case EX_GROUP:  return infer(e->as.group.inner);
        case EX_UNARY:  return infer(e->as.unary.e);
        case EX_BINARY: return infer_binary(e);
        case EX_INDEX:  return coll_vtype(e->as.index.coll).elem;
        case EX_DOT: {
            VType ov = coll_vtype(e->as.dot.obj);
            if ((ov.ck == CK_LIST || ov.ck == CK_DICT) && !strcmp(e->as.dot.field, "len")) return WT_INT;
            if (ov.ck == CK_SCALAR && ov.elem == WT_STR && !strcmp(e->as.dot.field, "len")) return WT_INT;
            return WT_UNKNOWN;
        }
        case EX_CALL: {
            const Expr *c = e->as.call.callee;
            if (c->kind == EX_DOT) {
                const WindCFunction *binding = imported_c_function(c);
                if (binding) return wt_base(binding->result);
                const Expr *o2 = c->as.dot.obj;
                if (o2->kind == EX_IDENT && !strcmp(o2->as.ident, "file")) {
                    if (!strcmp(c->as.dot.field, "read"))   return WT_STR;
                    if (!strcmp(c->as.dot.field, "exists")) return WT_BOOL;
                    return WT_VOID;
                }
                if (o2->kind == EX_IDENT && !strcmp(o2->as.ident, "time")) {
                    if (!strcmp(c->as.dot.field, "now"))   return WT_INT;
                    if (!strcmp(c->as.dot.field, "clock")) return WT_FRAC;
                    return WT_VOID;
                }
                if (o2->kind == EX_IDENT && !strcmp(o2->as.ident, "json")) {
                    const char *m = c->as.dot.field;
                    if (!strcmp(m, "parse") || !strcmp(m, "decode")) return WT_JSON;
                    if (!strcmp(m, "encode")) return WT_STR;
                    return WT_UNKNOWN;
                }
                if (o2->kind == EX_IDENT && !strcmp(o2->as.ident, "http")) {
                    const char *m = c->as.dot.field;
                    if (!strcmp(m, "get") || !strcmp(m, "post")) return WT_STR;
                    return WT_UNKNOWN;
                }
                if (infer(o2) == WT_JSON) {
                    const char *m = c->as.dot.field;
                    if (!strcmp(m, "get") || !strcmp(m, "at")) return WT_JSON;
                    if (!strcmp(m, "str") || !strcmp(m, "type")) return WT_STR;
                    if (!strcmp(m, "int") || !strcmp(m, "len")) return WT_INT;
                    if (!strcmp(m, "frac")) return WT_FRAC;
                    if (!strcmp(m, "bool") || !strcmp(m, "has")) return WT_BOOL;
                    return WT_UNKNOWN;
                }
                {
                    WType st = infer(o2);
                    if (st == WT_INT || st == WT_FRAC || st == WT_STR || st == WT_BOOL) {
                        const char *m = c->as.dot.field;
                        if (!strcmp(m, "int"))  return WT_INT;
                        if (!strcmp(m, "frac")) return WT_FRAC;
                        if (!strcmp(m, "str"))  return WT_STR;
                        if (!strcmp(m, "bool")) return WT_BOOL;
                    }
                }
                if (!strcmp(c->as.dot.field, "len")) return WT_INT;
                if (!strcmp(c->as.dot.field, "has")) return WT_BOOL;
                return WT_VOID;
            }
            if (c->kind == EX_IDENT) {
                if (!strcmp(c->as.ident, "int"))  return WT_INT;
                if (!strcmp(c->as.ident, "frac")) return WT_FRAC;
                if (!strcmp(c->as.ident, "str") || !strcmp(c->as.ident, "to_str")) return WT_STR;
                if (!strcmp(c->as.ident, "bool")) return WT_BOOL;
                if (!strcmp(c->as.ident, "len")) return WT_INT;
                WType t = func_ret(c->as.ident);
                if (t != WT_UNKNOWN) return t;
                const WindCFunction *binding = legacy_c_function(c->as.ident);
                return binding ? wt_base(binding->result) : native_ret(c->as.ident);
            }
            return WT_UNKNOWN;
        }
        default: return WT_UNKNOWN;  /* list/dict — считаются отдельно */
    }
}

static int numeric(WType t) {
    return t == WT_INT || t == WT_FRAC || t == WT_BOOL;
}

static void check_return(const Stmt *s) {
    if (!cg_func) return;
    WType want = wt_base(cg_func->as.func.ret);
    WType have = s->as.ret.value ? infer(s->as.ret.value) : WT_VOID;
    if (have == WT_UNKNOWN || have == want || (numeric(want) && numeric(have))) return;

    const char *expected = wind_type_name(want), *actual = wind_type_name(have);
    char message[256], expected_label[48], actual_label[48], note[192];
    snprintf(message, sizeof message, "expected `%s`, found `%s` in function `%.140s`",
             expected, actual, cg_func->as.func.name);
    snprintf(expected_label, sizeof expected_label, "expected `%s`", expected);
    snprintf(actual_label, sizeof actual_label, "found `%s`", actual);
    snprintf(note, sizeof note, "function `%.120s` declares a return type of `%s`", cg_func->as.func.name, expected);
    const char *suggestions = "Return a value of the declared type, or change the signature if the function\n"
                              "  is intended to return a different type.";
    if (want == WT_INT && have == WT_STR) {
        suggestions = "To return an integer literal, use `return 100` instead of `return \"100\"`.\n"
                      "  To convert a string containing a number, use `return int(text)`.\n"
                      "  If the function should return text, declare `-> str` instead.";
    } else if (want == WT_VOID) {
        suggestions = "Use `return` without a value if the function should not return one.\n"
                      "  Otherwise, declare the intended return type after `->`.";
    }
    wind_diag_emit(cg_diags, &(WindDiagnostic){
        .code = WD_RETURN, .source = cg_func->source,
        .primary = s->as.ret.range, .secondary = cg_func->as.func.type_range,
        .message = message, .title = "return type mismatch",
        .label = actual_label, .secondary_label = expected_label, .note = note,
        .suggestions = suggestions
    });
}

static void emit_expr(FILE *o, const Expr *e);

/* экранирование тела C-строки (без обрамляющих кавычек); dbl_pct — удваивать '%' */
static void emit_c_chars(FILE *o, const char *s, int dbl_pct) {
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", o); break;
            case '\\': fputs("\\\\", o); break;
            case '\n': fputs("\\n", o);  break;
            case '\t': fputs("\\t", o);  break;
            case '\r': fputs("\\r", o);  break;
            case '%':  fputs(dbl_pct ? "%%" : "%", o); break;
            default:   fputc(*p, o);
        }
    }
}
static void emit_c_string(FILE *o, const char *s) {
    fputc('"', o); emit_c_chars(o, s, 0); fputc('"', o);
}

static const char *interp_spec(WType t) {
    switch (t) {
        case WT_INT: case WT_BOOL: return "%d";
        case WT_FRAC: return "%g";
        case WT_STR:  return "%s";
        default: return NULL;
    }
}

/* упаковка выражения типа t в uint64 для рантайма словарей.
 * dup_str=1 — строковый ключ копируется (словарь владеет копией). */
static void emit_dict_pack(FILE *o, WType t, const Expr *e, int dup_str) {
    if (t == WT_STR) {
        fputs(dup_str ? "(uint64_t)(uintptr_t)wstr_dup(" : "(uint64_t)(uintptr_t)(", o);
        emit_expr(o, e); fputc(')', o);
    } else if (t == WT_FRAC) {
        fputs("_wd_packf(", o); emit_expr(o, e); fputc(')', o);
    } else {
        fputs("(uint64_t)(", o); emit_expr(o, e); fputc(')', o);
    }
}
/* открывающая часть распаковки uint64 -> тип t (закрывается одной ')') */
static void emit_unpack_open(FILE *o, WType t) {
    if (t == WT_STR)       fputs("(char*)(uintptr_t)(", o);
    else if (t == WT_FRAC) fputs("_wd_unpackf(", o);
    else                   fputs("(int)(", o);
}

static const char *binop_c(TokenKind op) {
    switch (op) {
        case TK_PLUS: return "+"; case TK_MINUS: return "-";
        case TK_STAR: return "*"; case TK_SLASH: return "/";
        case TK_MOD:  return "%";
        case TK_EQ: return "=="; case TK_NEQ: return "!=";
        case TK_LT: return "<";  case TK_GT: return ">";
        case TK_LE: return "<="; case TK_GE: return ">=";
        case TK_AND: return "&&"; case TK_OR: return "||";
        default: cg_fail("unsupported binary operator"); return "?";
    }
}

static void emit_var_ref(FILE *o, const Expr *e, int read) {
    const char *name = e->kind == EX_TYPED ? e->as.typed.name : e->as.ident;
    VType t = e->kind == EX_TYPED ? typed_ref(e) : var_vtype(name);
    if (g_gc_strings && read && (t.ck == CK_LIST || t.ck == CK_DICT ||
                                (t.ck == CK_SCALAR && (t.elem == WT_STR || t.elem == WT_JSON))))
        fprintf(o, "WIND_STR_KEEP(%s)", name);
    else fputs(name, o);
}

static int one_typo(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    if (!*a && !*b) return 0;
    if (*a && !strcmp(a + 1, b)) return 1;
    if (*b && !strcmp(a, b + 1)) return 1;
    if (!*a || !*b) return 0;
    return !strcmp(a + 1, b + 1) ||
           (a[0] == b[1] && a[1] == b[0] && !strcmp(a + 2, b + 2));
}

static _Noreturn void interpolation_error(const Expr *expr, WType type) {
    char message[256], title[256], note[256], suggestions[384];
    WindDiagnostic diag = {
        .code = WD_INTERPOLATION, .source = cg_source, .primary = cg_interp_range,
        .message = message,
        .suggestions = "Check the names and types in the expression. For example, with `str.title = \"Wind\"`,\n"
                       "  write `terminal.paste -> \"=== ${title} ===\"`."
    };
    if (type == WT_UNKNOWN) {
        snprintf(message, sizeof message, "cannot determine the type of this interpolation expression");
    } else {
        snprintf(message, sizeof message, "cannot interpolate a value of type `%s`", wind_type_name(type));
    }
    if (expr->kind == EX_DOT && expr->as.dot.obj->kind == EX_IDENT) {
        const char *prefix = expr->as.dot.obj->as.ident, *name = expr->as.dot.field;
        WType declared = var_type(name);
        const char *expected = wind_type_name(declared);
        if (var_vtype(prefix).elem == WT_UNKNOWN && interp_spec(declared) && one_typo(prefix, expected)) {
            snprintf(message, sizeof message, "unknown type prefix `%.80s` for `%.120s`", prefix, name);
            snprintf(title, sizeof title, "unknown type prefix `%.80s`", prefix);
            snprintf(note, sizeof note, "`%.120s` is declared as `%s`", name, expected);
            snprintf(suggestions, sizeof suggestions, "Use `${%s.%.120s}` or `${%.120s}`.",
                     expected, name, name);
            diag.title = title;
            diag.note = note;
            diag.suggestions = suggestions;
        }
    }
    wind_diag_emit(cg_diags, &diag);
    longjmp(cg_jb, 1);
}

static int callback_index(const Expr *arg, const WindCCallback *signature) {
    if (arg->kind != EX_IDENT || var_vtype(arg->as.ident).elem != WT_UNKNOWN) return -1;
    int index = 0;
    for (int i = 0; i < cg_program->body.n; i++) {
        const Stmt *s = cg_program->body.items[i];
        if (s->kind != ST_FUNC) continue;
        int id = index++;
        if (strcmp(s->as.func.name, arg->as.ident)) continue;
        if (s->as.func.ret != signature->result || s->as.func.nparams != signature->nargs) return -1;
        for (int j = 0; j < signature->nargs; j++)
            if (s->as.func.params[j].type != signature->args[j]) return -1;
        return id;
    }
    return -1;
}

static int scalar_callback(const Stmt *s) {
    WType result = wt_base(s->as.func.ret);
    if (result != WT_INT && result != WT_FRAC && result != WT_VOID) return 0;
    for (int i = 0; i < s->as.func.nparams; i++) {
        WType arg = wt_base(s->as.func.params[i].type);
        if (arg != WT_INT && arg != WT_FRAC) return 0;
    }
    return 1;
}

static void emit_callback(FILE *o, const Stmt *s, int id) {
    if (!scalar_callback(s)) return;
    int returns = wt_base(s->as.func.ret) != WT_VOID;
    const char *type = ctype(wt_base(s->as.func.ret));
    fprintf(o, "static _Thread_local int _wind_cb_active_%d;\n", id);
    fprintf(o, "__attribute__((unused)) static %s _wind_cb_%d(", type, id);
    if (!s->as.func.nparams) fputs("void", o);
    for (int i = 0; i < s->as.func.nparams; i++)
        fprintf(o, "%s%s _arg%d", i ? ", " : "", ctype(wt_base(s->as.func.params[i].type)), i);
    fprintf(o, ") {\n"
               "    if (!_wind_cb_active_%d) { fputs(\"Wind: C callback called outside its native call or on another thread\\n\", stderr); exit(EXIT_FAILURE); }\n"
               "    int _old_floor = _wind_cb_floor;\n"
               "    _wind_cb_floor = _wt_depth;\n    ", id);
    if (returns) fprintf(o, "%s _result = ", type);
    fprintf(o, "%s(", s->as.func.name);
    for (int i = 0; i < s->as.func.nparams; i++) fprintf(o, "%s_arg%d", i ? ", " : "", i);
    fputs(");\n    _wt_depth = _wind_cb_floor;\n    _wind_cb_floor = _old_floor;\n", o);
    if (returns) fputs("    return _result;\n", o);
    fputs("}\n", o);
}

static void emit_c_call(FILE *o, const Expr *e, const WindCFunction *binding) {
    /* Вложенный вызов может расширить таблицу привязки. */
    WindCFunction signature = *binding;
    binding = &signature;
    const Expr *callee = e->as.call.callee;
    char name[160], message[256];
    if (callee->kind == EX_DOT)
        snprintf(name, sizeof name, "%.70s.%.70s", callee->as.dot.obj->as.ident, binding->name);
    else
        snprintf(name, sizeof name, "%.150s", binding->symbol);
    int arg_index[WIND_C_MAX_PARAMS], source_buffer[WIND_C_MAX_PARAMS], callback[WIND_C_MAX_PARAMS];
    int nargs = 0, has_callback = 0;
    for (int i = 0; i < binding->nargs; i++) source_buffer[i] = callback[i] = -1;
    for (int i = 0; i < binding->nargs; i++)
        if (binding->buffer_lengths[i]) source_buffer[binding->buffer_lengths[i] - 1] = i;
    for (int i = 0; i < binding->nargs; i++)
        arg_index[i] = source_buffer[i] < 0 ? nargs++ : -1;
    if (e->as.call.nargs != nargs) {
        if (!nargs)
            snprintf(message, sizeof message, "`%s()` expects no arguments", name);
        else
            snprintf(message, sizeof message, "`%s()` expects %d argument(s), got %d",
                     name, nargs, e->as.call.nargs);
        c_call_error(callee, message, NULL);
    }
    for (int i = 0; i < binding->nargs; i++) {
        if (arg_index[i] < 0) continue;
        int n = arg_index[i];
        const Expr *arg = e->as.call.args[n];
        if (binding->args[i] == TK_C_CALLBACK) {
            callback[i] = callback_index(arg, &binding->callbacks[i]);
            if (callback[i] < 0)
                c_call_error(arg, "C callback requires a named Wind function with the exact signature", NULL);
            has_callback = 1;
            continue;
        }
        TokenKind elem = wind_c_buffer_element(binding->args[i]);
        if (elem != TK_BAD) {
            const char *var = arg->kind == EX_IDENT ? arg->as.ident :
                arg->kind == EX_TYPED ? arg->as.typed.name : NULL;
            VType v = coll_vtype(arg);
            if (v.ck != CK_ARRAY || v.elem != wt_base(elem))
                c_call_error(arg, "C buffer requires a named array of the matching element type", NULL);
            if (g_volatile && local_declared(var))
                c_call_error(arg, "local arrays in a function with `try` cannot be borrowed by C", NULL);
            continue;
        }
        WType expected = wt_base(binding->args[i]), actual = infer(arg);
        if (expected != actual) {
            snprintf(message, sizeof message, "argument %d of `%s()` expects `%s`, found `%s`",
                     n + 1, name, wind_type_name(expected), wind_type_name(actual));
            c_call_error(arg, message, NULL);
        }
    }
    int external = binding->range.line != 0;
    if (has_callback) {
        fputs("({ ", o);
        for (int i = 0; i < binding->nargs; i++) {
            if (arg_index[i] < 0 || callback[i] >= 0) continue;
            fprintf(o, "__auto_type _wind_arg_%d = (", i);
            emit_expr(o, e->as.call.args[arg_index[i]]);
            fputs("); ", o);
        }
        for (int i = 0; i < binding->nargs; i++)
            if (callback[i] >= 0) fprintf(o, "++_wind_cb_active_%d; ", callback[i]);
        if (binding->result != TK_KW_VOID) fputs("__auto_type _wind_cb_result = ", o);
    }
    if (binding->copy_string) fputs(external ? "_wind_copy_cstr(" : "wstr_dup(", o);
    if (external) fprintf(o, "(%s)(", binding->symbol);
    else fprintf(o, "%s(", binding->symbol);
    for (int i = 0; i < binding->nargs; i++) {
        if (i) fputs(", ", o);
        if (source_buffer[i] >= 0) {
            const Expr *array = e->as.call.args[arg_index[source_buffer[i]]];
            fputs("_wind_buffer_length(sizeof(", o);
            emit_expr(o, array);
            fputs(") / sizeof((", o);
            emit_expr(o, array);
            fputs(")[0]))", o);
        } else if (callback[i] >= 0) fprintf(o, "_wind_cb_%d", callback[i]);
        else if (has_callback) fprintf(o, "_wind_arg_%d", i);
        else emit_expr(o, e->as.call.args[arg_index[i]]);
    }
    fputc(')', o);
    if (binding->copy_string) {
        if (external) fprintf(o, ", \"%s\"", binding->symbol);
        fputc(')', o);
    }
    if (has_callback) {
        fputs("; ", o);
        for (int i = 0; i < binding->nargs; i++)
            if (callback[i] >= 0) fprintf(o, "--_wind_cb_active_%d; ", callback[i]);
        fputs(binding->result == TK_KW_VOID ? " (void)0; })" : "_wind_cb_result; })", o);
    }
}

static void emit_expr(FILE *o, const Expr *e) {
    switch (e->kind) {
        case EX_INT:   fprintf(o, "%ld", e->as.ival); break;
        case EX_FRAC:  fprintf(o, "%.17g", e->as.fval); break;
        case EX_STR:   emit_c_string(o, e->as.sval); break;
        case EX_INTERP: {
            int n = e->as.interp.n;
            WindRange outer = cg_interp_range;
            fputs("wstr_build(\"", o);
            for (int i = 0; i < n; i++) {
                emit_c_chars(o, e->as.interp.lits[i], 1);
                cg_interp_range = outer.line ? outer : e->as.interp.ranges[i];
                const Expr *expr = e->as.interp.exprs[i];
                WType type = infer(expr);
                const char *spec = interp_spec(type);
                if (!spec) interpolation_error(expr, type);
                fputs(spec, o);
            }
            emit_c_chars(o, e->as.interp.lits[n], 1);
            fputc('"', o);
            for (int i = 0; i < n; i++) {
                cg_interp_range = outer.line ? outer : e->as.interp.ranges[i];
                fputs(", ", o);
                WType pt = infer(e->as.interp.exprs[i]);
                if (pt == WT_INT || pt == WT_BOOL) {
                    fputs("(int)(", o); emit_expr(o, e->as.interp.exprs[i]); fputc(')', o);
                } else {
                    emit_expr(o, e->as.interp.exprs[i]);
                }
            }
            fputc(')', o);
            cg_interp_range = outer;
            break;
        }
        case EX_IDENT:
        case EX_TYPED:
            emit_var_ref(o, e, 1);
            break;
        case EX_GROUP: fputc('(', o); emit_expr(o, e->as.group.inner); fputc(')', o); break;
        case EX_UNARY:
            reject_handle(e->as.unary.e, "with operators");
            fputc('(', o);
            fputs(e->as.unary.op == TK_MINUS ? "-" : "", o);
            emit_expr(o, e->as.unary.e);
            fputc(')', o);
            break;
        case EX_BINARY: {
            reject_handle(e->as.binary.l, "with operators");
            reject_handle(e->as.binary.r, "with operators");
            TokenKind op = e->as.binary.op;
            if (op == TK_PLUS && infer(e) == WT_STR) {
                fputs("wstr_cat(", o);
                emit_expr(o, e->as.binary.l); fputs(", ", o);
                emit_expr(o, e->as.binary.r); fputc(')', o);
                break;
            }
            if ((op == TK_EQ || op == TK_NEQ)
                && infer(e->as.binary.l) == WT_STR && infer(e->as.binary.r) == WT_STR) {
                fputs("(strcmp(", o);
                emit_expr(o, e->as.binary.l); fputs(", ", o);
                emit_expr(o, e->as.binary.r);
                fprintf(o, ") %s 0)", op == TK_EQ ? "==" : "!=");
                break;
            }
            fputc('(', o);
            emit_expr(o, e->as.binary.l);
            fprintf(o, " %s ", binop_c(op));
            emit_expr(o, e->as.binary.r);
            fputc(')', o);
            break;
        }
        case EX_CALL: {
            const Expr *c = e->as.call.callee;
            if (c->kind == EX_DOT) {
                const WindCFunction *binding = imported_c_function(c);
                if (binding) {
                    emit_c_call(o, e, binding);
                    break;
                }
                for (int i = 0; i < e->as.call.nargs; i++)
                    reject_handle(e->as.call.args[i], "as arguments to non-binding functions");
                const Expr *obj = c->as.dot.obj;
                const char *m = c->as.dot.field;
            if (obj->kind == EX_IDENT && !strcmp(obj->as.ident, "time")) {
                if (!strcmp(m, "now"))   { fputs("_wtime_now()", o); break; }
                if (!strcmp(m, "clock")) { fputs("_wtime_clock()", o); break; }
                if (!strcmp(m, "sleep")) {
                    if (e->as.call.nargs != 1) cg_fail("time.sleep expects 1 argument");
                    fputs("_wtime_sleep((double)(", o); emit_expr(o, e->as.call.args[0]); fputs("))", o); break;
                }
                cg_fail("module time has no method .%s", m);
            }
            if (obj->kind == EX_IDENT && !strcmp(obj->as.ident, "file")) {
                int expected = (!strcmp(m, "write") || !strcmp(m, "append")) ? 2 : 1;
                if (e->as.call.nargs != expected) cg_fail("file.%s expects %d argument(s)", m, expected);
                if (!strcmp(m, "read"))   { fputs("_wf_read(", o); emit_expr(o, e->as.call.args[0]); fputc(')', o); break; }
                if (!strcmp(m, "exists")) { fputs("_wf_exists(", o); emit_expr(o, e->as.call.args[0]); fputc(')', o); break; }
                if (!strcmp(m, "write"))  { fputs("_wf_write(", o); emit_expr(o, e->as.call.args[0]); fputs(", ", o); emit_expr(o, e->as.call.args[1]); fputc(')', o); break; }
                if (!strcmp(m, "append")) { fputs("_wf_append(", o); emit_expr(o, e->as.call.args[0]); fputs(", ", o); emit_expr(o, e->as.call.args[1]); fputc(')', o); break; }
                cg_fail("module file has no method .%s", m);
            }
            if (obj->kind == EX_IDENT && !strcmp(obj->as.ident, "json")) {
                if ((!strcmp(m, "parse") || !strcmp(m, "decode")) && e->as.call.nargs == 1) {
                    fputs("_wj_parse(", o); emit_expr(o, e->as.call.args[0]); fputc(')', o); break;
                }
                if (!strcmp(m, "encode") && e->as.call.nargs == 1) {
                    const Expr *a = e->as.call.args[0];
                    if (infer(a) == WT_JSON) { fputs("_wj_encode(", o); emit_expr(o, a); fputc(')', o); break; }
                    VType av = coll_vtype(a);
                    if (av.ck == CK_DICT) {
                        if (av.key != WT_STR) cg_fail("json.encode: dict keys must be str");
                        fprintf(o, "_wj_enc_dict_%s(", list_suffix(av.elem));
                        emit_expr(o, a); fputc(')', o); break;
                    }
                    if (av.ck == CK_LIST) {
                        fprintf(o, "_wj_enc_list_%s(", list_suffix(av.elem));
                        emit_expr(o, a); fputc(')', o); break;
                    }
                    cg_fail("json.encode expects a json value, dict[str,_] or list");
                }
                cg_fail("module json has no method .%s", m);
            }
            if (obj->kind == EX_IDENT && !strcmp(obj->as.ident, "http")) {
                if (!strcmp(m, "get") && e->as.call.nargs == 1) {
                    fputs("_wh_get(", o); emit_expr(o, e->as.call.args[0]); fputc(')', o); break;
                }
                if (!strcmp(m, "post") && e->as.call.nargs == 2) {
                    fputs("_wh_post(", o); emit_expr(o, e->as.call.args[0]); fputs(", ", o); emit_expr(o, e->as.call.args[1]); fputc(')', o); break;
                }
                cg_fail("module http: bad call .%s (use http.get(url) / http.post(url,body); http.serve is a block)", m);
            }
            if (infer(obj) == WT_JSON) {
                if (!strcmp(m, "str"))  { fputs("_wj_str(",  o); emit_expr(o, obj); fputc(')', o); break; }
                if (!strcmp(m, "int"))  { fputs("_wj_int(",  o); emit_expr(o, obj); fputc(')', o); break; }
                if (!strcmp(m, "frac")) { fputs("_wj_frac(", o); emit_expr(o, obj); fputc(')', o); break; }
                if (!strcmp(m, "bool")) { fputs("_wj_bool(", o); emit_expr(o, obj); fputc(')', o); break; }
                if (!strcmp(m, "len"))  { fputs("_wj_len(",  o); emit_expr(o, obj); fputc(')', o); break; }
                if (!strcmp(m, "type")) { fputs("_wj_type(", o); emit_expr(o, obj); fputc(')', o); break; }
                if (e->as.call.nargs != 1) cg_fail("json .%s expects exactly 1 argument", m);
                if (!strcmp(m, "get")) { fputs("_wj_get(", o); emit_expr(o, obj); fputs(", ", o); emit_expr(o, e->as.call.args[0]); fputc(')', o); break; }
                if (!strcmp(m, "at"))  { fputs("_wj_at(",  o); emit_expr(o, obj); fputs(", ", o); emit_expr(o, e->as.call.args[0]); fputc(')', o); break; }
                if (!strcmp(m, "has")) { fputs("_wj_has(", o); emit_expr(o, obj); fputs(", ", o); emit_expr(o, e->as.call.args[0]); fputc(')', o); break; }
                cg_fail("json value has no method .%s", m);
            }
            {
                WType ot = infer(obj);
                if (ot == WT_INT || ot == WT_FRAC || ot == WT_STR || ot == WT_BOOL) {
                    if (!strcmp(m, "int")) {
                        if (ot == WT_STR) { fputs("wstr_to_int(", o); emit_expr(o, obj); fputc(')', o); }
                        else { fputs("(int)(", o); emit_expr(o, obj); fputc(')', o); }
                        break;
                    }
                    if (!strcmp(m, "frac")) {
                        if (ot == WT_STR) { fputs("wstr_to_frac(", o); emit_expr(o, obj); fputc(')', o); }
                        else { fputs("(double)(", o); emit_expr(o, obj); fputc(')', o); }
                        break;
                    }
                    if (!strcmp(m, "str")) {
                        if (ot == WT_STR) { emit_expr(o, obj); }
                        else if (ot == WT_FRAC) { fputs("wstr_from_frac(", o); emit_expr(o, obj); fputc(')', o); }
                        else { fputs("wstr_from_int(", o); emit_expr(o, obj); fputc(')', o); }
                        break;
                    }
                    if (!strcmp(m, "bool")) { fputs("((", o); emit_expr(o, obj); fputs(")!=0)", o); break; }
                }
            }

                VType ov = coll_vtype(obj);
                if (ov.ck == CK_LIST) {
                    if (!strcmp(m, "add")) {
                        if (e->as.call.nargs != 1) cg_fail(".add expects exactly 1 argument");
                        fprintf(o, "_wl_push_%s(", list_suffix(ov.elem));
                        emit_expr(o, obj); fputs(", ", o); emit_expr(o, e->as.call.args[0]); fputc(')', o);
                    } else if (!strcmp(m, "pop")) {
                        fputs("_wl_pop(", o); emit_expr(o, obj); fputc(')', o);
                    } else if (!strcmp(m, "len")) {
                        fputs("_wl_len(", o); emit_expr(o, obj); fputc(')', o);
                    } else cg_fail("unknown list method .%s", m);
                } else if (ov.ck == CK_DICT) {
                    if (!strcmp(m, "has")) {
                        if (e->as.call.nargs != 1) cg_fail(".has expects exactly 1 argument");
                        fputs("_wd_has(", o); emit_expr(o, obj); fputs(", ", o);
                        emit_dict_pack(o, ov.key, e->as.call.args[0], 0); fputc(')', o);
                    } else if (!strcmp(m, "len")) {
                        fputs("_wd_len(", o); emit_expr(o, obj); fputc(')', o);
                    } else cg_fail("unknown dict method .%s", m);
                } else cg_fail("method .%s is only supported on lists/dicts", m);
                break;
            }
            if (c->kind != EX_IDENT)
                cg_fail("call supported only by function name or list method");
            const char *fn = c->as.ident;
            for (int i = 0; i < e->as.call.nargs; i++)
                reject_handle(e->as.call.args[i], "as arguments to non-binding functions");
            if (!strcmp(fn, "len") && e->as.call.nargs == 1) {
                if (infer(e->as.call.args[0]) == WT_STR)
                    fputs("(int)strlen(", o);
                else if (g_gc_strings && coll_vtype(e->as.call.args[0]).ck == CK_DICT)
                    fputs("_wd_len(", o);
                else if (g_gc_strings && coll_vtype(e->as.call.args[0]).ck != CK_LIST)
                    cg_fail("--gc-strings: len() expects a string, list or dictionary");
                else
                    fputs("_wl_len(", o);
                emit_expr(o, e->as.call.args[0]); fputc(')', o);
                break;
            }
            if (e->as.call.nargs == 1 &&
                (!strcmp(fn, "str") || !strcmp(fn, "to_str") ||
                 !strcmp(fn, "int") || !strcmp(fn, "frac") || !strcmp(fn, "bool"))) {
                const Expr *arg = e->as.call.args[0];
                WType at = infer(arg);
                if (!strcmp(fn, "str") || !strcmp(fn, "to_str")) {
                    if (at == WT_STR) { emit_expr(o, arg); }
                    else if (at == WT_INT || at == WT_BOOL) {
                        fputs("wstr_from_int(", o); emit_expr(o, arg); fputc(')', o);
                    } else if (at == WT_FRAC) {
                        fputs("wstr_from_frac(", o); emit_expr(o, arg); fputc(')', o);
                    } else cg_fail("str(): cannot determine argument type");
                } else if (!strcmp(fn, "int")) {
                    if (at == WT_STR) { fputs("wstr_to_int(", o); emit_expr(o, arg); fputc(')', o); }
                    else { fputs("(int)(", o); emit_expr(o, arg); fputc(')', o); }
                } else if (!strcmp(fn, "bool")) {
                    fputs("((", o); emit_expr(o, arg); fputs(")!=0)", o);
                } else {
                    if (at == WT_STR) { fputs("wstr_to_frac(", o); emit_expr(o, arg); fputc(')', o); }
                    else { fputs("(double)(", o); emit_expr(o, arg); fputc(')', o); }
                }
                break;
            }
            WType user_ret = func_ret(fn);
            if (user_ret == WT_UNKNOWN) {
                const WindCFunction *binding = legacy_c_function(fn);
                if (binding) {
                    emit_c_call(o, e, binding);
                    break;
                }
                if (g_gc_strings && native_ret(fn) == WT_UNKNOWN)
                    cg_fail("--gc-strings: unknown function '%s'", fn);
            }
            int keep = g_gc_strings && user_ret == WT_STR;
            if (keep) fputs("WIND_STR_KEEP(", o);
            fprintf(o, "%s(", fn);
            for (int i = 0; i < e->as.call.nargs; i++) {
                if (i) fputs(", ", o);
                emit_expr(o, e->as.call.args[i]);
            }
            fputc(')', o);
            if (keep) fputc(')', o);
            break;
        }
        case EX_INDEX: {                              /* coll[idx]: массив / список / словарь */
            VType cv = coll_vtype(e->as.index.coll);
            int keep = g_gc_strings && cv.elem == WT_STR;
            if (g_gc_strings && cv.ck == CK_SCALAR && cv.elem != WT_STR)
                cg_fail("indexing requires a string, array, list or dictionary");
            if (keep) fputs("WIND_STR_KEEP(", o);
            if (g_gc_strings && cv.ck == CK_SCALAR) {
                fputs("wind_str_at(", o);
                emit_expr(o, e->as.index.coll); fputs(", ", o);
                emit_expr(o, e->as.index.idx); fputc(')', o);
            } else if (cv.ck == CK_LIST) {
                fprintf(o, "_wl_get_%s(", list_suffix(cv.elem));
                emit_expr(o, e->as.index.coll); fputs(", ", o);
                emit_expr(o, e->as.index.idx); fputc(')', o);
            } else if (cv.ck == CK_DICT) {
                emit_unpack_open(o, cv.elem);             /* распаковка значения */
                fputs("_wd_get(", o);
                emit_expr(o, e->as.index.coll); fputs(", ", o);
                emit_dict_pack(o, cv.key, e->as.index.idx, 0);  /* ключ, без dup */
                fputc(')', o);                            /* close _wd_get */
                fputc(')', o);                            /* close распаковки */
            } else {
                emit_expr(o, e->as.index.coll);
                fputc('[', o); emit_expr(o, e->as.index.idx); fputc(']', o);
            }
            if (keep) fputc(')', o);
            break;
        }
        case EX_DOT: {                                /* nums.len / cfg.len */
            VType ov = coll_vtype(e->as.dot.obj);
            if (ov.ck == CK_LIST && !strcmp(e->as.dot.field, "len")) {
                fputs("_wl_len(", o); emit_expr(o, e->as.dot.obj); fputc(')', o);
                break;
            }
            if (ov.ck == CK_DICT && !strcmp(e->as.dot.field, "len")) {
                fputs("_wd_len(", o); emit_expr(o, e->as.dot.obj); fputc(')', o);
                break;
            }
            if (ov.ck == CK_SCALAR && ov.elem == WT_STR && !strcmp(e->as.dot.field, "len")) {
                fputs("(int)strlen(", o); emit_expr(o, e->as.dot.obj); fputc(')', o);
                break;
            }
            cg_fail("access .%s is not supported", e->as.dot.field);
            break;
        }
        default:
            cg_fail("this expression (dict literal) is not supported yet");
    }
}

static void emit_block(FILE *o, const Block *b, int ind);
static void indent(FILE *o, int n) { for (int i = 0; i < n; i++) fputs("    ", o); }

static int needs_temps(const Expr *e) {
    switch (e->kind) {
        case EX_INTERP: case EX_CALL: return 1;
        case EX_IDENT: case EX_TYPED: {
            VType type = coll_vtype(e);
            return type.ck == CK_LIST || type.ck == CK_DICT ||
                   (type.ck == CK_SCALAR && (type.elem == WT_STR || type.elem == WT_JSON));
        }
        case EX_INDEX:
            return coll_vtype(e->as.index.coll).elem == WT_STR ||
                   needs_temps(e->as.index.coll) || needs_temps(e->as.index.idx);
        case EX_DOT: return needs_temps(e->as.dot.obj);
        case EX_GROUP: return needs_temps(e->as.group.inner);
        case EX_UNARY: return needs_temps(e->as.unary.e);
        case EX_BINARY:
            return (e->as.binary.op == TK_PLUS && infer(e) == WT_STR) ||
                   needs_temps(e->as.binary.l) || needs_temps(e->as.binary.r);
        default: return 0;
    }
}

static void emit_full_expr(FILE *o, const Expr *e) {
    int protect = g_gc_strings && needs_temps(e);
    if (protect) fputs("WIND_STR_EXPR(", o);
    emit_expr(o, e);
    if (protect) fputc(')', o);
}

static void emit_gc_root(FILE *o, const char *name, int ind) {
    indent(o, ind);
    fprintf(o, "WIND_STR_ROOT(%s, %d);\n", name, g_gc_id++);
}

static void emit_gc_json_root(FILE *o, const char *name, int ind) {
    indent(o, ind);
    fprintf(o, "WIND_ROOT(&%s, %d, 1, _wj_trace_slots);\n", name, g_gc_id++);
}

static void emit_gc_collection_root(FILE *o, const char *name, VType type, int ind) {
    if (!g_gc_strings) return;
    if (type.ck == CK_LIST || type.ck == CK_DICT) {
        indent(o, ind);
        fprintf(o, "WIND_ROOT(&%s, %d, 1, %s);\n", name, g_gc_id++,
                type.ck == CK_LIST ? "_wl_trace_slots" : "_wd_trace_slots");
    } else if (type.ck == CK_ARRAY && type.elem == WT_STR) {
        indent(o, ind);
        fprintf(o, "WIND_ROOT(%s, %d, sizeof %s / sizeof *%s, wind_str_trace_slots);\n",
                name, g_gc_id++, name, name);
    }
}

static void emit_gc_expr_start(FILE *o) {
    if (g_gc_strings) fputs("WIND_STR_EXPR(", o);
}

static void emit_gc_expr_end(FILE *o) {
    if (g_gc_strings) fputc(')', o);
}

static void emit_list_items(FILE *o, const Stmt *s, WType el, int ind) {
    const Expr *lst = s->as.decl.init;
    if (!lst) return;
    if (lst->kind != EX_LIST) cg_fail("list '%s' initializer expects a [..] literal", s->as.decl.name);
    for (int i = 0; i < lst->as.list.n; i++) {
        indent(o, ind);
        emit_gc_expr_start(o);
        fprintf(o, "_wl_push_%s(%s, ", list_suffix(el), s->as.decl.name);
        emit_expr(o, lst->as.list.items[i]);
        fputc(')', o);
        emit_gc_expr_end(o);
        fputs(";\n", o);
    }
}

static void emit_dict_items(FILE *o, const Stmt *s, WType key, WType value, int ind) {
    const Expr *d = s->as.decl.init;
    if (!d) return;
    if (d->kind != EX_DICT) cg_fail("dict '%s' initializer expects a [key: value, ...] literal", s->as.decl.name);
    for (int i = 0; i < d->as.dict.n; i++) {
        indent(o, ind);
        emit_gc_expr_start(o);
        fprintf(o, "_wd_set(%s, ", s->as.decl.name);
        emit_dict_pack(o, key, d->as.dict.keys[i], 1);
        fputs(", ", o);
        emit_dict_pack(o, value, d->as.dict.vals[i], 1);
        fputc(')', o);
        emit_gc_expr_end(o);
        fputs(";\n", o);
    }
}

static void emit_gc_checkpoint(FILE *o, int ind) {
    if (!g_gc_strings) return;
    indent(o, ind);
    fputs("WIND_CHECKPOINT();\n", o);
}

static void emit_print(FILE *o, const Expr *val, int ind) {
    WType t = infer(val);
    const char *fmt;
    switch (t) {
        case WT_INT: case WT_BOOL: fmt = "%d"; break;
        case WT_FRAC: fmt = "%g"; break;
        case WT_STR:  fmt = "%s"; break;
        default: cg_fail("terminal.paste: cannot determine value type");
                 return;
    }
    indent(o, ind);
    fprintf(o, "printf(\"%s\\n\", ", fmt);
    if (t == WT_INT || t == WT_BOOL) { fputs("(int)(", o); emit_full_expr(o, val); fputc(')', o); }
    else emit_full_expr(o, val);
    fputs(");\n", o);
}

static const char *local_qualifier(void) {
    return g_volatile ? " volatile" : "";
}

static void emit_local_decl(FILE *o, const Stmt *s, int ind) {
    const DeclType *dt = &s->as.decl.dtype;
    if (g_gc_strings && s->as.decl.is_global)
        cg_fail("--gc-strings: global declarations must be at the top level");
    if (dt->kind == DT_ARRAY) {
        if (local_declared(s->as.decl.name))
            cg_fail("array '%s' is already declared", s->as.decl.name);
        WType el = wt_base(dt->base);
        if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("unknown array type '%s'", s->as.decl.name);
        VType vt; vt.ck = CK_ARRAY; vt.elem = el; vt.key = WT_UNKNOWN;
        sym_add(g_loc, &g_nloc, s->as.decl.name, vt);
        indent(o, ind);
        fprintf(o, "%s%s %s[", ctype(el), local_qualifier(), s->as.decl.name);
        emit_expr(o, dt->array_size);
        fputs("] = {0};\n", o);
        emit_gc_collection_root(o, s->as.decl.name, vt, ind);
        return;
    }
    if (dt->kind == DT_LIST) {
        if (local_declared(s->as.decl.name))
            cg_fail("list '%s' is already declared", s->as.decl.name);
        WType el = wt_base(dt->base);
        if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("unknown list type '%s'", s->as.decl.name);
        VType vt; vt.ck = CK_LIST; vt.elem = el; vt.key = WT_UNKNOWN;
        sym_add(g_loc, &g_nloc, s->as.decl.name, vt);
        indent(o, ind);
        fprintf(o, "_wl *%s %s = _wl_new(sizeof(%s)", local_qualifier(), s->as.decl.name, ctype(el));
        if (g_gc_strings) fprintf(o, ", %d", el == WT_STR);
        fputs(");\n", o);
        emit_gc_collection_root(o, s->as.decl.name, vt, ind);
        emit_list_items(o, s, el, ind);
        return;
    }
    if (dt->kind == DT_DICT) {
        if (local_declared(s->as.decl.name))
            cg_fail("dict '%s' is already declared", s->as.decl.name);
        WType kt = wt_base(dt->key), vt = wt_base(dt->val);
        if (kt == WT_VOID || kt == WT_UNKNOWN || vt == WT_VOID || vt == WT_UNKNOWN)
            cg_fail("unknown dict type '%s'", s->as.decl.name);
        VType v; v.ck = CK_DICT; v.elem = vt; v.key = kt;
        sym_add(g_loc, &g_nloc, s->as.decl.name, v);
        indent(o, ind);
        fprintf(o, "_wd *%s %s = _wd_new(%d", local_qualifier(), s->as.decl.name, kt == WT_STR);
        if (g_gc_strings) fprintf(o, ", %d", vt == WT_STR);
        fputs(");\n", o);
        emit_gc_collection_root(o, s->as.decl.name, v, ind);
        emit_dict_items(o, s, kt, vt, ind);
        return;
    }
    WType t;
    if (dt->kind == DT_INFER)        t = infer(s->as.decl.init);
    else if (dt->kind == DT_SCALAR)  t = wt_base(dt->base);
    else cg_fail("unknown declaration kind");
    if (dt->kind != DT_INFER) reject_handle(s->as.decl.init, "in a scalar declaration; use var");
    if (t == WT_VOID || t == WT_UNKNOWN) cg_fail("cannot infer type of variable '%s'", s->as.decl.name);
    /* Повторное объявление того же типа — присваивание. */
    if (local_declared(s->as.decl.name)) {
        VType prev = var_vtype(s->as.decl.name);
        if (prev.ck != CK_SCALAR || prev.elem != t)
            cg_fail("variable '%s' is already declared with a different type", s->as.decl.name);
        if (!s->as.decl.init) return;
        indent(o, ind);
        fprintf(o, "%s = ", s->as.decl.name);
        emit_full_expr(o, s->as.decl.init);
        fputs(";\n", o);
        return;
    }
    if (g_gc_strings) {
        int temporary = g_gc_id++;
        if (s->as.decl.init) {
            /* Вычисляем до объявления: имя может совпадать с глобальным. */
            indent(o, ind);
            fprintf(o, "%s _wind_init_%d = ", ctype(t), temporary);
            emit_full_expr(o, s->as.decl.init);
            fputs(";\n", o);
        }
        indent(o, ind);
        fprintf(o, "%s%s %s = ", ctype(t), local_qualifier(), s->as.decl.name);
        if (s->as.decl.init) fprintf(o, "_wind_init_%d", temporary);
        else fputs(t == WT_STR ? "\"\"" : "0", o);
        fputs(";\n", o);
        sym_add(g_loc, &g_nloc, s->as.decl.name, vt_scalar(t));
        if (t == WT_STR) emit_gc_root(o, s->as.decl.name, ind);
        if (t == WT_JSON) emit_gc_json_root(o, s->as.decl.name, ind);
        return;
    }
    sym_add(g_loc, &g_nloc, s->as.decl.name, vt_scalar(t));
    indent(o, ind);
    fprintf(o, "%s %s", ctype(t), s->as.decl.name);
    if (s->as.decl.init) { fputs(" = ", o); emit_expr(o, s->as.decl.init); }
    fputs(";\n", o);
}

static const char *assignop_c(TokenKind op) {
    switch (op) {
        case TK_ASSIGN: return "="; case TK_PLUS_EQ: return "+=";
        case TK_MINUS_EQ: return "-="; case TK_STAR_EQ: return "*=";
        case TK_SLASH_EQ: return "/="; default: cg_fail("unknown assignment operator"); return "?";
    }
}

static void emit_loop_collection(FILE *o, const Expr *coll, int id) {
    if (g_gc_strings) fprintf(o, "_wind_iter_%d", id);
    else emit_expr(o, coll);
}

static void emit_wgc_try(FILE *o, const Stmt *s, int ind) {
    int saved_locals = g_nloc;
    indent(o, ind); fputs("{\n", o);
    indent(o, ind + 1); fputs("_wt_frame _wt_local __attribute__((cleanup(_wt_leave))) = _wt_enter();\n", o);
    indent(o, ind + 1); fputs("if (setjmp(_wt_jb[_wt_local.index]) == 0) {\n", o);
    emit_block(o, &s->as.tryc.body, ind + 2);
    indent(o, ind + 1); fputs("} else {\n", o);
    if (s->as.tryc.catch_var) {
        const char *name = s->as.tryc.catch_var;
        sym_add(g_loc, &g_nloc, name, vt_scalar(WT_STR));
        indent(o, ind + 2); fprintf(o, "char *volatile %s = _wt_msg;\n", name);
        emit_gc_root(o, name, ind + 2);
    }
    indent(o, ind + 2); fputs("_wt_msg = \"\";\n", o);
    emit_block(o, &s->as.tryc.catch_b, ind + 2);
    g_nloc = saved_locals;
    indent(o, ind + 1); fputs("}\n", o);
    indent(o, ind); fputs("}\n", o);
}

static void emit_stmt(FILE *o, const Stmt *s, int ind) {
    switch (s->kind) {
        case ST_VAR_DECL:
            emit_local_decl(o, s, ind);
            break;
        case ST_ASSIGN: {
            const Expr *tgt = s->as.assign.target;
            if (cg_has_handles) {
                WType target = infer(tgt), value = infer(s->as.assign.value);
                if ((handle_type(target) || handle_type(value)) &&
                    (target != value || s->as.assign.op != TK_ASSIGN))
                    cg_fail("C handle assignment requires '=' and matching handle types");
            }
            if (tgt->kind == EX_INDEX) {
                VType cv = coll_vtype(tgt->as.index.coll);
                if (cv.ck == CK_LIST) {
                    if (s->as.assign.op != TK_ASSIGN)
                        cg_fail("list: only '=' is supported for elements (not +=)");
                    indent(o, ind);
                    emit_gc_expr_start(o);
                    fprintf(o, "_wl_set_%s(", list_suffix(cv.elem));
                    emit_expr(o, tgt->as.index.coll); fputs(", ", o);
                    emit_expr(o, tgt->as.index.idx);  fputs(", ", o);
                    emit_expr(o, s->as.assign.value); fputc(')', o);
                    emit_gc_expr_end(o); fputs(";\n", o);
                    break;
                }
                if (cv.ck == CK_DICT) {
                    if (s->as.assign.op != TK_ASSIGN)
                        cg_fail("dict: only '=' is supported for elements (not +=)");
                    indent(o, ind);
                    emit_gc_expr_start(o);
                    fputs("_wd_set(", o);
                    emit_expr(o, tgt->as.index.coll); fputs(", ", o);
                    emit_dict_pack(o, cv.key, tgt->as.index.idx, 1);
                    fputs(", ", o);
                    emit_dict_pack(o, cv.elem, s->as.assign.value, 1);
                    fputc(')', o);
                    emit_gc_expr_end(o); fputs(";\n", o);
                    break;
                }
                if (g_gc_strings && cv.ck == CK_ARRAY) {
                    indent(o, ind);
                    int protect = needs_temps(tgt) || needs_temps(s->as.assign.value);
                    if (protect) emit_gc_expr_start(o);
                    emit_expr(o, tgt->as.index.coll); fputc('[', o);
                    emit_expr(o, tgt->as.index.idx); fputc(']', o);
                    fprintf(o, " %s ", assignop_c(s->as.assign.op));
                    emit_expr(o, s->as.assign.value);
                    if (protect) emit_gc_expr_end(o);
                    fputs(";\n", o);
                    break;
                }
                if (g_gc_strings) cg_fail("--gc-strings only supports indexed writes to collections");
            }
            if (tgt->kind != EX_IDENT && tgt->kind != EX_TYPED && tgt->kind != EX_INDEX)
                cg_fail("assignment supported to a variable or element a[i]");
            indent(o, ind);
            if (g_gc_strings) emit_var_ref(o, tgt, 0);
            else emit_expr(o, tgt);
            fprintf(o, " %s ", assignop_c(s->as.assign.op));
            emit_full_expr(o, s->as.assign.value);
            fputs(";\n", o);
            break;
        }
        case ST_IF: {
            for (int i = 0; i < s->as.iff.nclauses; i++) {
                reject_handle(s->as.iff.clauses[i].cond, "as conditions");
                indent(o, ind);
                fputs(i == 0 ? "if (" : "else if (", o);
                emit_full_expr(o, s->as.iff.clauses[i].cond);
                fputs(") {\n", o);
                emit_block(o, &s->as.iff.clauses[i].body, ind + 1);
                indent(o, ind); fputs("}\n", o);
            }
            if (s->as.iff.has_else) {
                indent(o, ind); fputs("else {\n", o);
                emit_block(o, &s->as.iff.else_b, ind + 1);
                indent(o, ind); fputs("}\n", o);
            }
            break;
        }
        case ST_WHILE:
            reject_handle(s->as.whilel.cond, "as conditions");
            indent(o, ind); fputs("while (", o);
            emit_full_expr(o, s->as.whilel.cond); fputs(") {\n", o);
            if (g_gc_strings && needs_temps(s->as.whilel.cond)) emit_gc_checkpoint(o, ind + 1);
            emit_block(o, &s->as.whilel.body, ind + 1);
            indent(o, ind); fputs("}\n", o);
            break;
        case ST_REPEAT: {
            int id = g_rep_id++;
            indent(o, ind);
            fprintf(o, "for (int __rep%d = 0; __rep%d < (", id, id);
            emit_full_expr(o, s->as.repeatl.count);
            fprintf(o, "); __rep%d++) {\n", id);
            if (g_gc_strings && needs_temps(s->as.repeatl.count)) emit_gc_checkpoint(o, ind + 1);
            emit_block(o, &s->as.repeatl.body, ind + 1);
            indent(o, ind); fputs("}\n", o);
            break;
        }
        case ST_FOR: {                              /* for i in A..B → for(int i=A; i<B; i++) */
            const char *v = s->as.forr.var;
            int before_loop = g_nloc;
            indent(o, ind);
            if (g_gc_strings) {
                int temporary = g_gc_id++;
                fprintf(o, "int _wind_init_%d = ", temporary);
                emit_full_expr(o, s->as.forr.from);
                fputs(";\n", o);
                indent(o, ind);
                fprintf(o, "for (int%s %s = (_wind_init_%d", local_qualifier(), v, temporary);
                sym_add(g_loc, &g_nloc, v, vt_scalar(WT_INT));
            } else {
                fprintf(o, "for (int %s = (", v);
                emit_expr(o, s->as.forr.from);
            }
            fprintf(o, "); %s < (", v);
            emit_full_expr(o, s->as.forr.to);
            fprintf(o, "); %s++) {\n", v);
            if (!g_gc_strings) sym_add(g_loc, &g_nloc, v, vt_scalar(WT_INT));
            if (g_gc_strings && needs_temps(s->as.forr.to)) emit_gc_checkpoint(o, ind + 1);
            emit_block(o, &s->as.forr.body, ind + 1);
            if (g_gc_strings) g_nloc = before_loop;
            indent(o, ind); fputs("}\n", o);
            break;
        }
        case ST_RETURN:
            if (g_gc_strings && g_in_main) {
                indent(o, ind); fputs("{ int _wind_status = ", o);
                if (s->as.ret.value) emit_full_expr(o, s->as.ret.value);
                else fputc('0', o);
                fputs("; wind_str_destroy(); return _wind_status; }\n", o);
                break;
            }
            indent(o, ind); fputs("return", o);
            if (s->as.ret.value) { fputc(' ', o); emit_full_expr(o, s->as.ret.value); }
            fputs(";\n", o);
            check_return(s);
            break;
        case ST_TRY: {
            if (g_gc_strings) {
                emit_wgc_try(o, s, ind);
                break;
            }
            indent(o, ind); fputs("{\n", o);
            indent(o, ind + 1);
            fputs("if (_wt_depth >= 32) { fprintf(stderr, \"try: too deep\\n\"); exit(1); }\n", o);
            indent(o, ind + 1);
            fputs("_wt_frame _wt_guard __attribute__((cleanup(_wt_leave))) = {_wt_depth++};\n", o);
            indent(o, ind + 1);
            fputs("if (setjmp(_wt_jb[_wt_guard.index]) == 0) {\n", o);
            emit_block(o, &s->as.tryc.body, ind + 2);
            indent(o, ind + 1); fputs("} else {\n", o);
            if (s->as.tryc.catch_var) {
                sym_add(g_loc, &g_nloc, s->as.tryc.catch_var, vt_scalar(WT_STR));
                indent(o, ind + 2);
                fprintf(o, "char *%s = (char*)_wt_msg; (void)%s;\n",
                        s->as.tryc.catch_var, s->as.tryc.catch_var);
            }
            emit_block(o, &s->as.tryc.catch_b, ind + 2);
            indent(o, ind + 1); fputs("}\n", o);
            indent(o, ind); fputs("}\n", o);
            break;
        }
        case ST_THROW:
            indent(o, ind); fputs("_wt_throw(", o);
            emit_full_expr(o, s->as.throwc.value);
            fputs(");\n", o);
            break;
        case ST_BREAK:    indent(o, ind); fputs("break;\n", o); break;
        case ST_CONTINUE: indent(o, ind); fputs("continue;\n", o); break;
        case ST_OUTPUT: {
            const Expr *t = s->as.output.target;
            int ok = t->kind == EX_DOT && t->as.dot.obj->kind == EX_IDENT
                     && !strcmp(t->as.dot.obj->as.ident, "terminal")
                     && !strcmp(t->as.dot.field, "paste");
            if (!ok) cg_fail("only terminal.paste is supported for output");
            emit_print(o, s->as.output.value, ind);
            break;
        }
        case ST_LOOP: {                              /* loop v in <список|словарь> ... end */
            VType cv = coll_vtype(s->as.loopl.coll);
            int id = g_rep_id++;
            int before_loop = g_nloc;
            if (g_gc_strings && (cv.ck == CK_LIST || cv.ck == CK_DICT)) {
                char name[40];
                snprintf(name, sizeof name, "_wind_iter_%d", id);
                indent(o, ind); fputs("{\n", o);
                indent(o, ++ind);
                fprintf(o, "%s *%s = ", cv.ck == CK_LIST ? "_wl" : "_wd", name);
                emit_full_expr(o, s->as.loopl.coll); fputs(";\n", o);
                emit_gc_collection_root(o, name, cv, ind);
            }
            if (cv.ck == CK_LIST) {
                indent(o, ind);
                fprintf(o, "for (int __it%d = 0; __it%d < _wl_len(", id, id);
                emit_loop_collection(o, s->as.loopl.coll, id);
                fprintf(o, "); __it%d++) {\n", id);
                indent(o, ind + 1);
                fprintf(o, "%s%s %s = _wl_get_%s(", ctype(cv.elem), local_qualifier(),
                        s->as.loopl.var, list_suffix(cv.elem));
                emit_loop_collection(o, s->as.loopl.coll, id);
                fprintf(o, ", __it%d);\n", id);
                sym_add(g_loc, &g_nloc, s->as.loopl.var, vt_scalar(cv.elem));
                if (g_gc_strings && cv.elem == WT_STR) emit_gc_root(o, s->as.loopl.var, ind + 1);
                emit_block(o, &s->as.loopl.body, ind + 1);
                indent(o, ind); fputs("}\n", o);
            } else if (cv.ck == CK_DICT) {
                indent(o, ind);
                fprintf(o, "for (int __b%d = 0; __b%d < (", id, id);
                emit_loop_collection(o, s->as.loopl.coll, id);
                fprintf(o, ")->nb; __b%d++)\n", id);
                indent(o, ind + 1);
                fprintf(o, "for (_wde *__e%d = (", id);
                emit_loop_collection(o, s->as.loopl.coll, id);
                fprintf(o, ")->b[__b%d]; __e%d; __e%d = __e%d->next) {\n", id, id, id, id);
                indent(o, ind + 2);
                fprintf(o, "%s%s %s = ", ctype(cv.key), local_qualifier(), s->as.loopl.var);
                emit_unpack_open(o, cv.key);
                fprintf(o, "__e%d->key);\n", id);
                sym_add(g_loc, &g_nloc, s->as.loopl.var, vt_scalar(cv.key));
                if (g_gc_strings && cv.key == WT_STR) emit_gc_root(o, s->as.loopl.var, ind + 2);
                emit_block(o, &s->as.loopl.body, ind + 2);
                indent(o, ind + 1); fputs("}\n", o);
            } else {
                cg_fail("loop..in is supported over lists and dicts");
            }
            if (g_gc_strings) {
                g_nloc = before_loop;
                indent(o, ind - 1); fputs("}\n", o);
            }
            break;
        }
        case ST_EXPR:
            indent(o, ind);
            emit_full_expr(o, s->as.expr.expr);
            fputs(";\n", o);
            break;
        case ST_HTTP_SERVE: {
            indent(o, ind);   fputs("{\n", o);
            indent(o, ind+1); fputs("int _wh_sfd = _wh_listen((int)(", o);
            emit_full_expr(o, s->as.serve.port); fputs("));\n", o);
            indent(o, ind+1); fputs("char _wh_path[2048];\n", o);
            indent(o, ind+1); fputs("for(;;){\n", o);
            indent(o, ind+2); fputs("int _wh_cfd = _wh_accept(_wh_sfd, _wh_path, sizeof _wh_path);\n", o);
            indent(o, ind+2); fputs("if(_wh_cfd < 0) continue;\n", o);
            indent(o, ind+2); fputs("const char *_wh_body;\n", o);
            for (int i = 0; i < s->as.serve.nroutes; i++) {
                const Route *rt = &s->as.serve.routes[i];
                if (infer(rt->handler) != WT_STR)
                    cg_fail("http.serve: route handler must produce a str");
                indent(o, ind+2);
                fprintf(o, "%sif(strcmp(_wh_path, ", i ? "else " : "");
                emit_full_expr(o, rt->path); fputs(")==0) _wh_body = ", o);
                emit_full_expr(o, rt->handler); fputs(";\n", o);
            }
            indent(o, ind+2);
            fputs(s->as.serve.nroutes ? "else _wh_body = \"404 Not Found\";\n"
                                      : "_wh_body = \"404 Not Found\";\n", o);
            indent(o, ind+2); fputs("_wh_respond(_wh_cfd, _wh_body);\n", o);
            emit_gc_checkpoint(o, ind + 2);
            indent(o, ind+1); fputs("}\n", o);
            indent(o, ind);   fputs("}\n", o);
            break;
        }
        default:
            cg_fail("this statement is not supported yet");
    }
}

static int stmt_needs_checkpoint(const Stmt *s) {
    switch (s->kind) {
        case ST_VAR_DECL:
            return s->as.decl.dtype.kind == DT_LIST || s->as.decl.dtype.kind == DT_DICT ||
                   (s->as.decl.init && needs_temps(s->as.decl.init));
        case ST_ASSIGN:
            return needs_temps(s->as.assign.target) || needs_temps(s->as.assign.value);
        case ST_OUTPUT: return needs_temps(s->as.output.value);
        case ST_EXPR: return needs_temps(s->as.expr.expr);
        case ST_IF:
            for (int i = 0; i < s->as.iff.nclauses; i++)
                if (needs_temps(s->as.iff.clauses[i].cond)) return 1;
            return 0;
        case ST_WHILE: return needs_temps(s->as.whilel.cond);
        case ST_REPEAT: return needs_temps(s->as.repeatl.count);
        case ST_FOR: return needs_temps(s->as.forr.from) || needs_temps(s->as.forr.to);
        default: return 0;
    }
}

static void emit_stmt_at_checkpoint(FILE *o, const Stmt *s, int ind) {
    const Stmt *saved_stmt = cg_stmt;
    const WindSource *saved_source = cg_source;
    cg_at(s);
    int checkpoint = g_gc_strings && stmt_needs_checkpoint(s);
    emit_stmt(o, s, ind);
    if (checkpoint) emit_gc_checkpoint(o, ind);
    cg_stmt = saved_stmt;
    cg_source = saved_source;
}

static void emit_block(FILE *o, const Block *b, int ind) {
    int before_block = g_nloc;
    for (int i = 0; i < b->n; i++) emit_stmt_at_checkpoint(o, b->items[i], ind);
    if (g_gc_strings) g_nloc = before_block;
}

static int block_has_try(const Block *block) {
    for (int i = 0; i < block->n; i++) {
        const Stmt *s = block->items[i];
        switch (s->kind) {
            case ST_TRY: return 1;
            case ST_IF:
                for (int j = 0; j < s->as.iff.nclauses; j++)
                    if (block_has_try(&s->as.iff.clauses[j].body)) return 1;
                if (s->as.iff.has_else && block_has_try(&s->as.iff.else_b)) return 1;
                break;
            case ST_WHILE: if (block_has_try(&s->as.whilel.body)) return 1; break;
            case ST_REPEAT: if (block_has_try(&s->as.repeatl.body)) return 1; break;
            case ST_FOR: if (block_has_try(&s->as.forr.body)) return 1; break;
            case ST_LOOP: if (block_has_try(&s->as.loopl.body)) return 1; break;
            default: break;
        }
    }
    return 0;
}

static void emit_func_signature(FILE *o, const Stmt *s) {
    WType ret = wt_base(s->as.func.ret);
    fprintf(o, "%s %s(", ctype(ret), s->as.func.name);
    if (s->as.func.nparams == 0) fputs("void", o);
    for (int i = 0; i < s->as.func.nparams; i++) {
        if (i) fputs(", ", o);
        fprintf(o, "%s%s %s", ctype(wt_base(s->as.func.params[i].type)), local_qualifier(),
                s->as.func.params[i].name);
    }
    fputc(')', o);
}

static void emit_func(FILE *o, const Stmt *s) {
    cg_at(s);
    cg_func = s;
    g_volatile = g_gc_strings && block_has_try(&s->as.func.body);
    emit_func_signature(o, s);
    fputs(" {\n", o);
    g_nloc = 0;
    for (int i = 0; i < s->as.func.nparams; i++) {
        const Param *p = &s->as.func.params[i];
        WType t = wt_base(p->type);
        sym_add(g_loc, &g_nloc, p->name, vt_scalar(t));
        if (g_gc_strings && t == WT_STR) emit_gc_root(o, p->name, 1);
    }
    emit_block(o, &s->as.func.body, 1);
    fputs("}\n\n", o);
    g_volatile = 0;
    cg_func = NULL;
}

int wind_codegen(const Program *p, FILE *out, WindDiagnostics *diags, int gc_strings) {
    cg_diags = diags;
    cg_program = p;
    cg_stmt = cg_func = NULL;
    cg_source = NULL;
    cg_interp_range = (WindRange){0};
    cg_has_handles = cg_has_callbacks = 0;
    for (int i = 0; i < p->nc_imports; i++) {
        if (p->c_imports[i].binding->runtime_source) cg_has_handles = 1;
        if (wind_c_has_callbacks(p->c_imports[i].binding)) cg_has_callbacks = 1;
    }
    unsigned previous = diags->count;
    g_nglob = g_nloc = g_nfunc = g_rep_id = 0;
    g_gc_strings = gc_strings;
    g_in_main = g_gc_id = g_volatile = 0;

    if (setjmp(cg_jb)) return 0;

    /* пасс 1: сигнатуры функций + типы глобалов (чтобы вызовы/ссылки типизировались) */
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        cg_at(s);
        if (s->kind == ST_FUNC) {
            if (g_nfunc >= 256) cg_fail("too many functions");
            snprintf(g_func[g_nfunc].name, sizeof g_func[g_nfunc].name, "%s", s->as.func.name);
            g_func[g_nfunc].ret = wt_base(s->as.func.ret);
            g_nfunc++;
        } else if (s->kind == ST_VAR_DECL && s->as.decl.is_global) {
            const DeclType *dt = &s->as.decl.dtype;
            if (g_gc_strings && (dt->kind == DT_LIST || dt->kind == DT_DICT)) {
                VType coll = dt->kind == DT_LIST
                    ? (VType){CK_LIST, wt_base(dt->base), WT_UNKNOWN}
                    : (VType){CK_DICT, wt_base(dt->val), wt_base(dt->key)};
                sym_add(g_glob, &g_nglob, s->as.decl.name, coll);
                continue;
            }
            VType vt;
            if (dt->kind == DT_ARRAY) {
                WType el = wt_base(dt->base);
                if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("unknown global array type '%s'", s->as.decl.name);
                vt.ck = CK_ARRAY; vt.elem = el; vt.key = WT_UNKNOWN;
            } else {
                WType t = (dt->kind == DT_INFER) ? infer(s->as.decl.init) : wt_base(dt->base);
                if (t == WT_VOID || t == WT_UNKNOWN) cg_fail("cannot infer type of global '%s'", s->as.decl.name);
                vt = vt_scalar(t);
            }
            sym_add(g_glob, &g_nglob, s->as.decl.name, vt);
        }
    }

    /* преамбула */
    fputs("/* Generated by Wind AST codegen */\n", out);
    fputs("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdarg.h>\n#include <stdint.h>\n#include <time.h>\n#include <math.h>\n\n", out);
    for (const WindCModule *m = wind_c_modules; m->name; m++)
        if (!ast_has_c_import(p, m) && legacy_c_link(m))
            fprintf(out, "#include <%s>\n", m->header);
    for (int i = 0; i < p->nc_imports; i++)
        wind_c_emit_header(out, p->c_imports[i].binding, i);

    if (cg_has_callbacks)
        fputs("static _Thread_local int _wind_cb_floor = -1;\n"
              "static void _wind_cb_check_throw(int depth) {\n"
              "    if (_wind_cb_floor >= 0 && depth <= _wind_cb_floor) {\n"
              "        fputs(\"Wind: exception cannot escape a C callback\\n\", stderr); exit(EXIT_FAILURE);\n"
              "    }\n}\n", out);
    else fputs("#define _wind_cb_check_throw(depth) ((void)0)\n", out);
    if (g_gc_strings) {
        fputs("#include \"strings_gc.h\"\n#include \"collections_gc.h\"\n"
              "#define WIND_GROW(p,used,n) wind_str_grow((p),(used),(n))\n"
              "#include <setjmp.h>\n"
              "typedef struct {int index;} _wt_frame;\n"
              "static jmp_buf _wt_jb[32];\n"
              "static WindStrState _wt_state[32];\n"
              "static int _wt_depth;\n"
              "static char *_wt_msg = \"\";\n"
              "__attribute__((unused)) static _wt_frame _wt_enter(void) {\n"
              "    if (_wt_depth >= 32) { fputs(\"try: too deep\\n\", stderr); exit(1); }\n"
              "    _wt_state[_wt_depth] = wind_str_snapshot();\n"
              "    return (_wt_frame){_wt_depth++};\n"
              "}\n"
              "__attribute__((unused)) static void _wt_leave(_wt_frame *frame) {\n"
              "    if (_wt_depth == frame->index + 1) --_wt_depth;\n"
              "}\n"
              "__attribute__((unused,noreturn)) static void _wt_throw(char *msg) {\n"
              "    _wind_cb_check_throw(_wt_depth);\n"
              "    _wt_msg = msg ? msg : \"\";\n"
              "    if (_wt_depth) {\n"
              "        int index = --_wt_depth;\n"
              "        wind_str_unwind(_wt_state[index]);\n"
              "        longjmp(_wt_jb[index], 1);\n"
              "    }\n"
              "    fprintf(stderr, \"uncaught: %s\\n\", _wt_msg); exit(1);\n"
              "}\n\n", out);
    } else {
    /* Boehm GC: весь malloc/calloc/realloc уходит под сборщик, free — пустышка */
    fputs("#include <gc.h>\n"
          "#define wind_str_alloc malloc\n"
          "#define WIND_GROW(p,used,n) realloc((p),(n))\n"
          "#define malloc(n)    GC_MALLOC(n)\n"
          "#define calloc(n,m)  GC_MALLOC((size_t)(n)*(size_t)(m))\n"
          "#define realloc(p,n) GC_REALLOC((p),(n))\n"
          "#define free(p)      ((void)(p))\n\n", out);

    fputs("#include <setjmp.h>\n"
          "static jmp_buf _wt_jb[32]; static int _wt_depth = 0;\n"
          "static const char *_wt_msg = \"\";\n"
          "typedef struct {int index;} _wt_frame;\n"
          "__attribute__((unused)) static void _wt_leave(_wt_frame *frame) {\n"
          "    if (_wt_depth == frame->index + 1) --_wt_depth;\n"
          "}\n"
          "__attribute__((unused)) static void _wt_throw(const char *m){\n"
          "    _wind_cb_check_throw(_wt_depth);\n"
          "    _wt_msg = m ? m : \"\";\n"
          "    if (_wt_depth > 0) longjmp(_wt_jb[--_wt_depth], 1);\n"
          "    fprintf(stderr, \"uncaught: %s\\n\", _wt_msg); exit(1);\n}\n\n", out);
    }

    fputs("__attribute__((unused)) static char *wstr_cat(const char *a, const char *b){\n"
          "    size_t la=strlen(a), lb=strlen(b);\n"
          "    char *r=wind_str_alloc(la+lb+1); memcpy(r,a,la); memcpy(r+la,b,lb+1); return r;\n}\n"
          "__attribute__((unused)) static char *wstr_build(const char *fmt, ...){\n"
          "    va_list a, b; va_start(a, fmt); va_copy(b, a);\n"
          "    int n = vsnprintf(NULL, 0, fmt, a); va_end(a);\n"
          "    char *r = wind_str_alloc((size_t)n + 1); vsnprintf(r, (size_t)n + 1, fmt, b); va_end(b);\n"
          "    return r;\n}\n"
          "__attribute__((unused)) static char *wstr_from_int(long v){\n"
          "    char t[32]; int n = snprintf(t, sizeof t, \"%ld\", v);\n"
          "    char *r = wind_str_alloc((size_t)n + 1); memcpy(r, t, (size_t)n + 1); return r;\n}\n"
          "__attribute__((unused)) static char *wstr_from_frac(double v){\n"
          "    char t[64]; int n = snprintf(t, sizeof t, \"%g\", v);\n"
          "    char *r = wind_str_alloc((size_t)n + 1); memcpy(r, t, (size_t)n + 1); return r;\n}\n"
          "__attribute__((unused)) static long   wstr_to_int(const char *s){ return strtol(s, NULL, 10); }\n"
          "__attribute__((unused)) static double wstr_to_frac(const char *s){ return strtod(s, NULL); }\n\n", out);

    if (!g_gc_strings) {
    /* рантайм динамических списков */
    fputs("typedef struct { void *data; int len; int cap; int esz; } _wl;\n"
          "__attribute__((unused)) static _wl *_wl_new(int esz){\n"
          "    _wl *l=malloc(sizeof *l); l->data=NULL; l->len=0; l->cap=0; l->esz=esz; return l; }\n"
          "__attribute__((unused)) static void _wl_grow(_wl *l){\n"
          "    if(l->len<l->cap) return; int nc=l->cap?l->cap*2:4;\n"
          "    l->data=realloc(l->data,(size_t)nc*l->esz); l->cap=nc; }\n"
          "__attribute__((unused)) static void _wl_oob(_wl *l,int i){\n"
          "    if(i<0||i>=l->len){ fprintf(stderr,\"list index %d out of range (len=%d)\\n\",i,l->len); exit(1);} }\n"
          "__attribute__((unused)) static void _wl_push_int(_wl *l,int v){ _wl_grow(l); ((int*)l->data)[l->len++]=v; }\n"
          "__attribute__((unused)) static void _wl_push_frac(_wl *l,double v){ _wl_grow(l); ((double*)l->data)[l->len++]=v; }\n"
          "__attribute__((unused)) static void _wl_push_str(_wl *l,char *v){ _wl_grow(l); ((char**)l->data)[l->len++]=v; }\n"
          "__attribute__((unused)) static int    _wl_get_int(_wl *l,int i){ _wl_oob(l,i); return ((int*)l->data)[i]; }\n"
          "__attribute__((unused)) static double _wl_get_frac(_wl *l,int i){ _wl_oob(l,i); return ((double*)l->data)[i]; }\n"
          "__attribute__((unused)) static char  *_wl_get_str(_wl *l,int i){ _wl_oob(l,i); return ((char**)l->data)[i]; }\n"
          "__attribute__((unused)) static void _wl_set_int(_wl *l,int i,int v){ _wl_oob(l,i); ((int*)l->data)[i]=v; }\n"
          "__attribute__((unused)) static void _wl_set_frac(_wl *l,int i,double v){ _wl_oob(l,i); ((double*)l->data)[i]=v; }\n"
          "__attribute__((unused)) static void _wl_set_str(_wl *l,int i,char *v){ _wl_oob(l,i); ((char**)l->data)[i]=v; }\n"
          "__attribute__((unused)) static int  _wl_len(_wl *l){ return l->len; }\n"
          "__attribute__((unused)) static void _wl_pop(_wl *l){ if(l->len>0) l->len--; }\n\n", out);

    /* рантайм словарей (хеш-таблица с цепочками; ключи/значения упакованы в uint64) */
    fputs("typedef struct _wde { struct _wde *next; uint64_t key; uint64_t val; } _wde;\n"
          "typedef struct { _wde **b; int nb; int count; int ks; } _wd;\n"
          "__attribute__((unused)) static char *wstr_dup(const char *s){ size_t n=strlen(s)+1; char *r=malloc(n); memcpy(r,s,n); return r; }\n"
          "__attribute__((unused)) static uint64_t _wd_packf(double d){ uint64_t u; memcpy(&u,&d,8); return u; }\n"
          "__attribute__((unused)) static double _wd_unpackf(uint64_t u){ double d; memcpy(&d,&u,8); return d; }\n"
          "__attribute__((unused)) static _wd *_wd_new(int ks){ _wd *d=malloc(sizeof *d); d->nb=16; d->b=calloc((size_t)d->nb,sizeof(_wde*)); d->count=0; d->ks=ks; return d; }\n"
          "__attribute__((unused)) static uint64_t _wd_h(_wd *d,uint64_t k){ if(d->ks){ const char*s=(const char*)(uintptr_t)k; uint64_t h=1469598103934665603ULL; while(*s){ h^=(unsigned char)*s++; h*=1099511628211ULL; } return h; } return k*1099511628211ULL+1234567ULL; }\n"
          "__attribute__((unused)) static int _wd_eq(_wd *d,uint64_t a,uint64_t b){ if(d->ks) return strcmp((const char*)(uintptr_t)a,(const char*)(uintptr_t)b)==0; return a==b; }\n"
          "__attribute__((unused)) static void _wd_set(_wd *d,uint64_t k,uint64_t v){ uint64_t i=_wd_h(d,k)%(uint64_t)d->nb; for(_wde*e=d->b[i];e;e=e->next) if(_wd_eq(d,e->key,k)){ e->val=v; return; } _wde*e=malloc(sizeof*e); e->key=k; e->val=v; e->next=d->b[i]; d->b[i]=e; d->count++; }\n"
          "__attribute__((unused)) static uint64_t _wd_get(_wd *d,uint64_t k){ uint64_t i=_wd_h(d,k)%(uint64_t)d->nb; for(_wde*e=d->b[i];e;e=e->next) if(_wd_eq(d,e->key,k)) return e->val; fprintf(stderr,\"dict: key not found\\n\"); exit(1); }\n"
          "__attribute__((unused)) static int _wd_has(_wd *d,uint64_t k){ uint64_t i=_wd_h(d,k)%(uint64_t)d->nb; for(_wde*e=d->b[i];e;e=e->next) if(_wd_eq(d,e->key,k)) return 1; return 0; }\n"
          "__attribute__((unused)) static int _wd_len(_wd *d){ return d->count; }\n\n", out);
    
    }

    fputs("__attribute__((unused)) static long _wtime_now(void){ return (long)time(NULL); }\n"
          "__attribute__((unused)) static double _wtime_clock(void){\n"
          "    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);\n"
          "    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9; }\n"
          "__attribute__((unused)) static void _wtime_sleep(double s){\n"
          "    struct timespec ts; ts.tv_sec=(time_t)s; ts.tv_nsec=(long)((s-(double)ts.tv_sec)*1e9);\n"
          "    nanosleep(&ts,NULL); }\n\n", out);

    fputs("__attribute__((unused)) static char *_wf_read(const char *path){\n"
          "    FILE *f=fopen(path,\"rb\"); if(!f){ fprintf(stderr,\"file.read: cannot open %s\\n\",path); exit(1);} \n"
          "    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<0) sz=0;\n"
          "    char *b=wind_str_alloc((size_t)sz+1); size_t g=fread(b,1,(size_t)sz,f); b[g]=0; fclose(f); return b; }\n"
          "__attribute__((unused)) static void _wf_write(const char *path,const char *t){\n"
          "    FILE *f=fopen(path,\"w\"); if(!f){ fprintf(stderr,\"file.write: cannot open %s\\n\",path); exit(1);} if(t) fputs(t,f); fclose(f); }\n"
          "__attribute__((unused)) static void _wf_append(const char *path,const char *t){\n"
          "    FILE *f=fopen(path,\"a\"); if(!f){ fprintf(stderr,\"file.append: cannot open %s\\n\",path); exit(1);} if(t) fputs(t,f); fclose(f); }\n"
          "__attribute__((unused)) static int _wf_exists(const char *path){\n"
          "    FILE *f=fopen(path,\"r\"); if(f){ fclose(f); return 1;} return 0; }\n\n", out);

    /* JSON: \\u-эскейпы пока не декодируются. */
    fputs(
      "typedef enum { WJ_NUL,WJ_BOO,WJ_INT,WJ_FRC,WJ_STR,WJ_ARR,WJ_OBJ } _wjt;\n"
      "typedef struct _wj { _wjt t; long i; double f; char *s; struct _wj **el; char **ky; int n; } _wj;\n", out);
    if (g_gc_strings) {
        fputs("static void _wj_trace(const void *data, WindGcVisit visit, void *context) {\n"
              "    const _wj *j = data;\n"
              "    visit(context, j->s); visit(context, j->el); visit(context, j->ky);\n"
              "    for (int i = 0; i < j->n; i++) {\n"
              "        visit(context, j->el[i]);\n"
              "        if (j->ky) visit(context, j->ky[i]);\n"
              "    }\n"
              "}\n"
              "__attribute__((unused)) static void _wj_trace_slots(const volatile void *data, size_t n, WindGcVisit visit, void *context) {\n"
              "    _wj *const volatile *slots = data;\n"
              "    for (size_t i = 0; i < n; i++) visit(context, slots[i]);\n"
              "}\n"
              "#define WIND_JSON_NEW() wind_str_alloc_traced(sizeof(_wj), _wj_trace)\n", out);
    } else {
        fputs("#define WIND_JSON_NEW() malloc(sizeof(_wj))\n", out);
    }
    fputs(
      "static _wj *_wj_pv(const char **p);\n"
      "__attribute__((unused)) static _wj *_wj_mk(_wjt t){ _wj *j=WIND_JSON_NEW(); *j=(_wj){.t=t}; return j; }\n"
      "__attribute__((unused)) static void _wj_ws(const char **p){ while(**p==' '||**p=='\\t'||**p=='\\n'||**p=='\\r') (*p)++; }\n"
      "__attribute__((unused)) static char *_wj_pstr(const char **p){\n"
      "    (*p)++; size_t cap=16,n=0; char *b=wind_str_alloc(cap);\n"
      "    while(**p && **p!='\"'){ char c=**p;\n"
      "        if(c=='\\\\'){ (*p)++; char e=**p; switch(e){ case 'n':c='\\n';break; case 't':c='\\t';break; case 'r':c='\\r';break; default:c=e; } }\n"
      "        if(n+1>=cap){ cap*=2; b=WIND_GROW(b,n,cap); }\n"
      "        b[n++]=c; (*p)++; }\n"
      "    if(**p=='\"') (*p)++;\n"
      "    b[n]=0; return b; }\n"
      "__attribute__((unused)) static _wj *_wj_pv(const char **p){ _wj_ws(p); char c=**p;\n"
      "    if(c=='\"'){ _wj *j=_wj_mk(WJ_STR); j->s=_wj_pstr(p); return j; }\n"
      "    if(c=='{'){ (*p)++; _wj *j=_wj_mk(WJ_OBJ); _wj_ws(p); if(**p=='}'){ (*p)++; return j; }\n"
      "        int cap=4; j->ky=wind_str_alloc((size_t)cap*sizeof(char*)); j->el=wind_str_alloc((size_t)cap*sizeof(_wj*));\n"
      "        for(;;){ _wj_ws(p); char *k=_wj_pstr(p); _wj_ws(p); if(**p==':')(*p)++; _wj *v=_wj_pv(p);\n"
      "            if(j->n>=cap){ cap*=2; j->ky=WIND_GROW(j->ky,(size_t)j->n*sizeof(char*),(size_t)cap*sizeof(char*)); j->el=WIND_GROW(j->el,(size_t)j->n*sizeof(_wj*),(size_t)cap*sizeof(_wj*)); }\n"
      "            j->ky[j->n]=k; j->el[j->n]=v; j->n++; _wj_ws(p);\n"
      "            if(**p==','){ (*p)++; continue; } if(**p=='}') (*p)++; break; }\n"
      "        return j; }\n"
      "    if(c=='['){ (*p)++; _wj *j=_wj_mk(WJ_ARR); _wj_ws(p); if(**p==']'){ (*p)++; return j; }\n"
      "        int cap=4; j->el=wind_str_alloc((size_t)cap*sizeof(_wj*));\n"
      "        for(;;){ _wj *v=_wj_pv(p);\n"
      "            if(j->n>=cap){ cap*=2; j->el=WIND_GROW(j->el,(size_t)j->n*sizeof(_wj*),(size_t)cap*sizeof(_wj*)); }\n"
      "            j->el[j->n++]=v; _wj_ws(p);\n"
      "            if(**p==','){ (*p)++; continue; } if(**p==']') (*p)++; break; }\n"
      "        return j; }\n"
      "    if(c=='t'){ *p+=4; _wj *j=_wj_mk(WJ_BOO); j->i=1; return j; }\n"
      "    if(c=='f'){ *p+=5; _wj *j=_wj_mk(WJ_BOO); j->i=0; return j; }\n"
      "    if(c=='n'){ *p+=4; return _wj_mk(WJ_NUL); }\n"
      "    char *end; double d=strtod(*p,&end); int isf=0;\n"
      "    for(const char *q=*p;q<end;q++) if(*q=='.'||*q=='e'||*q=='E') isf=1;\n"
      "    _wj *j; if(isf){ j=_wj_mk(WJ_FRC); j->f=d; } else { j=_wj_mk(WJ_INT); j->i=(long)d; }\n"
      "    *p=end; return j; }\n"
      "__attribute__((unused)) static _wj *_wj_parse(const char *s){ const char *p=s; return _wj_pv(&p); }\n", out);
    fputs(
      "__attribute__((unused)) static _wj _wj_nul_s = { WJ_NUL,0,0,0,0,0,0 };\n"
      "__attribute__((unused)) static _wj *_wj_get(_wj *j,const char *k){ if(j&&j->t==WJ_OBJ) for(int i=0;i<j->n;i++) if(!strcmp(j->ky[i],k)) return j->el[i]; return &_wj_nul_s; }\n"
      "__attribute__((unused)) static _wj *_wj_at(_wj *j,int i){ if(j&&j->t==WJ_ARR&&i>=0&&i<j->n) return j->el[i]; return &_wj_nul_s; }\n"
      "__attribute__((unused)) static int _wj_has(_wj *j,const char *k){ if(j&&j->t==WJ_OBJ) for(int i=0;i<j->n;i++) if(!strcmp(j->ky[i],k)) return 1; return 0; }\n"
      "__attribute__((unused)) static long _wj_int(_wj *j){ if(!j) return 0; switch(j->t){ case WJ_INT: case WJ_BOO: return j->i; case WJ_FRC: return (long)j->f; case WJ_STR: return strtol(j->s,0,10); default: return 0; } }\n"
      "__attribute__((unused)) static double _wj_frac(_wj *j){ if(!j) return 0; switch(j->t){ case WJ_FRC: return j->f; case WJ_INT: case WJ_BOO: return (double)j->i; case WJ_STR: return strtod(j->s,0); default: return 0; } }\n"
      "__attribute__((unused)) static int _wj_bool(_wj *j){ if(!j) return 0; switch(j->t){ case WJ_BOO: case WJ_INT: return j->i!=0; case WJ_NUL: return 0; default: return 1; } }\n"
      "__attribute__((unused)) static int _wj_len(_wj *j){ if(!j) return 0; if(j->t==WJ_ARR||j->t==WJ_OBJ) return j->n; if(j->t==WJ_STR) return (int)strlen(j->s); return 0; }\n"
      "__attribute__((unused)) static char *_wj_type(_wj *j){ static const char *nm[]={\"null\",\"bool\",\"int\",\"frac\",\"str\",\"array\",\"object\"}; return (char*)(j?nm[j->t]:\"null\"); }\n", out);
    fputs(
      "typedef struct { char *p; size_t n,cap; } _wjb;\n"
      "__attribute__((unused)) static void _wjb_ch(_wjb *b,char c){ if(b->n+1>=b->cap){ b->cap=b->cap?b->cap*2:32; b->p=WIND_GROW(b->p,b->n,b->cap); } b->p[b->n++]=c; }\n"
      "__attribute__((unused)) static void _wjb_s(_wjb *b,const char *s){ while(*s) _wjb_ch(b,*s++); }\n"
      "__attribute__((unused)) static void _wjb_q(_wjb *b,const char *s){ _wjb_ch(b,'\"'); for(;*s;s++){ unsigned char c=(unsigned char)*s; switch(c){ case '\"': _wjb_s(b,\"\\\\\\\"\"); break; case '\\\\': _wjb_s(b,\"\\\\\\\\\"); break; case '\\n': _wjb_s(b,\"\\\\n\"); break; case '\\t': _wjb_s(b,\"\\\\t\"); break; case '\\r': _wjb_s(b,\"\\\\r\"); break; default: _wjb_ch(b,(char)c); } } _wjb_ch(b,'\"'); }\n"
      "__attribute__((unused)) static void _wj_enc(_wjb *b,_wj *j){ char t[64]; if(!j){ _wjb_s(b,\"null\"); return; }\n"
      "    switch(j->t){ case WJ_NUL: _wjb_s(b,\"null\"); break; case WJ_BOO: _wjb_s(b,j->i?\"true\":\"false\"); break;\n"
      "        case WJ_INT: snprintf(t,sizeof t,\"%ld\",j->i); _wjb_s(b,t); break;\n"
      "        case WJ_FRC: snprintf(t,sizeof t,\"%g\",j->f); _wjb_s(b,t); break;\n"
      "        case WJ_STR: _wjb_q(b,j->s); break;\n"
      "        case WJ_ARR: _wjb_ch(b,'['); for(int i=0;i<j->n;i++){ if(i)_wjb_ch(b,','); _wj_enc(b,j->el[i]); } _wjb_ch(b,']'); break;\n"
      "        case WJ_OBJ: _wjb_ch(b,'{'); for(int i=0;i<j->n;i++){ if(i)_wjb_ch(b,','); _wjb_q(b,j->ky[i]); _wjb_ch(b,':'); _wj_enc(b,j->el[i]); } _wjb_ch(b,'}'); break; } }\n"
      "__attribute__((unused)) static char *_wj_encode(_wj *j){ _wjb b={0,0,0}; _wj_enc(&b,j); _wjb_ch(&b,0); return b.p; }\n"
      "__attribute__((unused)) static char *_wj_str(_wj *j){ if(!j) return wstr_dup(\"\"); switch(j->t){ case WJ_STR: return j->s; case WJ_INT: return wstr_from_int(j->i); case WJ_BOO: return wstr_dup(j->i?\"true\":\"false\"); case WJ_FRC: return wstr_from_frac(j->f); case WJ_NUL: return wstr_dup(\"null\"); default: return _wj_encode(j); } }\n", out);
    fputs(
      "__attribute__((unused)) static char *_wj_enc_dict_str(_wd *d){ _wjb b={0,0,0}; _wjb_ch(&b,'{'); int f=1; for(int i=0;i<d->nb;i++) for(_wde *e=d->b[i];e;e=e->next){ if(!f)_wjb_ch(&b,','); f=0; _wjb_q(&b,(char*)(uintptr_t)e->key); _wjb_ch(&b,':'); _wjb_q(&b,(char*)(uintptr_t)e->val); } _wjb_ch(&b,'}'); _wjb_ch(&b,0); return b.p; }\n"
      "__attribute__((unused)) static char *_wj_enc_dict_int(_wd *d){ _wjb b={0,0,0}; char t[32]; _wjb_ch(&b,'{'); int f=1; for(int i=0;i<d->nb;i++) for(_wde *e=d->b[i];e;e=e->next){ if(!f)_wjb_ch(&b,','); f=0; _wjb_q(&b,(char*)(uintptr_t)e->key); _wjb_ch(&b,':'); snprintf(t,sizeof t,\"%d\",(int)e->val); _wjb_s(&b,t); } _wjb_ch(&b,'}'); _wjb_ch(&b,0); return b.p; }\n"
      "__attribute__((unused)) static char *_wj_enc_dict_frac(_wd *d){ _wjb b={0,0,0}; char t[64]; _wjb_ch(&b,'{'); int f=1; for(int i=0;i<d->nb;i++) for(_wde *e=d->b[i];e;e=e->next){ if(!f)_wjb_ch(&b,','); f=0; _wjb_q(&b,(char*)(uintptr_t)e->key); _wjb_ch(&b,':'); snprintf(t,sizeof t,\"%g\",_wd_unpackf(e->val)); _wjb_s(&b,t); } _wjb_ch(&b,'}'); _wjb_ch(&b,0); return b.p; }\n"
      "__attribute__((unused)) static char *_wj_enc_list_int(_wl *l){ _wjb b={0,0,0}; char t[32]; _wjb_ch(&b,'['); for(int i=0;i<l->len;i++){ if(i)_wjb_ch(&b,','); snprintf(t,sizeof t,\"%d\",((int*)l->data)[i]); _wjb_s(&b,t); } _wjb_ch(&b,']'); _wjb_ch(&b,0); return b.p; }\n"
      "__attribute__((unused)) static char *_wj_enc_list_frac(_wl *l){ _wjb b={0,0,0}; char t[64]; _wjb_ch(&b,'['); for(int i=0;i<l->len;i++){ if(i)_wjb_ch(&b,','); snprintf(t,sizeof t,\"%g\",((double*)l->data)[i]); _wjb_s(&b,t); } _wjb_ch(&b,']'); _wjb_ch(&b,0); return b.p; }\n"
      "__attribute__((unused)) static char *_wj_enc_list_str(_wl *l){ _wjb b={0,0,0}; _wjb_ch(&b,'['); for(int i=0;i<l->len;i++){ if(i)_wjb_ch(&b,','); _wjb_q(&b,((char**)l->data)[i]); } _wjb_ch(&b,']'); _wjb_ch(&b,0); return b.p; }\n\n", out);

    /* рантайм модуля http: raw-сокеты, HTTP/1.0 + Connection: close (без chunked), без TLS.
     * Буфер ответа собирается через _wjb (из json-рантайма). */
    fputs(
      "#ifdef _WIN32\n"
      "#include <winsock2.h>\n#include <ws2tcpip.h>\n"
      "#ifndef _SSIZE_T_DEFINED\n#define _SSIZE_T_DEFINED\ntypedef long long ssize_t;\n#endif\n"
      "static int _wsa_ready=0;\n"
      "static void _wsa_init(void){ if(!_wsa_ready){ WSADATA _w; WSAStartup(MAKEWORD(2,2),&_w); _wsa_ready=1; } }\n"
      "#define close closesocket\n"
      "#define read(f,b,n) recv((f),(char*)(b),(int)(n),0)\n"
      "#define write(f,b,n) send((f),(const char*)(b),(int)(n),0)\n"
      "#else\n"
      "#include <sys/socket.h>\n#include <netinet/in.h>\n#include <arpa/inet.h>\n#include <netdb.h>\n#include <unistd.h>\n"
      "static void _wsa_init(void){}\n"
      "#endif\n"
      "__attribute__((unused)) static char *_wh_req(const char *method,const char *url,const char *body){\n"
      "    _wsa_init();\n"
      "    const char *u=url; if(!strncmp(u,\"http://\",7)) u+=7;\n"
      "    char host[256]; int port=80; char path[1024];\n"
      "    const char *slash=strchr(u,'/'); const char *hostend=slash?slash:(u+strlen(u));\n"
      "    const char *colon=memchr(u,':',(size_t)(hostend-u));\n"
      "    size_t hl=(size_t)((colon?colon:hostend)-u); if(hl>=sizeof host) hl=sizeof host-1;\n"
      "    memcpy(host,u,hl); host[hl]=0; if(colon) port=atoi(colon+1);\n"
      "    if(slash) snprintf(path,sizeof path,\"%s\",slash); else { path[0]='/'; path[1]=0; }\n"
      "    struct addrinfo hints; memset(&hints,0,sizeof hints); hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;\n"
      "    char ps[16]; snprintf(ps,sizeof ps,\"%d\",port); struct addrinfo *res;\n"
      "    if(getaddrinfo(host,ps,&hints,&res)){ fprintf(stderr,\"http: cannot resolve %s\\n\",host); exit(1); }\n"
      "    int fd=socket(res->ai_family,res->ai_socktype,res->ai_protocol);\n"
      "    if(fd<0||connect(fd,res->ai_addr,res->ai_addrlen)){ fprintf(stderr,\"http: cannot connect %s:%d\\n\",host,port); exit(1); }\n"
      "    freeaddrinfo(res);\n"
      "    _wjb rq={0,0,0}; _wjb_s(&rq,method); _wjb_ch(&rq,' '); _wjb_s(&rq,path);\n"
      "    _wjb_s(&rq,\" HTTP/1.0\\r\\nHost: \"); _wjb_s(&rq,host); _wjb_s(&rq,\"\\r\\nConnection: close\\r\\n\");\n"
      "    if(body){ char cl[96]; snprintf(cl,sizeof cl,\"Content-Type: application/json\\r\\nContent-Length: %zu\\r\\n\",strlen(body)); _wjb_s(&rq,cl); }\n"
      "    _wjb_s(&rq,\"\\r\\n\"); if(body) _wjb_s(&rq,body); _wjb_ch(&rq,0);\n"
      "    size_t total=rq.n-1,sent=0; while(sent<total){ ssize_t w=write(fd,rq.p+sent,total-sent); if(w<=0) break; sent+=(size_t)w; }\n"
      "    _wjb rs={0,0,0}; char buf[4096]; ssize_t r;\n"
      "    while((r=read(fd,buf,sizeof buf))>0) for(ssize_t i=0;i<r;i++) _wjb_ch(&rs,buf[i]);\n"
      "    close(fd); _wjb_ch(&rs,0); if(!rs.p) return wstr_dup(\"\");\n"
      "    char *sep=strstr(rs.p,\"\\r\\n\\r\\n\"); return wstr_dup(sep?sep+4:rs.p); }\n"
      "__attribute__((unused)) static char *_wh_get(const char *url){ return _wh_req(\"GET\",url,0); }\n"
      "__attribute__((unused)) static char *_wh_post(const char *url,const char *body){ return _wh_req(\"POST\",url,body); }\n", out);
    fputs(
      "__attribute__((unused)) static int _wh_listen(int port){\n"
      "    _wsa_init();\n"
      "    int sfd=socket(AF_INET,SOCK_STREAM,0); if(sfd<0){ perror(\"socket\"); exit(1); }\n"
      "    int yes=1; setsockopt(sfd,SOL_SOCKET,SO_REUSEADDR,(char*)&yes,(int)sizeof yes);\n"
      "    struct sockaddr_in addr; memset(&addr,0,sizeof addr);\n"
      "    addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((unsigned short)port);\n"
      "    if(bind(sfd,(struct sockaddr*)&addr,sizeof addr)<0){ fprintf(stderr,\"http.serve: cannot bind port %d\\n\",port); exit(1); }\n"
      "    if(listen(sfd,16)<0){ perror(\"listen\"); exit(1); }\n"
      "    fprintf(stderr,\"http.serve: listening on port %d\\n\",port); return sfd; }\n"
      "__attribute__((unused)) static int _wh_accept(int sfd,char *path,size_t cap){\n"
      "    int cfd=accept(sfd,0,0); if(cfd<0) return -1;\n"
      "    char req[8192]; ssize_t n=read(cfd,req,sizeof req-1); if(n<0) n=0; req[n]=0;\n"
      "    path[0]='/'; path[1]=0; char *sp=strchr(req,' ');\n"
      "    if(sp){ char *sp2=strchr(sp+1,' '); if(sp2){ size_t pl=(size_t)(sp2-sp-1); if(pl>=cap) pl=cap-1; memcpy(path,sp+1,pl); path[pl]=0; } }\n"
      "    return cfd; }\n"
      "__attribute__((unused)) static void _wh_respond(int cfd,const char *body){\n"
      "    if(!body) body=\"\";\n"
      "    char hdr[256]; int hn=snprintf(hdr,sizeof hdr,\"HTTP/1.1 200 OK\\r\\nContent-Type: text/html; charset=utf-8\\r\\nContent-Length: %zu\\r\\nConnection: close\\r\\n\\r\\n\",strlen(body));\n"
      "    if(write(cfd,hdr,(size_t)hn)>=0){ size_t bl=strlen(body),off=0; while(off<bl){ ssize_t ww=write(cfd,body+off,bl-off); if(ww<=0) break; off+=(size_t)ww; } }\n"
      "    close(cfd); }\n\n", out);

    for (int i = 0; i < p->body.n; i++)
        if (p->body.items[i]->kind == ST_FUNC) {
            cg_at(p->body.items[i]);
            emit_func_signature(out, p->body.items[i]); fputs(";\n", out);
        }
    if (cg_has_callbacks) {
        int id = 0;
        for (int i = 0; i < p->body.n; i++)
            if (p->body.items[i]->kind == ST_FUNC) emit_callback(out, p->body.items[i], id++);
    }
    fputs("\n", out);

    /* глобалы в file-scope (скаляры — init в main; массивы — zero-init тут) */
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s->kind != ST_VAR_DECL || !s->as.decl.is_global) continue;
        cg_at(s);
        const DeclType *dt = &s->as.decl.dtype;
        if (dt->kind == DT_ARRAY) {
            fprintf(out, "%s %s[", ctype(wt_base(dt->base)), s->as.decl.name);
            emit_expr(out, dt->array_size);
            fputs("];\n", out);
        } else if (g_gc_strings && (dt->kind == DT_LIST || dt->kind == DT_DICT)) {
            fprintf(out, "%s *%s;\n", dt->kind == DT_LIST ? "_wl" : "_wd", s->as.decl.name);
        } else {
            WType t = (dt->kind == DT_INFER) ? infer(s->as.decl.init) : wt_base(dt->base);
            fprintf(out, "%s %s%s;\n", ctype(t), s->as.decl.name,
                    g_gc_strings && t == WT_STR ? " = \"\"" : "");
        }
    }
    fputs("\n", out);

    fputs("__attribute__((unused)) static char *_wind_copy_cstr(const char *value, const char *name) {\n"
          "    if (!value) { fprintf(stderr, \"Wind: C function '%s' returned NULL\\n\", name); exit(EXIT_FAILURE); }\n"
          "    return wstr_dup(value);\n"
          "}\n\n", out);
    if (p->nc_imports)
        fputs("#include <limits.h>\n"
              "__attribute__((unused)) static int _wind_buffer_length(size_t n) {\n"
              "    if (n > INT_MAX) { fputs(\"Wind: C buffer length exceeds INT_MAX\\n\", stderr); exit(EXIT_FAILURE); }\n"
              "    return (int)n;\n}\n\n", out);

    for (int i = 0; i < p->body.n; i++)
        if (p->body.items[i]->kind == ST_FUNC) emit_func(out, p->body.items[i]);

    /* main: сперва инициализация глобалов, потом top-level инструкции */
    fputs("int main(void) {\n", out);
    if (!g_gc_strings) fputs("    GC_INIT();\n", out);
    g_nloc = 0;
    g_in_main = 1;
    g_volatile = g_gc_strings && block_has_try(&p->body);
    if (g_gc_strings) {
        emit_gc_root(out, "_wt_msg", 1);
        for (int i = 0; i < g_nglob; i++) {
            VType type = g_glob[i].vt;
            if (type.ck == CK_SCALAR && type.elem == WT_STR)
                emit_gc_root(out, g_glob[i].name, 1);
            else if (type.ck == CK_SCALAR && type.elem == WT_JSON)
                emit_gc_json_root(out, g_glob[i].name, 1);
            else
                emit_gc_collection_root(out, g_glob[i].name, type, 1);
        }
    }
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s->kind != ST_VAR_DECL || !s->as.decl.is_global) continue;
        cg_at(s);
        const DeclType *dt = &s->as.decl.dtype;
        if (g_gc_strings && dt->kind == DT_LIST) {
            WType el = wt_base(dt->base);
            fprintf(out, "    %s = _wl_new(sizeof(%s), %d);\n", s->as.decl.name, ctype(el), el == WT_STR);
            emit_list_items(out, s, el, 1);
            emit_gc_checkpoint(out, 1);
        } else if (g_gc_strings && dt->kind == DT_DICT) {
            WType key = wt_base(dt->key), value = wt_base(dt->val);
            fprintf(out, "    %s = _wd_new(%d, %d);\n", s->as.decl.name, key == WT_STR, value == WT_STR);
            emit_dict_items(out, s, key, value, 1);
            emit_gc_checkpoint(out, 1);
        } else if (s->as.decl.init) {
            indent(out, 1);
            fprintf(out, "%s = ", s->as.decl.name);
            emit_full_expr(out, s->as.decl.init);
            fputs(";\n", out);
            emit_gc_checkpoint(out, 1);
        }
    }
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s->kind == ST_FUNC) continue;
        if (s->kind == ST_VAR_DECL && s->as.decl.is_global) continue;
        emit_stmt_at_checkpoint(out, s, 1);
    }
    if (g_gc_strings) fputs("    wind_str_destroy();\n", out);
    indent(out, 1); fputs("return 0;\n}\n", out);
    for (int i = 0; i < p->nc_imports; i++)
        wind_c_emit_checks(out, p->c_imports[i].binding, i);
    return diags->count == previous;
}
