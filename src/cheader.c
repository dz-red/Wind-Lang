#include <stdio.h>

#include "cheader.h"

#if defined(__linux__) && __has_include(<clang-c/Index.h>) && !defined(WIND_NO_CLANG)
#include <clang-c/Index.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLANG_API(X) \
    X(createIndex) X(disposeIndex) X(parseTranslationUnit2) X(disposeTranslationUnit) \
    X(getNumDiagnostics) X(getDiagnostic) X(getDiagnosticSeverity) X(getDiagnosticSpelling) \
    X(disposeDiagnostic) X(getCString) X(disposeString) X(getTranslationUnitCursor) \
    X(visitChildren) X(getCursorSpelling) X(getCursorType) X(getCanonicalType) \
    X(getResultType) X(getNumArgTypes) X(getArgType) X(getPointeeType) \
    X(isConstQualifiedType) X(isVolatileQualifiedType) X(isFunctionTypeVariadic) \
    X(getFunctionTypeCallingConv) X(getClangVersion)

static struct {
#define MEMBER(name) __typeof__(&clang_##name) name;
    CLANG_API(MEMBER)
#undef MEMBER
} api;

struct WindCHeader {
    CXIndex index;
    CXTranslationUnit unit;
};

static int load_clang(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    void *lib = dlopen("libclang.so", RTLD_NOW | RTLD_LOCAL);
    for (int version = 32; !lib && version >= 14; version--) {
        char name[32];
        snprintf(name, sizeof name, "libclang-%d.so.1", version);
        lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            snprintf(name, sizeof name, "libclang.so.%d", version);
            lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        }
    }
    if (!lib) return 0;
#define LOAD(name) \
    api.name = (__typeof__(api.name))dlsym(lib, "clang_" #name); \
    if (!api.name) { dlclose(lib); return 0; }
    CLANG_API(LOAD)
#undef LOAD
    state = 1;
    return 1;
}

void wind_cheader_free(WindCHeader *header) {
    if (!header) return;
    if (header->unit) api.disposeTranslationUnit(header->unit);
    if (header->index) api.disposeIndex(header->index);
    free(header);
}

static int resource_dir(char *out, size_t cap) {
    const char *configured = getenv("WIND_CLANG_RESOURCE_DIR");
    if (configured && *configured && strlen(configured) < cap) {
        strcpy(out, configured);
        return 1;
    }
    CXString version = api.getClangVersion();
    const char *text = api.getCString(version);
    const char *digits = text ? strstr(text, "version ") : NULL;
    unsigned major = 0, minor = 0, patch = 0;
    if (digits) sscanf(digits + strlen("version "), "%u.%u.%u", &major, &minor, &patch);
    api.disposeString(version);
    if (!major) return 0;
    const char *formats[] = {
        "/usr/lib/clang/%u", "/usr/lib64/clang/%u", "/usr/local/lib/clang/%u",
        "/usr/lib/llvm-%u/lib/clang", "/usr/lib64/llvm%u/lib/clang"
    };
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) {
        char parent[128];
        snprintf(parent, sizeof parent, formats[i], major);
        char path[512];
        snprintf(path, sizeof path, "%s/include/stdarg.h", parent);
        if (access(path, R_OK) == 0) { snprintf(out, cap, "%s", parent); return 1; }
        for (int full = 0; full <= 1; full++) {
            char candidate[256];
            if (full) snprintf(candidate, sizeof candidate, "%s/%u.%u.%u", parent, major, minor, patch);
            else snprintf(candidate, sizeof candidate, "%s/%u", parent, major);
            snprintf(path, sizeof path, "%s/include/stdarg.h", candidate);
            if (access(path, R_OK) == 0) { snprintf(out, cap, "%s", candidate); return 1; }
        }
    }
    return 0;
}

WindCHeaderStatus wind_cheader_open(const char *header, WindCHeader **out, char *detail, size_t cap) {
    if (!load_clang()) return C_HEADER_UNAVAILABLE;
    WindCHeader *parsed = calloc(1, sizeof *parsed);
    if (!parsed) return C_HEADER_INVALID;
    parsed->index = api.createIndex(1, 0);
    char text[272];
    int len = snprintf(text, sizeof text, "#include <%s>\n", header);
    char resources[512];
    const char *args[] = {"-x", "c", "-std=gnu11", "-resource-dir", resources};
    int nargs = resource_dir(resources, sizeof resources) ? 5 : 3;
    struct CXUnsavedFile source = {"wind-ffi-header.c", text, (unsigned long)len};
    if (!parsed->index || api.parseTranslationUnit2(parsed->index, source.Filename, args, nargs,
        &source, 1, CXTranslationUnit_SkipFunctionBodies, &parsed->unit) != CXError_Success) {
        wind_cheader_free(parsed);
        return C_HEADER_INVALID;
    }
    unsigned count = api.getNumDiagnostics(parsed->unit);
    for (unsigned i = 0; i < count; i++) {
        CXDiagnostic diagnostic = api.getDiagnostic(parsed->unit, i);
        if (api.getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
            CXString spelling = api.getDiagnosticSpelling(diagnostic);
            const char *message = api.getCString(spelling);
            char missing[280];
            snprintf(missing, sizeof missing, "'%s' file not found", header);
            WindCHeaderStatus status = message && !strcmp(message, missing) ?
                C_HEADER_NOT_FOUND : C_HEADER_INVALID;
            if (message) snprintf(detail, cap, "%s", message);
            api.disposeString(spelling);
            api.disposeDiagnostic(diagnostic);
            wind_cheader_free(parsed);
            return status;
        }
        api.disposeDiagnostic(diagnostic);
    }
    *out = parsed;
    return C_HEADER_OK;
}

static TokenKind wind_type(CXType type) {
    type = api.getCanonicalType(type);
    switch (type.kind) {
        case CXType_Int: return TK_KW_INT;
        case CXType_Double: return TK_KW_FRAC;
        case CXType_Void: return TK_KW_VOID;
        case CXType_Pointer: {
            CXType pointee = api.getCanonicalType(api.getPointeeType(type));
            if (api.isVolatileQualifiedType(pointee)) break;
            if (pointee.kind == CXType_Int)
                return api.isConstQualifiedType(pointee) ? TK_C_CONST_INT_BUFFER : TK_C_INT_BUFFER;
            if (pointee.kind == CXType_Double)
                return api.isConstQualifiedType(pointee) ? TK_C_CONST_FRAC_BUFFER : TK_C_FRAC_BUFFER;
            if (pointee.kind == CXType_FunctionProto) return TK_C_CALLBACK;
            if ((pointee.kind == CXType_Char_S || pointee.kind == CXType_Char_U) &&
                api.isConstQualifiedType(pointee) && !api.isVolatileQualifiedType(pointee)) return TK_KW_STR;
            break;
        }
        default: break;
    }
    return TK_BAD;
}

static int scalar_signature(CXType type, WindCCallback *signature) {
    if (type.kind != CXType_FunctionProto || api.isFunctionTypeVariadic(type)) return 0;
    enum CXCallingConv convention = api.getFunctionTypeCallingConv(type);
    if (convention != CXCallingConv_C && convention != CXCallingConv_Default) return 0;
    signature->nargs = api.getNumArgTypes(type);
    if (signature->nargs < 0 || signature->nargs > WIND_C_MAX_PARAMS) return 0;
    signature->result = wind_type(api.getResultType(type));
    if (signature->result != TK_KW_INT && signature->result != TK_KW_FRAC &&
        signature->result != TK_KW_VOID) return 0;
    for (int i = 0; i < signature->nargs; i++) {
        signature->args[i] = wind_type(api.getArgType(type, (unsigned)i));
        if (signature->args[i] != TK_KW_INT && signature->args[i] != TK_KW_FRAC) return 0;
    }
    return 1;
}

typedef struct {
    const char *name;
    WindCFunction *out;
    WindCHeaderStatus status;
} Lookup;

static enum CXChildVisitResult visit(CXCursor cursor, CXCursor parent, CXClientData data) {
    (void)parent;
    Lookup *lookup = data;
    if (cursor.kind != CXCursor_FunctionDecl) return CXChildVisit_Continue;
    CXString spelling = api.getCursorSpelling(cursor);
    const char *name = api.getCString(spelling);
    int match = name && !strcmp(name, lookup->name);
    api.disposeString(spelling);
    if (!match) return CXChildVisit_Continue;

    lookup->status = C_HEADER_UNSUPPORTED;
    CXType type = api.getCanonicalType(api.getCursorType(cursor));
    if (type.kind != CXType_FunctionProto || api.isFunctionTypeVariadic(type))
        return CXChildVisit_Break;
    enum CXCallingConv convention = api.getFunctionTypeCallingConv(type);
    if (convention != CXCallingConv_C && convention != CXCallingConv_Default)
        return CXChildVisit_Break;
    WindCFunction *f = lookup->out;
    f->nargs = api.getNumArgTypes(type);
    if (f->nargs < 0 || f->nargs > WIND_C_MAX_PARAMS) return CXChildVisit_Break;
    f->result = wind_type(api.getResultType(type));
    if (f->result == TK_BAD || f->result == TK_C_CALLBACK ||
        wind_c_buffer_element(f->result) != TK_BAD) return CXChildVisit_Break;
    for (int i = 0; i < f->nargs; i++) {
        CXType arg = api.getCanonicalType(api.getArgType(type, (unsigned)i));
        f->args[i] = wind_type(arg);
        if (f->args[i] == TK_BAD || f->args[i] == TK_KW_VOID) return CXChildVisit_Break;
        if (f->args[i] == TK_C_CALLBACK &&
            !scalar_signature(api.getCanonicalType(api.getPointeeType(arg)), &f->callbacks[i]))
            return CXChildVisit_Break;
    }
    lookup->status = C_HEADER_OK;
    return CXChildVisit_Break;
}

WindCHeaderStatus wind_cheader_function(WindCHeader *header, const char *name, WindCFunction *out) {
    Lookup lookup = {name, out, C_HEADER_NO_FUNCTION};
    api.visitChildren(api.getTranslationUnitCursor(header->unit), visit, &lookup);
    return lookup.status;
}
#else
WindCHeaderStatus wind_cheader_open(const char *header, WindCHeader **out, char *detail, size_t cap) {
    (void)header; (void)out;
    snprintf(detail, cap, "This Wind build does not include header discovery.");
    return C_HEADER_UNAVAILABLE;
}

WindCHeaderStatus wind_cheader_function(WindCHeader *header, const char *name, WindCFunction *out) {
    (void)header; (void)name; (void)out;
    return C_HEADER_UNAVAILABLE;
}

void wind_cheader_free(WindCHeader *header) { (void)header; }
#endif
