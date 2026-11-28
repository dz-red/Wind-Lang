#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#endif

#include "errors.h"

const char *current_file = "<stdin>";

typedef struct {
    const char *red, *yellow, *green, *cyan, *reset;
} DiagStyle;

static const DiagStyle plain = {"", "", "", "", ""};
static const DiagStyle colored = {C_RED, C_YEL, C_GRN, C_CYN, C_RST};

#ifdef _WIN32
static HANDLE console;
static DWORD original_console_mode;

static void restore_console_mode(void) {
    SetConsoleMode(console, original_console_mode);
}
#endif

static const DiagStyle *diag_style(void) {
    const char *no_color = getenv("NO_COLOR");
    const char *term = getenv("TERM");
    if ((no_color && *no_color) || (term && !strcmp(term, "dumb"))) return &plain;
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(handle, &mode)) return &plain;
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        if (!SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return &plain;
        console = handle;
        original_console_mode = mode;
        atexit(restore_console_mode);
    }
    return &colored;
#else
    return isatty(STDERR_FILENO) ? &colored : &plain;
#endif
}

static const char *explanation(WindDiagCode code) {
    switch (code) {
        case WD_TERMINAL:
            return "`terminal.paste` displays a value. Write the expression to display\n"
                   "  after `->`.";
        case WD_PARSE:
            return "The source does not match Wind syntax at the reported location.\n"
                   "  The highlighted position is where parsing stopped. A required token\n"
                   "  may be missing immediately before it.";
        case WD_RETURN:
            return "The type after `->` in the function signature is its return type.\n"
                   "  For example, `100` is an `int`, while `\"100\"` is a `str`.\n"
                   "  A function with return type `void` does not return a value.";
        case WD_INTERPOLATION:
            return "An expression inside `${...}` must produce an `int`, `frac`, `str`, or `bool`.\n"
                   "  A type prefix such as `str` in `str.title` is optional.\n"
                   "  If present, it must match the variable's declared type.";
        case WD_C_IMPORT:
            return "`c.import` enables a named C binding, not an arbitrary C header.\n"
                   "  Each binding defines the functions available in its namespace.";
        case WD_C_NOT_FOUND:
            return "A C library needs a Wind binding that declares its functions, types, and ownership rules.";
        case WD_C_BINDING:
            return "A `.windbind` file declares a header and a library. Functions may be discovered from the header.";
        case WD_C_HEADER:
            return "GCC could not include the C header declared by the binding.";
        case WD_C_LIBRARY:
            return "The linker could not find the library declared by the binding.";
        case WD_C_SIGNATURE:
            return "The binding's C function signature must exactly match the declaration in its header.";
        case WD_C_HEADER_DISCOVERY:
            return "Automatic C bindings use libclang to read function declarations. String pointers,\n"
                   "  numeric buffers and synchronous callbacks require explicit borrowing or copying rules.";
        default: return NULL;
    }
}

int wind_diag_explain(const char *code) {
    const WindDiagCode codes[] = {WD_TERMINAL, WD_PARSE, WD_RETURN, WD_INTERPOLATION, WD_C_IMPORT,
        WD_C_NOT_FOUND, WD_C_BINDING, WD_C_HEADER, WD_C_LIBRARY, WD_C_SIGNATURE, WD_C_HEADER_DISCOVERY};
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) {
        WindDiagCode id = codes[i];
        const char *text = explanation(id);
        if (!text) continue;
        char name[16];
        snprintf(name, sizeof name, "W%04d", id);
        if (strcmp(name, code) == 0) {
            printf("%s\n  %s\n", name, text);
            if (id == WD_TERMINAL) puts("\n  Example: terminal.paste -> \"Hello\"");
            if (id == WD_PARSE)
                puts("\n  Example: `terminal.paste value` is missing `->`.\n"
                     "  Write `terminal.paste -> value`.");
            if (id == WD_RETURN) puts("\n  Example:\n    func price() -> int\n        return 100\n    end");
            if (id == WD_INTERPOLATION)
                puts("\n  Example:\n    str.title = \"Wind\"\n    terminal.paste -> \"=== ${title} ===\"");
            if (id == WD_C_IMPORT)
                puts("\n  Example:\n    c.import <sqlite>\n    terminal.paste -> sqlite.version()");
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
    WindSource *src = wind_source_read(diags, path, f);
    fclose(f);
    return src;
}

WindSource *wind_source_read(WindDiagnostics *diags, const char *path, FILE *stream) {
    size_t size = 0, cap = 4096;
    char *text = malloc(cap);
    if (!text) return NULL;
    for (;;) {
        size_t room = cap - size - 1;
        size_t n = fread(text + size, 1, room, stream);
        size += n;
        if (n < room) break;
        char *grown = cap <= SIZE_MAX / 2 ? realloc(text, cap * 2) : NULL;
        if (!grown) { free(text); return NULL; }
        text = grown;
        cap *= 2;
    }
    if (ferror(stream)) { free(text); return NULL; }
    WindSource *src = malloc(sizeof *src);
    char *name = malloc(strlen(path) + 1);
    if (!src || !name) {
        free(src); free(text); free(name);
        return NULL;
    }
    text[size] = '\0';
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

static void excerpt(const WindSource *src, WindRange r, const char *label, int gutter,
                    int primary, const DiagStyle *style) {
    const char *s = source_line(src, r.line);
    if (!s) return;
    size_t len = strcspn(s, "\n");
    if (len && s[len - 1] == '\r') len--;
    size_t start = r.col > 0 ? (size_t)r.col - 1 : 0;
    size_t end = r.end_line > r.line ? len : r.end_col > 0 ? (size_t)r.end_col - 1 : start;
    if (start > len) start = len;
    if (end > len) end = len;
    fprintf(stderr, "%s%*d │%s ", style->cyan, gutter, r.line, style->reset);
    columns(s, len, 1);
    fputc('\n', stderr);
    int offset = columns(s, start, 0);
    int width = columns(s, end, 0) - offset;
    fprintf(stderr, "%s%*s │%s %*s%s", style->cyan, gutter, "", style->reset,
            offset, "", primary ? style->red : style->yellow);
    do { fputs(primary ? "^" : "─", stderr); } while (--width > 0);
    if (label) fprintf(stderr, " %s", label);
    if (r.end_line > r.line) fputs(" …", stderr);
    fprintf(stderr, "%s\n", style->reset);
}

static void location(const char *path, WindRange range) {
    fputs(path, stderr);
    if (range.line > 0) fprintf(stderr, ":%d:%d", range.line, range.col);
}

static void json_string(const char *s) {
    fputc('"', stderr);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(stderr, "\\%c", c);
        else if (c < 32) fprintf(stderr, "\\u%04x", c);
        else fputc(c, stderr);
    }
    fputc('"', stderr);
}

static void json_range(WindRange r) {
    fprintf(stderr, "{\"line\":%d,\"column\":%d,\"endLine\":%d,\"endColumn\":%d}",
            r.line, r.col, r.end_line, r.end_col);
}

static void json_diagnostic(const WindDiagnostic *d, const char *path) {
    fprintf(stderr, "{\"code\":\"W%04d\",\"severity\":\"error\",\"file\":", d->code);
    json_string(path);
    fputs(",\"message\":", stderr);
    json_string(d->message);
    if (d->primary.line) { fputs(",\"range\":", stderr); json_range(d->primary); }
    if (d->note) { fputs(",\"note\":", stderr); json_string(d->note); }
    if (d->secondary.line) {
        fputs(",\"related\":{\"range\":", stderr);
        json_range(d->secondary);
        fputs(",\"message\":", stderr);
        json_string(d->secondary_label ? d->secondary_label : "Related location");
        fputc('}', stderr);
    }
    fputs("}\n", stderr);
}

void wind_diag_emit(WindDiagnostics *diags, const WindDiagnostic *d) {
    const char *path = d->source ? d->source->path : current_file;
    diags->count++;
    if (diags->mode == DIAG_JSON) {
        json_diagnostic(d, path);
        return;
    }
    if (diags->mode == DIAG_SIMPLE) {
        location(path, d->primary);
        fprintf(stderr, ": error: %s\n", d->message);
        return;
    }

    const DiagStyle *style = diag_style();
    if (diags->mode == DIAG_COMPACT) {
        fputs(style->cyan, stderr);
        location(path, d->primary);
        fprintf(stderr, "%s: %serror[W%04d]%s: %s\n",
                style->reset, style->red, d->code, style->reset, d->message);
        return;
    }

    fprintf(stderr, "%serror[W%04d]%s: %s\n%s  --> ", style->red, d->code, style->reset,
            d->title ? d->title : d->message, style->cyan);
    location(path, d->primary);
    fprintf(stderr, "%s\n", style->reset);
    if (d->source && d->primary.line) {
        int last = d->primary.line > d->secondary.line ? d->primary.line : d->secondary.line;
        int gutter = 1;
        for (; last >= 10; last /= 10) gutter++;
        fprintf(stderr, "%s%*s │%s\n", style->cyan, gutter, "", style->reset);
        excerpt(d->source, d->secondary, d->secondary_label, gutter, 0, style);
        if (d->secondary.line && d->primary.line - d->secondary.line > 1)
            fprintf(stderr, "%s%*s │ …%s\n", style->cyan, gutter, "", style->reset);
        excerpt(d->source, d->primary, d->label, gutter, 1, style);
    }
    if (d->note) fprintf(stderr, "%snote:%s %s\n", style->yellow, style->reset, d->note);
    const char *text = d->explanation ? d->explanation : explanation(d->code);
    if (diags->mode == DIAG_LEARN && text) {
        fprintf(stderr, "\n%sEXPLANATION%s\n  %s\n", style->cyan, style->reset, text);
        if (d->suggestions)
            fprintf(stderr, "\n%sSUGGESTIONS\n  %s%s\n", style->green, d->suggestions, style->reset);
        if (d->search_paths)
            fprintf(stderr, "\n%sSEARCH PATHS%s\n  %s\n", style->cyan, style->reset, d->search_paths);
        fprintf(stderr, "\n%sDETAILS%s\n  wind --explain W%04d\n", style->cyan, style->reset, d->code);
    }
    fputc('\n', stderr);
}

void wind_diag_summary(const WindDiagnostics *diags) {
    if (!diags->count || diags->mode == DIAG_SIMPLE || diags->mode == DIAG_JSON) return;
    const DiagStyle *style = diag_style();
    fprintf(stderr, "%s[wind]%s compilation failed: %s%u error%s%s\n", style->cyan,
            style->reset, style->red, diags->count, diags->count == 1 ? "" : "s", style->reset);
}
