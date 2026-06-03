/*
 * astparse.c — рекурсивный нисходящий парсер Wind (Token[] → AST).
 *
 * Грамматика — НОВЫЙ синтаксис (func/global/end/on/http.serve, скобки
 * группировки [...], dict[K,V].name, int.list.name, точки/стрелки опциональны).
 * Ошибки прерывают разбор через longjmp.
 */

#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astparse.h"

typedef struct {
    Token  *toks;
    int     count;
    int     pos;
    jmp_buf jb;
    char    err[256];
    int     eline, ecol;
} P;

/* ---------- утилиты ---------- */

static char *dup_s(const char *s) {
    size_t n = (s ? strlen(s) : 0) + 1;
    char *r = (char *)malloc(n);
    if (!r) { perror("malloc"); exit(1); }
    if (s) memcpy(r, s, n); else r[0] = '\0';
    return r;
}

static Token *tok(P *p)            { return &p->toks[p->pos]; }
static TokenKind cur(P *p)         { return p->toks[p->pos].kind; }
static TokenKind peekk(P *p, int o){
    int i = p->pos + o;
    return (i < p->count) ? p->toks[i].kind : TK_EOF;
}
static const char *peektext(P *p, int o) {
    int i = p->pos + o;
    return (i < p->count) ? p->toks[i].text : NULL;
}

static Token *adv(P *p) {
    Token *t = &p->toks[p->pos];
    if (t->kind != TK_EOF) p->pos++;
    return t;
}

static void perr(P *p, const char *fmt, ...) {
    Token *t = tok(p);
    p->eline = t->line; p->ecol = t->col;
    va_list ap; va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
    longjmp(p->jb, 1);
}

static int accept(P *p, TokenKind k) {
    if (cur(p) == k) { adv(p); return 1; }
    return 0;
}
static Token *expect(P *p, TokenKind k, const char *what) {
    if (cur(p) != k)
        perr(p, "ожидалось %s, получено %s", what, wind_token_kind_name(cur(p)));
    return adv(p);
}
static void skipnl(P *p)   { while (cur(p) == TK_NEWLINE) adv(p); }

static void expect_eol(P *p) {
    if (cur(p) == TK_NEWLINE) { adv(p); return; }
    if (cur(p) == TK_EOF) return;
    perr(p, "ожидался конец строки, получено %s", wind_token_kind_name(cur(p)));
}
/* съесть `end` и всё до конца строки (`end`, `end func`, `end http.serve`...) */
static void consume_end(P *p) {
    expect(p, TK_KW_END, "end");
    while (cur(p) != TK_NEWLINE && cur(p) != TK_EOF) adv(p);
    expect_eol(p);
}

static int is_scalar_type(TokenKind k) {
    return k == TK_KW_INT || k == TK_KW_FRAC || k == TK_KW_STR;
}
static TokenKind expect_scalar_type(P *p, const char *what) {
    if (!is_scalar_type(cur(p)))
        perr(p, "ожидался тип (%s), получено %s", what, wind_token_kind_name(cur(p)));
    return adv(p)->kind;
}

/* ---------- forward ---------- */
static Expr  *parse_expr(P *p);
static Expr  *parse_postfix(P *p);
static Block  parse_block(P *p);
static Stmt  *parse_stmt(P *p);

/* ---------- аргументы вызова: ( e, e, ... ) ---------- */
static Expr **parse_args(P *p, int *out_n) {
    expect(p, TK_LPAREN, "(");
    Expr **args = NULL; int n = 0, cap = 0;
    skipnl(p);
    if (cur(p) != TK_RPAREN) {
        do {
            skipnl(p);
            exprvec_push(&args, &n, &cap, parse_expr(p));
            skipnl(p);
        } while (accept(p, TK_COMMA));
    }
    skipnl(p);
    expect(p, TK_RPAREN, ")");
    *out_n = n;
    return args;
}

/* [...] в позиции примари: пусто [] | dict [k:v,..] | list [a,..] | группа [e] */
static Expr *parse_bracket(P *p) {
    Token *lb = tok(p);
    expect(p, TK_LBRACKET, "[");
    skipnl(p);
    if (cur(p) == TK_RBRACKET) { adv(p); return ast_list(NULL, 0, lb->line, lb->col); }

    Expr *first = parse_expr(p);
    skipnl(p);

    if (cur(p) == TK_COLON) {                 /* dict */
        adv(p); skipnl(p);
        Expr **keys = NULL, **vals = NULL;
        int nk = 0, kcap = 0, nv = 0, vcap = 0;
        exprvec_push(&keys, &nk, &kcap, first);
        exprvec_push(&vals, &nv, &vcap, parse_expr(p));
        skipnl(p);
        while (accept(p, TK_COMMA)) {
            skipnl(p);
            Expr *k = parse_expr(p);
            skipnl(p);
            expect(p, TK_COLON, ":");
            skipnl(p);
            Expr *v = parse_expr(p);
            exprvec_push(&keys, &nk, &kcap, k);
            exprvec_push(&vals, &nv, &vcap, v);
            skipnl(p);
        }
        expect(p, TK_RBRACKET, "]");
        return ast_dict(keys, vals, nk, lb->line, lb->col);
    }

    if (cur(p) == TK_COMMA) {                 /* list */
        Expr **items = NULL; int n = 0, cap = 0;
        exprvec_push(&items, &n, &cap, first);
        while (accept(p, TK_COMMA)) {
            skipnl(p);
            exprvec_push(&items, &n, &cap, parse_expr(p));
            skipnl(p);
        }
        expect(p, TK_RBRACKET, "]");
        return ast_list(items, n, lb->line, lb->col);
    }
    /* одиночное выражение → скобки группировки */
    expect(p, TK_RBRACKET, "]");
    return ast_group(first, lb->line, lb->col);
}

static const char *scalar_type_word(TokenKind k) {
    switch (k) { case TK_KW_INT: return "int"; case TK_KW_FRAC: return "frac";
                 case TK_KW_STR: return "str"; default: return "?"; }
}

/* ---------- интерполяция строк "...{expr}..." ---------- */

static void strvec_push(char ***arr, int *n, int *cap, char *s) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = (char **)realloc(*arr, (size_t)*cap * sizeof(char *));
        if (!*arr) { perror("realloc"); exit(1); }
    }
    (*arr)[(*n)++] = s;
}

/* Разбирает текст одного плейсхолдера через полноценный parse_expr
 * (вложенный лексер+парсер). Ошибки внутри {…} пробрасываются наружу. */
static Expr *parse_interp_placeholder(P *p, const char *inner, int line, int col) {
    while (*inner == ' ' || *inner == '\t') inner++;
    if (!*inner) { p->eline = line; p->ecol = col; perr(p, "пустые скобки {} в строке"); }

    int nt = 0;
    Token *toks = wind_lex(inner, &nt);
    P sub;
    memset(&sub, 0, sizeof sub);
    sub.toks = toks; sub.count = nt; sub.pos = 0;

    if (setjmp(sub.jb)) {                 /* ошибка во вложенном разборе */
        char msg[256];
        snprintf(msg, sizeof msg, "%s", sub.err);
        wind_tokens_free(toks, nt);
        p->eline = line; p->ecol = col;
        perr(p, "в {…}: %s", msg);
    }
    Expr *e = parse_expr(&sub);
    if (cur(&sub) != TK_EOF) {
        TokenKind extra = cur(&sub);
        wind_tokens_free(toks, nt);
        p->eline = line; p->ecol = col;
        perr(p, "лишнее в {…}: %s", wind_token_kind_name(extra));
    }
    wind_tokens_free(toks, nt);           /* выражение сделало dup_s текстов — токены не нужны */
    return e;
}

/* Строковый литерал: если есть {…} — строим EX_INTERP, иначе EX_STR. */
static Expr *parse_string_literal(P *p, const char *raw, int line, int col) {
    if (!raw || !strchr(raw, '{'))
        return ast_str(dup_s(raw ? raw : ""), line, col);

    char **lits = NULL;  int nlit = 0, litcap = 0;
    Expr **exprs = NULL; int nexp = 0, expcap = 0;

    size_t rlen = strlen(raw);
    char *buf = (char *)malloc(rlen + 1);
    if (!buf) { perror("malloc"); exit(1); }
    size_t bi = 0;

    for (const char *s = raw; *s; ) {
        if (*s == '{') {
            const char *close = strchr(s, '}');
            if (!close) { free(buf); p->eline = line; p->ecol = col;
                          perr(p, "незакрытая { в строке (нет '}')"); }
            buf[bi] = '\0';
            strvec_push(&lits, &nlit, &litcap, dup_s(buf));   /* флашим литерал */
            bi = 0;

            size_t inner_len = (size_t)(close - s - 1);
            char *inner = (char *)malloc(inner_len + 1);
            if (!inner) { perror("malloc"); exit(1); }
            memcpy(inner, s + 1, inner_len); inner[inner_len] = '\0';
            Expr *ph = parse_interp_placeholder(p, inner, line, col);
            free(inner);
            exprvec_push(&exprs, &nexp, &expcap, ph);
            s = close + 1;
        } else {
            buf[bi++] = *s++;
        }
    }
    buf[bi] = '\0';
    strvec_push(&lits, &nlit, &litcap, dup_s(buf));           /* хвостовой литерал */
    free(buf);

    return ast_interp(lits, exprs, nexp, line, col);          /* nlit == nexp+1 по построению */
}

static Expr *parse_primary(P *p) {
    Token *t = tok(p);
    switch (cur(p)) {
        case TK_INT_LIT:  adv(p); return ast_int(t->ival, t->line, t->col);
        case TK_FRAC_LIT: adv(p); return ast_frac(t->fval, t->line, t->col);
        case TK_STR_LIT:  adv(p); return parse_string_literal(p, t->text, t->line, t->col);
        case TK_IDENT:    adv(p); return ast_ident(dup_s(t->text), t->line, t->col);
        case TK_LBRACKET: return parse_bracket(p);
        case TK_KW_INT: case TK_KW_FRAC: case TK_KW_STR: {
            TokenKind ty = cur(p); adv(p);
            if (cur(p) == TK_LPAREN) {         /* каст: int(expr) */
                int n = 0; Expr **args = parse_args(p, &n);
                Expr *callee = ast_ident(dup_s(scalar_type_word(ty)), t->line, t->col);
                return ast_call(callee, args, n, t->line, t->col);
            }
            if (accept(p, TK_DOT)) {           /* типизированное имя: int.x */
                Token *nm = expect(p, TK_IDENT, "имя после тип.");
                return ast_typed(ty, dup_s(nm->text), t->line, t->col);
            }
            perr(p, "после типа ожидалось '.' или '('");
        }
        default:
            perr(p, "неожиданный токен в выражении: %s", wind_token_kind_name(cur(p)));
    }
    return NULL; /* недостижимо */
}

/* постфиксы: вызов (), индексация [], доступ .field */
static Expr *parse_postfix(P *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        if (cur(p) == TK_LPAREN) {
            int n = 0; Expr **args = parse_args(p, &n);
            e = ast_call(e, args, n, e->line, e->col);
        } else if (cur(p) == TK_LBRACKET) {
            Token *lb = tok(p); adv(p);
            Expr *idx = parse_expr(p);
            expect(p, TK_RBRACKET, "]");
            e = ast_index(e, idx, lb->line, lb->col);
        } else if (cur(p) == TK_DOT) {
            adv(p);
            Token *nm = expect(p, TK_IDENT, "имя поля после '.'");
            e = ast_dot(e, dup_s(nm->text), e->line, e->col);
        } else break;
    }
    return e;
}

static Expr *parse_unary(P *p) {
    if (cur(p) == TK_MINUS) {
        Token *t = adv(p);
        Expr *e = parse_unary(p);
        return ast_unary(TK_MINUS, e, t->line, t->col);
    }
    return parse_postfix(p);
}

static Expr *parse_mul(P *p) {
    Expr *l = parse_unary(p);
    while (cur(p) == TK_STAR || cur(p) == TK_SLASH || cur(p) == TK_MOD) {
        Token *t = adv(p);
        Expr *r = parse_unary(p);
        l = ast_binary(t->kind, l, r, t->line, t->col);
    }
    return l;
}

static Expr *parse_add(P *p) {
    Expr *l = parse_mul(p);
    while (cur(p) == TK_PLUS || cur(p) == TK_MINUS) {
        Token *t = adv(p);
        Expr *r = parse_mul(p);
        l = ast_binary(t->kind, l, r, t->line, t->col);
    }
    return l;
}

static Expr *parse_cmp(P *p) {
    Expr *l = parse_add(p);
    while (cur(p) == TK_EQ || cur(p) == TK_NEQ || cur(p) == TK_LT ||
           cur(p) == TK_GT || cur(p) == TK_LE || cur(p) == TK_GE) {
        Token *t = adv(p);
        Expr *r = parse_add(p);
        l = ast_binary(t->kind, l, r, t->line, t->col);
    }
    return l;
}

static Expr *parse_and(P *p) {
    Expr *l = parse_cmp(p);
    while (cur(p) == TK_AND) {
        Token *t = adv(p);
        Expr *r = parse_cmp(p);
        l = ast_binary(TK_AND, l, r, t->line, t->col);
    }
    return l;
}

static Expr *parse_expr(P *p) {
    Expr *l = parse_and(p);
    while (cur(p) == TK_OR) {
        Token *t = adv(p);
        Expr *r = parse_and(p);
        l = ast_binary(TK_OR, l, r, t->line, t->col);
    }
    return l;
}

/* ---------- объявление/типизированное присваивание ---------- */
static Stmt *parse_decl(P *p, int is_global, int line, int col) {
    TokenKind ty = cur(p);

    if (ty == TK_KW_DICT) {
        adv(p);
        expect(p, TK_LBRACKET, "[");
        TokenKind kty = expect_scalar_type(p, "тип ключа");
        expect(p, TK_COMMA, ",");
        TokenKind vty = expect_scalar_type(p, "тип значения");
        expect(p, TK_RBRACKET, "]");
        accept(p, TK_DOT);                       /* точка опциональна: dict[..].x или dict[..] x */
        Token *nm = expect(p, TK_IDENT, "имя dict");
        Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
        s->as.decl.is_global = is_global;
        s->as.decl.dtype.kind = DT_DICT;
        s->as.decl.dtype.key = kty;
        s->as.decl.dtype.val = vty;
        s->as.decl.name = dup_s(nm->text);
        /* dict[..].name["k"] = v  — присваивание элемента с префиксом */
        if (cur(p) == TK_LBRACKET) {
            adv(p);
            Expr *idx = parse_expr(p);
            expect(p, TK_RBRACKET, "]");
            expect(p, TK_ASSIGN, "=");
            Expr *val = parse_expr(p);
            expect_eol(p);
            free(s->as.decl.name); free(s);
            Stmt *a = ast_stmt(ST_ASSIGN, line, col);
            a->as.assign.target = ast_index(ast_ident(dup_s(nm->text), line, col), idx, line, col);
            a->as.assign.op = TK_ASSIGN;
            a->as.assign.value = val;
            return a;
        }
        if (accept(p, TK_ASSIGN)) s->as.decl.init = parse_expr(p);
        expect_eol(p);
        return s;
    }

    /* scalar / list / array, базовый тип int|frac|str */
    if (!is_scalar_type(ty))
        perr(p, "ожидался тип объявления, получено %s", wind_token_kind_name(ty));
    adv(p);
    accept(p, TK_DOT);                            /* точка опциональна: int.x или int x */

    int is_list = 0;
    if (cur(p) == TK_KW_LIST) { adv(p); accept(p, TK_DOT); is_list = 1; }

    Token *nm = expect(p, TK_IDENT, "имя переменной");
    char *name = dup_s(nm->text);

    /* трейлинг [idx]: либо размер массива (decl), либо запись элемента (assign) */
    if (cur(p) == TK_LBRACKET) {
        adv(p);
        Expr *idx = parse_expr(p);
        expect(p, TK_RBRACKET, "]");
        if (accept(p, TK_ASSIGN)) {                 /* a[i] = v — запись элемента */
            Expr *val = parse_expr(p);
            expect_eol(p);
            Stmt *a = ast_stmt(ST_ASSIGN, line, col);
            a->as.assign.target = ast_index(ast_ident(name, line, col), idx, line, col);
            a->as.assign.op = TK_ASSIGN;
            a->as.assign.value = val;
            return a;
        }
        /* объявление статического массива int.nums[5] */
        expect_eol(p);
        Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
        s->as.decl.is_global = is_global;
        s->as.decl.dtype.kind = DT_ARRAY;
        s->as.decl.dtype.base = ty;
        s->as.decl.dtype.array_size = idx;
        s->as.decl.name = name;
        return s;
    }

    Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
    s->as.decl.is_global = is_global;
    s->as.decl.dtype.kind = is_list ? DT_LIST : DT_SCALAR;
    s->as.decl.dtype.base = ty;
    s->as.decl.name = name;
    if (accept(p, TK_ASSIGN)) s->as.decl.init = parse_expr(p);
    expect_eol(p);
    return s;
}

/* var x = expr — тип выводится в кодогене */
static Stmt *parse_var(P *p, int line, int col) {
    expect(p, TK_KW_VAR, "var");
    Token *nm = expect(p, TK_IDENT, "имя переменной");
    expect(p, TK_ASSIGN, "=");
    Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
    s->as.decl.is_global = 0;
    s->as.decl.dtype.kind = DT_INFER;
    s->as.decl.name = dup_s(nm->text);
    s->as.decl.init = parse_expr(p);
    expect_eol(p);
    return s;
}

/* ---------- управляющие конструкции ---------- */
static void ifclause_push(IfClause **arr, int *n, int *cap, Expr *cond, Block body) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = (IfClause *)realloc(*arr, (size_t)*cap * sizeof(IfClause));
        if (!*arr) { perror("realloc"); exit(1); }
    }
    (*arr)[*n].cond = cond; (*arr)[*n].body = body; (*n)++;
}

static Stmt *parse_if(P *p, int line, int col) {
    expect(p, TK_KW_IF, "if");
    IfClause *clauses = NULL; int n = 0, cap = 0;
    Block else_b = {0}; int has_else = 0;

    Expr *cond = parse_expr(p); expect_eol(p);
    Block body = parse_block(p);
    ifclause_push(&clauses, &n, &cap, cond, body);

    while (cur(p) == TK_KW_ELSE) {
        adv(p);
        if (cur(p) == TK_KW_IF) {                 /* else if */
            adv(p);
            Expr *c2 = parse_expr(p); expect_eol(p);
            Block b2 = parse_block(p);
            ifclause_push(&clauses, &n, &cap, c2, b2);
        } else {                                  /* else */
            expect_eol(p);
            else_b = parse_block(p);
            has_else = 1;
            break;
        }
    }
    consume_end(p);
    Stmt *s = ast_stmt(ST_IF, line, col);
    s->as.iff.clauses = clauses; s->as.iff.nclauses = n;
    s->as.iff.else_b = else_b; s->as.iff.has_else = has_else;
    return s;
}

static Stmt *parse_while(P *p, int line, int col) {
    expect(p, TK_KW_WHILE, "while");
    Expr *cond = parse_expr(p); expect_eol(p);
    Block body = parse_block(p);
    consume_end(p);
    Stmt *s = ast_stmt(ST_WHILE, line, col);
    s->as.whilel.cond = cond; s->as.whilel.body = body;
    return s;
}

static Stmt *parse_repeat(P *p, int line, int col) {
    expect(p, TK_KW_REPEAT, "repeat");
    Expr *count = parse_expr(p); expect_eol(p);
    Block body = parse_block(p);
    consume_end(p);
    Stmt *s = ast_stmt(ST_REPEAT, line, col);
    s->as.repeatl.count = count; s->as.repeatl.body = body;
    return s;
}

static Stmt *parse_loop(P *p, int line, int col) {
    expect(p, TK_KW_LOOP, "loop");
    Token *v = expect(p, TK_IDENT, "имя переменной цикла");
    expect(p, TK_KW_IN, "in");
    Expr *coll = parse_expr(p); expect_eol(p);
    Block body = parse_block(p);
    consume_end(p);
    Stmt *s = ast_stmt(ST_LOOP, line, col);
    s->as.loopl.var = dup_s(v->text); s->as.loopl.coll = coll; s->as.loopl.body = body;
    return s;
}

static Stmt *parse_func(P *p, int line, int col) {
    expect(p, TK_KW_FUNC, "func");
    Token *nm = expect(p, TK_IDENT, "имя функции");
    expect(p, TK_LPAREN, "(");
    Param *params = NULL; int nparams = 0, cap = 0;
    skipnl(p);
    if (cur(p) != TK_RPAREN) {
        do {
            skipnl(p);
            TokenKind pty = expect_scalar_type(p, "тип параметра");
            accept(p, TK_DOT);                    /* точка опциональна: int.x или int x */
            Token *pn = expect(p, TK_IDENT, "имя параметра");
            if (nparams >= cap) {
                cap = cap ? cap * 2 : 4;
                params = (Param *)realloc(params, (size_t)cap * sizeof(Param));
                if (!params) { perror("realloc"); exit(1); }
            }
            params[nparams].type = pty;
            params[nparams].name = dup_s(pn->text);
            nparams++;
            skipnl(p);
        } while (accept(p, TK_COMMA));
    }
    skipnl(p);
    expect(p, TK_RPAREN, ")");
    accept(p, TK_ARROW);                          /* стрелка опциональна: -> str или str */
    TokenKind ret = TK_KW_VOID;
    if (is_scalar_type(cur(p)) || cur(p) == TK_KW_VOID) ret = adv(p)->kind;
    expect_eol(p);
    Block body = parse_block(p);
    consume_end(p);
    Stmt *s = ast_stmt(ST_FUNC, line, col);
    s->as.func.name = dup_s(nm->text);
    s->as.func.params = params; s->as.func.nparams = nparams;
    s->as.func.ret = ret; s->as.func.body = body;
    return s;
}

static Stmt *parse_return(P *p, int line, int col) {
    expect(p, TK_KW_RETURN, "return");
    Stmt *s = ast_stmt(ST_RETURN, line, col);
    if (cur(p) != TK_NEWLINE && cur(p) != TK_EOF)
        s->as.ret.value = parse_expr(p);
    else
        s->as.ret.value = NULL;
    expect_eol(p);
    return s;
}

static Stmt *parse_try(P *p, int line, int col) {
    expect(p, TK_KW_TRY, "try");
    expect_eol(p);
    Block body = parse_block(p);
    expect(p, TK_KW_CATCH, "catch");
    char *cv = NULL;
    if (cur(p) == TK_IDENT) cv = dup_s(adv(p)->text);
    expect_eol(p);
    Block catch_b = parse_block(p);
    consume_end(p);
    Stmt *s = ast_stmt(ST_TRY, line, col);
    s->as.tryc.body = body; s->as.tryc.catch_var = cv; s->as.tryc.catch_b = catch_b;
    return s;
}

static Stmt *parse_throw(P *p, int line, int col) {
    expect(p, TK_KW_THROW, "throw");
    Expr *v = parse_expr(p);
    expect_eol(p);
    Stmt *s = ast_stmt(ST_THROW, line, col);
    s->as.throwc.value = v;
    return s;
}

/* terminal.paste -> expr */
static Stmt *parse_output(P *p, int line, int col) {
    Token *obj = expect(p, TK_IDENT, "terminal");
    expect(p, TK_DOT, ".");
    Token *field = expect(p, TK_IDENT, "paste");
    Expr *target = ast_dot(ast_ident(dup_s(obj->text), line, col), dup_s(field->text), line, col);
    expect(p, TK_ARROW, "->");
    Expr *val = parse_expr(p);
    expect_eol(p);
    Stmt *s = ast_stmt(ST_OUTPUT, line, col);
    s->as.output.target = target; s->as.output.value = val;
    return s;
}

/* http.serve PORT \n on "path" -> handler \n ... end */
static Stmt *parse_http_serve(P *p, int line, int col) {
    expect(p, TK_IDENT, "http"); expect(p, TK_DOT, "."); expect(p, TK_IDENT, "serve");
    Expr *port = parse_expr(p);
    expect_eol(p);
    Route *routes = NULL; int nroutes = 0, cap = 0;
    skipnl(p);
    while (cur(p) == TK_KW_ON) {
        adv(p);
        Expr *path = parse_expr(p);
        expect(p, TK_ARROW, "->");
        Expr *handler = parse_expr(p);
        expect_eol(p);
        if (nroutes >= cap) {
            cap = cap ? cap * 2 : 4;
            routes = (Route *)realloc(routes, (size_t)cap * sizeof(Route));
            if (!routes) { perror("realloc"); exit(1); }
        }
        routes[nroutes].path = path; routes[nroutes].handler = handler; nroutes++;
        skipnl(p);
    }
    consume_end(p);
    Stmt *s = ast_stmt(ST_HTTP_SERVE, line, col);
    s->as.serve.port = port; s->as.serve.routes = routes; s->as.serve.nroutes = nroutes;
    s->as.serve.body.items = NULL; s->as.serve.body.n = 0;
    return s;
}

/* присваивание (x=, nums[i]=, ...) или голое выражение-инструкция (вызов) */
static Stmt *parse_assign_or_expr(P *p, int line, int col) {
    Expr *lhs = parse_postfix(p);
    TokenKind k = cur(p);
    if (k == TK_ASSIGN || k == TK_PLUS_EQ || k == TK_MINUS_EQ ||
        k == TK_STAR_EQ || k == TK_SLASH_EQ) {
        adv(p);
        Expr *val = parse_expr(p);
        expect_eol(p);
        Stmt *s = ast_stmt(ST_ASSIGN, line, col);
        s->as.assign.target = lhs; s->as.assign.op = k; s->as.assign.value = val;
        return s;
    }
    expect_eol(p);
    Stmt *s = ast_stmt(ST_EXPR, line, col);
    s->as.expr.expr = lhs;
    return s;
}

/* ---------- инструкция / блок ---------- */
static int at_block_end(P *p) {
    TokenKind k = cur(p);
    return k == TK_KW_END || k == TK_KW_ELSE || k == TK_KW_CATCH || k == TK_EOF;
}

static Block parse_block(P *p) {
    Block b; b.items = NULL; b.n = 0;
    skipnl(p);
    while (!at_block_end(p)) {
        Stmt *s = parse_stmt(p);
        if (s) block_push(&b, s);
        skipnl(p);
    }
    return b;
}

static Stmt *parse_stmt(P *p) {
    skipnl(p);
    Token *t = tok(p);
    int line = t->line, col = t->col;
    switch (cur(p)) {
        case TK_KW_GLOBAL: adv(p); return parse_decl(p, 1, line, col);
        case TK_KW_INT: case TK_KW_FRAC: case TK_KW_STR: case TK_KW_DICT:
            return parse_decl(p, 0, line, col);
        case TK_KW_VAR:      return parse_var(p, line, col);
        case TK_KW_IF:       return parse_if(p, line, col);
        case TK_KW_WHILE:    return parse_while(p, line, col);
        case TK_KW_REPEAT:   return parse_repeat(p, line, col);
        case TK_KW_LOOP:     return parse_loop(p, line, col);
        case TK_KW_FUNC:     return parse_func(p, line, col);
        case TK_KW_RETURN:   return parse_return(p, line, col);
        case TK_KW_TRY:      return parse_try(p, line, col);
        case TK_KW_THROW:    return parse_throw(p, line, col);
        case TK_KW_BREAK:    adv(p); expect_eol(p); return ast_stmt(ST_BREAK, line, col);
        case TK_KW_CONTINUE: adv(p); expect_eol(p); return ast_stmt(ST_CONTINUE, line, col);
        case TK_IDENT: {
            const char *tx = t->text;
            if (tx && !strcmp(tx, "terminal") && peekk(p, 1) == TK_DOT &&
                peektext(p, 2) && !strcmp(peektext(p, 2), "paste"))
                return parse_output(p, line, col);
            if (tx && !strcmp(tx, "http") && peekk(p, 1) == TK_DOT &&
                peektext(p, 2) && !strcmp(peektext(p, 2), "serve"))
                return parse_http_serve(p, line, col);
            return parse_assign_or_expr(p, line, col);
        }
        default:
            perr(p, "неожиданное начало инструкции: %s", wind_token_kind_name(cur(p)));
    }
    return NULL; /* недостижимо */
}

/* ---------- точка входа ---------- */
int wind_parse(Token *toks, int count, Program *out,
               char *errbuf, int errcap, int *out_line, int *out_col) {
    P p;
    p.toks = toks; p.count = count; p.pos = 0;
    p.err[0] = '\0'; p.eline = 0; p.ecol = 0;

    if (setjmp(p.jb)) {
        if (errbuf && errcap > 0) snprintf(errbuf, errcap, "%s", p.err);
        if (out_line) *out_line = p.eline;
        if (out_col)  *out_col = p.ecol;
        return 0;
    }

    Program prog; prog.body.items = NULL; prog.body.n = 0;
    skipnl(&p);
    while (cur(&p) != TK_EOF) {
        Stmt *s = parse_stmt(&p);
        if (s) block_push(&prog.body, s);
        skipnl(&p);
    }
    *out = prog;
    return 1;
}
