#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules.h"
#include "lexer.h"
#include "astparse.h"

#define MAX_LOADED 64
#define PATH_MAX_W 1024

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

static void dir_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    char *back  = strrchr(out, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (slash) slash[1] = '\0';
    else       out[0]   = '\0';
}

static Stmt *find_func(const Program *p, const char *name) {
    for (int i = 0; i < p->body.n; i++) {
        Stmt *s = p->body.items[i];
        if (s && s->kind == ST_FUNC && strcmp(s->as.func.name, name) == 0)
            return s;
    }
    return NULL;
}

static int name_listed(char **list, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(list[i], name) == 0) return 1;
    return 0;
}



static int load_one(Program *prog, const char *dir, const Import *import,
                    const WindSource *from, WindDiagnostics *diags);

static int load_imports_of(Program *prog, const Program *src,
                           const char *dir, const WindSource *from, WindDiagnostics *diags) {
    for (int i = 0; i < src->nimports; i++) {
        if (!load_one(prog, dir, &src->imports[i], from, diags))
            return 0;
    }
    return 1;
}

static int load_one(Program *prog, const char *dir, const Import *import,
                    const WindSource *from, WindDiagnostics *diags) {
    const char *module = import->module;
    if (already_loaded(module)) return 1;
    mark_loaded(module);                    /* метим ДО разбора: иначе a->b->a зациклится */

    char path[PATH_MAX_W];
    snprintf(path, sizeof path, "%s%s.wnd", dir, module);

    char error[PATH_MAX_W + 128];
    WindSource *src = wind_source_open(diags, path);
    if (!src) {
        snprintf(error, sizeof error, "cannot read module '%.64s' (looked for %s)", module, path);
        goto report;
    }

    int ntok = 0;
    Token *toks = wind_lex(src->text, &ntok);

    Program sub;
    int ok = wind_parse(toks, ntok, &sub, diags, src);
    wind_tokens_free(toks, ntok);
    if (!ok || !load_imports_of(prog, &sub, dir, src, diags)) {
        ast_free_program(&sub);
        return 0;
    }

    for (int i = 0; i < sub.nc_imports; i++) {
        const CImport *im = &sub.c_imports[i];
        if (ast_add_c_import(prog, im->name, im->source, im->range)) continue;
        snprintf(error, sizeof error, "too many C imports in module '%s'", module);
        ast_free_program(&sub);
        goto report;
    }

    char *fnames[128];
    int   nfn   = 0;

    for (int i = 0; i < sub.body.n; i++) {
        Stmt *s = sub.body.items[i];
        if (!s) continue;

        if (s->kind != ST_FUNC) continue;

        if (find_func(prog, s->as.func.name)) {
            snprintf(error, sizeof error,
                     "name conflict: '%s' from module '%s' is already defined",
                     s->as.func.name, module);
            ast_free_program(&sub);
            goto report;
        }

        if (nfn < 128) fnames[nfn++] = s->as.func.name;
        block_push(&prog->body, s);    /* владение переходит к prog */
        sub.body.items[i] = NULL;
    }

    for (int i = 0; i < sub.nlinks && prog->nlinks < WIND_MAX_LINKS; i++) {
        prog->links[prog->nlinks++] = sub.links[i];
        sub.links[i] = NULL;
    }
    ast_free_program(&sub);

    for (int i = 0; i < prog->nimports; i++) {
        Import *im = &prog->imports[i];
        if (im->names == NULL) continue;
        if (strcmp(im->module, module) != 0) continue;
        for (int j = 0; j < im->nnames; j++) {
            if (!name_listed(fnames, nfn, im->names[j])) {
                snprintf(error, sizeof error,
                         "module '%s' has no function '%s'", module, im->names[j]);
                goto report;
            }
        }
    }

    return 1;

report:
    wind_diag_emit(diags, &(WindDiagnostic){
        .code = WD_MODULE, .source = from, .primary = import->range, .message = error
    });
    return 0;
}

int wind_load_modules(Program *prog, const WindSource *source, WindDiagnostics *diags) {
    char dir[PATH_MAX_W];
    dir_of(source->path, dir, sizeof dir);

    forget_all();

    int ok = load_imports_of(prog, prog, dir, source, diags);

    forget_all();
    return ok;
}
