/*
 * lexer.h — токенизатор Wind. Первый шаг к AST-парсеру.
 *
 * Принцип: исходник .wnd → массив Token'ов с позицией и значением.
 * Парсер потом работает не с raw-bytes, а с этим массивом — может
 * легко смотреть вперёд/назад, ошибки получают точную позицию.
 *
 * Точечные имена (int.x, http.serve, json.encode) лексер НЕ склеивает —
 * выдаёт как IDENT . IDENT. Смысл собирает уже парсер.
 *
 * Многосимвольные операторы (->, ==, !=, &&, +=, ...) — ОДИН токен.
 *
 * Переводы строк — значимые токены (TK_NEWLINE), потому что в Wind
 * statement-разделитель — конец строки (как в Python). Несколько
 * подряд \n сворачиваются в один TK_NEWLINE.
 */

#ifndef WIND_LEXER_H
#define WIND_LEXER_H

typedef enum {
    TK_EOF = 0,
    TK_NEWLINE,

    /* Литералы */
    TK_INT_LIT,        /* 42, -17                  → ival                 */
    TK_FRAC_LIT,       /* 3,14 (запятая в Wind!)   → fval                 */
    TK_STR_LIT,        /* "hello"                  → text (без кавычек)   */
    TK_IDENT,          /* counter, my_var, foo123  → text                 */

    /* Type keywords (одновременно type-имена) */
    TK_KW_INT,         /* int   */
    TK_KW_FRAC,        /* frac  */
    TK_KW_STR,         /* str   */
    TK_KW_LIST,        /* list  */
    TK_KW_ARRAY,       /* array */
    TK_KW_DICT,        /* dict  */
    TK_KW_VOID,        /* void  */
    TK_KW_BOOL,        /* bool  */
    TK_KW_TRUE,        /* true  → 1 */
    TK_KW_FALSE,       /* false → 0 */

    /* Control flow keywords */
    TK_KW_IF,
    TK_KW_ELSE,
    TK_KW_END,
    TK_KW_REPEAT,
    TK_KW_WHILE,
    TK_KW_LOOP,
    TK_KW_IN,
    TK_KW_BREAK,
    TK_KW_CONTINUE,
    TK_KW_TRY,
    TK_KW_CATCH,
    TK_KW_THROW,
    TK_KW_RETURN,

    /* Декларации */
    TK_KW_FUNC,        /* func, fn */
    TK_KW_GLOBAL,
    TK_KW_VAR,         /* var — объявление с выводом типа */
    TK_KW_CLASS,
    TK_KW_ON,           /* для http.serve */
    TK_KW_LINK,          /* для подключения Сишных либ */
    /* Пунктуация */
    TK_LPAREN,         /* (  */
    TK_RPAREN,         /* )  */
    TK_LBRACKET,       /* [  — группировка/массив/list-литерал/dict-литерал */
    TK_RBRACKET,       /* ]  */
    TK_LBRACE,         /* {  — интерполяция в строке (не используется отдельно пока) */
    TK_RBRACE,         /* }  */
    TK_COMMA,          /* ,  — НО внутри числа 3,14 это часть TK_FRAC_LIT */
    TK_DOT,            /* .  */
    TK_COLON,          /* :  */

    /* Присваивания */
    TK_ASSIGN,         /* =   */
    TK_PLUS_EQ,        /* +=  */
    TK_MINUS_EQ,       /* -=  */
    TK_STAR_EQ,        /* *=  */
    TK_SLASH_EQ,       /* /=  */

    /* Арифметика */
    TK_PLUS,           /* +   */
    TK_MINUS,          /* -   */
    TK_STAR,           /* *   */
    TK_SLASH,          /* /   */
    TK_MOD,            /* /%  (Wind-стиль остатка) */

    /* Сравнения */
    TK_EQ,             /* ==  */
    TK_NEQ,            /* !=  */
    TK_LT,             /* <   */
    TK_GT,             /* >   */
    TK_LE,             /* <=  */
    TK_GE,             /* >=  */

    /* Логические */
    TK_AND,            /* &&  */
    TK_OR,             /* ||  */

    /* Прочее */
    TK_ARROW,          /* ->  */

    /* Маркер ошибки лексинга */
    TK_BAD,
} TokenKind;

typedef struct {
    TokenKind kind;
    char *text;        /* malloc'нутая копия для TK_IDENT, TK_STR_LIT;
                          NULL для остальных. Освобождает wind_tokens_free. */
    long ival;         /* для TK_INT_LIT */
    double fval;       /* для TK_FRAC_LIT */
    int line;          /* 1-based */
    int col;           /* 1-based, на старте токена */
} Token;

/* Лексит src в массив токенов. Последний токен всегда TK_EOF.
 * *out_count — общее число токенов включая EOF.
 * Caller освобождает через wind_tokens_free. */
Token *wind_lex(const char *src, int *out_count);

void wind_tokens_free(Token *tokens, int count);

/* Имя токена для отладки/ошибок. Возвращает указатель на статическую строку. */
const char *wind_token_kind_name(TokenKind kind);

#endif /* WIND_LEXER_H */
