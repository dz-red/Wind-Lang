#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules.h"
#include "lexer.h"
#include "astparse.h"

#define MAX_LOADED 64
#define PATH_MAX_W 1024

/* ---------- список уже загруженных: защита от повторов и циклов ---------- */

static char *loaded[MAX_LOADED];
static int   nloaded = 0;

static int already_loaded(const char *name) {
    for (int i = 0; i < nloaded; i++)
        if (strcmp(loaded[i], name) == 0) return 1;
    return 0;
}

static void mark_loaded(const char *name) {
    if (nloaded >= MAX_LOADED) return;
    size_t n = strlen(name) + 1;
    char *copy = (char *)malloc(n);
    if (!copy) return;
    memcpy(copy, name, n);
    loaded[nloaded++] = copy;
}

static void forget_all(void) {
    for (int i = 0; i < nloaded; i++) free(loaded[i]);
    nloaded = 0;
}

/* ---------- мелкие помощники ---------- */

/* Каталог, в котором лежит файл, вместе с завершающим разделителем.
   "C:\proj\demo.wnd" -> "C:\proj\";  "demo.wnd" -> "" */
static void dir_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    char *back  = strrchr(out, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (slash) slash[1] = '\0';
    else       out[0]   = '\0';
}

/* Читает файл целиком в malloc'нутый буфер с '\0' на конце. NULL если не открылся. */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* Ищет функцию с таким именем среди уже собранных в программе. */
static Stmt *find_func(const Program *p, const char *name) {
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s && s->kind == ST_FUNC && strcmp(s->as.func.name, name) == 0)
            return s;
    }
    return NULL;
}

/* Есть ли имя в списке? */
static int name_listed(char **list, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(list[i], name) == 0) return 1;
    return 0;
}


/* ---------- собственно загрузка ---------- */

static int load_one(Program *prog, const char *dir, const char *module,
                    char *errbuf, int errcap);

/* Обрабатывает все импорты одной Program. Вынесено отдельно, потому что
   зовётся и для главного файла, и рекурсивно для каждого модуля. */
static int load_imports_of(Program *prog, const Program *src,
                           const char *dir, char *errbuf, int errcap) {
    for (int i = 0; i < src->nimports; i++) {
        if (!load_one(prog, dir, src->imports[i].module, errbuf, errcap))
            return 0;
    }
    return 1;
}

static int load_one(Program *prog, const char *dir, const char *module,
                    char *errbuf, int errcap) {
    if (already_loaded(module)) return 1;   /* уже втянули — и цикл, и повтор */
    mark_loaded(module);                    /* метим ДО разбора: иначе a->b->a зациклится */

    char path[PATH_MAX_W];
    snprintf(path, sizeof path, "%s%s.wnd", dir, module);

    char *src = read_whole_file(path);
    if (!src) {
        snprintf(errbuf, errcap, "module '%s' not found (looked for %s)", module, path);
        return 0;
    }

    int ntok = 0;
    Token *toks = wind_lex(src, &ntok);

    Program sub;
    char perr[256];
    int eline = 0, ecol = 0;
    if (!wind_parse(toks, ntok, &sub, perr, sizeof perr, &eline, &ecol)) {
        snprintf(errbuf, errcap, "%s:%d:%d: %s", path, eline, ecol, perr);
        wind_tokens_free(toks, ntok);
        free(src);
        return 0;
    }

    /* Сначала вложенные импорты модуля, потом он сам. */
    if (!load_imports_of(prog, &sub, dir, errbuf, errcap)) {
        wind_tokens_free(toks, ntok);
        free(src);
        return 0;
    }

    /* Функции переезжают в главную программу, остальное выбрасываем. */
    char *fnames[128];      /* имена функций ЭТОГО модуля — для проверки take */
    int   nfn   = 0;
    int   taken = 0;

    for (int i = 0; i < sub.body.n; i++) {
        Stmt *s = sub.body.items[i];
        if (!s) continue;

        if (s->kind != ST_FUNC) {
            ast_free_stmt(s);          /* код верхнего уровня модуля не нужен */
            continue;
        }

        if (find_func(prog, s->as.func.name)) {
            snprintf(errbuf, errcap,
                     "name conflict: '%s' from module '%s' is already defined",
                     s->as.func.name, module);
            wind_tokens_free(toks, ntok);
            free(src);
            return 0;
        }

        if (nfn < 128) fnames[nfn++] = s->as.func.name;
        block_push(&prog->body, s);    /* владение переходит к prog */
        taken++;
    }
    free(sub.body.items);

    /* link-директивы модуля тоже нужны, иначе gcc не найдёт библиотеку. */
    for (int i = 0; i < sub.nlinks; i++) {
        if (prog->nlinks < 32) prog->links[prog->nlinks++] = sub.links[i];
        else                   free(sub.links[i]);
    }

        /* Всё, что просили через `take ... from <этот модуль>`, обязано в нём быть. */
    for (int i = 0; i < prog->nimports; i++) {
        Import *im = &prog->imports[i];
        if (im->names == NULL) continue;
        if (strcmp(im->module, module) != 0) continue;
        for (int j = 0; j < im->nnames; j++) {
            if (!name_listed(fnames, nfn, im->names[j])) {
                snprintf(errbuf, errcap,
                         "module '%s' has no function '%s'", module, im->names[j]);
                wind_tokens_free(toks, ntok);
                free(src);
                return 0;
            }
        }
    }

    wind_tokens_free(toks, ntok);
    free(src);
    fprintf(stderr, "[wind] module '%s': %d function(s)\n", module, taken);
    return 1;
}

int wind_load_modules(Program *prog, const char *base_path,
                      char *errbuf, int errcap) {
    char dir[PATH_MAX_W];
    dir_of(base_path, dir, sizeof dir);

    forget_all();

    /* Копируем импорты главного файла: prog->body будет расти во время
       загрузки, но nimports не меняется, так что читать можно прямо из prog. */
    int ok = load_imports_of(prog, prog, dir, errbuf, errcap);

    forget_all();
    return ok;
}
