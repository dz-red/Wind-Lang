/*
 * lexer.c — реализация токенизатора Wind.
 *
 * Алгоритм: один проход по src, символ за символом.
 * Состояние — позиция (line/col), флаг "внутри парного // комментария".
 * Каждый шаг: пропустить пробелы → распознать токен → emit → продвинуться.
 *
 * Wind-специфика:
 *   - `[` и `]` — отдельные токены (группировка вместо круглых)
 *   - `/%` — остаток (один токен TK_MOD, не / и %)
 *   - `3,14` — запятая внутри числа = десятичный разделитель
 *   - `//` парные комментарии (открыли — игнор до следующего //)
 *   - Внутри строкового литерала `"..."` все спец-символы пропускаются
 *   - `\n` значимый (TK_NEWLINE), несколько подряд = один токен
 */

#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* === Keyword table — порядок не важен === */
static const struct { const char *word; TokenKind kind; } KEYWORDS[] = {
    {"int",      TK_KW_INT},
    {"frac",     TK_KW_FRAC},
    {"str",      TK_KW_STR},
    {"list",     TK_KW_LIST},
    {"dict",     TK_KW_DICT},
    {"void",     TK_KW_VOID},

    {"if",       TK_KW_IF},
    {"else",     TK_KW_ELSE},
    {"end",      TK_KW_END},
    {"repeat",   TK_KW_REPEAT},
    {"while",    TK_KW_WHILE},
    {"loop",     TK_KW_LOOP},
    {"in",       TK_KW_IN},
    {"break",    TK_KW_BREAK},
    {"continue", TK_KW_CONTINUE},
    {"try",      TK_KW_TRY},
    {"catch",    TK_KW_CATCH},
    {"throw",    TK_KW_THROW},
    {"return",   TK_KW_RETURN},

    {"func",     TK_KW_FUNC},
    {"fn",       TK_KW_FUNC},          /* fn — синоним func */
    {"global",   TK_KW_GLOBAL},
    {"var",      TK_KW_VAR},           /* var x = ... — тип выводится на этапе кодогена */
    {"class",    TK_KW_CLASS},
    {"on",       TK_KW_ON},

    {NULL, TK_EOF}
};

static TokenKind keyword_lookup(const char *word) {
    for (int i = 0; KEYWORDS[i].word; i++) {
        if (strcmp(KEYWORDS[i].word, word) == 0) return KEYWORDS[i].kind;
    }
    return TK_IDENT;
}

/* === Состояние лексера === */
typedef struct {
    const char *src;
    int pos;        /* индекс в src */
    int line;       /* 1-based */
    int col;        /* 1-based */
    int in_comment; /* флаг для парных // ... // */
    Token *out;
    int out_count;
    int out_cap;
} Lexer;

/* Безопасный peek/advance с обновлением line/col. */
static char peek(Lexer *l) { return l->src[l->pos]; }
static char peek_at(Lexer *l, int offset) { return l->src[l->pos + offset]; }

static char advance(Lexer *l) {
    char c = l->src[l->pos];
    if (c == '\0') return c;
    l->pos++;
    if (c == '\n') { l->line++; l->col = 1; }
    else { l->col++; }
    return c;
}

/* Растим out-массив при необходимости. */
static void emit(Lexer *l, Token t) {
    if (l->out_count >= l->out_cap) {
        l->out_cap = l->out_cap ? l->out_cap * 2 : 64;
        l->out = (Token *)realloc(l->out, (size_t)l->out_cap * sizeof(Token));
        if (!l->out) { perror("realloc"); exit(1); }
    }
    l->out[l->out_count++] = t;
}

static Token simple_tok(Lexer *l, TokenKind k, int start_line, int start_col) {
    Token t;
    t.kind = k;
    t.text = NULL;
    t.ival = 0;
    t.fval = 0.0;
    t.line = start_line;
    t.col = start_col;
    return t;
}

/* === Лексинг отдельных конструкций === */

/* Идентификатор/keyword. Начало уже проверено как isalpha/_. */
static Token lex_ident(Lexer *l) {
    int sl = l->line, sc = l->col;
    int start = l->pos;
    while (peek(l) && (isalnum((unsigned char)peek(l)) || peek(l) == '_')) {
        advance(l);
    }
    int len = l->pos - start;
    char *word = (char *)malloc((size_t)len + 1);
    if (!word) { perror("malloc"); exit(1); }
    memcpy(word, l->src + start, (size_t)len);
    word[len] = '\0';

    TokenKind k = keyword_lookup(word);
    Token t = simple_tok(l, k, sl, sc);
    if (k == TK_IDENT) {
        t.text = word;  /* identы тащим текст с собой */
    } else {
        free(word);     /* keyword'у текст не нужен */
    }
    return t;
}

/* Число. Может быть int или frac (с запятой как десятичным разделителем). */
static Token lex_number(Lexer *l) {
    int sl = l->line, sc = l->col;
    int start = l->pos;
    while (isdigit((unsigned char)peek(l))) advance(l);
    int is_frac = 0;
    if (peek(l) == ',' && isdigit((unsigned char)peek_at(l, 1))) {
        is_frac = 1;
        advance(l);  /* съели ',' */
        while (isdigit((unsigned char)peek(l))) advance(l);
    }

    int len = l->pos - start;
    char buf[64];
    int copy = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, l->src + start, (size_t)copy);
    buf[copy] = '\0';

    Token t;
    if (is_frac) {
        /* '.' для strtod */
        for (int i = 0; buf[i]; i++) if (buf[i] == ',') buf[i] = '.';
        t = simple_tok(l, TK_FRAC_LIT, sl, sc);
        t.fval = strtod(buf, NULL);
    } else {
        t = simple_tok(l, TK_INT_LIT, sl, sc);
        t.ival = strtol(buf, NULL, 10);
    }
    return t;
}

static Token lex_string(Lexer *l) {
    int sl = l->line, sc = l->col;
    advance(l);
    int start = l->pos;
    int has_escape = 0;
    while (peek(l) && peek(l) != '"') {
        if (peek(l) == '\\' && peek_at(l, 1)) {
            has_escape = 1;
            advance(l);  /* съели backslash */
            advance(l);  /* съели следующий char */
        } else if (peek(l) == '\n') {
            /* Многострочные литералы — разрешаем (как в Wind было). */
            advance(l);
        } else {
            advance(l);
        }
    }
    int end = l->pos;
    int len = end - start;

    Token t = simple_tok(l, TK_STR_LIT, sl, sc);
    if (peek(l) != '"') {
        t.kind = TK_BAD;
        fprintf(stderr, "[lexer] %d:%d: unterminated string\n", sl, sc);
        return t;
    }
    advance(l);  /* закрывающая " */

    /* Распаковываем escape-последовательности если были. */
    char *text = (char *)malloc((size_t)len + 1);
    if (!text) { perror("malloc"); exit(1); }
    if (!has_escape) {
        memcpy(text, l->src + start, (size_t)len);
        text[len] = '\0';
    } else {
        int j = 0;
        for (int i = 0; i < len; i++) {
            char c = l->src[start + i];
            if (c == '\\' && i + 1 < len) {
                char n = l->src[start + i + 1];
                switch (n) {
                    case 'n':  text[j++] = '\n'; break;
                    case 't':  text[j++] = '\t'; break;
                    case 'r':  text[j++] = '\r'; break;
                    case '"':  text[j++] = '"';  break;
                    case '\\': text[j++] = '\\'; break;
                    default:   text[j++] = n;    break;
                }
                i++;
            } else {
                text[j++] = c;
            }
        }
        text[j] = '\0';
    }
    t.text = text;
    return t;
}

Token *wind_lex(const char *src, int *out_count) {
    Lexer l;
    l.src = src;
    l.pos = 0;
    l.line = 1;
    l.col = 1;
    l.in_comment = 0;
    l.out = NULL;
    l.out_count = 0;
    l.out_cap = 0;

    int newline_pending = 0;

    while (peek(&l)) {
        if (peek(&l) == '/' && peek_at(&l, 1) == '/') {
            l.in_comment = !l.in_comment;
            advance(&l); advance(&l);
            continue;
        }
        if (l.in_comment) {
            advance(&l);
            continue;
        }
        char c = peek(&l);
        if (c == '\n') {
            advance(&l);
            newline_pending = 1;
            continue;
        }
        if (c == '\r') { advance(&l); continue; }
        if (c == ' ' || c == '\t') {
            advance(&l);
            continue;
        }
        if (newline_pending) {
            Token nl = simple_tok(&l, TK_NEWLINE, l.line, l.col);
            emit(&l, nl);
            newline_pending = 0;
        }

        int sl = l.line, sc = l.col;

        /* Идентификатор / keyword */
        if (isalpha((unsigned char)c) || c == '_') {
            emit(&l, lex_ident(&l));
            continue;
        }

        /* Число */
        if (isdigit((unsigned char)c)) {
            emit(&l, lex_number(&l));
            continue;
        }

        /* Строка */
        if (c == '"') {
            emit(&l, lex_string(&l));
            continue;
        }

        /* Двусимвольные операторы — проверяем первыми */
        if (c == '-' && peek_at(&l, 1) == '>') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_ARROW, sl, sc)); continue;
        }
        if (c == '/' && peek_at(&l, 1) == '%') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_MOD, sl, sc)); continue;
        }
        if (c == '=' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_EQ, sl, sc)); continue;
        }
        if (c == '!' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_NEQ, sl, sc)); continue;
        }
        if (c == '<' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_LE, sl, sc)); continue;
        }
        if (c == '>' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_GE, sl, sc)); continue;
        }
        if (c == '&' && peek_at(&l, 1) == '&') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_AND, sl, sc)); continue;
        }
        if (c == '|' && peek_at(&l, 1) == '|') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_OR, sl, sc)); continue;
        }
        if (c == '+' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_PLUS_EQ, sl, sc)); continue;
        }
        if (c == '-' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_MINUS_EQ, sl, sc)); continue;
        }
        if (c == '*' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_STAR_EQ, sl, sc)); continue;
        }
        if (c == '/' && peek_at(&l, 1) == '=') {
            advance(&l); advance(&l);
            emit(&l, simple_tok(&l, TK_SLASH_EQ, sl, sc)); continue;
        }

        /* Односимвольные */
        TokenKind k = TK_BAD;
        switch (c) {
            case '(': k = TK_LPAREN; break;
            case ')': k = TK_RPAREN; break;
            case '[': k = TK_LBRACKET; break;
            case ']': k = TK_RBRACKET; break;
            case '{': k = TK_LBRACE; break;
            case '}': k = TK_RBRACE; break;
            case ',': k = TK_COMMA; break;
            case '.': k = TK_DOT; break;
            case ':': k = TK_COLON; break;
            case '=': k = TK_ASSIGN; break;
            case '+': k = TK_PLUS; break;
            case '-': k = TK_MINUS; break;
            case '*': k = TK_STAR; break;
            case '/': k = TK_SLASH; break;
            case '<': k = TK_LT; break;
            case '>': k = TK_GT; break;
        }
        if (k != TK_BAD) {
            advance(&l);
            emit(&l, simple_tok(&l, k, sl, sc));
            continue;
        }

        /* Неизвестный символ */
        fprintf(stderr, "[lexer] %d:%d: unexpected character '%c' (0x%02x)\n",
                sl, sc, c, (unsigned char)c);
        advance(&l);
        emit(&l, simple_tok(&l, TK_BAD, sl, sc));
    }

    /* Финальный newline если файл им заканчивался */
    if (newline_pending) {
        emit(&l, simple_tok(&l, TK_NEWLINE, l.line, l.col));
    }

    /* EOF */
    emit(&l, simple_tok(&l, TK_EOF, l.line, l.col));

    if (out_count) *out_count = l.out_count;
    return l.out;
}

void wind_tokens_free(Token *tokens, int count) {
    if (!tokens) return;
    for (int i = 0; i < count; i++) {
        if (tokens[i].text) free(tokens[i].text);
    }
    free(tokens);
}

const char *wind_token_kind_name(TokenKind kind) {
    switch (kind) {
        case TK_EOF:        return "EOF";
        case TK_NEWLINE:    return "NEWLINE";
        case TK_INT_LIT:    return "INT_LIT";
        case TK_FRAC_LIT:   return "FRAC_LIT";
        case TK_STR_LIT:    return "STR_LIT";
        case TK_IDENT:      return "IDENT";
        case TK_KW_INT:     return "KW_INT";
        case TK_KW_FRAC:    return "KW_FRAC";
        case TK_KW_STR:     return "KW_STR";
        case TK_KW_LIST:    return "KW_LIST";
        case TK_KW_DICT:    return "KW_DICT";
        case TK_KW_VOID:    return "KW_VOID";
        case TK_KW_IF:      return "KW_IF";
        case TK_KW_ELSE:    return "KW_ELSE";
        case TK_KW_END:     return "KW_END";
        case TK_KW_REPEAT:  return "KW_REPEAT";
        case TK_KW_WHILE:   return "KW_WHILE";
        case TK_KW_LOOP:    return "KW_LOOP";
        case TK_KW_IN:      return "KW_IN";
        case TK_KW_BREAK:   return "KW_BREAK";
        case TK_KW_CONTINUE:return "KW_CONTINUE";
        case TK_KW_TRY:     return "KW_TRY";
        case TK_KW_CATCH:   return "KW_CATCH";
        case TK_KW_THROW:   return "KW_THROW";
        case TK_KW_RETURN:  return "KW_RETURN";
        case TK_KW_FUNC:    return "KW_FUNC";
        case TK_KW_GLOBAL:  return "KW_GLOBAL";
        case TK_KW_VAR:     return "KW_VAR";
        case TK_KW_CLASS:   return "KW_CLASS";
        case TK_KW_ON:      return "KW_ON";
        case TK_LPAREN:     return "LPAREN";
        case TK_RPAREN:     return "RPAREN";
        case TK_LBRACKET:   return "LBRACKET";
        case TK_RBRACKET:   return "RBRACKET";
        case TK_LBRACE:     return "LBRACE";
        case TK_RBRACE:     return "RBRACE";
        case TK_COMMA:      return "COMMA";
        case TK_DOT:        return "DOT";
        case TK_COLON:      return "COLON";
        case TK_ASSIGN:     return "ASSIGN";
        case TK_PLUS_EQ:    return "PLUS_EQ";
        case TK_MINUS_EQ:   return "MINUS_EQ";
        case TK_STAR_EQ:    return "STAR_EQ";
        case TK_SLASH_EQ:   return "SLASH_EQ";
        case TK_PLUS:       return "PLUS";
        case TK_MINUS:      return "MINUS";
        case TK_STAR:       return "STAR";
        case TK_SLASH:      return "SLASH";
        case TK_MOD:        return "MOD";
        case TK_EQ:         return "EQ";
        case TK_NEQ:        return "NEQ";
        case TK_LT:         return "LT";
        case TK_GT:         return "GT";
        case TK_LE:         return "LE";
        case TK_GE:         return "GE";
        case TK_AND:        return "AND";
        case TK_OR:         return "OR";
        case TK_ARROW:      return "ARROW";
        case TK_BAD:        return "BAD";
    }
    return "?";
}
