#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"

/* ======================= КОНСТРУКТОРЫ ======================= */

static Expr *new_expr(ExprKind kind, int line, int col) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) { perror("malloc"); exit(1); }
    e->kind = kind; e->line = line; e->col = col;
    return e;
}

Expr *ast_int(long v, int line, int col) {
    Expr *e = new_expr(EX_INT, line, col); e->as.ival = v; return e;
}
Expr *ast_frac(double v, int line, int col) {
    Expr *e = new_expr(EX_FRAC, line, col); e->as.fval = v; return e;
}
Expr *ast_str(char *raw, int line, int col) {
    Expr *e = new_expr(EX_STR, line, col); e->as.sval = raw; return e;
}
Expr *ast_interp(char **lits, Expr **exprs, int n, int line, int col) {
    Expr *e = new_expr(EX_INTERP, line, col);
    e->as.interp.lits = lits; e->as.interp.exprs = exprs; e->as.interp.n = n;
    return e;
}
Expr *ast_ident(char *nm, int line, int col) {
    Expr *e = new_expr(EX_IDENT, line, col); e->as.ident = nm; return e;
}
Expr *ast_typed(TokenKind t, char *nm, int line, int col) {
    Expr *e = new_expr(EX_TYPED, line, col);
    e->as.typed.type = t; e->as.typed.name = nm; return e;
}
Expr *ast_dot(Expr *obj, char *field, int line, int col) {
    Expr *e = new_expr(EX_DOT, line, col);
    e->as.dot.obj = obj; e->as.dot.field = field; return e;
}
Expr *ast_call(Expr *callee, Expr **args, int nargs, int line, int col) {
    Expr *e = new_expr(EX_CALL, line, col);
    e->as.call.callee = callee; e->as.call.args = args; e->as.call.nargs = nargs; return e;
}
Expr *ast_index(Expr *coll, Expr *idx, int line, int col) {
    Expr *e = new_expr(EX_INDEX, line, col);
    e->as.index.coll = coll; e->as.index.idx = idx; return e;
}
Expr *ast_list(Expr **items, int n, int line, int col) {
    Expr *e = new_expr(EX_LIST, line, col);
    e->as.list.items = items; e->as.list.n = n; return e;
}
Expr *ast_dict(Expr **keys, Expr **vals, int n, int line, int col) {
    Expr *e = new_expr(EX_DICT, line, col);
    e->as.dict.keys = keys; e->as.dict.vals = vals; e->as.dict.n = n; return e;
}
Expr *ast_group(Expr *inner, int line, int col) {
    Expr *e = new_expr(EX_GROUP, line, col);
    e->as.group.inner = inner; return e;
}
Expr *ast_unary(TokenKind op, Expr *inner, int line, int col) {
    Expr *e = new_expr(EX_UNARY, line, col);
    e->as.unary.op = op; e->as.unary.e = inner; return e;
}
Expr *ast_binary(TokenKind op, Expr *l, Expr *r, int line, int col) {
    Expr *e = new_expr(EX_BINARY, line, col);
    e->as.binary.op = op; e->as.binary.l = l; e->as.binary.r = r; return e;
}

Stmt *ast_stmt(StmtKind kind, int line, int col) {
    Stmt *s = (Stmt *)calloc(1, sizeof(Stmt));
    if (!s) { perror("malloc"); exit(1); }
    s->kind = kind; s->line = line; s->col = col;
    return s;
}

/* ======================= РАСТУЩИЕ МАССИВЫ ======================= */

void block_push(Block *b, Stmt *s) {
    b->items = (Stmt **)realloc(b->items, (size_t)(b->n + 1) * sizeof(Stmt *));
    if (!b->items) { perror("realloc"); exit(1); }
    b->items[b->n++] = s;
}

void exprvec_push(Expr ***arr, int *n, int *cap, Expr *e) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = (Expr **)realloc(*arr, (size_t)*cap * sizeof(Expr *));
        if (!*arr) { perror("realloc"); exit(1); }
    }
    (*arr)[(*n)++] = e;
}

/* ======================= ОСВОБОЖДЕНИЕ ======================= */

void ast_free_expr(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EX_INT: case EX_FRAC: break;
        case EX_STR:   free(e->as.sval); break;
        case EX_INTERP:
            for (int i = 0; i < e->as.interp.n; i++) ast_free_expr(e->as.interp.exprs[i]);
            for (int i = 0; i < e->as.interp.n + 1; i++) free(e->as.interp.lits[i]);
            free(e->as.interp.exprs); free(e->as.interp.lits); break;
        case EX_IDENT: free(e->as.ident); break;
        case EX_TYPED: free(e->as.typed.name); break;
        case EX_DOT:
            ast_free_expr(e->as.dot.obj); free(e->as.dot.field); break;
        case EX_CALL:
            ast_free_expr(e->as.call.callee);
            for (int i = 0; i < e->as.call.nargs; i++) ast_free_expr(e->as.call.args[i]);
            free(e->as.call.args); break;
        case EX_INDEX:
            ast_free_expr(e->as.index.coll); ast_free_expr(e->as.index.idx); break;
        case EX_LIST:
            for (int i = 0; i < e->as.list.n; i++) ast_free_expr(e->as.list.items[i]);
            free(e->as.list.items); break;
        case EX_DICT:
            for (int i = 0; i < e->as.dict.n; i++) {
                ast_free_expr(e->as.dict.keys[i]);
                ast_free_expr(e->as.dict.vals[i]);
            }
            free(e->as.dict.keys); free(e->as.dict.vals); break;
        case EX_GROUP: ast_free_expr(e->as.group.inner); break;
        case EX_UNARY: ast_free_expr(e->as.unary.e); break;
        case EX_BINARY:
            ast_free_expr(e->as.binary.l); ast_free_expr(e->as.binary.r); break;
    }
    free(e);
}

void ast_free_block(Block *b);

void ast_free_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case ST_VAR_DECL:
            free(s->as.decl.name);
            ast_free_expr(s->as.decl.init);
            if (s->as.decl.dtype.kind == DT_ARRAY) ast_free_expr(s->as.decl.dtype.array_size);
            break;
        case ST_ASSIGN:
            ast_free_expr(s->as.assign.target);
            ast_free_expr(s->as.assign.value);
            break;
        case ST_IF:
            for (int i = 0; i < s->as.iff.nclauses; i++) {
                ast_free_expr(s->as.iff.clauses[i].cond);
                ast_free_block(&s->as.iff.clauses[i].body);
            }
            free(s->as.iff.clauses);
            if (s->as.iff.has_else) ast_free_block(&s->as.iff.else_b);
            break;
        case ST_WHILE:
            ast_free_expr(s->as.whilel.cond); ast_free_block(&s->as.whilel.body); break;
        case ST_REPEAT:
            ast_free_expr(s->as.repeatl.count); ast_free_block(&s->as.repeatl.body); break;
        case ST_LOOP:
            free(s->as.loopl.var); ast_free_expr(s->as.loopl.coll);
            ast_free_block(&s->as.loopl.body); break;
        case ST_FOR:
            free(s->as.forr.var); ast_free_expr(s->as.forr.from); ast_free_expr(s->as.forr.to);
            ast_free_block(&s->as.forr.body); break;
        case ST_FUNC:
            free(s->as.func.name);
            for (int i = 0; i < s->as.func.nparams; i++) free(s->as.func.params[i].name);
            free(s->as.func.params);
            ast_free_block(&s->as.func.body); break;
        case ST_RETURN: ast_free_expr(s->as.ret.value); break;
        case ST_BREAK: case ST_CONTINUE: break;
        case ST_TRY:
            ast_free_block(&s->as.tryc.body); free(s->as.tryc.catch_var);
            ast_free_block(&s->as.tryc.catch_b); break;
        case ST_THROW: ast_free_expr(s->as.throwc.value); break;
        case ST_HTTP_SERVE:
            ast_free_expr(s->as.serve.port);
            for (int i = 0; i < s->as.serve.nroutes; i++) {
                ast_free_expr(s->as.serve.routes[i].path);
                ast_free_expr(s->as.serve.routes[i].handler);
            }
            free(s->as.serve.routes);
            ast_free_block(&s->as.serve.body);
            break;
        case ST_OUTPUT:
            ast_free_expr(s->as.output.target); ast_free_expr(s->as.output.value); break;
        case ST_EXPR: ast_free_expr(s->as.expr.expr); break;
    }
    free(s);
}

void ast_free_block(Block *b) {
    if (!b) return;
    for (int i = 0; i < b->n; i++) ast_free_stmt(b->items[i]);
    free(b->items); b->items = NULL; b->n = 0;
}

void ast_free_program(Program *p) {
    if (!p) return;
    ast_free_block(&p->body);
}

/* ======================= ОТЛАДОЧНЫЙ ДАМП ======================= */

static void indent(FILE *o, int d) { for (int i = 0; i < d; i++) fputs("  ", o); }

static const char *type_name(TokenKind t) {
    switch (t) {
        case TK_KW_INT:  return "int";
        case TK_KW_FRAC: return "frac";
        case TK_KW_STR:  return "str";
        case TK_KW_VOID: return "void";
        case TK_KW_LIST: return "list";
        case TK_KW_DICT: return "dict";
        default:         return "?";
    }
}

static const char *tok_name(TokenKind t) { return wind_token_kind_name(t); }

static void dump_expr(FILE *o, const Expr *e, int d) {
    indent(o, d);
    if (!e) { fprintf(o, "<null>\n"); return; }
    switch (e->kind) {
        case EX_INT:   fprintf(o, "Int %ld\n", e->as.ival); break;
        case EX_FRAC:  fprintf(o, "Frac %g\n", e->as.fval); break;
        case EX_STR:   fprintf(o, "Str \"%s\"\n", e->as.sval ? e->as.sval : ""); break;
        case EX_INTERP:
            fprintf(o, "Interp (%d плейсхолдеров)\n", e->as.interp.n);
            for (int i = 0; i < e->as.interp.n; i++) {
                indent(o, d + 1); fprintf(o, "lit[%d]: \"%s\"\n", i, e->as.interp.lits[i]);
                indent(o, d + 1); fprintf(o, "expr[%d]:\n", i);
                dump_expr(o, e->as.interp.exprs[i], d + 2);
            }
            indent(o, d + 1);
            fprintf(o, "lit[%d]: \"%s\"\n", e->as.interp.n, e->as.interp.lits[e->as.interp.n]);
            break;
        case EX_IDENT: fprintf(o, "Ident %s\n", e->as.ident); break;
        case EX_TYPED: fprintf(o, "Typed %s.%s\n", type_name(e->as.typed.type), e->as.typed.name); break;
        case EX_DOT:
            fprintf(o, "Dot .%s\n", e->as.dot.field);
            dump_expr(o, e->as.dot.obj, d + 1); break;
        case EX_CALL:
            fprintf(o, "Call (%d args)\n", e->as.call.nargs);
            indent(o, d + 1); fprintf(o, "callee:\n");
            dump_expr(o, e->as.call.callee, d + 2);
            for (int i = 0; i < e->as.call.nargs; i++) {
                indent(o, d + 1); fprintf(o, "arg %d:\n", i);
                dump_expr(o, e->as.call.args[i], d + 2);
            }
            break;
        case EX_INDEX:
            fprintf(o, "Index\n");
            indent(o, d + 1); fprintf(o, "coll:\n"); dump_expr(o, e->as.index.coll, d + 2);
            indent(o, d + 1); fprintf(o, "idx:\n");  dump_expr(o, e->as.index.idx, d + 2);
            break;
        case EX_LIST:
            fprintf(o, "List [%d]\n", e->as.list.n);
            for (int i = 0; i < e->as.list.n; i++) dump_expr(o, e->as.list.items[i], d + 1);
            break;
        case EX_DICT:
            fprintf(o, "Dict [%d]\n", e->as.dict.n);
            for (int i = 0; i < e->as.dict.n; i++) {
                indent(o, d + 1); fprintf(o, "key:\n"); dump_expr(o, e->as.dict.keys[i], d + 2);
                indent(o, d + 1); fprintf(o, "val:\n"); dump_expr(o, e->as.dict.vals[i], d + 2);
            }
            break;
        case EX_GROUP:
            fprintf(o, "Group\n"); dump_expr(o, e->as.group.inner, d + 1); break;
        case EX_UNARY:
            fprintf(o, "Unary %s\n", tok_name(e->as.unary.op));
            dump_expr(o, e->as.unary.e, d + 1); break;
        case EX_BINARY:
            fprintf(o, "Binary %s\n", tok_name(e->as.binary.op));
            dump_expr(o, e->as.binary.l, d + 1);
            dump_expr(o, e->as.binary.r, d + 1);
            break;
    }
}

static void dump_decltype(FILE *o, const DeclType *dt) {
    switch (dt->kind) {
        case DT_SCALAR: fprintf(o, "%s", type_name(dt->base)); break;
        case DT_LIST:   fprintf(o, "%s.list", type_name(dt->base)); break;
        case DT_ARRAY:  fprintf(o, "%s[]", type_name(dt->base)); break;
        case DT_DICT:   fprintf(o, "dict[%s,%s]", type_name(dt->key), type_name(dt->val)); break;
        case DT_INFER:  fprintf(o, "var (inferred)"); break;
    }
}

static void dump_block(FILE *o, const Block *b, int d);

static void dump_stmt(FILE *o, const Stmt *s, int d) {
    indent(o, d);
    switch (s->kind) {
        case ST_VAR_DECL:
            if (s->as.decl.is_global) fputs("global ", o);
            fprintf(o, "VarDecl %s : ", s->as.decl.name);
            dump_decltype(o, &s->as.decl.dtype); fputc('\n', o);
            if (s->as.decl.init) {
                indent(o, d + 1); fprintf(o, "init:\n");
                dump_expr(o, s->as.decl.init, d + 2);
            }
            break;
        case ST_ASSIGN:
            fprintf(o, "Assign %s\n", tok_name(s->as.assign.op));
            indent(o, d + 1); fprintf(o, "target:\n"); dump_expr(o, s->as.assign.target, d + 2);
            indent(o, d + 1); fprintf(o, "value:\n");  dump_expr(o, s->as.assign.value, d + 2);
            break;
        case ST_IF:
            fprintf(o, "If (%d веток%s)\n", s->as.iff.nclauses, s->as.iff.has_else ? " + else" : "");
            for (int i = 0; i < s->as.iff.nclauses; i++) {
                indent(o, d + 1); fprintf(o, "cond %d:\n", i);
                dump_expr(o, s->as.iff.clauses[i].cond, d + 2);
                indent(o, d + 1); fprintf(o, "body %d:\n", i);
                dump_block(o, &s->as.iff.clauses[i].body, d + 2);
            }
            if (s->as.iff.has_else) {
                indent(o, d + 1); fprintf(o, "else:\n");
                dump_block(o, &s->as.iff.else_b, d + 2);
            }
            break;
        case ST_WHILE:
            fprintf(o, "While\n");
            indent(o, d + 1); fprintf(o, "cond:\n"); dump_expr(o, s->as.whilel.cond, d + 2);
            indent(o, d + 1); fprintf(o, "body:\n"); dump_block(o, &s->as.whilel.body, d + 2);
            break;
        case ST_REPEAT:
            fprintf(o, "Repeat\n");
            indent(o, d + 1); fprintf(o, "count:\n"); dump_expr(o, s->as.repeatl.count, d + 2);
            indent(o, d + 1); fprintf(o, "body:\n");  dump_block(o, &s->as.repeatl.body, d + 2);
            break;
        case ST_LOOP:
            fprintf(o, "Loop %s in\n", s->as.loopl.var);
            indent(o, d + 1); fprintf(o, "coll:\n"); dump_expr(o, s->as.loopl.coll, d + 2);
            indent(o, d + 1); fprintf(o, "body:\n"); dump_block(o, &s->as.loopl.body, d + 2);
            break;
        case ST_FOR:
            fprintf(o, "For %s in range\n", s->as.forr.var);
            indent(o, d + 1); fprintf(o, "from:\n"); dump_expr(o, s->as.forr.from, d + 2);
            indent(o, d + 1); fprintf(o, "to:\n");   dump_expr(o, s->as.forr.to, d + 2);
            indent(o, d + 1); fprintf(o, "body:\n"); dump_block(o, &s->as.forr.body, d + 2);
            break;
        case ST_FUNC:
            fprintf(o, "Func %s(", s->as.func.name);
            for (int i = 0; i < s->as.func.nparams; i++) {
                if (i) fputs(", ", o);
                fprintf(o, "%s %s", type_name(s->as.func.params[i].type), s->as.func.params[i].name);
            }
            fprintf(o, ") -> %s\n", type_name(s->as.func.ret));
            dump_block(o, &s->as.func.body, d + 1);
            break;
        case ST_RETURN:
            fprintf(o, "Return\n");
            if (s->as.ret.value) dump_expr(o, s->as.ret.value, d + 1);
            break;
        case ST_BREAK:    fprintf(o, "Break\n"); break;
        case ST_CONTINUE: fprintf(o, "Continue\n"); break;
        case ST_TRY:
            fprintf(o, "Try (catch %s)\n", s->as.tryc.catch_var ? s->as.tryc.catch_var : "");
            indent(o, d + 1); fprintf(o, "body:\n");  dump_block(o, &s->as.tryc.body, d + 2);
            indent(o, d + 1); fprintf(o, "catch:\n"); dump_block(o, &s->as.tryc.catch_b, d + 2);
            break;
        case ST_THROW:
            fprintf(o, "Throw\n"); dump_expr(o, s->as.throwc.value, d + 1); break;
        case ST_HTTP_SERVE:
            fprintf(o, "HttpServe (%d routes)\n", s->as.serve.nroutes);
            indent(o, d + 1); fprintf(o, "port:\n"); dump_expr(o, s->as.serve.port, d + 2);
            for (int i = 0; i < s->as.serve.nroutes; i++) {
                indent(o, d + 1); fprintf(o, "route %d path:\n", i);
                dump_expr(o, s->as.serve.routes[i].path, d + 2);
                indent(o, d + 1); fprintf(o, "route %d handler:\n", i);
                dump_expr(o, s->as.serve.routes[i].handler, d + 2);
            }
            break;
        case ST_OUTPUT:
            fprintf(o, "Output\n");
            indent(o, d + 1); fprintf(o, "target:\n"); dump_expr(o, s->as.output.target, d + 2);
            indent(o, d + 1); fprintf(o, "value:\n");  dump_expr(o, s->as.output.value, d + 2);
            break;
        case ST_EXPR:
            fprintf(o, "ExprStmt\n"); dump_expr(o, s->as.expr.expr, d + 1); break;
    }
}

static void dump_block(FILE *o, const Block *b, int d) {
    for (int i = 0; i < b->n; i++) dump_stmt(o, b->items[i], d);
}

void ast_dump_program(const Program *p, FILE *out) {
    fprintf(out, "Program [%d top-level]:\n", p->body.n);
        for (int i = 0; i < p->nimports; i++) {
        const Import *im = &p->imports[i];
        if (im->names == NULL) {
            fprintf(out, "  Use %s\n", im->module);
        } else {
            fprintf(out, "  Take from %s:", im->module);
            for (int j = 0; j < im->nnames; j++) fprintf(out, " %s", im->names[j]);
            fprintf(out, "\n");
        }
    }
    dump_block(out, &p->body, 1);
}
