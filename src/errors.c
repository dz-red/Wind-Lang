#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"

const char *current_file = "<stdin>";

static const char *explanation(WindDiagCode code) {
    switch (code) {
        case WD_TERMINAL:
            return "`terminal.paste` displays a value. Write the expression to display\n"
                   "  after `->`.";
        case WD_RETURN:
            return "The type after `->` in the function signature is its return type.\n"
                   "  For example, `100` is an `int`, while `\"100\"` is a `str`.\n"
                   "  A function with return type `void` does not return a value.";
        case WD_INTERPOLATION:
            return "An expression inside `${...}` must produce an `int`, `frac`, `str`, or `bool`.\n"
                   "  A type prefix such as `str` in `str.title` is optional.\n"
                   "  If present, it must match the variable's declared type.";
        default: return NULL;
    }
}

int wind_diag_explain(const char *code) {
    const WindDiagCode codes[] = {WD_TERMINAL, WD_RETURN, WD_INTERPOLATION};
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) {
        WindDiagCode id = codes[i];
        const char *text = explanation(id);
        if (!text) continue;
        char name[16];
        snprintf(name, sizeof name, "W%04d", id);
        if (strcmp(name, code) == 0) {
            printf("%s\n  %s\n", name, text);
            if (id == WD_TERMINAL) puts("\n  Example: terminal.paste -> \"Hello\"");
            if (id == WD_RETURN) puts("\n  Example:\n    func price() -> int\n        return 100\n    end");
            if (id == WD_INTERPOLATION)
                puts("\n  Example:\n    str.title = \"Wind\"\n    terminal.paste -> \"=== ${title} ===\"");
            return 1;
        }
    }
    return 0;
}

int wind_diag_mode(const char *value, WindDiagMode *mode) {
    if (!strcmp(value, "normal")) *mode = DIAG_NORMAL;
    else if (!strcmp(value, "compact")) *mode = DIAG_COMPACT;
    else if (!strcmp(value, "learn")) *mode = DIAG_LEARN;
    else return 0;
    return 1;
}

WindSource *wind_source_open(WindDiagnostics *diags, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    WindSource *src = malloc(sizeof *src);
    char *text = malloc((size_t)size + 1), *name = malloc(strlen(path) + 1);
    if (!src || !text || !name) {
        fclose(f); free(src); free(text); free(name);
        return NULL;
    }
    size_t read = fread(text, 1, (size_t)size, f);
    int failed = ferror(f);
    fclose(f);
    if (failed) { free(src); free(text); free(name); return NULL; }
    text[read] = '\0';
    strcpy(name, path);
    *src = (WindSource){name, text, diags->sources};
    diags->sources = src;
    return src;
}

void wind_diag_free(WindDiagnostics *diags) {
    while (diags->sources) {
        WindSource *src = diags->sources;
        diags->sources = src->next;
        free(src->path); free(src->text); free(src);
    }
}

static const char *source_line(const WindSource *src, int line) {
    if (!src || line < 1) return NULL;
    const char *s = src->text;
    while (s && --line > 0) {
        s = strchr(s, '\n');
        if (s) s++;
    }
    return s;
}

static int columns(const char *s, size_t len, int print) {
    int width = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') {
            int n = 4 - width % 4;
            if (print) fprintf(stderr, "%*s", n, "");
            width += n;
        } else if (c < 32 || c == 127) {
            if (print) fputc('?', stderr);
            width++;
        } else {
            if (print) fputc(c, stderr);
            if ((c & 0xc0) != 0x80) width++;
        }
    }
    return width;
}

static void excerpt(const WindSource *src, WindRange r, const char *label, int gutter, int primary) {
    const char *s = source_line(src, r.line);
    if (!s) return;
    size_t len = strcspn(s, "\n");
    if (len && s[len - 1] == '\r') len--;
    size_t start = r.col > 0 ? (size_t)r.col - 1 : 0;
    size_t end = r.end_line > r.line ? len : r.end_col > 0 ? (size_t)r.end_col - 1 : start;
    if (start > len) start = len;
    if (end > len) end = len;
    fprintf(stderr, "%*d │ ", gutter, r.line);
    columns(s, len, 1);
    fputc('\n', stderr);
    int offset = columns(s, start, 0);
    int width = columns(s, end, 0) - offset;
    fprintf(stderr, "%*s │ %*s", gutter, "", offset, "");
    do { fputs(primary ? "^" : "─", stderr); } while (--width > 0);
    if (label) fprintf(stderr, " %s", label);
    if (r.end_line > r.line) fputs(" …", stderr);
    fputc('\n', stderr);
}

static void location(const char *path, WindRange range) {
    fputs(path, stderr);
    if (range.line > 0) fprintf(stderr, ":%d:%d", range.line, range.col);
}

void wind_diag_emit(WindDiagnostics *diags, const WindDiagnostic *d) {
    const char *path = d->source ? d->source->path : current_file;
    diags->count++;
    if (diags->mode == DIAG_COMPACT || diags->mode == DIAG_SIMPLE) {
        location(path, d->primary);
        if (diags->mode == DIAG_SIMPLE) fprintf(stderr, ": error: %s\n", d->message);
        else fprintf(stderr, ": error[W%04d]: %s\n", d->code, d->message);
        return;
    }

    fprintf(stderr, "error[W%04d]: %s\n  --> ", d->code, d->title ? d->title : d->message);
    location(path, d->primary);
    fputc('\n', stderr);
    if (d->source && d->primary.line) {
        int last = d->primary.line > d->secondary.line ? d->primary.line : d->secondary.line;
        int gutter = 1;
        for (; last >= 10; last /= 10) gutter++;
        fprintf(stderr, "%*s │\n", gutter, "");
        excerpt(d->source, d->secondary, d->secondary_label, gutter, 0);
        if (d->secondary.line && d->primary.line - d->secondary.line > 1)
            fprintf(stderr, "%*s │ …\n", gutter, "");
        excerpt(d->source, d->primary, d->label, gutter, 1);
    }
    if (d->note) fprintf(stderr, "note: %s\n", d->note);
    const char *text = explanation(d->code);
    if (diags->mode == DIAG_LEARN && text) {
        fprintf(stderr, "\nEXPLANATION\n  %s\n", text);
        if (d->suggestions) fprintf(stderr, "\nSUGGESTIONS\n  %s\n", d->suggestions);
        fprintf(stderr, "\nDETAILS\n  wind --explain W%04d\n", d->code);
    }
    fputc('\n', stderr);
}

void wind_diag_summary(const WindDiagnostics *diags) {
    if (diags->count && diags->mode != DIAG_SIMPLE)
        fprintf(stderr, "[wind] compilation failed: %u error%s\n", diags->count,
                diags->count == 1 ? "" : "s");
}
