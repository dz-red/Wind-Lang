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
    X(getFunctionTypeCallingConv) X(getClangVersion) X(getTypeDeclaration) \
    X(getCursorDefinition) X(getCursorSemanticParent) X(equalCursors) \
    X(Cursor_isBitField) X(Cursor_getOffsetOfField) X(Type_getSizeOf) X(Type_getAlignOf) X(getTypeSpelling)

static struct {
#define MEMBER(name) __typeof__(&clang_##name) name;
    CLANG_API(MEMBER)
#undef MEMBER
} api;

typedef struct ParsedRecord {
    WindCRecord value;
    CXCursor definition;
    struct ParsedRecord *next;
} ParsedRecord;

struct WindCHeader {
    CXIndex index;
    CXTranslationUnit unit;
    ParsedRecord *records;
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
    while (header->records) {
        ParsedRecord *record = header->records;
        header->records = record->next;
        free(record);
    }
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
            int is_const = api.isConstQualifiedType(pointee);
            if (pointee.kind == CXType_Void) return is_const ? TK_C_CONST_VOID_BUFFER : TK_C_VOID_BUFFER;
            if (pointee.kind == CXType_UChar) return is_const ? TK_C_CONST_UCHAR_BUFFER : TK_C_UCHAR_BUFFER;
            if (pointee.kind == CXType_Int)
                return is_const ? TK_C_CONST_INT_BUFFER : TK_C_INT_BUFFER;
            if (pointee.kind == CXType_Double)
                return is_const ? TK_C_CONST_FRAC_BUFFER : TK_C_FRAC_BUFFER;
            if (pointee.kind == CXType_FunctionProto) return TK_C_CALLBACK;
            if ((pointee.kind == CXType_Char_S || pointee.kind == CXType_Char_U) && is_const)
                return TK_KW_STR;
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

static int copy_identifier(CXCursor cursor, char *out, size_t cap) {
    CXString spelling = api.getCursorSpelling(cursor);
    const char *name = api.getCString(spelling);
    const char *letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_";
    int valid = name && *name && strlen(name) < cap && strchr(letters, *name) &&
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") == strlen(name);
    if (valid) strcpy(out, name);
    api.disposeString(spelling);
    return valid;
}

typedef struct {
    WindCRecord *record;
    int invalid;
} Fields;

static enum CXChildVisitResult visit_field(CXCursor cursor, CXCursor parent, CXClientData data) {
    (void)parent;
    Fields *fields = data;
    if (cursor.kind == CXCursor_StructDecl || cursor.kind == CXCursor_UnionDecl) {
        fields->invalid = 1;
        return CXChildVisit_Break;
    }
    if (cursor.kind != CXCursor_FieldDecl) return CXChildVisit_Continue;
    CXType type = api.getCanonicalType(api.getCursorType(cursor));
    TokenKind kind = wind_type(type);
    long long offset = api.Cursor_getOffsetOfField(cursor);
    WindCRecord *record = fields->record;
    if (record->nfields < WIND_C_RECORD_FIELDS && (kind == TK_KW_INT || kind == TK_KW_FRAC) &&
        !api.isConstQualifiedType(type) && !api.isVolatileQualifiedType(type) &&
        !api.Cursor_isBitField(cursor) && offset >= 0) {
        WindCField *field = &record->fields[record->nfields];
        if (copy_identifier(cursor, field->name, sizeof field->name)) {
            field->type = kind;
            field->offset = offset;
            record->nfields++;
            return CXChildVisit_Continue;
        }
    }
    fields->invalid = 1;
    return CXChildVisit_Break;
}

static const WindCRecord *record_type(WindCHeader *header, CXType type) {
    CXType original = type;
    type = api.getCanonicalType(type);
    if (type.kind != CXType_Record) return NULL;
    CXCursor declaration = api.getTypeDeclaration(type);
    if (declaration.kind != CXCursor_StructDecl) return NULL;
    CXCursor definition = api.getCursorDefinition(declaration);
    if (definition.kind != CXCursor_StructDecl) return NULL;
    CXCursor root = api.getTranslationUnitCursor(header->unit);
    if (!api.equalCursors(api.getCursorSemanticParent(definition), root)) return NULL;
    for (ParsedRecord *record = header->records; record; record = record->next)
        if (api.equalCursors(record->definition, definition)) return &record->value;

    char tag[WIND_C_RECORD_NAME], c_name[sizeof "struct " + WIND_C_RECORD_NAME];
    CXString spelling = api.getTypeSpelling(type);
    const char *text = api.getCString(spelling);
    int has_tag = text && (!strncmp(text, "struct ", 7) || !strncmp(text, "const struct ", 13) ||
                          !strncmp(text, "volatile struct ", 16));
    api.disposeString(spelling);
    if (has_tag && copy_identifier(declaration, tag, sizeof tag)) {
        snprintf(c_name, sizeof c_name, "struct %s", tag);
    } else {
        CXCursor alias = api.getTypeDeclaration(original);
        if (alias.kind != CXCursor_TypedefDecl ||
            !api.equalCursors(api.getCursorSemanticParent(alias), root) ||
            !copy_identifier(alias, c_name, WIND_C_RECORD_NAME)) return NULL;
    }
    ParsedRecord *parsed = calloc(1, sizeof *parsed);
    if (!parsed) return NULL;
    parsed->definition = definition;
    WindCRecord *record = &parsed->value;
    strcpy(record->c_type, c_name);
    record->size = api.Type_getSizeOf(type);
    record->alignment = api.Type_getAlignOf(type);
    Fields fields = {record, 0};
    api.visitChildren(definition, visit_field, &fields);
    if (fields.invalid || !record->nfields || record->size <= 0 || record->alignment <= 0) {
        free(parsed);
        return NULL;
    }
    parsed->next = header->records;
    header->records = parsed;
    return record;
}

typedef struct {
    WindCHeader *header;
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
    CXType original = api.getCursorType(cursor);
    CXType type = api.getCanonicalType(original);
    if (type.kind != CXType_FunctionProto || api.isFunctionTypeVariadic(type))
        return CXChildVisit_Break;
    enum CXCallingConv convention = api.getFunctionTypeCallingConv(type);
    if (convention != CXCallingConv_C && convention != CXCallingConv_Default)
        return CXChildVisit_Break;
    WindCFunction *f = lookup->out;
    f->nargs = api.getNumArgTypes(type);
    if (f->nargs < 0 || f->nargs > WIND_C_MAX_PARAMS) return CXChildVisit_Break;
    CXType result = api.getResultType(original);
    f->result_record = record_type(lookup->header, result);
    f->result = f->result_record ? TK_C_RECORD : wind_type(result);
    if (f->result == TK_BAD || f->result == TK_C_CALLBACK ||
        wind_c_buffer_element(f->result) != TK_BAD || wind_c_byte_buffer(f->result))
        return CXChildVisit_Break;
    for (int i = 0; i < f->nargs; i++) {
        CXType raw = api.getArgType(original, (unsigned)i);
        CXType arg = api.getCanonicalType(raw);
        f->arg_records[i] = record_type(lookup->header, raw);
        f->args[i] = f->arg_records[i] ? TK_C_RECORD : wind_type(arg);
        if (f->args[i] == TK_BAD && arg.kind == _Generic((size_t)0,
                unsigned short: CXType_UShort, unsigned int: CXType_UInt,
                unsigned long: CXType_ULong, unsigned long long: CXType_ULongLong,
                default: CXType_Invalid))
            f->args[i] = TK_C_SIZE;
        if (f->args[i] == TK_BAD || f->args[i] == TK_KW_VOID) return CXChildVisit_Break;
        if (f->args[i] == TK_C_CALLBACK &&
            !scalar_signature(api.getCanonicalType(api.getPointeeType(arg)), &f->callbacks[i]))
            return CXChildVisit_Break;
    }
    lookup->status = C_HEADER_OK;
    return CXChildVisit_Break;
}

WindCHeaderStatus wind_cheader_function(WindCHeader *header, const char *name, WindCFunction *out) {
    Lookup lookup = {header, name, out, C_HEADER_NO_FUNCTION};
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
