#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "sprfst/common.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

const Span SPAN_NONE = { -1, 0, 0 };
bool g_color = false;

/* ---------------------------------------------------------------- arena */
#define ARENA_BLOCK (1u << 20)

void arena_init(Arena *a) { a->head = NULL; a->total = 0; }

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
        ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
        if (!b) { fprintf(stderr, "sprfst: out of memory\n"); exit(70); }
        b->next = a->head; b->used = 0; b->cap = cap;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    a->total += n;
    return p;
}

void *arena_zalloc(Arena *a, size_t n) {
    void *p = arena_alloc(a, n);
    memset(p, 0, n);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *arena_strdup(Arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

char *arena_vsprintf(Arena *a, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) { ArenaBlock *n = b->next; free(b); b = n; }
    a->head = NULL; a->total = 0;
}

char *str_dup(Arena *a, Str s) { return arena_strndup(a, s.p, s.n); }

/* --------------------------------------------------------------- strbuf */
void sb_init(StrBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
void sb_free(StrBuf *b) { free(b->data); sb_init(b); }

static void sb_grow(StrBuf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t c = b->cap ? b->cap : 128;
    while (c < b->len + need + 1) c *= 2;
    b->data = realloc(b->data, c);
    b->cap = c;
}

void sb_putc(StrBuf *b, char c) { sb_grow(b, 1); b->data[b->len++] = c; b->data[b->len] = 0; }
void sb_put(StrBuf *b, const char *s, size_t n) {
    if (!n) return;
    sb_grow(b, n); memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0;
}
void sb_puts(StrBuf *b, const char *s) { if (s) sb_put(b, s, strlen(s)); }
void sb_putstr(StrBuf *b, Str s) { sb_put(b, s.p, s.n); }

void sb_printf(StrBuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_grow(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void sb_indent(StrBuf *b, int depth) { for (int i = 0; i < depth; i++) sb_puts(b, "    "); }

char *sb_take(StrBuf *b) {
    if (!b->data) { b->data = malloc(1); b->data[0] = 0; }
    char *d = b->data;
    sb_init(b);
    return d;
}

/* ------------------------------------------------------------- interner */
typedef struct INode { struct INode *next; size_t n; char s[]; } INode;
struct Interner { INode **buckets; size_t nbuckets, count; };

Interner *interner_new(void) {
    Interner *in = calloc(1, sizeof(Interner));
    in->nbuckets = 4096;
    in->buckets = calloc(in->nbuckets, sizeof(INode *));
    return in;
}

uint64_t hash_bytes(const void *data, size_t n) {
    const unsigned char *p = data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return h;
}

void hash_hex(const void *data, size_t n, char out[65]) {
    /* Four independently-seeded FNV streams -> 256 bit digest (content addressing,
       not a cryptographic hash; used for build caches and package checksums.) */
    const unsigned char *p = data;
    uint64_t h[4] = { 0xcbf29ce484222325ULL, 0x9e3779b97f4a7c15ULL,
                      0x165667b19e3779f9ULL, 0x27d4eb2f165667c5ULL };
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) {
            h[k] ^= (uint64_t)p[i] + (uint64_t)k * 131u;
            h[k] *= 1099511628211ULL;
            h[k] ^= h[k] >> 29;
        }
    }
    for (int k = 0; k < 4; k++) { h[k] ^= h[k] >> 32; h[k] *= 0xd6e8feb86659fd93ULL; h[k] ^= h[k] >> 32; }
    for (int k = 0; k < 4; k++) snprintf(out + k * 16, 17, "%016llx", (unsigned long long)h[k]);
    out[64] = 0;
}

const char *intern(Interner *in, Str s) {
    uint64_t h = hash_bytes(s.p, s.n) & (in->nbuckets - 1);
    for (INode *n = in->buckets[h]; n; n = n->next)
        if (n->n == s.n && memcmp(n->s, s.p, s.n) == 0) return n->s;
    INode *n = malloc(sizeof(INode) + s.n + 1);
    n->n = s.n; memcpy(n->s, s.p, s.n); n->s[s.n] = 0;
    n->next = in->buckets[h]; in->buckets[h] = n; in->count++;
    return n->s;
}

const char *internc(Interner *in, const char *s) { return intern(in, str_cstr(s)); }

void interner_free(Interner *in) {
    if (!in) return;
    for (size_t i = 0; i < in->nbuckets; i++) {
        INode *n = in->buckets[i];
        while (n) { INode *x = n->next; free(n); n = x; }
    }
    free(in->buckets); free(in);
}

/* ----------------------------------------------------------- source map */
SourceMap *sourcemap_new(Arena *a) {
    SourceMap *sm = NEW(a, SourceMap);
    sm->arena = a;
    vec_init(&sm->files);
    return sm;
}

static void index_lines(SourceFile *f) {
    int cap = 64, n = 0;
    int *lines = malloc(sizeof(int) * cap);
    lines[n++] = 0;
    for (size_t i = 0; i < f->len; i++) {
        if (f->src[i] == '\n') {
            if (n + 1 >= cap) { cap *= 2; lines = realloc(lines, sizeof(int) * cap); }
            lines[n++] = (int)i + 1;
        }
    }
    f->lines = lines; f->nlines = n;
}

SourceFile *sourcemap_add(SourceMap *sm, const char *path, char *src, size_t len) {
    SourceFile *f = NEW(sm->arena, SourceFile);
    f->id = sm->files.len;
    f->path = arena_strdup(sm->arena, path);
    f->abspath = path_abs(sm->arena, path);
    f->src = src; f->len = len;
    index_lines(f);
    vec_push(&sm->files, f);
    return f;
}

SourceFile *sourcemap_load(SourceMap *sm, const char *path) {
    size_t len = 0;
    char *src = read_file(path, &len);
    if (!src) return NULL;
    return sourcemap_add(sm, path, src, len);
}

SourceFile *sourcemap_get(SourceMap *sm, int id) {
    if (id < 0 || id >= sm->files.len) return NULL;
    return sm->files.items[id];
}

void sourcemap_pos(SourceMap *sm, Span sp, int *line, int *col) {
    *line = 1; *col = 1;
    SourceFile *f = sourcemap_get(sm, sp.file);
    if (!f) return;
    int lo = 0, hi = f->nlines - 1, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->lines[mid] <= sp.start) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    *line = best + 1;
    *col = sp.start - f->lines[best] + 1;
}

Str sourcemap_line(SourceMap *sm, int file, int line) {
    SourceFile *f = sourcemap_get(sm, file);
    if (!f || line < 1 || line > f->nlines) return str_make("", 0);
    int s = f->lines[line - 1];
    int e = (line < f->nlines) ? f->lines[line] - 1 : (int)f->len;
    while (e > s && (f->src[e - 1] == '\r' || f->src[e - 1] == '\n')) e--;
    return str_make(f->src + s, (size_t)(e - s));
}

/* ---------------------------------------------------------- diagnostics */
DiagBag *diagbag_new(Arena *a, SourceMap *sm) {
    DiagBag *db = NEW(a, DiagBag);
    db->arena = a; db->sm = sm; db->error_limit = 64;
    vec_init(&db->items);
    return db;
}

Diag *diag_new(DiagBag *db, DiagLevel lvl, const char *code, const char *fmt, ...) {
    Diag *d = NEW(db->arena, Diag);
    d->level = lvl;
    d->code = code;
    va_list ap, ap2;
    va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *p = arena_alloc(db->arena, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2); va_end(ap2);
    d->title = p;
    vec_init(&d->labels); vec_init(&d->notes); vec_init(&d->fixes);
    vec_push(&db->items, d);
    if (lvl == DIAG_ERROR) db->errors++;
    else if (lvl == DIAG_WARNING) db->warnings++;
    return d;
}

static const char *vfmt(Arena *a, const char *fmt, va_list ap) {
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    char *p = arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void diag_label(DiagBag *db, Diag *d, Span sp, bool primary, const char *fmt, ...) {
    DiagLabel l = { sp, NULL, primary };
    if (fmt) { va_list ap; va_start(ap, fmt); l.msg = vfmt(db->arena, fmt, ap); va_end(ap); }
    vec_push(&d->labels, l);
}

void diag_note(DiagBag *db, Diag *d, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const char *s = vfmt(db->arena, fmt, ap); va_end(ap);
    vec_push(&d->notes, s);
}

void diag_fix(DiagBag *db, Diag *d, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const char *s = vfmt(db->arena, fmt, ap); va_end(ap);
    vec_push(&d->fixes, s);
}

void diag_given_wanted(DiagBag *db, Diag *d, const char *given, const char *wanted) {
    d->given = arena_strdup(db->arena, given);
    d->wanted = arena_strdup(db->arena, wanted);
}

bool diagbag_has_errors(DiagBag *db) { return db->errors > 0; }
void diagbag_clear(DiagBag *db) { vec_clear(&db->items); db->errors = db->warnings = 0; }

static const char *level_word(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR: return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE: return "note";
        default: return "help";
    }
}
static const char *level_color(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR: return C_RED;
        case DIAG_WARNING: return C_AMBER;
        case DIAG_NOTE: return C_BLUE;
        default: return C_GREEN;
    }
}

/* Render one diagnostic in the SPRFST house style:

   error[E0201]  Type mismatch
     ┌─ src/main.spf:7:23
     │
   7 │     let total: Text = count + 1
     │                       ━━━━━━━━━ this expression is Int
     │
     ├ you gave      Int
     ├ required      Text
     │
     ╰ fixes
        • convert the value with to_text(count + 1)
        • change the declared type of `total` to Int
*/
static void render_one(DiagBag *db, Diag *d, StrBuf *out) {
    const char *lc = level_color(d->level);
    sb_printf(out, "%s%s%s", C_BOLD, lc, level_word(d->level));
    if (d->code) sb_printf(out, "[%s]", d->code);
    sb_printf(out, "%s  %s%s%s\n", C_RESET, C_BOLD, d->title, C_RESET);

    /* figure out the gutter width */
    int maxline = 1;
    vec_foreach(i, &d->labels) {
        int l, c;
        if (d->labels.items[i].span.file < 0) continue;
        sourcemap_pos(db->sm, d->labels.items[i].span, &l, &c);
        if (l > maxline) maxline = l;
    }
    char gut[16];
    snprintf(gut, sizeof gut, "%d", maxline);
    int gw = (int)strlen(gut);

    bool first = true;
    vec_foreach(i, &d->labels) {
        DiagLabel *lab = &d->labels.items[i];
        if (lab->span.file < 0) continue;
        int line, col;
        sourcemap_pos(db->sm, lab->span, &line, &col);
        SourceFile *f = sourcemap_get(db->sm, lab->span.file);
        if (first) {
            sb_printf(out, "%s%*s ┌─%s %s%s:%d:%d%s\n", C_GRAY, gw, "", C_RESET,
                      C_WHITE, f ? f->path : "?", line, col, C_RESET);
            sb_printf(out, "%s%*s │%s\n", C_GRAY, gw, "", C_RESET);
            first = false;
        }
        Str text = sourcemap_line(db->sm, lab->span.file, line);
        sb_printf(out, "%s%*d │%s ", C_GRAY, gw, line, C_RESET);
        sb_putstr(out, text);
        sb_putc(out, '\n');

        int width = lab->span.end - lab->span.start;
        if (width < 1) width = 1;
        if (col - 1 + width > (int)text.n) width = (int)text.n - (col - 1);
        if (width < 1) width = 1;
        sb_printf(out, "%s%*s │%s ", C_GRAY, gw, "", C_RESET);
        for (int k = 1; k < col; k++) sb_putc(out, text.p && k - 1 < (int)text.n && text.p[k - 1] == '\t' ? '\t' : ' ');
        sb_puts(out, lab->primary ? lc : C_BLUE);
        for (int k = 0; k < width; k++) sb_puts(out, lab->primary ? "━" : "─");
        if (lab->msg) sb_printf(out, " %s", lab->msg);
        sb_printf(out, "%s\n", C_RESET);
    }
    if (!first) sb_printf(out, "%s%*s │%s\n", C_GRAY, gw, "", C_RESET);

    if (d->given || d->wanted) {
        if (d->given)  sb_printf(out, "%s%*s ├%s you gave      %s%s%s\n", C_GRAY, gw, "", C_RESET, C_AMBER, d->given, C_RESET);
        if (d->wanted) sb_printf(out, "%s%*s ├%s required      %s%s%s\n", C_GRAY, gw, "", C_RESET, C_GREEN, d->wanted, C_RESET);
        sb_printf(out, "%s%*s │%s\n", C_GRAY, gw, "", C_RESET);
    }
    vec_foreach(i, &d->notes)
        sb_printf(out, "%s%*s ├%s %snote%s  %s\n", C_GRAY, gw, "", C_RESET, C_BLUE, C_RESET, d->notes.items[i]);
    if (d->fixes.len) {
        sb_printf(out, "%s%*s ╰%s %sfixes%s\n", C_GRAY, gw, "", C_RESET, C_GREEN, C_RESET);
        vec_foreach(i, &d->fixes)
            sb_printf(out, "%s%*s   %s• %s\n", C_GRAY, gw, "", C_RESET, d->fixes.items[i]);
    }
    sb_putc(out, '\n');
}

void diagbag_render(DiagBag *db, FILE *outf, bool color) {
    bool saved = g_color;
    g_color = color;
    StrBuf b; sb_init(&b);
    int shown = 0;
    vec_foreach(i, &db->items) {
        if (db->items.items[i]->level == DIAG_ERROR && shown >= db->error_limit) break;
        render_one(db, db->items.items[i], &b);
        shown++;
    }
    if (b.len) fwrite(b.data, 1, b.len, outf);
    sb_free(&b);
    g_color = saved;
}

static void json_escape(StrBuf *b, const char *s) {
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"': sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n"); break;
            case '\r': sb_puts(b, "\\r"); break;
            case '\t': sb_puts(b, "\\t"); break;
            default:
                if (c < 0x20) sb_printf(b, "\\u%04x", c);
                else sb_putc(b, (char)c);
        }
    }
}

char *diagbag_to_json(DiagBag *db) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, "[");
    vec_foreach(i, &db->items) {
        Diag *d = db->items.items[i];
        if (i) sb_puts(&b, ",");
        int line = 0, col = 1, eline = 0, ecol = 1;
        const char *path = "";
        Span sp = SPAN_NONE;
        vec_foreach(k, &d->labels) if (d->labels.items[k].primary) { sp = d->labels.items[k].span; break; }
        if (sp.file < 0 && d->labels.len) sp = d->labels.items[0].span;
        if (sp.file >= 0) {
            sourcemap_pos(db->sm, sp, &line, &col);
            Span e = span_make(sp.file, sp.end, sp.end);
            sourcemap_pos(db->sm, e, &eline, &ecol);
            SourceFile *f = sourcemap_get(db->sm, sp.file);
            if (f) path = f->path;
        }
        sb_printf(&b, "{\"level\":\"%s\",\"code\":\"%s\",\"title\":\"", level_word(d->level), d->code ? d->code : "");
        json_escape(&b, d->title);
        sb_printf(&b, "\",\"file\":\"");
        json_escape(&b, path);
        sb_printf(&b, "\",\"line\":%d,\"col\":%d,\"endLine\":%d,\"endCol\":%d", line, col, eline, ecol);
        sb_puts(&b, ",\"labels\":[");
        vec_foreach(k, &d->labels) {
            DiagLabel *l = &d->labels.items[k];
            int ll = 0, lcc = 0;
            if (l->span.file >= 0) sourcemap_pos(db->sm, l->span, &ll, &lcc);
            if (k) sb_puts(&b, ",");
            sb_printf(&b, "{\"line\":%d,\"col\":%d,\"primary\":%s,\"msg\":\"", ll, lcc, l->primary ? "true" : "false");
            json_escape(&b, l->msg ? l->msg : "");
            sb_puts(&b, "\"}");
        }
        sb_puts(&b, "],\"fixes\":[");
        vec_foreach(k, &d->fixes) { if (k) sb_puts(&b, ","); sb_puts(&b, "\""); json_escape(&b, d->fixes.items[k]); sb_puts(&b, "\""); }
        sb_puts(&b, "],\"notes\":[");
        vec_foreach(k, &d->notes) { if (k) sb_puts(&b, ","); sb_puts(&b, "\""); json_escape(&b, d->notes.items[k]); sb_puts(&b, "\""); }
        sb_puts(&b, "]");
        if (d->given) { sb_puts(&b, ",\"given\":\""); json_escape(&b, d->given); sb_puts(&b, "\""); }
        if (d->wanted) { sb_puts(&b, ",\"wanted\":\""); json_escape(&b, d->wanted); sb_puts(&b, "\""); }
        sb_puts(&b, "}");
    }
    sb_puts(&b, "]");
    return sb_take(&b);
}

bool term_supports_color(FILE *f) {
    if (!isatty(fileno(f))) return false;
    const char *t = getenv("TERM");
    if (!t || strcmp(t, "dumb") == 0) return false;
    if (getenv("NO_COLOR")) return false;
    return true;
}

/* ------------------------------------------------------------------- io */
char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    if (out_len) *out_len = got;
    return buf;
}

bool write_file_bytes(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    return w == len;
}

bool file_exists(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISREG(st.st_mode); }
bool dir_exists(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }

bool make_dir_all(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n && tmp[n - 1] == '/') tmp[n - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

bool remove_dir_all(const char *path) {
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *e;
    char buf[4096];
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(buf, sizeof buf, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) remove_dir_all(buf);
        else unlink(buf);
    }
    closedir(d);
    return rmdir(path) == 0;
}

char *path_join(Arena *a, const char *x, const char *y) {
    if (!x || !*x) return arena_strdup(a, y);
    if (y && y[0] == '/') return arena_strdup(a, y);
    size_t n = strlen(x);
    bool slash = n && x[n - 1] == '/';
    return arena_vsprintf(a, "%s%s%s", x, slash ? "" : "/", y ? y : "");
}

char *path_dirname(Arena *a, const char *p) {
    const char *s = strrchr(p, '/');
    if (!s) return arena_strdup(a, ".");
    if (s == p) return arena_strdup(a, "/");
    return arena_strndup(a, p, (size_t)(s - p));
}

const char *path_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

const char *path_ext(const char *p) {
    const char *b = path_basename(p);
    const char *d = strrchr(b, '.');
    return d ? d + 1 : "";
}

char *path_abs(Arena *a, const char *p) {
    char buf[4096];
    if (p && p[0] == '/') return arena_strdup(a, p);
    if (!getcwd(buf, sizeof buf)) return arena_strdup(a, p);
    return path_join(a, buf, p);
}

uint64_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_mtime * 1000000ull;
}

double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

char **list_dir(const char *path, int *count, bool *isdir_out) {
    DIR *d = opendir(path);
    *count = 0;
    if (!d) return NULL;
    int cap = 16, n = 0;
    char **names = malloc(sizeof(char *) * cap);
    bool *isd = isdir_out ? NULL : NULL;
    (void)isd;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n + 1 >= cap) { cap *= 2; names = realloc(names, sizeof(char *) * cap); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    names[n] = NULL;
    *count = n;
    return names;
}

/* --------------------------------------------------------------- sha256 */
static const uint32_t K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

void sha256_raw(const void *vdata, size_t len, unsigned char out[32]) {
    const unsigned char *data = (const unsigned char *)vdata;
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total = ((len + 9 + 63) / 64) * 64;
    unsigned char *buf = calloc(total, 1);
    memcpy(buf, data, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off+i*4] << 24) | ((uint32_t)buf[off+i*4+1] << 16) |
                   ((uint32_t)buf[off+i*4+2] << 8) | (uint32_t)buf[off+i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f2=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
            uint32_t ch = (e & f2) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
            uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f2; f2=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f2; h[6]+=g; h[7]+=hh;
    }
    free(buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(h[i] >> 24);
        out[i*4+1] = (unsigned char)(h[i] >> 16);
        out[i*4+2] = (unsigned char)(h[i] >> 8);
        out[i*4+3] = (unsigned char)h[i];
    }
}
void sha256_hex(const void *data, size_t n, char out[65]) {
    unsigned char d[32];
    sha256_raw(data, n, d);
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i*2] = H[d[i] >> 4]; out[i*2+1] = H[d[i] & 15]; }
    out[64] = 0;
}
