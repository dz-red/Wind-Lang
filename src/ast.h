#ifndef WIND_AST_H
#define WIND_AST_H

#include <stdio.h>
#include "lexer.h"   /* TokenKind */

/* ======================= ВЫРАЖЕНИЯ ======================= */

typedef enum {
    EX_INT,      /* 42                        */
    EX_FRAC,     /* 3,14                      */
    EX_STR,      /* "hello" (raw, без кавычек)*/
    EX_INTERP,   /* "x={int.x}" — шаблон с {…} */
    EX_IDENT,    /* counter, render, nums     */
    EX_TYPED,    /* int.x, str.name (тип.имя) */
    EX_DOT,      /* a.b   (obj . field)       */
    EX_CALL,     /* f(args), a.b(args), int(x)*/
    EX_INDEX,    /* coll[idx]                 */
    EX_LIST,     /* [a, b, c]  / []           */
    EX_DICT,     /* [k: v, ...]               */
    EX_GROUP,    /* [expr]  — скобки группировки Wind */
    EX_UNARY,    /* -x                        */
    EX_BINARY,   /* a + b, a && b, a == b     */
} ExprKind;

typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    int line, col;
    union {
        long   ival;                                  /* EX_INT   */
        double fval;                                  /* EX_FRAC  */
        char  *sval;                                  /* EX_STR   */
        struct { char **lits; Expr **exprs; int n; } interp; /* EX_INTERP: n выраж., n+1 литералов */
        char  *ident;                                 /* EX_IDENT */
        struct { TokenKind type; char *name; } typed; /* EX_TYPED */
        struct { Expr *obj; char *field; } dot;       /* EX_DOT   */
        struct { Expr *callee; Expr **args; int nargs; } call;     /* EX_CALL  */
        struct { Expr *coll; Expr *idx; } index;      /* EX_INDEX */
        struct { Expr **items; int n; } list;         /* EX_LIST  */
        struct { Expr **keys; Expr **vals; int n; } dict; /* EX_DICT */
        struct { Expr *inner; } group;                /* EX_GROUP */
        struct { TokenKind op; Expr *e; } unary;      /* EX_UNARY */
        struct { TokenKind op; Expr *l, *r; } binary; /* EX_BINARY*/
    } as;
};

/* ======================= ТИП ДЕКЛАРАЦИИ ======================= */

typedef enum {
    DT_SCALAR,   /* int.x                */
    DT_LIST,     /* int.list.nums        */
    DT_DICT,     /* dict[str,int].cfg    */
    DT_ARRAY,    /* int.nums[5]          */
    DT_INFER,    /* var x = ... — тип выводится в кодогене */
} DeclKind;

typedef struct {
    DeclKind  kind;
    TokenKind base;        /* элемент: KW_INT/KW_FRAC/KW_STR (scalar/list/array) */
    TokenKind key;         /* DT_DICT: тип ключа   */
    TokenKind val;         /* DT_DICT: тип значения*/
    Expr     *array_size;  /* DT_ARRAY: размер     */
} DeclType;

/* ======================= ИНСТРУКЦИИ ======================= */

typedef struct Stmt Stmt;

typedef struct { Stmt **items; int n; } Block;          /* тело блока */
typedef struct { TokenKind type; char *name; } Param;    /* параметр func: int.x */
typedef struct { Expr *path; Expr *handler; } Route;     /* on "/" -> home | on "/" -> json.encode(...) */
typedef struct { Expr *cond; Block body; } IfClause;     /* if/else-if ветка */

typedef enum {
    ST_VAR_DECL,   /* [global] <type>.name [= init]   */
    ST_ASSIGN,     /* lvalue с op= (=, +=, -=, ...)    */
    ST_IF,         /* if/else if/else ... end         */
    ST_WHILE,      /* while cond ... end              */
    ST_REPEAT,     /* repeat N ... end                */
    ST_LOOP,       /* loop v in coll ... end          */
    ST_FOR,        /* for i in 0..N ... end           */
    ST_FUNC,       /* func name(params) -> ret ... end*/
    ST_RETURN,     /* return [expr]                   */
    ST_BREAK,
    ST_CONTINUE,
    ST_TRY,        /* try ... catch e ... end         */
    ST_THROW,      /* throw expr                      */
    ST_HTTP_SERVE, /* http.serve PORT ... end         */
    ST_OUTPUT,     /* terminal.paste -> expr          */
    ST_EXPR,       /* bare выражение (вызов/метод)    */
} StmtKind;

struct Stmt {
    StmtKind kind;
    int line, col;
    union {
        struct { int is_global; DeclType dtype; char *name; Expr *init; } decl;
        struct { Expr *target; TokenKind op; Expr *value; } assign;
        struct { IfClause *clauses; int nclauses; Block else_b; int has_else; } iff;
        struct { Expr *cond; Block body; } whilel;
        struct { Expr *count; Block body; } repeatl;
        struct { char *var; Expr *coll; Block body; } loopl;
        struct { char *var; Expr *from; Expr *to; Block body; } forr;
        struct { char *name; Param *params; int nparams; TokenKind ret; Block body; } func;
        struct { Expr *value; } ret;                 /* value может быть NULL */
        struct { Block body; char *catch_var; Block catch_b; } tryc;
        struct { Expr *value; } throwc;
        struct { Expr *port; Route *routes; int nroutes; Block body; } serve;
        struct { Expr *target; Expr *value; } output;
        struct { Expr *expr; } expr;
    } as;
};

typedef struct {
    char *module; /*"math" имя без расширения*/
    char **names; /*массив имён*/
    int    nnames; /*вкурить сколько имён в массиве*/
} Import;

typedef struct { 
    Block body; 
    char *links[32];
    int nlinks;
    Import imports[32];
    int nimports;
} Program;

/* ======================= КОНСТРУКТОРЫ ======================= */

Expr *ast_int   (long v,   int line, int col);
Expr *ast_frac  (double v, int line, int col);
Expr *ast_str   (char *raw,int line, int col);  /* забирает владение raw */
Expr *ast_interp(char **lits, Expr **exprs, int n, int line, int col); /* забирает владение массивами */
Expr *ast_ident (char *nm, int line, int col);  /* забирает владение nm  */
Expr *ast_typed (TokenKind t, char *nm, int line, int col);
Expr *ast_dot   (Expr *obj, char *field, int line, int col);
Expr *ast_call  (Expr *callee, Expr **args, int nargs, int line, int col);
Expr *ast_index (Expr *coll, Expr *idx, int line, int col);
Expr *ast_list  (Expr **items, int n, int line, int col);
Expr *ast_dict  (Expr **keys, Expr **vals, int n, int line, int col);
Expr *ast_group (Expr *inner, int line, int col);
Expr *ast_unary (TokenKind op, Expr *e, int line, int col);
Expr *ast_binary(TokenKind op, Expr *l, Expr *r, int line, int col);

Stmt *ast_stmt(StmtKind kind, int line, int col);  /* пустой узел нужного вида */

/* добавить узел в блок (растущий массив) */
void block_push(Block *b, Stmt *s);
void exprvec_push(Expr ***arr, int *n, int *cap, Expr *e);

/* ======================= ОСВОБОЖДЕНИЕ ======================= */

void ast_free_expr(Expr *e);
void ast_free_stmt(Stmt *s);
void ast_free_block(Block *b);
void ast_free_program(Program *p);

/* ======================= ОТЛАДОЧНЫЙ ВЫВОД ======================= */

void ast_dump_program(const Program *p, FILE *out);

#endif /* WIND_AST_H */
