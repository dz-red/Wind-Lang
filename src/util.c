/*
 * util.c — реализации утилит общего назначения.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "util.h"
#include "errors.h"   /* нужен current_file для func_name_conflicts_with_file */

int in_comment = 0;

void strip_comments(char *line) {
    char *src = line, *dst = line;
    int in_q = 0;
    while (*src) {
        /* Внутри строкового литерала не парсим '//' как комментарий —
         * иначе URL'ы вроде "https://x" ломают парсер. */
        if (!in_comment && *src == '"') {
            in_q = !in_q;
            *dst++ = *src++;
            continue;
        }
        if (!in_q && src[0] == '/' && src[1] == '/') {
            in_comment = !in_comment;
            src += 2;
            continue;
        }
        if (!in_comment) *dst++ = *src;
        src++;
    }
    *dst = '\0';
}

char *ltrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

void rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

int levenshtein(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    int *prev = (int *)malloc((lb + 1) * sizeof(int));
    int *curr = (int *)malloc((lb + 1) * sizeof(int));
    if (!prev || !curr) { free(prev); free(curr); return 99; }
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int m = del;
            if (ins < m) m = ins;
            if (sub < m) m = sub;
            curr[j] = m;
        }
        int *t = prev; prev = curr; curr = t;
    }
    int result = prev[lb];
    free(prev); free(curr);
    return result;
}

/* Список ключевых слов Wind — приватный для этого .c-файла, наружу не торчит.
 * Если бы хотели чтобы парсер тоже его видел — пришлось бы вынести в .h. */
static const char *wind_keywords[] = {
    "int.", "frac.", "str.",
    "if", "else", "else if", "end if",
    "terminal.paste", "terminal.clear",
    "st.import",
    "wnd.func", "end wnd.func", "wnd.run", "return",
    "end repeat", "repeat",
    "while", "end while",
    "loop", "end loop", "in",
    "try", "catch", "end try", "throw",
    "break", "continue",
    "len", "slice", "slice_chars", "chars", "find", "replace", "split", "join", "random",
    "file.read", "file.lines", "file.exists", "file.write", "file.append",
    "json.encode", "json.decode_str_int", "json.decode_str_str",
    "http.get", "http.post", "http.post_json", "http.put", "http.delete", "http.status",
    "sqrt", "pow", "sin", "cos", "tan", "log", "exp",
    "floor", "ceil", "round", "abs", "min", "max", "PI", "E",
    "shell", "shell_run",
    NULL
};

const char *suggest_keyword(const char *word) {
    int best = 99;
    const char *match = NULL;
    for (int i = 0; wind_keywords[i]; i++) {
        int d = levenshtein(word, wind_keywords[i]);
        if (d < best) {
            best = d;
            match = wind_keywords[i];
        }
    }
    int wl = (int)strlen(word);
    int threshold = wl / 3 + 1;
    if (threshold > 3) threshold = 3;
    if (best <= threshold) return match;
    return NULL;
}

int func_name_conflicts_with_file(const char *func_name) {
    const char *base = strrchr(current_file, '/');
    base = base ? base + 1 : current_file;
    size_t fn_len = strlen(func_name);
    if (strncmp(base, func_name, fn_len) == 0) {
        char next = base[fn_len];
        if (next == '\0' || next == '.') return 1;
    }
    return 0;
}
