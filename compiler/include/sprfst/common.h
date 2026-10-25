/* ========================================================================
   SPRFST — common infrastructure
   Arena allocation, slices, growable buffers, interning, source maps,
   and the diagnostic engine shared by every stage of the toolchain.
   ======================================================================== */
#ifndef SPRFST_COMMON_H
#define SPRFST_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPRFST_VERSION "0.1.0-beta"
#define SPRFST_CODENAME "Ember"

/* ---------------------------------------------------------------- arena */
typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock {
    ArenaBlock *next;
    size_t used, cap;
    char data[];
};

typedef struct {
    ArenaBlock *head;
    size_t total;
} Arena;

void  arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t n);
void *arena_zalloc(Arena *a, size_t n);
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_strdup(Arena *a, const char *s);
char *arena_vsprintf(Arena *a, const char *fmt, ...);
void  arena_free(Arena *a);

#define NEW(a, T)      ((T *)arena_zalloc((a), sizeof(T)))
#define NEWN(a, T, n)  ((T *)arena_zalloc((a), sizeof(T) * (size_t)(n)))

/* ------------------------------------------------------------- str slice */
typedef struct {
    const char *p;
    size_t      n;
} Str;

static inline Str str_make(const char *p, size_t n) { Str s = { p, n }; return s; }
static inline Str str_cstr(const char *p) { Str s = { p, p ? strlen(p) : 0 }; return s; }
static inline bool str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static inline bool str_eqc(Str a, const char *b) { size_t n = strlen(b); return a.n == n && memcmp(a.p, b, n) == 0; }
char *str_dup(Arena *a, Str s);        /* NUL terminated copy */
#define STRFMT "%.*s"
#define STRARG(s) (int)(s).n, (s).p

/* --------------------------------------------------------------- strbuf */
typedef struct {
    char  *data;
    size_t len, cap;
} StrBuf;

void  sb_init(StrBuf *b);
void  sb_free(StrBuf *b);
void  sb_putc(StrBuf *b, char c);
void  sb_put(StrBuf *b, const char *s, size_t n);
void  sb_puts(StrBuf *b, const char *s);
void  sb_putstr(StrBuf *b, Str s);
void  sb_printf(StrBuf *b, const char *fmt, ...);
void  sb_indent(StrBuf *b, int depth);
char *sb_take(StrBuf *b); /* malloc'd, caller frees */

/* ----------------------------------------------------------------- vec */
/* Lightweight type-safe growable array used all over the compiler. */
#define VEC(T) struct { T *items; int len, cap; }
#define vec_init(v) do { (v)->items = NULL; (v)->len = 0; (v)->cap = 0; } while (0)
#define vec_reserve(v, n)                                                     \
    do {                                                                      \
        if ((n) > (v)->cap) {                                                 \
            int _c = (v)->cap ? (v)->cap : 8;                                 \
            while (_c < (n)) _c *= 2;                                         \
            (v)->items = realloc((v)->items, (size_t)_c * sizeof(*(v)->items));\
            (v)->cap = _c;                                                    \
        }                                                                     \
    } while (0)
#define vec_push(v, x)                                                        \
    do { vec_reserve((v), (v)->len + 1); (v)->items[(v)->len++] = (x); } while (0)
#define vec_pop(v) ((v)->items[--(v)->len])
#define vec_last(v) ((v)->items[(v)->len - 1])
#define vec_clear(v) ((v)->len = 0)
#define vec_free(v) do { free((v)->items); (v)->items = NULL; (v)->len = (v)->cap = 0; } while (0)
#define vec_foreach(it, v) for (int it = 0; it < (v)->len; it++)

/* --------------------------------------------------------------- interner */
typedef struct Interner Interner;
Interner  *interner_new(void);
const char *intern(Interner *in, Str s);     /* returns stable NUL-terminated */
const char *internc(Interner *in, const char *s);
void       interner_free(Interner *in);

/* ----------------------------------------------------------- source files */
typedef struct {
    int         id;
    const char *path;      /* interned, display path */
    const char *abspath;
    char       *src;       /* NUL terminated, owned */
    size_t      len;
    int        *lines;     /* byte offset of each line start */
    int         nlines;
} SourceFile;

typedef struct {
    int file;
    int start;  /* byte offset */
    int end;
} Span;

static inline Span span_make(int file, int s, int e) { Span sp = { file, s, e }; return sp; }
static inline Span span_join(Span a, Span b) {
    Span r = a;
    if (b.file == a.file && b.end > a.end) r.end = b.end;
    return r;
}
extern const Span SPAN_NONE;

typedef struct {
    VEC(SourceFile *) files;
    Arena            *arena;
} SourceMap;

SourceMap  *sourcemap_new(Arena *a);
SourceFile *sourcemap_add(SourceMap *sm, const char *path, char *src, size_t len);
SourceFile *sourcemap_load(SourceMap *sm, const char *path);
SourceFile *sourcemap_get(SourceMap *sm, int id);
void        sourcemap_pos(SourceMap *sm, Span sp, int *line, int *col);
Str         sourcemap_line(SourceMap *sm, int file, int line);

/* ----------------------------------------------------------- diagnostics */
typedef enum { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE, DIAG_HELP } DiagLevel;

typedef struct DiagLabel {
    Span        span;
    const char *msg;      /* owned by arena */
    bool        primary;
} DiagLabel;

typedef struct Diag {
    DiagLevel   level;
    const char *code;     /* e.g. "E0201" */
    const char *title;
    VEC(DiagLabel) labels;
    VEC(const char *) notes;
    VEC(const char *) fixes;
    const char *given;    /* "You gave:" line (optional) */
    const char *wanted;   /* "This operation requires:" (optional) */
} Diag;

typedef struct {
    VEC(Diag *) items;
    Arena      *arena;
    SourceMap  *sm;
    int         errors, warnings;
    int         error_limit;
} DiagBag;

DiagBag *diagbag_new(Arena *a, SourceMap *sm);
Diag    *diag_new(DiagBag *db, DiagLevel lvl, const char *code, const char *fmt, ...);
void     diag_label(DiagBag *db, Diag *d, Span sp, bool primary, const char *fmt, ...);
void     diag_note(DiagBag *db, Diag *d, const char *fmt, ...);
void     diag_fix(DiagBag *db, Diag *d, const char *fmt, ...);
void     diag_given_wanted(DiagBag *db, Diag *d, const char *given, const char *wanted);
void     diagbag_render(DiagBag *db, FILE *out, bool color);
char    *diagbag_to_json(DiagBag *db);  /* malloc'd; used by Studio */
bool     diagbag_has_errors(DiagBag *db);
void     diagbag_clear(DiagBag *db);

/* -------------------------------------------------------------- terminal */
extern bool g_color;
bool term_supports_color(FILE *f);
#define C_RESET  (g_color ? "\x1b[0m"      : "")
#define C_BOLD   (g_color ? "\x1b[1m"      : "")
#define C_DIM    (g_color ? "\x1b[2m"      : "")
#define C_RED    (g_color ? "\x1b[38;5;203m" : "")
#define C_ORANGE (g_color ? "\x1b[38;5;208m" : "")
#define C_AMBER  (g_color ? "\x1b[38;5;214m" : "")
#define C_YELLOW (g_color ? "\x1b[38;5;221m" : "")
#define C_GREEN  (g_color ? "\x1b[38;5;114m" : "")
#define C_BLUE   (g_color ? "\x1b[38;5;75m"  : "")
#define C_GRAY   (g_color ? "\x1b[38;5;245m" : "")
#define C_WHITE  (g_color ? "\x1b[38;5;255m" : "")

/* ------------------------------------------------------------------- io */
char *read_file(const char *path, size_t *out_len);
bool  write_file_bytes(const char *path, const void *data, size_t len);
bool  file_exists(const char *path);
bool  dir_exists(const char *path);
bool  make_dir_all(const char *path);
bool  remove_dir_all(const char *path);
char *path_join(Arena *a, const char *a1, const char *b);
char *path_dirname(Arena *a, const char *p);
const char *path_basename(const char *p);
const char *path_ext(const char *p);
char *path_abs(Arena *a, const char *p);
uint64_t file_mtime(const char *path);
double now_seconds(void);
uint64_t hash_bytes(const void *data, size_t n);
void     hash_hex(const void *data, size_t n, char out[65]); /* FNV+mix 256-bit-ish digest */
void     sha256_raw(const void *data, size_t n, unsigned char out[32]);
void     sha256_hex(const void *data, size_t n, char out[65]);

/* List a directory; returns malloc'd array of malloc'd names, NULL terminated */
char **list_dir(const char *path, int *count, bool *isdir_out);

#endif /* SPRFST_COMMON_H */
