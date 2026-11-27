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
    WindDiagnostics *diags;
    const WindSource *source;
    char *nested_error;
} P;

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

static WindRange token_range(const Token *t) {
    return (WindRange){t->line, t->col, t->end_line, t->end_col};
}

static _Noreturn void parse_error_detail(P *p, const Token *t, const char *message,
                                        const char *explanation, const char *suggestions) {
    if (t->problem) message = t->problem;
    if (p->nested_error) snprintf(p->nested_error, 256, "%s", message);
    else wind_diag_emit(p->diags, &(WindDiagnostic){
        .code = t->problem ? WD_LEX : WD_PARSE, .source = p->source,
        .primary = token_range(t), .message = message,
        .explanation = t->problem ? NULL : explanation,
        .suggestions = t->problem ? NULL : suggestions
    });
    longjmp(p->jb, 1);
}

static _Noreturn void parse_error(P *p, const Token *t, const char *message) {
    parse_error_detail(p, t, message, NULL, NULL);
}

static _Noreturn void perr(P *p, const char *fmt, ...) {
    char message[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    parse_error(p, tok(p), message);
}

static int accept(P *p, TokenKind k) {
    if (cur(p) == k) { adv(p); return 1; }
    return 0;
}
static Token *expect(P *p, TokenKind k, const char *what) {
    if (cur(p) != k)
        perr(p, "expected %s, got %s", what, wind_token_kind_name(cur(p)));
    return adv(p);
}
static void skipnl(P *p)   { while (cur(p) == TK_NEWLINE) adv(p); }

static void expect_eol(P *p) {
    if (cur(p) == TK_NEWLINE) { adv(p); return; }
    if (cur(p) == TK_EOF) return;
    perr(p, "expected end of line, got %s", wind_token_kind_name(cur(p)));
}
static void consume_end(P *p, TokenKind opener, const char *name) {
    expect(p, TK_KW_END, "end");
    if (cur(p) != TK_NEWLINE && cur(p) != TK_EOF) {
        if (opener == TK_IDENT) { /* у http.serve нет отдельного токена */
            if (cur(p) != TK_IDENT || strcmp(tok(p)->text, "http") != 0)
                perr(p, "expected 'end' or 'end %s'", name);
            adv(p);
            expect(p, TK_DOT, "'.' in 'end http.serve'");
            if (cur(p) != TK_IDENT || strcmp(tok(p)->text, "serve") != 0)
                perr(p, "expected 'serve' in 'end http.serve'");
            adv(p);
        } else if (!accept(p, opener)) {
            perr(p, "expected 'end' or 'end %s'", name);
        }
    }
    expect_eol(p);
}

static int is_scalar_type(TokenKind k) {
    return k == TK_KW_INT || k == TK_KW_FRAC || k == TK_KW_STR || k == TK_KW_BOOL;
}
static TokenKind expect_scalar_type(P *p, const char *what) {
    if (!is_scalar_type(cur(p)))
        perr(p, "expected type (%s), got %s", what, wind_token_kind_name(cur(p)));
    return adv(p)->kind;
}

static Expr  *parse_expr(P *p);
static Expr  *parse_postfix(P *p);
static Block  parse_block(P *p);
static Stmt  *parse_stmt(P *p);

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

    if (cur(p) == TK_COLON) {
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

    if (cur(p) == TK_COMMA) {
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
    expect(p, TK_RBRACKET, "]");
    return ast_group(first, lb->line, lb->col);
}

static const char *scalar_type_word(TokenKind k) {
    switch (k) { case TK_KW_INT: return "int"; case TK_KW_FRAC: return "frac";
                 case TK_KW_STR: return "str"; case TK_KW_BOOL: return "bool";
                 default: return "?"; }
}

static void strvec_push(char ***arr, int *n, int *cap, char *s) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = (char **)realloc(*arr, (size_t)*cap * sizeof(char *));
        if (!*arr) { perror("realloc"); exit(1); }
    }
    (*arr)[(*n)++] = s;
}

static Expr *parse_interp_expr(Token *toks, int nt, char *msg) {
    P sub;
    memset(&sub, 0, sizeof sub);
    sub.toks = toks; sub.count = nt; sub.pos = 0;
    sub.nested_error = msg;

    if (setjmp(sub.jb)) return NULL;
    Expr *e = parse_expr(&sub);
    if (cur(&sub) != TK_EOF) {
        ast_free_expr(e);
        perr(&sub, "extra tokens: %s", wind_token_kind_name(cur(&sub)));
    }
    return e;
}

static Expr *parse_interp_placeholder(const char *inner, char *msg) {
    while (*inner == ' ' || *inner == '\t') inner++;
    if (!*inner) {
        snprintf(msg, 256, "empty expression");
        return NULL;
    }

    int nt = 0;
    Token *toks = wind_lex(inner, &nt);
    Expr *e = parse_interp_expr(toks, nt, msg);
    wind_tokens_free(toks, nt);
    return e;
}

typedef struct {
    const char *source, *decoded;
    int line, col;
} StringPosition;

static void advance_string_position(StringPosition *p, const char *to) {
    while (p->decoded < to) {
        /* Экранированная пара занимает один байт после декодирования. */
        int bytes = *p->source == '\\' ? 2 : 1;
        for (int i = 0; i < bytes; i++) {
            if (*p->source++ == '\n') { p->line++; p->col = 1; }
            else p->col++;
        }
        p->decoded++;
    }
}

static WindRange interp_range(StringPosition *p, const char *start, const char *end) {
    while (start < end && strchr(" \t\r\n", *start)) start++;
    while (end > start && strchr(" \t\r\n", end[-1])) end--;
    advance_string_position(p, start);
    WindRange range = {.line = p->line, .col = p->col};
    advance_string_position(p, end);
    range.end_line = p->line;
    range.end_col = p->col;
    return range;
}

/* Только ${…} начинает интерполяцию; одиночные скобки остаются текстом. */
static Expr *parse_string_literal(P *p, const Token *literal) {
    const char *raw = literal->text;
    int line = literal->line, col = literal->col;
    if (!raw || !strstr(raw, "${"))
        return ast_str(dup_s(raw ? raw : ""), line, col);

    char **lits = NULL;  int nlit = 0, litcap = 0;
    Expr **exprs = NULL; int nexp = 0, expcap = 0;
    WindRange *ranges = NULL;
    StringPosition position = {literal->string_source, raw, line, col + 1};

    size_t rlen = strlen(raw);
    char *buf = (char *)malloc(rlen + 1);
    if (!buf) { perror("malloc"); exit(1); }
    size_t bi = 0;

    for (const char *s = raw; *s; ) {
        if (s[0] == '$' && s[1] == '{') {
            const char *close = strchr(s + 2, '}');
            char reason[256];
            Expr *ph = NULL;
            if (!close) {
                snprintf(reason, sizeof reason, "missing '}'");
            } else {
                size_t inner_len = (size_t)(close - (s + 2));
                char *inner = (char *)malloc(inner_len + 1);
                if (!inner) { perror("malloc"); exit(1); }
                memcpy(inner, s + 2, inner_len); inner[inner_len] = '\0';
                ph = parse_interp_placeholder(inner, reason);
                free(inner);
            }
            if (!ph) {
                char message[280];
                snprintf(message, sizeof message, "in ${…}: %s", reason);
                free(buf);
                for (int i = 0; i < nlit; i++) free(lits[i]);
                for (int i = 0; i < nexp; i++) ast_free_expr(exprs[i]);
                free(lits); free(exprs); free(ranges);
                parse_error(p, literal, message);
            }
            buf[bi] = '\0';
            strvec_push(&lits, &nlit, &litcap, dup_s(buf));
            bi = 0;
            int oldcap = expcap;
            exprvec_push(&exprs, &nexp, &expcap, ph);
            if (expcap != oldcap) {
                WindRange *grown = realloc(ranges, (size_t)expcap * sizeof *ranges);
                if (!grown) { perror("realloc"); exit(1); }
                ranges = grown;
            }
            ranges[nexp - 1] = interp_range(&position, s + 2, close);
            s = close + 1;
        } else {
            buf[bi++] = *s++;
        }
    }
    buf[bi] = '\0';
    strvec_push(&lits, &nlit, &litcap, dup_s(buf));
    free(buf);

    return ast_interp(lits, exprs, ranges, nexp, line, col);
}

static Expr *parse_primary(P *p) {
    Token *t = tok(p);
    switch (cur(p)) {
        case TK_INT_LIT:  adv(p); return ast_int(t->ival, t->line, t->col);
        case TK_FRAC_LIT: adv(p); return ast_frac(t->fval, t->line, t->col);
        case TK_KW_TRUE:  adv(p); return ast_int(1, t->line, t->col);
        case TK_KW_FALSE: adv(p); return ast_int(0, t->line, t->col);
        case TK_STR_LIT:  adv(p); return parse_string_literal(p, t);
        case TK_IDENT:    adv(p); return ast_ident(dup_s(t->text), t->line, t->col);
        case TK_LBRACKET: return parse_bracket(p);
        case TK_KW_INT: case TK_KW_FRAC: case TK_KW_STR: case TK_KW_BOOL: {
            TokenKind ty = cur(p); adv(p);
            if (cur(p) == TK_LPAREN) {
                int n = 0; Expr **args = parse_args(p, &n);
                Expr *callee = ast_ident(dup_s(scalar_type_word(ty)), t->line, t->col);
                return ast_call(callee, args, n, t->line, t->col);
            }
            if (accept(p, TK_DOT)) {
                Token *nm = expect(p, TK_IDENT, "name after type.");
                return ast_typed(ty, dup_s(nm->text), t->line, t->col);
            }
            perr(p, "expected '.' or '(' after type");
        }
        default:
            perr(p, "unexpected token in expression: %s", wind_token_kind_name(cur(p)));
    }
    return NULL; /* недостижимо */
}

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
            const char *field;                        /* типы-keyword как имена аксессоров: .int()/.str()/.frac()/.bool() */
            if      (cur(p) == TK_KW_INT)  { field = "int";  adv(p); }
            else if (cur(p) == TK_KW_STR)  { field = "str";  adv(p); }
            else if (cur(p) == TK_KW_FRAC) { field = "frac"; adv(p); }
            else if (cur(p) == TK_KW_BOOL) { field = "bool"; adv(p); }
            else field = expect(p, TK_IDENT, "field name after '.'")->text;
            e = ast_dot(e, dup_s(field), e->line, e->col);
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

static Stmt *parse_decl(P *p, int is_global, int line, int col) {
    TokenKind ty = cur(p);

    if (ty == TK_KW_DICT) {
        adv(p);
        expect(p, TK_LBRACKET, "[");
        TokenKind kty = expect_scalar_type(p, "key type");
        expect(p, TK_COMMA, ",");
        TokenKind vty = expect_scalar_type(p, "value type");
        expect(p, TK_RBRACKET, "]");
        expect(p, TK_DOT, "'.' between dict[..] and name (dict[str,int].cfg)");
        Token *nm = expect(p, TK_IDENT, "dict name");
        Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
        s->as.decl.is_global = is_global;
        s->as.decl.dtype.kind = DT_DICT;
        s->as.decl.dtype.key = kty;
        s->as.decl.dtype.val = vty;
        s->as.decl.name = dup_s(nm->text);
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

    if (ty == TK_KW_LIST) {
        adv(p);
        expect(p, TK_LBRACKET, "[");
        TokenKind ety = expect_scalar_type(p, "list element type");
        expect(p, TK_RBRACKET, "]");
        expect(p, TK_DOT, "'.' between list[..] and name (list[int].nums)");
        Token *nm = expect(p, TK_IDENT, "list name");
        Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
        s->as.decl.is_global = is_global;
        s->as.decl.dtype.kind = DT_LIST;
        s->as.decl.dtype.base = ety;
        s->as.decl.name = dup_s(nm->text);
        if (accept(p, TK_ASSIGN)) s->as.decl.init = parse_expr(p);
        expect_eol(p);
        return s;
    }

    if (ty == TK_KW_ARRAY) {
        adv(p);
        expect(p, TK_LBRACKET, "[");
        TokenKind ety = expect_scalar_type(p, "array element type");
        expect(p, TK_COMMA, ",");
        Expr *size = parse_expr(p);
        expect(p, TK_RBRACKET, "]");
        expect(p, TK_DOT, "'.' between array[..] and name (array[int,5].nums)");
        Token *nm = expect(p, TK_IDENT, "array name");
        expect_eol(p);
        Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
        s->as.decl.is_global = is_global;
        s->as.decl.dtype.kind = DT_ARRAY;
        s->as.decl.dtype.base = ety;
        s->as.decl.dtype.array_size = size;
        s->as.decl.name = dup_s(nm->text);
        return s;
    }

    if (!is_scalar_type(ty))
        perr(p, "expected declaration type, got %s", wind_token_kind_name(ty));
    adv(p);
    expect(p, TK_DOT, "'.' between type and name (e.g. int.x)");

    int is_list = 0;
    if (cur(p) == TK_KW_LIST) { adv(p); expect(p, TK_DOT, "'.' after list (int.list.nums)"); is_list = 1; }

    Token *nm = expect(p, TK_IDENT, "variable name");
    char *name = dup_s(nm->text);

    /* трейлинг [idx]: либо размер массива (decl), либо запись элемента (assign) */
    if (cur(p) == TK_LBRACKET) {
        adv(p);
        Expr *idx = parse_expr(p);
        expect(p, TK_RBRACKET, "]");
        if (accept(p, TK_ASSIGN)) {
            Expr *val = parse_expr(p);
            expect_eol(p);
            Stmt *a = ast_stmt(ST_ASSIGN, line, col);
            a->as.assign.target = ast_index(ast_ident(name, line, col), idx, line, col);
            a->as.assign.op = TK_ASSIGN;
            a->as.assign.value = val;
            return a;
        }
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

static Stmt *parse_var(P *p, int line, int col) {
    expect(p, TK_KW_VAR, "var");
    Token *nm = expect(p, TK_IDENT, "variable name");
    expect(p, TK_ASSIGN, "=");
    Stmt *s = ast_stmt(ST_VAR_DECL, line, col);
    s->as.decl.is_global = 0;
    s->as.decl.dtype.kind = DT_INFER;
    s->as.decl.name = dup_s(nm->text);
    s->as.decl.init = parse_expr(p);
    expect_eol(p);
    return s;
}

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
        if (cur(p) == TK_KW_IF) {
            adv(p);
            Expr *c2 = parse_expr(p); expect_eol(p);
            Block b2 = parse_block(p);
            ifclause_push(&clauses, &n, &cap, c2, b2);
        } else {
            expect_eol(p);
            else_b = parse_block(p);
            has_else = 1;
            break;
        }
    }
    consume_end(p, TK_KW_IF, "if");
    Stmt *s = ast_stmt(ST_IF, line, col);
    s->as.iff.clauses = clauses; s->as.iff.nclauses = n;
    s->as.iff.else_b = else_b; s->as.iff.has_else = has_else;
    return s;
}

static Stmt *parse_while(P *p, int line, int col) {
    expect(p, TK_KW_WHILE, "while");
    Expr *cond = parse_expr(p); expect_eol(p);
    Block body = parse_block(p);
    consume_end(p, TK_KW_WHILE, "while");
    Stmt *s = ast_stmt(ST_WHILE, line, col);
    s->as.whilel.cond = cond; s->as.whilel.body = body;
    return s;
}

static Stmt *parse_repeat(P *p, int line, int col) {
    expect(p, TK_KW_REPEAT, "repeat");
    Expr *count = parse_expr(p); expect_eol(p);
    Block body = parse_block(p);
    consume_end(p, TK_KW_REPEAT, "repeat");
    Stmt *s = ast_stmt(ST_REPEAT, line, col);
    s->as.repeatl.count = count; s->as.repeatl.body = body;
    return s;
}

static Stmt *parse_loop(P *p, int line, int col) {
    expect(p, TK_KW_LOOP, "loop");
    Token *v = expect(p, TK_IDENT, "loop variable name");
    expect(p, TK_KW_IN, "in");
    Expr *coll = parse_expr(p); expect_eol(p);
    Block body = parse_block(p);
    consume_end(p, TK_KW_LOOP, "loop");
    Stmt *s = ast_stmt(ST_LOOP, line, col);
    s->as.loopl.var = dup_s(v->text); s->as.loopl.coll = coll; s->as.loopl.body = body;
    return s;
}

static Stmt *parse_for(P *p, int line, int col) {
    expect(p, TK_KW_FOR, "for");
    Token *v = expect(p, TK_IDENT, "loop variable name");
    expect(p, TK_KW_IN, "in");
    Expr *first = parse_expr(p);
    if (accept(p, TK_DOTDOT)) {                   /* for i in A..B — диапазон [A, B) */
        Expr *to = parse_expr(p); expect_eol(p);
        Block body = parse_block(p);
        consume_end(p, TK_KW_FOR, "for");
        Stmt *s = ast_stmt(ST_FOR, line, col);
        s->as.forr.var = dup_s(v->text);
        s->as.forr.from = first; s->as.forr.to = to; s->as.forr.body = body;
        return s;
    }
    expect_eol(p);                                /* for v in coll — как loop */
    Block body = parse_block(p);
    consume_end(p, TK_KW_FOR, "for");
    Stmt *s = ast_stmt(ST_LOOP, line, col);
    s->as.loopl.var = dup_s(v->text); s->as.loopl.coll = first; s->as.loopl.body = body;
    return s;
}

static Stmt *parse_func(P *p, int line, int col) {
    Token *start = expect(p, TK_KW_FUNC, "func");
    Token *nm = expect(p, TK_IDENT, "function name");
    expect(p, TK_LPAREN, "(");
    Param *params = NULL; int nparams = 0, cap = 0;
    skipnl(p);
    if (cur(p) != TK_RPAREN) {
        do {
            skipnl(p);
            TokenKind pty = expect_scalar_type(p, "parameter type");
            expect(p, TK_DOT, "'.' between type and parameter name (int.x)");
            Token *pn = expect(p, TK_IDENT, "parameter name");
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
    WindRange type_range = token_range(start);
    TokenKind ret = TK_KW_VOID;
    if (accept(p, TK_ARROW)) {
        if (!is_scalar_type(cur(p)) && cur(p) != TK_KW_VOID)
            perr(p, "expected return type after '->', got %s", wind_token_kind_name(cur(p)));
        Token *type = adv(p);
        ret = type->kind;
        type_range = token_range(type);
    } else if (is_scalar_type(cur(p)) || cur(p) == TK_KW_VOID) {
        perr(p, "return type requires '->' (e.g. func f() -> str)");
    }
    expect_eol(p);
    Block body = parse_block(p);
    consume_end(p, TK_KW_FUNC, "func");
    Stmt *s = ast_stmt(ST_FUNC, line, col);
    s->as.func.name = dup_s(nm->text);
    s->as.func.params = params; s->as.func.nparams = nparams;
    s->as.func.ret = ret; s->as.func.body = body;
    s->as.func.type_range = type_range;
    return s;
}

static Stmt *parse_return(P *p, int line, int col) {
    Token *keyword = expect(p, TK_KW_RETURN, "return");
    Stmt *s = ast_stmt(ST_RETURN, line, col);
    s->as.ret.range = token_range(keyword);
    if (cur(p) != TK_NEWLINE && cur(p) != TK_EOF) {
        s->as.ret.range = token_range(tok(p));
        s->as.ret.value = parse_expr(p);
        const Token *last = &p->toks[p->pos - 1];
        s->as.ret.range.end_line = last->end_line;
        s->as.ret.range.end_col = last->end_col;
    }
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
    consume_end(p, TK_KW_TRY, "try");
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

static Stmt *parse_output(P *p, int line, int col) {
    Token *obj = expect(p, TK_IDENT, "terminal");
    expect(p, TK_DOT, ".");
    Token *field = expect(p, TK_IDENT, "paste");
    if (strcmp(field->text, "paste")) {
        char message[256];
        snprintf(message, sizeof message, "unknown terminal method `%.160s`", field->text);
        wind_diag_emit(p->diags, &(WindDiagnostic){
            .code = WD_TERMINAL, .source = p->source, .primary = token_range(field),
            .message = message,
            .suggestions = "Use `terminal.paste` to display a value:\n    terminal.paste -> \"Hello\""
        });
    }
    if (!accept(p, TK_ARROW)) {
        Token at = *tok(p);
        if (at.problem) parse_error(p, &at, at.problem);
        if (at.kind == TK_NEWLINE || at.kind == TK_EOF) {
            at.line = field->end_line;
            at.col = field->end_col;
        }
        at.end_line = at.line;
        at.end_col = at.col;
        char example[256];
        const char *suggestion = "Write `terminal.paste -> expression`, for example:\n"
                                 "    terminal.paste -> \"Hello\"";
        if (at.kind == TK_IDENT && (peekk(p, 1) == TK_NEWLINE || peekk(p, 1) == TK_EOF)) {
            snprintf(example, sizeof example, "Use `terminal.paste -> %.160s`.", at.text);
            suggestion = example;
        }
        parse_error_detail(p, &at, "expected `->` after `terminal.paste`",
                           "`terminal.paste` requires `->` before the expression to display.",
                           suggestion);
    }
    Expr *val = parse_expr(p);
    expect_eol(p);
    Stmt *s = ast_stmt(ST_OUTPUT, line, col);
    s->as.output.target = ast_dot(ast_ident(dup_s(obj->text), line, col), dup_s(field->text), line, col);
    s->as.output.value = val;
    return s;
}

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
    consume_end(p, TK_IDENT, "http.serve");
    Stmt *s = ast_stmt(ST_HTTP_SERVE, line, col);
    s->as.serve.port = port; s->as.serve.routes = routes; s->as.serve.nroutes = nroutes;
    s->as.serve.body.items = NULL; s->as.serve.body.n = 0;
    return s;
}

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

static int at_c_import(P *p) {
    return cur(p) == TK_IDENT && !strcmp(tok(p)->text, "c") &&
           peekk(p, 1) == TK_DOT && peekk(p, 2) == TK_IDENT &&
           !strcmp(peektext(p, 2), "import");
}

static void parse_c_import(P *p, Program *out) {
    adv(p); adv(p); adv(p);
    expect(p, TK_LT, "'<' after 'c.import'");
    Token *name = expect(p, TK_IDENT, "C binding name");
    expect(p, TK_GT, "'>' after C binding name");
    expect_eol(p);

    if (!ast_add_c_import(out, name->text, p->source, token_range(name)))
        parse_error(p, name, "too many C imports");
}

static Stmt *parse_stmt(P *p) {
    skipnl(p);
    Token *t = tok(p);
    int line = t->line, col = t->col;
    switch (cur(p)) {
        case TK_KW_GLOBAL: adv(p); return parse_decl(p, 1, line, col);
        case TK_KW_INT: case TK_KW_FRAC: case TK_KW_STR: case TK_KW_BOOL:
        case TK_KW_DICT: case TK_KW_LIST: case TK_KW_ARRAY:
            return parse_decl(p, 0, line, col);
        case TK_KW_VAR:      return parse_var(p, line, col);
        case TK_KW_IF:       return parse_if(p, line, col);
        case TK_KW_WHILE:    return parse_while(p, line, col);
        case TK_KW_REPEAT:   return parse_repeat(p, line, col);
        case TK_KW_LOOP:     return parse_loop(p, line, col);
        case TK_KW_FOR:      return parse_for(p, line, col);
        case TK_KW_FUNC:     return parse_func(p, line, col);
        case TK_KW_RETURN:   return parse_return(p, line, col);
        case TK_KW_TRY:      return parse_try(p, line, col);
        case TK_KW_THROW:    return parse_throw(p, line, col);
        case TK_KW_BREAK:    adv(p); expect_eol(p); return ast_stmt(ST_BREAK, line, col);
        case TK_KW_CONTINUE: adv(p); expect_eol(p); return ast_stmt(ST_CONTINUE, line, col);
        case TK_IDENT: {
            const char *tx = t->text;
            if (at_c_import(p)) perr(p, "`c.import` is only allowed at the top level");
            if (tx && !strcmp(tx, "terminal") && peekk(p, 1) == TK_DOT && peekk(p, 2) == TK_IDENT &&
                (!strcmp(peektext(p, 2), "paste") || peekk(p, 3) == TK_ARROW))
                return parse_output(p, line, col);
            if (tx && !strcmp(tx, "http") && peekk(p, 1) == TK_DOT &&
                peektext(p, 2) && !strcmp(peektext(p, 2), "serve"))
                return parse_http_serve(p, line, col);
            return parse_assign_or_expr(p, line, col);
        }
        default:
            perr(p, "unexpected statement start: %s", wind_token_kind_name(cur(p)));
    }
    return NULL; /* недостижимо */
}

int wind_parse(Token *toks, int count, Program *out,
               WindDiagnostics *diags, const WindSource *source) {
    P p;
    p.toks = toks; p.count = count; p.pos = 0;
    p.diags = diags; p.source = source; p.nested_error = NULL;
    unsigned previous = diags->count;
    *out = (Program){0};

    if (setjmp(p.jb)) return 0;

    skipnl(&p);
    while (cur(&p) != TK_EOF) {
        if (at_c_import(&p)) {
            parse_c_import(&p, out);
            skipnl(&p);
            continue;
        }
        if (cur(&p) == TK_KW_LINK) {
            adv(&p);
            if (cur(&p) != TK_STR_LIT) {
                perr(&p, "expected library name string after 'link', got %s",
                     wind_token_kind_name(cur(&p)));
            }
            Token *t_str = tok(&p);
            if (out->nlinks < WIND_MAX_LINKS) {
                out->links[out->nlinks++] = dup_s(t_str->text);
            }
            adv(&p);
            expect_eol(&p);
            skipnl(&p);
            continue;
        }
        if (cur(&p) == TK_KW_USE) {
            adv(&p);
            Token *m = expect(&p, TK_IDENT, "module name after 'use'");
            if (out->nimports < 32) {
                out->imports[out->nimports++] = (Import){dup_s(m->text), NULL, 0, token_range(m)};
            }
            expect_eol(&p);
            skipnl(&p);
            continue;
        }

        if (cur(&p) == TK_KW_TAKE) {
            adv(&p);

            char **names = NULL;
            int n = 0, cap = 0;

            do {
                Token *nm = expect(&p, TK_IDENT, "name after 'take'");
                if (n == cap) {
                    cap = cap ? cap * 2 : 4;
                    char **grown = (char **)realloc(names, (size_t)cap * sizeof *names);
                    if (!grown) perr(&p, "out of memory in take-list");
                    names = grown;
                }
                names[n++] = dup_s(nm->text);
            } while (accept(&p, TK_COMMA));

            expect(&p, TK_KW_FROM, "'from' after the list of names");
            Token *m = expect(&p, TK_IDENT, "module name after 'from'");

            if (out->nimports < 32) {
                out->imports[out->nimports++] = (Import){dup_s(m->text), names, n, token_range(m)};
            } else {
                for (int i = 0; i < n; i++) free(names[i]);
                free(names);
            }
            expect_eol(&p);
            skipnl(&p);
            continue;
        }

        Stmt *s = parse_stmt(&p);
        if (s) {
            s->source = source;
            block_push(&out->body, s);
        }
        skipnl(&p);
    }

    return diags->count == previous;
}
