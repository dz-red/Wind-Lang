#ifndef WIND_AST_H
#define WIND_AST_H

#include <stdio.h>
#include "lexer.h"

typedef enum {
    EX_INT,
    EX_FRAC,
    EX_STR,
    EX_INTERP,
    EX_IDENT,
    EX_TYPED,
    EX_DOT,
    EX_CALL,
    EX_INDEX,
    EX_LIST,
    EX_DICT,
    EX_GROUP,
    EX_UNARY,
    EX_BINARY,
} ExprKind;

typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    int line, col;
    union {
        long   ival;
        double fval;
        char  *sval;
        struct { char **lits; Expr **exprs; int n; } interp; /* n выражений, n+1 литералов */
        char  *ident;
        struct { TokenKind type; char *name; } typed;
        struct { Expr *obj; char *field; } dot;
        struct { Expr *callee; Expr **args; int nargs; } call;
        struct { Expr *coll; Expr *idx; } index;
        struct { Expr **items; int n; } list;
        struct { Expr **keys; Expr **vals; int n; } dict;
        struct { Expr *inner; } group;
        struct { TokenKind op; Expr *e; } unary;
        struct { TokenKind op; Expr *l, *r; } binary;
    } as;
};

typedef enum {
    DT_SCALAR,
    DT_LIST,
    DT_DICT,
    DT_ARRAY,
    DT_INFER,
} DeclKind;

typedef struct {
    DeclKind  kind;
    TokenKind base;
    TokenKind key;
    TokenKind val;
    Expr     *array_size;
} DeclType;

typedef struct Stmt Stmt;

typedef struct { Stmt **items; int n; } Block;
typedef struct { TokenKind type; char *name; } Param;
typedef struct { Expr *path; Expr *handler; } Route;
typedef struct { Expr *cond; Block body; } IfClause;

typedef enum {
    ST_VAR_DECL,
    ST_ASSIGN,
    ST_IF,
    ST_WHILE,
    ST_REPEAT,
    ST_LOOP,
    ST_FOR,
    ST_FUNC,
    ST_RETURN,
    ST_BREAK,
    ST_CONTINUE,
    ST_TRY,
    ST_THROW,
    ST_HTTP_SERVE,
    ST_OUTPUT,
    ST_EXPR,
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
    char *module;
    char **names;
    int    nnames;
} Import;

typedef struct { 
    Block body; 
    char *links[32];
    int nlinks;
    Import imports[32];
    int nimports;
} Program;

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

Stmt *ast_stmt(StmtKind kind, int line, int col);

void block_push(Block *b, Stmt *s);
void exprvec_push(Expr ***arr, int *n, int *cap, Expr *e);

void ast_free_expr(Expr *e);
void ast_free_stmt(Stmt *s);
void ast_free_block(Block *b);
void ast_free_program(Program *p);

void ast_dump_program(const Program *p, FILE *out);

#endif /* WIND_AST_H */
