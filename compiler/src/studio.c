/* ========================================================================
   sprfst studio — the language service behind SPRFST Studio

   A line protocol, one request per line, one JSON object per reply.  The
   Mac app keeps a single process alive and talks to it for everything
   that has to feel instant.

     check <file>                 diagnostics
     outline <file>               document symbols
     tokens <file>                semantic highlighting
     complete <file> <offset>     completion list
     hover <file> <offset>        type and documentation
     define <file> <offset>       jump to definition
     rename <file> <offset>       every occurrence of a name
     format <file>                formatted text
     natives                      the whole standard library
     version
     bye
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include "sprfst/parser.h"
#include "sprfst/lexer.h"
#include "sprfst/natives.h"
#include <ctype.h>

char *sprfst_format_source(const char *src, bool *changed);

static void jstr(StrBuf *b, const char *s) {
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"': sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n"); break;
            case '\r': sb_puts(b, "\\r"); break;
            case '\t': sb_puts(b, "\\t"); break;
            default: if (*p < 0x20) sb_printf(b, "\\u%04x", *p); else sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

static void reply(StrBuf *b) {
    fputs(b->data ? b->data : "{}", stdout);
    fputc('\n', stdout);
    fflush(stdout);
    sb_free(b);
}

static void reply_error(const char *msg) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, "{\"ok\":false,\"error\":");
    jstr(&b, msg);
    sb_putc(&b, '}');
    reply(&b);
}

/* ---------------------------------------------------------------- check */
static void svc_check(const char *file) {
    Build b;
    build_init(&b, 0);
    b.json_diags = true;
    build_load(&b, file);
    build_check(&b);
    char *json = diagbag_to_json(b.db);
    StrBuf out; sb_init(&out);
    sb_printf(&out, "{\"ok\":true,\"files\":%d,\"parse_ms\":%.2f,\"check_ms\":%.2f,\"diagnostics\":%s}",
              b.nfiles, b.t_parse * 1e3, b.t_check * 1e3, json ? json : "[]");
    free(json);
    reply(&out);
    build_dispose(&b);
}

/* -------------------------------------------------------------- outline */
static void outline_fn(StrBuf *b, SourceMap *sm, const char *kind, const char *name, Span sp, const char *detail, int *n) {
    int line = 1, col = 1;
    sourcemap_pos(sm, sp, &line, &col);
    if ((*n)++) sb_putc(b, ',');
    sb_puts(b, "{\"kind\":");
    jstr(b, kind);
    sb_puts(b, ",\"name\":");
    jstr(b, name ? name : "?");
    sb_printf(b, ",\"line\":%d,\"col\":%d,\"start\":%d,\"end\":%d,\"detail\":", line, col, sp.start, sp.end);
    jstr(b, detail ? detail : "");
    sb_putc(b, '}');
}

static void svc_outline(const char *file) {
    Build b;
    build_init(&b, 0);
    build_load(&b, file);
    StrBuf out; sb_init(&out);
    sb_puts(&out, "{\"ok\":true,\"symbols\":[");
    int n = 0;
    vec_foreach(mi, &b.sema->modules) {
        Module *m = b.sema->modules.items[mi];
        if (m->path && strcmp(path_basename(m->path), path_basename(file)) != 0) continue;
        vec_foreach(i, &m->decls) {
            Decl *d = m->decls.items[i];
            switch (d->kind) {
                case D_FN: {
                    FnDecl *fn = d->as.fn;
                    StrBuf sig; sb_init(&sig);
                    sb_printf(&sig, "(");
                    int shown = 0;
                    vec_foreach(k, &fn->params) {
                        if (shown++) sb_puts(&sig, ", ");
                        if (fn->params.items[k].is_self) { sb_puts(&sig, "self"); continue; }
                        sb_printf(&sig, "%s: %s", fn->params.items[k].name,
                                  fn->params.items[k].type ? typeexpr_to_text(&b.arena, fn->params.items[k].type) : "Any");
                    }
                    sb_printf(&sig, ") -> %s", fn->ret ? typeexpr_to_text(&b.arena, fn->ret) : "Nil");
                    outline_fn(&out, b.sm, fn->is_test ? "test" : fn->is_bench ? "bench" : fn->is_task ? "task" : "fn",
                               fn->name, fn->span, sig.data ? sig.data : "", &n);
                    sb_free(&sig);
                    break;
                }
                case D_TYPE: {
                    TypeDecl *td = d->as.type;
                    const char *k = td->kind == TD_OBJECT ? "object" : td->kind == TD_DATA ? "data" :
                                    td->kind == TD_ENUM ? "enum" : td->kind == TD_TRAIT ? "trait" : "type";
                    outline_fn(&out, b.sm, k, td->name, td->span, td->doc, &n);
                    vec_foreach(mk, &td->methods)
                        outline_fn(&out, b.sm, "method", td->methods.items[mk]->name, td->methods.items[mk]->span, td->name, &n);
                    break;
                }
                case D_CONST: case D_LET:
                    outline_fn(&out, b.sm, d->kind == D_CONST ? "const" : "var", d->as.konst.name, d->span, "", &n);
                    break;
                case D_APP:
                    outline_fn(&out, b.sm, "app", d->as.app.name, d->span, "", &n);
                    break;
                case D_IMPL:
                    vec_foreach(mk, &d->as.impl->methods)
                        outline_fn(&out, b.sm, "method", d->as.impl->methods.items[mk]->name,
                                   d->as.impl->methods.items[mk]->span, d->as.impl->trait_name, &n);
                    break;
                default: break;
            }
        }
    }
    sb_puts(&out, "]}");
    reply(&out);
    build_dispose(&b);
}

/* --------------------------------------------------------------- tokens */
static const char *token_class(TokKind k) {
    if (tok_is_keyword(k)) return "keyword";
    switch (k) {
        case T_INT: case T_NUM: return "number";
        case T_TEXT: return "text";
        case T_DOC: return "doc";
        case T_IDENT: return "name";
        case T_AT: return "attr";
        default: return "punct";
    }
}

static void svc_tokens(const char *file) {
    Arena a;
    arena_init(&a);
    Interner *in = interner_new();
    SourceMap *sm = sourcemap_new(&a);
    DiagBag *db = diagbag_new(&a, sm);
    SourceFile *f = sourcemap_load(sm, file);
    if (!f) { reply_error("cannot read file"); interner_free(in); arena_free(&a); return; }
    int n = 0;
    Token *toks = lex_file(&a, in, db, f, &n);
    StrBuf out; sb_init(&out);
    sb_puts(&out, "{\"ok\":true,\"tokens\":[");
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (toks[i].kind == T_EOF || toks[i].kind == T_NEWLINE) continue;
        int line = 1, col = 1;
        sourcemap_pos(sm, toks[i].span, &line, &col);
        if (shown++) sb_putc(&out, ',');
        sb_printf(&out, "{\"line\":%d,\"col\":%d,\"len\":%d,\"kind\":\"%s\"}",
                  line, col, toks[i].span.end - toks[i].span.start, token_class(toks[i].kind));
    }
    sb_puts(&out, "]}");
    reply(&out);
    interner_free(in);
    arena_free(&a);
}

/* ----------------------------------------------------------- completion */
static void svc_complete(const char *file, int offset) {
    Build b;
    build_init(&b, 0);
    build_load(&b, file);
    build_check(&b);
    CompletionVec items;
    memset(&items, 0, sizeof items);
    Module *target = NULL;
    vec_foreach(i, &b.sema->modules) {
        Module *m = b.sema->modules.items[i];
        if (m->path && strcmp(path_basename(m->path), path_basename(file)) == 0) target = m;
    }
    sema_collect_completions(b.sema, target, offset, &items);

    StrBuf out; sb_init(&out);
    sb_puts(&out, "{\"ok\":true,\"items\":[");
    for (int i = 0; i < items.len; i++) {
        if (i) sb_putc(&out, ',');
        sb_puts(&out, "{\"label\":");
        jstr(&out, items.items[i].label);
        sb_puts(&out, ",\"detail\":");
        jstr(&out, items.items[i].detail);
        sb_puts(&out, ",\"doc\":");
        jstr(&out, items.items[i].doc);
        sb_puts(&out, ",\"kind\":");
        jstr(&out, items.items[i].kind);
        sb_putc(&out, '}');
    }
    sb_puts(&out, "]}");
    reply(&out);
    build_dispose(&b);
}

/* --------------------------------------------- word under a byte offset */
static bool word_at(const char *src, size_t len, int offset, char *out, size_t cap, int *start, int *end) {
    if (offset < 0 || (size_t)offset > len) return false;
    size_t i = (size_t)offset;
    if (i == len || (!isalnum((unsigned char)src[i]) && src[i] != '_')) { if (i) i--; }
    if (!isalnum((unsigned char)src[i]) && src[i] != '_') return false;
    size_t s = i, e = i;
    while (s > 0 && (isalnum((unsigned char)src[s-1]) || src[s-1] == '_')) s--;
    while (e + 1 < len && (isalnum((unsigned char)src[e+1]) || src[e+1] == '_')) e++;
    size_t n = e - s + 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, src + s, n);
    out[n] = 0;
    *start = (int)s;
    *end = (int)(e + 1);
    return true;
}

static void svc_hover(const char *file, int offset, bool want_definition) {
    Build b;
    build_init(&b, 0);
    build_load(&b, file);
    build_check(&b);

    SourceFile *sf = NULL;
    vec_foreach(i, &b.sema->modules) {
        Module *m = b.sema->modules.items[i];
        if (m->path && strcmp(path_basename(m->path), path_basename(file)) == 0)
            sf = sourcemap_get(b.sm, m->file_id);
    }
    if (!sf) { reply_error("file not loaded"); build_dispose(&b); return; }

    char word[160];
    int ws = 0, we = 0;
    if (!word_at(sf->src, sf->len, offset, word, sizeof word, &ws, &we)) {
        reply_error("nothing under the cursor");
        build_dispose(&b);
        return;
    }

    StrBuf out; sb_init(&out);
    /* a native function? */
    int nmods = 0;
    const char **mods = natives_module_names(&nmods);
    for (int k = -1; k < nmods; k++) {
        int id = natives_lookup(k < 0 ? "" : mods[k], word);
        if (id < 0) continue;
        const NativeFn *nf = &SPRFST_NATIVES[id];
        sb_puts(&out, "{\"ok\":true,\"kind\":\"native\",\"name\":");
        jstr(&out, nf->name);
        sb_puts(&out, ",\"signature\":");
        jstr(&out, nf->sig);
        sb_puts(&out, ",\"doc\":");
        jstr(&out, nf->doc);
        sb_puts(&out, ",\"module\":");
        jstr(&out, nf->module);
        sb_putc(&out, '}');
        reply(&out);
        build_dispose(&b);
        return;
    }

    /* a user symbol */
    Symbol *sym = sema_lookup(b.sema, intern(b.in, str_cstr(word)));
    if (!sym) {
        vec_foreach(i, &b.sema->all_fns)
            if (b.sema->all_fns.items[i]->name && strcmp(b.sema->all_fns.items[i]->name, word) == 0) {
                FnDecl *fn = b.sema->all_fns.items[i];
                int line = 1, col = 1;
                sourcemap_pos(b.sm, fn->span, &line, &col);
                sb_printf(&out, "{\"ok\":true,\"kind\":\"fn\",\"name\":");
                jstr(&out, fn->name);
                sb_printf(&out, ",\"line\":%d,\"col\":%d,\"start\":%d,\"end\":%d,\"doc\":",
                          line, col, fn->span.start, fn->span.end);
                jstr(&out, fn->doc ? fn->doc : "");
                sb_putc(&out, '}');
                reply(&out);
                build_dispose(&b);
                return;
            }
        reply_error("unknown name");
        build_dispose(&b);
        return;
    }
    int line = 1, col = 1;
    sourcemap_pos(b.sm, sym->span, &line, &col);
    const char *kind = sym->kind == SYM_FN ? "fn" : sym->kind == SYM_TYPE ? "type" :
                       sym->kind == SYM_CONST ? "const" : sym->kind == SYM_GLOBAL ? "var" :
                       sym->kind == SYM_VARIANT ? "variant" : "value";
    sb_puts(&out, "{\"ok\":true,\"kind\":");
    jstr(&out, kind);
    sb_puts(&out, ",\"name\":");
    jstr(&out, sym->name);
    sb_puts(&out, ",\"type\":");
    jstr(&out, sym->type ? type_text(&b.arena, sym->type) : "?");
    sb_printf(&out, ",\"line\":%d,\"col\":%d,\"start\":%d,\"end\":%d",
              line, col, sym->span.start, sym->span.end);
    if (want_definition) sb_printf(&out, ",\"file\":"), jstr(&out, file);
    sb_putc(&out, '}');
    reply(&out);
    build_dispose(&b);
}

/* --------------------------------------------------------------- rename */
static void svc_rename(const char *file, int offset) {
    size_t len = 0;
    char *src = read_file(file, &len);
    if (!src) { reply_error("cannot read file"); return; }
    char word[160];
    int ws = 0, we = 0;
    if (!word_at(src, len, offset, word, sizeof word, &ws, &we)) {
        reply_error("nothing under the cursor");
        free(src);
        return;
    }
    size_t wl = strlen(word);
    StrBuf out; sb_init(&out);
    sb_puts(&out, "{\"ok\":true,\"name\":");
    jstr(&out, word);
    sb_puts(&out, ",\"occurrences\":[");
    int n = 0;
    for (size_t i = 0; i + wl <= len; i++) {
        if (strncmp(src + i, word, wl) != 0) continue;
        if (i && (isalnum((unsigned char)src[i-1]) || src[i-1] == '_')) continue;
        if (i + wl < len && (isalnum((unsigned char)src[i+wl]) || src[i+wl] == '_')) continue;
        if (n++) sb_putc(&out, ',');
        sb_printf(&out, "{\"start\":%zu,\"end\":%zu}", i, i + wl);
    }
    sb_printf(&out, "],\"count\":%d}", n);
    reply(&out);
    free(src);
}

/* --------------------------------------------------------------- format */
static void svc_format(const char *file) {
    size_t len = 0;
    char *src = read_file(file, &len);
    if (!src) { reply_error("cannot read file"); return; }
    bool changed = false;
    char *out = sprfst_format_source(src, &changed);
    StrBuf b; sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"changed\":%s,\"text\":", changed ? "true" : "false");
    jstr(&b, out);
    sb_putc(&b, '}');
    reply(&b);
    free(src);
    free(out);
}

static void svc_natives(void) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, "{\"ok\":true,\"natives\":[");
    for (int i = 0; i < NF_COUNT; i++) {
        if (i) sb_putc(&b, ',');
        sb_puts(&b, "{\"module\":");
        jstr(&b, SPRFST_NATIVES[i].module);
        sb_puts(&b, ",\"name\":");
        jstr(&b, SPRFST_NATIVES[i].name);
        sb_puts(&b, ",\"sig\":");
        jstr(&b, SPRFST_NATIVES[i].sig);
        sb_puts(&b, ",\"doc\":");
        jstr(&b, SPRFST_NATIVES[i].doc);
        sb_putc(&b, '}');
    }
    sb_puts(&b, "]}");
    reply(&b);
}

/* ---------------------------------------------------------------- entry */
int cmd_studio(int argc, char **argv) {
    (void)argc; (void)argv;
    g_color = false;                       /* the host wants clean JSON */
    setvbuf(stdout, NULL, _IOLBF, 0);

    StrBuf hello; sb_init(&hello);
    sb_printf(&hello, "{\"ok\":true,\"service\":\"sprfst-studio\",\"version\":\"%s\",\"codename\":\"%s\",\"natives\":%d}",
              SPRFST_VERSION, SPRFST_CODENAME, NF_COUNT);
    reply(&hello);

    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && isspace((unsigned char)line[n-1])) line[--n] = 0;
        if (!n) continue;

        char verb[32] = { 0 }, arg1[2048] = { 0 };
        int offset = -1;
        int got = sscanf(line, "%31s %2047s %d", verb, arg1, &offset);
        if (got < 1) continue;

        if (strcmp(verb, "bye") == 0 || strcmp(verb, "quit") == 0) break;
        else if (strcmp(verb, "version") == 0) {
            StrBuf b; sb_init(&b);
            sb_printf(&b, "{\"ok\":true,\"version\":\"%s\",\"codename\":\"%s\"}", SPRFST_VERSION, SPRFST_CODENAME);
            reply(&b);
        }
        else if (strcmp(verb, "natives") == 0) svc_natives();
        else if (got < 2) reply_error("this request needs a file");
        else if (strcmp(verb, "check") == 0) svc_check(arg1);
        else if (strcmp(verb, "outline") == 0) svc_outline(arg1);
        else if (strcmp(verb, "tokens") == 0) svc_tokens(arg1);
        else if (strcmp(verb, "format") == 0) svc_format(arg1);
        else if (strcmp(verb, "complete") == 0) svc_complete(arg1, offset < 0 ? 0 : offset);
        else if (strcmp(verb, "hover") == 0) svc_hover(arg1, offset < 0 ? 0 : offset, false);
        else if (strcmp(verb, "define") == 0) svc_hover(arg1, offset < 0 ? 0 : offset, true);
        else if (strcmp(verb, "rename") == 0) svc_rename(arg1, offset < 0 ? 0 : offset);
        else reply_error("unknown request");
    }
    return 0;
}
