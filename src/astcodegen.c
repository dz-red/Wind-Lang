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
typedef enum { WT_INT, WT_FRAC, WT_STR, WT_VOID, WT_BOOL, WT_UNKNOWN } WType;

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
    if (*n >= 256) cg_fail("слишком много переменных");
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
        case WT_VOID: return "void";
        default: cg_fail("неизвестный тип в кодогене"); return "?";
    }
}
/* суффикс рантайм-функций списка по типу элемента: _wl_get_int/_frac/_str */
static const char *list_suffix(WType t) {
    switch (t) {
        case WT_INT: case WT_BOOL: return "int";
        case WT_FRAC: return "frac";
        case WT_STR:  return "str";
        default: cg_fail("список: неподдержанный тип элемента"); return "?";
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
            if (c->kind == EX_IDENT) {
                if (!strcmp(c->as.ident, "int"))  return WT_INT;
                if (!strcmp(c->as.ident, "frac")) return WT_FRAC;
                if (!strcmp(c->as.ident, "str") || !strcmp(c->as.ident, "to_str")) return WT_STR;
                if (!strcmp(c->as.ident, "len")) return WT_INT;
                return func_ret(c->as.ident);
            }
            if (c->kind == EX_DOT) {
                if (!strcmp(c->as.dot.field, "len")) return WT_INT;
                if (!strcmp(c->as.dot.field, "has")) return WT_BOOL;
                return WT_VOID;   /* .add / .pop */
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
        default: cg_fail("неподдержанный бинарный оператор"); return "?";
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
                if (!spec) cg_fail("интерполяция: не определить тип плейсхолдера #%d", i + 1);
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
                VType ov = coll_vtype(obj);
                if (ov.ck == CK_LIST) {
                    if (!strcmp(m, "add")) {
                        if (e->as.call.nargs != 1) cg_fail(".add ждёт ровно 1 аргумент");
                        fprintf(o, "_wl_push_%s(", list_suffix(ov.elem));
                        emit_expr(o, obj); fputs(", ", o); emit_expr(o, e->as.call.args[0]); fputc(')', o);
                    } else if (!strcmp(m, "pop")) {
                        fputs("_wl_pop(", o); emit_expr(o, obj); fputc(')', o);
                    } else if (!strcmp(m, "len")) {
                        fputs("_wl_len(", o); emit_expr(o, obj); fputc(')', o);
                    } else cg_fail("неизвестный метод списка .%s", m);
                } else if (ov.ck == CK_DICT) {
                    if (!strcmp(m, "has")) {
                        if (e->as.call.nargs != 1) cg_fail(".has ждёт ровно 1 аргумент");
                        fputs("_wd_has(", o); emit_expr(o, obj); fputs(", ", o);
                        emit_dict_pack(o, ov.key, e->as.call.args[0], 0); fputc(')', o);
                    } else if (!strcmp(m, "len")) {
                        fputs("_wd_len(", o); emit_expr(o, obj); fputc(')', o);
                    } else cg_fail("неизвестный метод словаря .%s", m);
                } else cg_fail("метод .%s поддержан только у списков/словарей", m);
                break;
            }
            if (c->kind != EX_IDENT)
                cg_fail("вызов поддержан по имени функции или как метод списка");
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
                    } else cg_fail("str(): не могу определить тип аргумента");
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
            cg_fail("доступ .%s не поддержан", e->as.dot.field);
            break;
        }
        default:
            cg_fail("срез 3: это выражение (dict-литерал) пока не поддержано");
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
        default: cg_fail("terminal.paste: не могу определить тип значения");
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
        if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("неизвестный тип массива '%s'", s->as.decl.name);
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
        if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("неизвестный тип списка '%s'", s->as.decl.name);
        VType vt; vt.ck = CK_LIST; vt.elem = el; vt.key = WT_UNKNOWN;
        sym_add(g_loc, &g_nloc, s->as.decl.name, vt);
        indent(o, ind);
        fprintf(o, "_wl *%s = _wl_new(sizeof(%s));\n", s->as.decl.name, ctype(el));
        if (s->as.decl.init) {
            if (s->as.decl.init->kind != EX_LIST)
                cg_fail("инициализация списка '%s' ожидает литерал [..]", s->as.decl.name);
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
            cg_fail("неизвестный тип словаря '%s'", s->as.decl.name);
        VType v; v.ck = CK_DICT; v.elem = vt; v.key = kt;
        sym_add(g_loc, &g_nloc, s->as.decl.name, v);
        indent(o, ind);
        fprintf(o, "_wd *%s = _wd_new(%d);\n", s->as.decl.name, kt == WT_STR ? 1 : 0);
        if (s->as.decl.init) {
            if (s->as.decl.init->kind != EX_DICT)
                cg_fail("инициализация словаря '%s' ожидает литерал [ключ: значение, ...]", s->as.decl.name);
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
    else cg_fail("неизвестный вид объявления");
    if (t == WT_VOID || t == WT_UNKNOWN) cg_fail("не могу вывести тип переменной '%s'", s->as.decl.name);
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
        case TK_SLASH_EQ: return "/="; default: cg_fail("неизвестный оператор присваивания"); return "?";
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
                        cg_fail("список: для элемента поддержано только '=' (не +=)");
                    indent(o, ind);
                    fprintf(o, "_wl_set_%s(", list_suffix(cv.elem));
                    emit_expr(o, tgt->as.index.coll); fputs(", ", o);
                    emit_expr(o, tgt->as.index.idx);  fputs(", ", o);
                    emit_expr(o, s->as.assign.value); fputs(");\n", o);
                    break;
                }
                if (cv.ck == CK_DICT) {
                    if (s->as.assign.op != TK_ASSIGN)
                        cg_fail("словарь: для элемента поддержано только '=' (не +=)");
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
                cg_fail("присваивание поддержано в переменную или элемент a[i]");
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
            if (!ok) cg_fail("из вывода поддержан только terminal.paste");
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
                cg_fail("loop..in поддержан по спискам и словарям");
            }
            break;
        }
        case ST_EXPR:
            indent(o, ind);
            emit_expr(o, s->as.expr.expr);
            fputs(";\n", o);
            break;
        default:
            cg_fail("срез 3c: эта инструкция (try/http.serve) пока не поддержана");
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
            if (g_nfunc >= 256) cg_fail("слишком много функций");
            snprintf(g_func[g_nfunc].name, sizeof g_func[g_nfunc].name, "%s", s->as.func.name);
            g_func[g_nfunc].ret = wt_base(s->as.func.ret);
            g_nfunc++;
        } else if (s->kind == ST_VAR_DECL && s->as.decl.is_global) {
            const DeclType *dt = &s->as.decl.dtype;
            VType vt;
            if (dt->kind == DT_ARRAY) {
                WType el = wt_base(dt->base);
                if (el == WT_VOID || el == WT_UNKNOWN) cg_fail("неизвестный тип массива-глобала '%s'", s->as.decl.name);
                vt.ck = CK_ARRAY; vt.elem = el; vt.key = WT_UNKNOWN;
            } else {
                WType t = (dt->kind == DT_INFER) ? infer(s->as.decl.init) : wt_base(dt->base);
                if (t == WT_VOID || t == WT_UNKNOWN) cg_fail("не вывести тип глобала '%s'", s->as.decl.name);
                vt = vt_scalar(t);
            }
            sym_add(g_glob, &g_nglob, s->as.decl.name, vt);
        }
    }

    /* преамбула */
    fputs("/* Сгенерировано Wind AST-кодогеном (этап 4, срезы 1-3a) */\n", out);
    fputs("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdarg.h>\n#include <stdint.h>\n\n", out);
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
