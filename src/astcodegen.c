/*
 * astcodegen.c — генерация C из AST. Этап 4, срезы 1-3a.
 * См. astcodegen.h для границ среза.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astcodegen.h"

/* ---------- типы Wind для вывода ---------- */
typedef enum { WT_INT, WT_FRAC, WT_STR, WT_VOID, WT_BOOL, WT_JSON, WT_UNKNOWN } WType;

/* Полный тип переменной: скаляр или коллекция. */
typedef enum { CK_SCALAR, CK_ARRAY, CK_LIST, CK_DICT } CKind;
typedef struct { CKind ck; WType elem; WType key; } VType;  /* elem: тип элемента/значения; key: ключ dict */
static VType vt_scalar(WType t) { VType v; v.ck = CK_SCALAR; v.elem = t; v.key = WT_UNKNOWN; return v; }

/* ---------- состояние кодогена (компилятор однопоточный → статики ок) ---------- */
typedef struct { char name[64]; VType vt; } Sym;
typedef struct { char name[64]; WType ret;  } FuncSig;

static jmp_buf  cg_jb;
static char    *cg_err;
static int      cg_errcap;

static Sym      g_glob[256]; static int g_nglob;
static Sym      g_loc[256];  static int g_nloc;     /* сбрасывается на каждую функцию/main */
static FuncSig  g_func[256]; static int g_nfunc;
static int      g_rep_id;                            /* уникальные счётчики repeat-циклов */

static void cg_fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (cg_err && cg_errcap > 0) vsnprintf(cg_err, cg_errcap, fmt, ap);
    va_end(ap);
    longjmp(cg_jb, 1);
}

/* ---------- symtab ---------- */
static void sym_add(Sym *arr, int *n, const char *name, VType vt) {
    if (*n >= 256) cg_fail("too many variables");
    snprintf(arr[*n].name, sizeof arr[*n].name, "%s", name);
    arr[*n].vt = vt; (*n)++;
}
static VType var_vtype(const char *name) {
    for (int i = g_nloc - 1; i >= 0; i--) if (!strcmp(g_loc[i].name, name)) return g_loc[i].vt;
    for (int i = g_nglob - 1; i >= 0; i--) if (!strcmp(g_glob[i].name, name)) return g_glob[i].vt;
    return vt_scalar(WT_UNKNOWN);
}
static WType var_type(const char *name) {       /* скалярный тип переменной (или элемент, если не коллекция) */
    VType v = var_vtype(name);
    return v.ck == CK_SCALAR ? v.elem : WT_UNKNOWN;
}
static WType wt_base(TokenKind t);
/* полный тип коллекции по выражению-носителю (nums или int.nums) */
static VType coll_vtype(const Expr *coll) {
    if (coll->kind == EX_IDENT) return var_vtype(coll->as.ident);
    if (coll->kind == EX_TYPED) {
        VType v = var_vtype(coll->as.typed.name);
        if (v.elem != WT_UNKNOWN) return v;
        return vt_scalar(wt_base(coll->as.typed.type));   /* fallback на аннотацию {int.nums[i]} */
    }
    return vt_scalar(WT_UNKNOWN);
}
static WType func_ret(const char *name) {
    for (int i = 0; i < g_nfunc; i++) if (!strcmp(g_func[i].name, name)) return g_func[i].ret;
    return WT_UNKNOWN;
}

static WType wt_base(TokenKind t) {
    switch (t) {
        case TK_KW_INT:  return WT_INT;
        case TK_KW_FRAC: return WT_FRAC;
        case TK_KW_STR:  return WT_STR;
        case TK_KW_VOID: return WT_VOID;
        default:         return WT_UNKNOWN;
    }
}
static const char *ctype(WType t) {
    switch (t) {
        case WT_INT: case WT_BOOL: return "int";
        case WT_FRAC: return "double";
        case WT_STR:  return "char*";
        case WT_JSON: return "_wj*";
        case WT_VOID: return "void";
        default: cg_fail("unknown type in codegen"); return "?";
    }
}
/* суффикс рантайм-функций списка по типу элемента: _wl_get_int/_frac/_str */
static const char *list_suffix(WType t) {
    switch (t) {
        case WT_INT: case WT_BOOL: return "int";
        case WT_FRAC: return "frac";
        case WT_STR:  return "str";
        default: cg_fail("list: unsupported element type"); return "?";
    }
}

/* ---------- вывод типа выражения ---------- */
static WType infer(const Expr *e);

static WType infer_binary(const Expr *e) {
    TokenKind op = e->as.binary.op;
    switch (op) {
        case TK_EQ: case TK_NEQ: case TK_LT: case TK_GT:
        case TK_LE: case TK_GE: case TK_AND: case TK_OR:
            return WT_BOOL;
        default: break;
    }
    WType l = infer(e->as.binary.l), r = infer(e->as.binary.r);
    if (op == TK_PLUS && (l == WT_STR || r == WT_STR)) return WT_STR;  /* конкатенация */
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
        case EX_TYPED:  return wt_base(e->as.typed.type);
        case EX_GROUP:  return infer(e->as.group.inner);
        case EX_UNARY:  return infer(e->as.unary.e);
        case EX_BINARY: return infer_binary(e);
        case EX_INDEX:  return coll_vtype(e->as.index.coll).elem;
        case EX_DOT: {
            VType ov = coll_vtype(e->as.dot.obj);
            if ((ov.ck == CK_LIST || ov.ck == CK_DICT) && !strcmp(e->as.dot.field, "len")) return WT_INT;
            return WT_UNKNOWN;
        }
        case EX_CALL: {
            const Expr *c = e->as.call.callee;
            if (c->kind == EX_DOT) {
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
                if (infer(o2) == WT_JSON) {                 /* аксессоры json-значения */
                    const char *m = c->as.dot.field;
                    if (!strcmp(m, "get") || !strcmp(m, "at")) return WT_JSON;
                    if (!strcmp(m, "str") || !strcmp(m, "type")) return WT_STR;
                    if (!strcmp(m, "int") || !strcmp(m, "len")) return WT_INT;
                    if (!strcmp(m, "frac")) return WT_FRAC;
                    if (!strcmp(m, "bool") || !strcmp(m, "has")) return WT_BOOL;
                    return WT_UNKNOWN;
                }
                if (!strcmp(c->as.dot.field, "len")) return WT_INT;
                if (!strcmp(c->as.dot.field, "has")) return WT_BOOL;
                return WT_VOID;
            }
            if (c->kind == EX_IDENT) {
                if (!strcmp(c->as.ident, "int"))  return WT_INT;
                if (!strcmp(c->as.ident, "frac")) return WT_FRAC;
                if (!strcmp(c->as.ident, "str") || !strcmp(c->as.ident, "to_str")) return WT_STR;
                if (!strcmp(c->as.ident, "len")) return WT_INT;
                return func_ret(c->as.ident);
            }
            return WT_UNKNOWN;
        }
        default: return WT_UNKNOWN;  /* list/dict — не в срезе 3a */
    }
}

/* ---------- эмиссия выражений ---------- */
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

/* спецификатор printf по типу плейсхолдера интерполяции */
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

static void emit_expr(FILE *o, const Expr *e) {
    switch (e->kind) {
        case EX_INT:   fprintf(o, "%ld", e->as.ival); break;
        case EX_FRAC:  fprintf(o, "%.17g", e->as.fval); break;
        case EX_STR:   emit_c_string(o, e->as.sval); break;
        case EX_INTERP: {
            int n = e->as.interp.n;
            fputs("wstr_build(\"", o);
            for (int i = 0; i < n; i++) {
                emit_c_chars(o, e->as.interp.lits[i], 1);
                const char *spec = interp_spec(infer(e->as.interp.exprs[i]));
                if (!spec) cg_fail("interpolation: cannot determine type of placeholder #%d", i + 1);
                fputs(spec, o);
            }
            emit_c_chars(o, e->as.interp.lits[n], 1);
            fputc('"', o);
            for (int i = 0; i < n; i++) {
                fputs(", ", o);
                WType pt = infer(e->as.interp.exprs[i]);
                if (pt == WT_INT || pt == WT_BOOL) {     /* в variadic int идёт как int */
                    fputs("(int)(", o); emit_expr(o, e->as.interp.exprs[i]); fputc(')', o);
                } else {
                    emit_expr(o, e->as.interp.exprs[i]);
                }
            }
            fputc(')', o);
            break;
        }
        case EX_IDENT: fprintf(o, "%s", e->as.ident); break;
        case EX_TYPED: fprintf(o, "%s", e->as.typed.name); break;   /* тип-префикс в C не нужен */
        case EX_GROUP: fputc('(', o); emit_expr(o, e->as.group.inner); fputc(')', o); break;
        case EX_UNARY:
            fputc('(', o);
            fputs(e->as.unary.op == TK_MINUS ? "-" : "", o);
            emit_expr(o, e->as.unary.e);
            fputc(')', o);
            break;
        case EX_BINARY: {
            TokenKind op = e->as.binary.op;
            if (op == TK_PLUS && infer(e) == WT_STR) {       /* конкатенация строк */
                fputs("wstr_cat(", o);
                emit_expr(o, e->as.binary.l); fputs(", ", o);
                emit_expr(o, e->as.binary.r); fputc(')', o);
                break;
            }
            if ((op == TK_EQ || op == TK_NEQ)                /* сравнение строк */
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
            /* методы коллекций: nums.add(v)/.pop()/.len(), cfg.has(k)/.len() */
            if (c->kind == EX_DOT) {
                const Expr *obj = c->as.dot.obj;
                const char *m = c->as.dot.field;
                /* встроенный модуль time */
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
            if (infer(obj) == WT_JSON) {                  /* аксессоры json-значения */
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
            /* len(nums) */
            if (!strcmp(fn, "len") && e->as.call.nargs == 1) {
                fputs("_wl_len(", o); emit_expr(o, e->as.call.args[0]); fputc(')', o);
                break;
            }
            /* касты: str/to_str/int/frac с одним аргументом */
            if (e->as.call.nargs == 1 &&
                (!strcmp(fn, "str") || !strcmp(fn, "to_str") ||
                 !strcmp(fn, "int") || !strcmp(fn, "frac"))) {
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
                } else { /* frac */
                    if (at == WT_STR) { fputs("wstr_to_frac(", o); emit_expr(o, arg); fputc(')', o); }
                    else { fputs("(double)(", o); emit_expr(o, arg); fputc(')', o); }
                }
                break;
            }
            fprintf(o, "%s(", fn);
            for (int i = 0; i < e->as.call.nargs; i++) {
                if (i) fputs(", ", o);
                emit_expr(o, e->as.call.args[i]);
            }
            fputc(')', o);
            break;
        }
        case EX_INDEX: {                              /* coll[idx]: массив / список / словарь */
            VType cv = coll_vtype(e->as.index.coll);
            if (cv.ck == CK_LIST) {
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
            cg_fail("access .%s is not supported", e->as.dot.field);
            break;
        }
        default:
            cg_fail("this expression (dict literal) is not supported yet");
    }
}

/* ---------- эмиссия инструкций ---------- */
static void emit_block(FILE *o, const Block *b, int ind);
static void indent(FILE *o, int n) { for (int i = 0; i < n; i++) fputs("    ", o); }

/* печать значения по типу (terminal.paste) */
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
    if (t == WT_INT || t == WT_BOOL) { fputs("(int)(", o); emit_expr(o, val); fputc(')', o); }
    else emit_expr(o, val);
    fputs(");\n", o);
}

/* объявление локальной переменной (в теле функции/main) */
static void emit_local_decl(FILE *o, const Stmt *s, int ind) {
    const DeclType *dt = &s->as.decl.dtype;
    if (dt->kind == DT_ARRAY) {                       /* int.nums[5] → int nums[5] = {0}; */
        WType el = wt_base(dt->base);
        if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("unknown array type '%s'", s->as.decl.name);
        VType vt; vt.ck = CK_ARRAY; vt.elem = el; vt.key = WT_UNKNOWN;
        sym_add(g_loc, &g_nloc, s->as.decl.name, vt);
        indent(o, ind);
        fprintf(o, "%s %s[", ctype(el), s->as.decl.name);
        emit_expr(o, dt->array_size);
        fputs("] = {0};\n", o);
        return;
    }
    if (dt->kind == DT_LIST) {                        /* int.list.nums → _wl *nums = _wl_new(...) */
        WType el = wt_base(dt->base);
        if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("unknown list type '%s'", s->as.decl.name);
        VType vt; vt.ck = CK_LIST; vt.elem = el; vt.key = WT_UNKNOWN;
        sym_add(g_loc, &g_nloc, s->as.decl.name, vt);
        indent(o, ind);
        fprintf(o, "_wl *%s = _wl_new(sizeof(%s));\n", s->as.decl.name, ctype(el));
        if (s->as.decl.init) {
            if (s->as.decl.init->kind != EX_LIST)
                cg_fail("list '%s' initializer expects a [..] literal", s->as.decl.name);
            const Expr *lst = s->as.decl.init;
            for (int i = 0; i < lst->as.list.n; i++) {
                indent(o, ind);
                fprintf(o, "_wl_push_%s(%s, ", list_suffix(el), s->as.decl.name);
                emit_expr(o, lst->as.list.items[i]);
                fputs(");\n", o);
            }
        }
        return;
    }
    if (dt->kind == DT_DICT) {                        /* dict[str,int].cfg → _wd *cfg = _wd_new(ks) */
        WType kt = wt_base(dt->key), vt = wt_base(dt->val);
        if (kt == WT_VOID || kt == WT_UNKNOWN || vt == WT_VOID || vt == WT_UNKNOWN)
            cg_fail("unknown dict type '%s'", s->as.decl.name);
        VType v; v.ck = CK_DICT; v.elem = vt; v.key = kt;
        sym_add(g_loc, &g_nloc, s->as.decl.name, v);
        indent(o, ind);
        fprintf(o, "_wd *%s = _wd_new(%d);\n", s->as.decl.name, kt == WT_STR ? 1 : 0);
        if (s->as.decl.init) {
            if (s->as.decl.init->kind != EX_DICT)
                cg_fail("dict '%s' initializer expects a [key: value, ...] literal", s->as.decl.name);
            const Expr *d = s->as.decl.init;
            for (int i = 0; i < d->as.dict.n; i++) {
                indent(o, ind);
                fprintf(o, "_wd_set(%s, ", s->as.decl.name);
                emit_dict_pack(o, kt, d->as.dict.keys[i], 1);
                fputs(", ", o);
                emit_dict_pack(o, vt, d->as.dict.vals[i], 1);
                fputs(");\n", o);
            }
        }
        return;
    }
    WType t;
    if (dt->kind == DT_INFER)        t = infer(s->as.decl.init);
    else if (dt->kind == DT_SCALAR)  t = wt_base(dt->base);
    else cg_fail("unknown declaration kind");
    if (t == WT_VOID || t == WT_UNKNOWN) cg_fail("cannot infer type of variable '%s'", s->as.decl.name);
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

static void emit_stmt(FILE *o, const Stmt *s, int ind) {
    switch (s->kind) {
        case ST_VAR_DECL:
            emit_local_decl(o, s, ind);
            break;
        case ST_ASSIGN: {
            const Expr *tgt = s->as.assign.target;
            if (tgt->kind == EX_INDEX) {              /* запись элемента списка через рантайм */
                VType cv = coll_vtype(tgt->as.index.coll);
                if (cv.ck == CK_LIST) {
                    if (s->as.assign.op != TK_ASSIGN)
                        cg_fail("list: only '=' is supported for elements (not +=)");
                    indent(o, ind);
                    fprintf(o, "_wl_set_%s(", list_suffix(cv.elem));
                    emit_expr(o, tgt->as.index.coll); fputs(", ", o);
                    emit_expr(o, tgt->as.index.idx);  fputs(", ", o);
                    emit_expr(o, s->as.assign.value); fputs(");\n", o);
                    break;
                }
                if (cv.ck == CK_DICT) {
                    if (s->as.assign.op != TK_ASSIGN)
                        cg_fail("dict: only '=' is supported for elements (not +=)");
                    indent(o, ind);
                    fputs("_wd_set(", o);
                    emit_expr(o, tgt->as.index.coll); fputs(", ", o);
                    emit_dict_pack(o, cv.key, tgt->as.index.idx, 1);
                    fputs(", ", o);
                    emit_dict_pack(o, cv.elem, s->as.assign.value, 1);
                    fputs(");\n", o);
                    break;
                }
            }
            if (tgt->kind != EX_IDENT && tgt->kind != EX_TYPED && tgt->kind != EX_INDEX)
                cg_fail("assignment supported to a variable or element a[i]");
            indent(o, ind);
            emit_expr(o, tgt);
            fprintf(o, " %s ", assignop_c(s->as.assign.op));
            emit_expr(o, s->as.assign.value);
            fputs(";\n", o);
            break;
        }
        case ST_IF: {
            for (int i = 0; i < s->as.iff.nclauses; i++) {
                indent(o, ind);
                fputs(i == 0 ? "if (" : "else if (", o);
                emit_expr(o, s->as.iff.clauses[i].cond);
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
            indent(o, ind); fputs("while (", o);
            emit_expr(o, s->as.whilel.cond); fputs(") {\n", o);
            emit_block(o, &s->as.whilel.body, ind + 1);
            indent(o, ind); fputs("}\n", o);
            break;
        case ST_REPEAT: {
            int id = g_rep_id++;
            indent(o, ind);
            fprintf(o, "for (int __rep%d = 0; __rep%d < (", id, id);
            emit_expr(o, s->as.repeatl.count);
            fprintf(o, "); __rep%d++) {\n", id);
            emit_block(o, &s->as.repeatl.body, ind + 1);
            indent(o, ind); fputs("}\n", o);
            break;
        }
        case ST_RETURN:
            indent(o, ind); fputs("return", o);
            if (s->as.ret.value) { fputc(' ', o); emit_expr(o, s->as.ret.value); }
            fputs(";\n", o);
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
            if (cv.ck == CK_LIST) {
                indent(o, ind);
                fprintf(o, "for (int __it%d = 0; __it%d < _wl_len(", id, id);
                emit_expr(o, s->as.loopl.coll);
                fprintf(o, "); __it%d++) {\n", id);
                indent(o, ind + 1);
                fprintf(o, "%s %s = _wl_get_%s(", ctype(cv.elem), s->as.loopl.var, list_suffix(cv.elem));
                emit_expr(o, s->as.loopl.coll);
                fprintf(o, ", __it%d);\n", id);
                sym_add(g_loc, &g_nloc, s->as.loopl.var, vt_scalar(cv.elem));
                emit_block(o, &s->as.loopl.body, ind + 1);
                indent(o, ind); fputs("}\n", o);
            } else if (cv.ck == CK_DICT) {           /* итерируем по ключам */
                indent(o, ind);
                fprintf(o, "for (int __b%d = 0; __b%d < (", id, id);
                emit_expr(o, s->as.loopl.coll);
                fprintf(o, ")->nb; __b%d++)\n", id);
                indent(o, ind + 1);
                fprintf(o, "for (_wde *__e%d = (", id);
                emit_expr(o, s->as.loopl.coll);
                fprintf(o, ")->b[__b%d]; __e%d; __e%d = __e%d->next) {\n", id, id, id, id);
                indent(o, ind + 2);
                fprintf(o, "%s %s = ", ctype(cv.key), s->as.loopl.var);
                emit_unpack_open(o, cv.key);
                fprintf(o, "__e%d->key);\n", id);
                sym_add(g_loc, &g_nloc, s->as.loopl.var, vt_scalar(cv.key));
                emit_block(o, &s->as.loopl.body, ind + 2);
                indent(o, ind + 1); fputs("}\n", o);
            } else {
                cg_fail("loop..in is supported over lists and dicts");
            }
            break;
        }
        case ST_EXPR:
            indent(o, ind);
            emit_expr(o, s->as.expr.expr);
            fputs(";\n", o);
            break;
        default:
            cg_fail("this statement (try/http.serve) is not supported yet");
    }
}

static void emit_block(FILE *o, const Block *b, int ind) {
    for (int i = 0; i < b->n; i++) emit_stmt(o, b->items[i], ind);
}

/* ---------- функция ---------- */
static void emit_func_signature(FILE *o, const Stmt *s) {
    WType ret = wt_base(s->as.func.ret);
    fprintf(o, "%s %s(", ctype(ret), s->as.func.name);
    if (s->as.func.nparams == 0) fputs("void", o);
    for (int i = 0; i < s->as.func.nparams; i++) {
        if (i) fputs(", ", o);
        fprintf(o, "%s %s", ctype(wt_base(s->as.func.params[i].type)), s->as.func.params[i].name);
    }
    fputc(')', o);
}

static void emit_func(FILE *o, const Stmt *s) {
    emit_func_signature(o, s);
    fputs(" {\n", o);
    g_nloc = 0;  /* свежая область видимости */
    for (int i = 0; i < s->as.func.nparams; i++)
        sym_add(g_loc, &g_nloc, s->as.func.params[i].name, vt_scalar(wt_base(s->as.func.params[i].type)));
    emit_block(o, &s->as.func.body, 1);
    fputs("}\n\n", o);
}

/* ---------- точка входа ---------- */
int wind_codegen(const Program *p, FILE *out, char *errbuf, int errcap) {
    cg_err = errbuf; cg_errcap = errcap;
    g_nglob = g_nloc = g_nfunc = g_rep_id = 0;

    if (setjmp(cg_jb)) return 0;

    /* пасс 1: сигнатуры функций + типы глобалов (чтобы вызовы/ссылки типизировались) */
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s->kind == ST_FUNC) {
            if (g_nfunc >= 256) cg_fail("too many functions");
            snprintf(g_func[g_nfunc].name, sizeof g_func[g_nfunc].name, "%s", s->as.func.name);
            g_func[g_nfunc].ret = wt_base(s->as.func.ret);
            g_nfunc++;
        } else if (s->kind == ST_VAR_DECL && s->as.decl.is_global) {
            const DeclType *dt = &s->as.decl.dtype;
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
        fputs("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdarg.h>\n#include <stdint.h>\n#include <time.h>\n#include <gc.h>\n\n", out);
    /* Boehm GC: весь malloc/calloc/realloc уходит под сборщик, free — пустышка */
    fputs("#define malloc(n)    GC_MALLOC(n)\n"
          "#define calloc(n,m)  GC_MALLOC((size_t)(n)*(size_t)(m))\n"
          "#define realloc(p,n) GC_REALLOC((p),(n))\n"
          "#define free(p)      ((void)(p))\n\n", out);

    fputs("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdarg.h>\n#include <stdint.h>\n#include <time.h>\n\n", out);
    fputs("__attribute__((unused)) static char *wstr_cat(const char *a, const char *b){\n"
          "    size_t la=strlen(a), lb=strlen(b);\n"
          "    char *r=malloc(la+lb+1); memcpy(r,a,la); memcpy(r+la,b,lb+1); return r;\n}\n"
          "__attribute__((unused)) static char *wstr_build(const char *fmt, ...){\n"
          "    va_list a, b; va_start(a, fmt); va_copy(b, a);\n"
          "    int n = vsnprintf(NULL, 0, fmt, a); va_end(a);\n"
          "    char *r = malloc((size_t)n + 1); vsnprintf(r, (size_t)n + 1, fmt, b); va_end(b);\n"
          "    return r;\n}\n"
          "__attribute__((unused)) static char *wstr_from_int(long v){\n"
          "    char t[32]; int n = snprintf(t, sizeof t, \"%ld\", v);\n"
          "    char *r = malloc((size_t)n + 1); memcpy(r, t, (size_t)n + 1); return r;\n}\n"
          "__attribute__((unused)) static char *wstr_from_frac(double v){\n"
          "    char t[64]; int n = snprintf(t, sizeof t, \"%g\", v);\n"
          "    char *r = malloc((size_t)n + 1); memcpy(r, t, (size_t)n + 1); return r;\n}\n"
          "__attribute__((unused)) static long   wstr_to_int(const char *s){ return strtol(s, NULL, 10); }\n"
          "__attribute__((unused)) static double wstr_to_frac(const char *s){ return strtod(s, NULL); }\n\n", out);

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
    
    /* рантайм модуля time */
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
          "    char *b=malloc((size_t)sz+1); size_t g=fread(b,1,(size_t)sz,f); b[g]=0; fclose(f); return b; }\n"
          "__attribute__((unused)) static void _wf_write(const char *path,const char *t){\n"
          "    FILE *f=fopen(path,\"w\"); if(!f){ fprintf(stderr,\"file.write: cannot open %s\\n\",path); exit(1);} if(t) fputs(t,f); fclose(f); }\n"
          "__attribute__((unused)) static void _wf_append(const char *path,const char *t){\n"
          "    FILE *f=fopen(path,\"a\"); if(!f){ fprintf(stderr,\"file.append: cannot open %s\\n\",path); exit(1);} if(t) fputs(t,f); fclose(f); }\n"
          "__attribute__((unused)) static int _wf_exists(const char *path){\n"
          "    FILE *f=fopen(path,\"r\"); if(f){ fclose(f); return 1;} return 0; }\n\n", out);

    /* рантайм модуля json: tagged union + рекурсивный парсер + аксессоры + encode.
     * Память под GC, поэтому free не нужен. \\u-эскейпы парсятся как литералы (MVP). */
    fputs(
      "typedef enum { WJ_NUL,WJ_BOO,WJ_INT,WJ_FRC,WJ_STR,WJ_ARR,WJ_OBJ } _wjt;\n"
      "typedef struct _wj { _wjt t; long i; double f; char *s; struct _wj **el; char **ky; int n; } _wj;\n"
      "static _wj *_wj_pv(const char **p);\n"
      "__attribute__((unused)) static _wj *_wj_mk(_wjt t){ _wj *j=calloc(1,sizeof *j); j->t=t; return j; }\n"
      "__attribute__((unused)) static void _wj_ws(const char **p){ while(**p==' '||**p=='\\t'||**p=='\\n'||**p=='\\r') (*p)++; }\n"
      "__attribute__((unused)) static char *_wj_pstr(const char **p){\n"
      "    (*p)++; size_t cap=16,n=0; char *b=malloc(cap);\n"
      "    while(**p && **p!='\"'){ char c=**p;\n"
      "        if(c=='\\\\'){ (*p)++; char e=**p; switch(e){ case 'n':c='\\n';break; case 't':c='\\t';break; case 'r':c='\\r';break; default:c=e; } }\n"
      "        if(n+1>=cap){ cap*=2; b=realloc(b,cap); }\n"
      "        b[n++]=c; (*p)++; }\n"
      "    if(**p=='\"') (*p)++;\n"
      "    b[n]=0; return b; }\n"
      "__attribute__((unused)) static _wj *_wj_pv(const char **p){ _wj_ws(p); char c=**p;\n"
      "    if(c=='\"'){ _wj *j=_wj_mk(WJ_STR); j->s=_wj_pstr(p); return j; }\n"
      "    if(c=='{'){ (*p)++; _wj *j=_wj_mk(WJ_OBJ); _wj_ws(p); if(**p=='}'){ (*p)++; return j; }\n"
      "        int cap=4; j->ky=malloc((size_t)cap*sizeof(char*)); j->el=malloc((size_t)cap*sizeof(_wj*));\n"
      "        for(;;){ _wj_ws(p); char *k=_wj_pstr(p); _wj_ws(p); if(**p==':')(*p)++; _wj *v=_wj_pv(p);\n"
      "            if(j->n>=cap){ cap*=2; j->ky=realloc(j->ky,(size_t)cap*sizeof(char*)); j->el=realloc(j->el,(size_t)cap*sizeof(_wj*)); }\n"
      "            j->ky[j->n]=k; j->el[j->n]=v; j->n++; _wj_ws(p);\n"
      "            if(**p==','){ (*p)++; continue; } if(**p=='}') (*p)++; break; }\n"
      "        return j; }\n"
      "    if(c=='['){ (*p)++; _wj *j=_wj_mk(WJ_ARR); _wj_ws(p); if(**p==']'){ (*p)++; return j; }\n"
      "        int cap=4; j->el=malloc((size_t)cap*sizeof(_wj*));\n"
      "        for(;;){ _wj *v=_wj_pv(p);\n"
      "            if(j->n>=cap){ cap*=2; j->el=realloc(j->el,(size_t)cap*sizeof(_wj*)); }\n"
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
      "__attribute__((unused)) static void _wjb_ch(_wjb *b,char c){ if(b->n+1>=b->cap){ b->cap=b->cap?b->cap*2:32; b->p=realloc(b->p,b->cap); } b->p[b->n++]=c; }\n"
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

    /* forward-декларации функций (рекурсия/взаимные вызовы) */
    for (int i = 0; i < p->body.n; i++)
        if (p->body.items[i]->kind == ST_FUNC) {
            emit_func_signature(out, p->body.items[i]); fputs(";\n", out);
        }
    fputs("\n", out);

    /* глобалы в file-scope (скаляры — init в main; массивы — zero-init тут) */
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s->kind != ST_VAR_DECL || !s->as.decl.is_global) continue;
        const DeclType *dt = &s->as.decl.dtype;
        if (dt->kind == DT_ARRAY) {
            fprintf(out, "%s %s[", ctype(wt_base(dt->base)), s->as.decl.name);
            emit_expr(out, dt->array_size);
            fputs("];\n", out);
        } else {
            WType t = (dt->kind == DT_INFER) ? infer(s->as.decl.init) : wt_base(dt->base);
            fprintf(out, "%s %s;\n", ctype(t), s->as.decl.name);
        }
    }
    fputs("\n", out);

    /* определения функций */
    for (int i = 0; i < p->body.n; i++)
        if (p->body.items[i]->kind == ST_FUNC) emit_func(out, p->body.items[i]);

    /* main: сперва инициализация глобалов, потом top-level инструкции */
    fputs("int main(void) {\n", out);
    fputs("    GC_INIT();\n", out);
    g_nloc = 0;
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s->kind == ST_VAR_DECL && s->as.decl.is_global && s->as.decl.init) {
            indent(out, 1);
            fprintf(out, "%s = ", s->as.decl.name);
            emit_expr(out, s->as.decl.init);
            fputs(";\n", out);
        }
    }
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s->kind == ST_FUNC) continue;
        if (s->kind == ST_VAR_DECL && s->as.decl.is_global) continue;  /* уже выше */
        emit_stmt(out, s, 1);
    }
    indent(out, 1); fputs("return 0;\n}\n", out);
    return 1;
}
