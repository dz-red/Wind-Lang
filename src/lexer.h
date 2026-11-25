#ifndef WIND_LEXER_H
#define WIND_LEXER_H

typedef enum {
    TK_EOF = 0,
    TK_NEWLINE,

    TK_INT_LIT,
    TK_FRAC_LIT,
    TK_STR_LIT,
    TK_IDENT,

    TK_KW_INT,
    TK_KW_FRAC,
    TK_KW_STR,
    TK_KW_LIST,
    TK_KW_ARRAY,
    TK_KW_DICT,
    TK_KW_VOID,
    TK_KW_BOOL,
    TK_KW_TRUE,
    TK_KW_FALSE,

    TK_KW_IF,
    TK_KW_ELSE,
    TK_KW_END,
    TK_KW_REPEAT,
    TK_KW_WHILE,
    TK_KW_LOOP,
    TK_KW_FOR,
    TK_KW_IN,
    TK_KW_BREAK,
    TK_KW_CONTINUE,
    TK_KW_TRY,
    TK_KW_CATCH,
    TK_KW_THROW,
    TK_KW_RETURN,

    TK_KW_FUNC,
    TK_KW_GLOBAL,
    TK_KW_VAR,
    TK_KW_CLASS,
    TK_KW_ON,
    TK_KW_LINK,
    TK_KW_USE, 
    TK_KW_TAKE,
    TK_KW_FROM,

    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_LBRACE,
    TK_RBRACE,
    TK_COMMA,
    TK_DOT,
    TK_DOTDOT,
    TK_COLON,

    TK_ASSIGN,
    TK_PLUS_EQ,
    TK_MINUS_EQ,
    TK_STAR_EQ,
    TK_SLASH_EQ,

    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_MOD,

    TK_EQ,
    TK_NEQ,
    TK_LT,
    TK_GT,
    TK_LE,
    TK_GE,

    TK_AND,
    TK_OR,
    TK_ARROW,
    TK_BAD,
} TokenKind;

typedef struct {
    TokenKind kind;
    char *text;
    long ival;
    double fval;
    int line;          /* 1-based */
    int col;           /* 1-based, на старте токена */
} Token;

/* out_count включает TK_EOF. Массив освобождает wind_tokens_free. */
Token *wind_lex(const char *src, int *out_count);

void wind_tokens_free(Token *tokens, int count);

const char *wind_token_kind_name(TokenKind kind);

#endif /* WIND_LEXER_H */
