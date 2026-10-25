/* ========================================================================
   SPRFST — native standard library implementations
   ======================================================================== */
#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "sprfst/vm.h"
#include "sprfst/natives.h"
#include <ctype.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>

void sprfst_lock(void);
void sprfst_unlock(void);

/* ------------------------------------------------------------- helpers */
static int    g_argc = 0;
static char **g_argv = NULL;
void vm_set_args(int argc, char **argv) { g_argc = argc; g_argv = argv; }

#define A(i)  (i < nargs ? args[i] : v_nil())
#define NUM(i) v_tonum(A(i))
#define INT(i) v_toint(A(i))

static const char *txt(Value v) {
    if (IS_OBJ(v, O_TEXT)) return AS_TEXT(v)->chars;
    return "";
}
static Value mktext(VM *vm, const char *s) { return v_obj((Obj *)vm_text_cstr(vm, s ? s : "")); }
static Value mktextn(VM *vm, const char *s, int n) { return v_obj((Obj *)vm_text(vm, s, n)); }

static Value list_of_strings(VM *vm, char **items, int n) {
    ObjList *l = vm_list(vm);
    for (int i = 0; i < n; i++) vec_push(&l->items, mktext(vm, items[i]));
    return v_obj((Obj *)l);
}

/* --------------------------------------------------------------- random */
static uint64_t g_rand_state = 0x853c49e6748fea9bULL;
static uint64_t rnd64(void) {
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 7;
    g_rand_state ^= g_rand_state << 17;
    return g_rand_state;
}
static double rnd_unit(void) { return (double)(rnd64() >> 11) / 9007199254740992.0; }

/* ----------------------------------------------------------------- JSON */
typedef struct { const char *p; VM *vm; bool ok; } JP;
static void jp_ws(JP *j) { while (*j->p == ' ' || *j->p == '\n' || *j->p == '\t' || *j->p == '\r') j->p++; }
static Value jp_value(JP *j);

static Value jp_string(JP *j) {
    StrBuf b; sb_init(&b);
    j->p++;
    while (*j->p && *j->p != '"') {
        if (*j->p == '\\') {
            j->p++;
            switch (*j->p) {
                case 'n': sb_putc(&b, '\n'); break;
                case 't': sb_putc(&b, '\t'); break;
                case 'r': sb_putc(&b, '\r'); break;
                case 'b': sb_putc(&b, '\b'); break;
                case 'f': sb_putc(&b, '\f'); break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4 && j->p[1]; i++) {
                        j->p++;
                        char c = *j->p;
                        cp = cp * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10));
                    }
                    if (cp < 0x80) sb_putc(&b, (char)cp);
                    else if (cp < 0x800) { sb_putc(&b, (char)(0xC0 | (cp >> 6))); sb_putc(&b, (char)(0x80 | (cp & 0x3F))); }
                    else { sb_putc(&b, (char)(0xE0 | (cp >> 12))); sb_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F))); sb_putc(&b, (char)(0x80 | (cp & 0x3F))); }
                    break;
                }
                default: sb_putc(&b, *j->p);
            }
            j->p++;
        } else sb_putc(&b, *j->p++);
    }
    if (*j->p == '"') j->p++;
    Value v = mktextn(j->vm, b.data ? b.data : "", (int)b.len);
    sb_free(&b);
    return v;
}

static Value jp_value(JP *j) {
    jp_ws(j);
    switch (*j->p) {
        case '"': return jp_string(j);
        case '{': {
            j->p++;
            ObjMap *m = vm_map(j->vm);
            jp_ws(j);
            if (*j->p == '}') { j->p++; return v_obj((Obj *)m); }
            for (;;) {
                jp_ws(j);
                if (*j->p != '"') { j->ok = false; break; }
                Value k = jp_string(j);
                jp_ws(j);
                if (*j->p == ':') j->p++;
                Value v = jp_value(j);
                vm_map_set(j->vm, m, k, v);
                jp_ws(j);
                if (*j->p == ',') { j->p++; continue; }
                if (*j->p == '}') { j->p++; break; }
                j->ok = false;
                break;
            }
            return v_obj((Obj *)m);
        }
        case '[': {
            j->p++;
            ObjList *l = vm_list(j->vm);
            jp_ws(j);
            if (*j->p == ']') { j->p++; return v_obj((Obj *)l); }
            for (;;) {
                Value v = jp_value(j);
                vec_push(&l->items, v);
                jp_ws(j);
                if (*j->p == ',') { j->p++; continue; }
                if (*j->p == ']') { j->p++; break; }
                j->ok = false;
                break;
            }
            return v_obj((Obj *)l);
        }
        case 't': if (strncmp(j->p, "true", 4) == 0) { j->p += 4; return v_bool(true); } j->ok = false; return v_nil();
        case 'f': if (strncmp(j->p, "false", 5) == 0) { j->p += 5; return v_bool(false); } j->ok = false; return v_nil();
        case 'n': if (strncmp(j->p, "null", 4) == 0) { j->p += 4; return v_nil(); } j->ok = false; return v_nil();
        default: {
            char *end = NULL;
            double d = strtod(j->p, &end);
            if (end == j->p) { j->ok = false; return v_nil(); }
            bool isint = true;
            for (const char *q = j->p; q < end; q++) if (*q == '.' || *q == 'e' || *q == 'E') isint = false;
            j->p = end;
            return isint ? v_int((int64_t)d) : v_num(d);
        }
    }
}

static void json_write_value(VM *vm, Value v, StrBuf *b, int indent, int depth) {
    switch (v.tag) {
        case V_NIL: sb_puts(b, "null"); break;
        case V_BOOL: sb_puts(b, v.as.b ? "true" : "false"); break;
        case V_INT: sb_printf(b, "%lld", (long long)v.as.i); break;
        case V_NUM: {
            double d = v.as.n;
            if (d == (double)(int64_t)d && fabs(d) < 1e15) sb_printf(b, "%lld", (long long)d);
            else sb_printf(b, "%.17g", d);
            break;
        }
        case V_OBJ: {
            Obj *o = v.as.o;
            if (!o) { sb_puts(b, "null"); break; }
            switch (o->kind) {
                case O_TEXT: {
                    ObjText *t = (ObjText *)o;
                    sb_putc(b, '"');
                    for (int i = 0; i < t->len; i++) {
                        unsigned char c = (unsigned char)t->chars[i];
                        switch (c) {
                            case '"': sb_puts(b, "\\\""); break;
                            case '\\': sb_puts(b, "\\\\"); break;
                            case '\n': sb_puts(b, "\\n"); break;
                            case '\r': sb_puts(b, "\\r"); break;
                            case '\t': sb_puts(b, "\\t"); break;
                            default: if (c < 0x20) sb_printf(b, "\\u%04x", c); else sb_putc(b, (char)c);
                        }
                    }
                    sb_putc(b, '"');
                    break;
                }
                case O_LIST: {
                    ObjList *l = (ObjList *)o;
                    sb_putc(b, '[');
                    vec_foreach(i, &l->items) {
                        if (i) sb_putc(b, ',');
                        if (indent) { sb_putc(b, '\n'); for (int k = 0; k <= depth; k++) sb_puts(b, "  "); }
                        json_write_value(vm, l->items.items[i], b, indent, depth + 1);
                    }
                    if (indent && l->items.len) { sb_putc(b, '\n'); for (int k = 0; k < depth; k++) sb_puts(b, "  "); }
                    sb_putc(b, ']');
                    break;
                }
                case O_MAP: {
                    ObjMap *m = (ObjMap *)o;
                    sb_putc(b, '{');
                    vec_foreach(i, &m->keys) {
                        if (i) sb_putc(b, ',');
                        if (indent) { sb_putc(b, '\n'); for (int k = 0; k <= depth; k++) sb_puts(b, "  "); }
                        char *ks = vm_value_text(vm, m->keys.items[i], false);
                        sb_putc(b, '"'); sb_puts(b, ks); sb_putc(b, '"');
                        free(ks);
                        sb_putc(b, ':');
                        if (indent) sb_putc(b, ' ');
                        json_write_value(vm, m->vals.items[i], b, indent, depth + 1);
                    }
                    if (indent && m->keys.len) { sb_putc(b, '\n'); for (int k = 0; k < depth; k++) sb_puts(b, "  "); }
                    sb_putc(b, '}');
                    break;
                }
                case O_INSTANCE: {
                    ObjInstance *n = (ObjInstance *)o;
                    IRType *t = (n->type_id >= 0 && n->type_id < vm->prog->types.len) ? vm->prog->types.items[n->type_id] : NULL;
                    sb_putc(b, '{');
                    vec_foreach(i, &n->fields) {
                        if (i) sb_putc(b, ',');
                        if (indent) { sb_putc(b, '\n'); for (int k = 0; k <= depth; k++) sb_puts(b, "  "); }
                        sb_printf(b, "\"%s\":", (t && i < t->nfields) ? t->field_names[i] : "field");
                        if (indent) sb_putc(b, ' ');
                        json_write_value(vm, n->fields.items[i], b, indent, depth + 1);
                    }
                    if (indent && n->fields.len) { sb_putc(b, '\n'); for (int k = 0; k < depth; k++) sb_puts(b, "  "); }
                    sb_putc(b, '}');
                    break;
                }
                case O_SET: {
                    ObjSet *s = (ObjSet *)o;
                    sb_putc(b, '[');
                    vec_foreach(i, &s->items) { if (i) sb_putc(b, ','); json_write_value(vm, s->items.items[i], b, indent, depth + 1); }
                    sb_putc(b, ']');
                    break;
                }
                default: { char *s = vm_value_text(vm, v, false); sb_printf(b, "\"%s\"", s); free(s); }
            }
            break;
        }
    }
}

/* sha256 lives in common.c so the package manager shares it */

static uint32_t crc32_bytes(const unsigned char *p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ------------------------------------------------------------------ PNG */
static void png_chunk(FILE *f, const char *type, const unsigned char *data, size_t len) {
    unsigned char hdr[8];
    hdr[0] = (unsigned char)(len >> 24); hdr[1] = (unsigned char)(len >> 16);
    hdr[2] = (unsigned char)(len >> 8);  hdr[3] = (unsigned char)len;
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc32_bytes((const unsigned char *)type, 4, 0);
    if (len) crc = crc32_bytes(data, len, crc ^ 0xffffffffu) ;
    /* recompute properly over type+data */
    unsigned char *tmp = malloc(4 + len);
    memcpy(tmp, type, 4);
    if (len) memcpy(tmp + 4, data, len);
    crc = crc32_bytes(tmp, 4 + len, 0);
    free(tmp);
    unsigned char c4[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                            (unsigned char)(crc >> 8), (unsigned char)crc };
    fwrite(c4, 1, 4, f);
}

/* ------------------------------------------------------- deflate (fixed)
   A real compressor: LZ77 over a 32 KB window, emitted with deflate's
   fixed Huffman codes. Small enough to read, good enough that a picture
   SPRFST draws is a normal sized PNG rather than a raw dump. */
typedef struct { unsigned char *buf; size_t len, cap; uint32_t bits; int nbits; } BitW;

static void bw_byte(BitW *b, unsigned char c) {
    if (b->len + 1 > b->cap) { b->cap = b->cap * 2 + 256; b->buf = realloc(b->buf, b->cap); }
    b->buf[b->len++] = c;
}
/* deflate packs bits least significant first */
static void bw_bits(BitW *b, uint32_t value, int n) {
    b->bits |= (value & ((1u << n) - 1u)) << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) { bw_byte(b, (unsigned char)(b->bits & 0xff)); b->bits >>= 8; b->nbits -= 8; }
}
/* Huffman codes are written most significant bit first */
static void bw_code(BitW *b, uint32_t code, int n) {
    for (int i = n - 1; i >= 0; i--) bw_bits(b, (code >> i) & 1u, 1);
}
static void bw_flush(BitW *b) {
    if (b->nbits) { bw_byte(b, (unsigned char)(b->bits & 0xff)); b->bits = 0; b->nbits = 0; }
}

static const unsigned short LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned char  LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned char  DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void z_literal(BitW *b, int sym) {
    if (sym <= 143)      bw_code(b, 0x30u + (uint32_t)sym, 8);
    else if (sym <= 255) bw_code(b, 0x190u + (uint32_t)(sym - 144), 9);
    else if (sym <= 279) bw_code(b, (uint32_t)(sym - 256), 7);
    else                 bw_code(b, 0xC0u + (uint32_t)(sym - 280), 8);
}

static void z_match(BitW *b, int len, int dist) {
    int lc = 28;
    while (lc > 0 && LEN_BASE[lc] > len) lc--;
    z_literal(b, 257 + lc);
    if (LEN_EXTRA[lc]) bw_bits(b, (uint32_t)(len - LEN_BASE[lc]), LEN_EXTRA[lc]);
    int dc = 29;
    while (dc > 0 && DIST_BASE[dc] > dist) dc--;
    bw_code(b, (uint32_t)dc, 5);
    if (DIST_EXTRA[dc]) bw_bits(b, (uint32_t)(dist - DIST_BASE[dc]), DIST_EXTRA[dc]);
}

#define Z_HASHBITS 15
#define Z_HASHSIZE (1 << Z_HASHBITS)
#define Z_WINDOW   32768
#define Z_CHAIN    48

static uint32_t z_hash(const unsigned char *p) {
    return ((uint32_t)p[0] * 0x9E3779B1u ^ (uint32_t)p[1] * 0x85EBCA77u ^ (uint32_t)p[2] * 0xC2B2AE3Du)
           >> (32 - Z_HASHBITS);
}

/* zlib stream (header, one fixed Huffman block, adler32) */
static unsigned char *zlib_compress(const unsigned char *src, size_t n, size_t *out_len) {
    BitW b;
    b.cap = n / 3 + 1024; b.buf = malloc(b.cap); b.len = 0; b.bits = 0; b.nbits = 0;
    bw_byte(&b, 0x78); bw_byte(&b, 0x01);
    bw_bits(&b, 1, 1);          /* final block */
    bw_bits(&b, 1, 2);          /* fixed Huffman */

    int *head = malloc(sizeof(int) * Z_HASHSIZE);
    int *prev = malloc(sizeof(int) * (n ? n : 1));
    for (int i = 0; i < Z_HASHSIZE; i++) head[i] = -1;

    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            uint32_t h = z_hash(src + i);
            int cand = head[h];
            size_t maxlen = n - i; if (maxlen > 258) maxlen = 258;
            for (int chain = 0; cand >= 0 && chain < Z_CHAIN; chain++) {
                if ((size_t)cand + Z_WINDOW <= i) break;
                size_t l = 0;
                while (l < maxlen && src[(size_t)cand + l] == src[i + l]) l++;
                if ((int)l > best_len) { best_len = (int)l; best_dist = (int)(i - (size_t)cand); }
                if ((size_t)best_len >= maxlen) break;
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = (int)i;
        }
        if (best_len >= 3) {
            z_match(&b, best_len, best_dist);
            for (size_t k = 1; k < (size_t)best_len; k++) {
                if (i + k + 3 > n) break;
                uint32_t h2 = z_hash(src + i + k);
                prev[i + k] = head[h2];
                head[h2] = (int)(i + k);
            }
            i += (size_t)best_len;
        } else {
            z_literal(&b, src[i]);
            i++;
        }
    }
    z_literal(&b, 256);         /* end of block */
    bw_flush(&b);
    free(head); free(prev);

    uint32_t a1 = 1, a2 = 0;
    for (size_t k = 0; k < n; k++) { a1 = (a1 + src[k]) % 65521; a2 = (a2 + a1) % 65521; }
    uint32_t adler = (a2 << 16) | a1;
    bw_byte(&b, (unsigned char)(adler >> 24)); bw_byte(&b, (unsigned char)(adler >> 16));
    bw_byte(&b, (unsigned char)(adler >> 8));  bw_byte(&b, (unsigned char)adler);

    *out_len = b.len;
    return b.buf;
}

static bool write_png(const char *path, int w, int h, uint32_t *px) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
    ihdr[2] = (unsigned char)(w >> 8);  ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
    ihdr[6] = (unsigned char)(h >> 8);  ihdr[7] = (unsigned char)h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);

    size_t stride = (size_t)w * 4;
    size_t raw_len = (size_t)h * (1 + stride);
    unsigned char *raw = malloc(raw_len);
    unsigned char *line = malloc(stride);
    unsigned char *above = calloc(stride, 1);
    unsigned char *try_sub = malloc(stride);
    unsigned char *try_up = malloc(stride);
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * w + x];
            line[x * 4 + 0] = (unsigned char)((c >> 16) & 0xff);
            line[x * 4 + 1] = (unsigned char)((c >> 8) & 0xff);
            line[x * 4 + 2] = (unsigned char)(c & 0xff);
            line[x * 4 + 3] = (unsigned char)((c >> 24) & 0xff ? (c >> 24) & 0xff : 255);
        }
        /* pick the row filter that leaves the least for the compressor:
           0 none, 1 difference from the pixel to the left, 2 from above */
        long score_none = 0, score_sub = 0, score_up = 0;
        for (size_t k = 0; k < stride; k++) {
            unsigned char left = k >= 4 ? line[k - 4] : 0;
            try_sub[k] = (unsigned char)(line[k] - left);
            try_up[k]  = (unsigned char)(line[k] - above[k]);
            score_none += line[k] < 128 ? line[k] : 256 - line[k];
            score_sub  += try_sub[k] < 128 ? try_sub[k] : 256 - try_sub[k];
            score_up   += try_up[k]  < 128 ? try_up[k]  : 256 - try_up[k];
        }
        if (score_sub <= score_none && score_sub <= score_up) {
            raw[o++] = 1;
            memcpy(raw + o, try_sub, stride);
        } else if (score_up <= score_none) {
            raw[o++] = 2;
            memcpy(raw + o, try_up, stride);
        } else {
            raw[o++] = 0;
            memcpy(raw + o, line, stride);
        }
        o += stride;
        memcpy(above, line, stride);
    }
    free(line); free(above); free(try_sub); free(try_up);
    size_t zo = 0;
    unsigned char *z = zlib_compress(raw, raw_len, &zo);
    png_chunk(f, "IDAT", z, zo);
    png_chunk(f, "IEND", NULL, 0);
    free(raw); free(z);
    fclose(f);
    return true;
}

/* 5x7 bitmap font for draw.text — covers ASCII 32..126 */
static const unsigned char FONT5x7[95][5] = {
{0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},
{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},{0,0x1C,0x22,0x41,0},
{0,0x41,0x22,0x1C,0},{0x14,8,0x3E,8,0x14},{8,8,0x3E,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},
{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},{0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},
{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},
{6,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},
{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3E},
{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},
{0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},
{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0x0C,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
{0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
{0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7F,0x41,0x41,0},
{2,4,8,0x10,0x20},{0,0x41,0x41,0x7F,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},{0,1,2,4,0},
{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{8,0x7E,9,1,2},{0x0C,0x52,0x52,0x52,0x3E},
{0x7F,8,4,4,0x78},{0,0x44,0x7D,0x40,0},{0x20,0x40,0x44,0x3D,0},{0x7F,0x10,0x28,0x44,0},
{0,0x41,0x7F,0x40,0},{0x7C,4,0x18,4,0x78},{0x7C,8,4,4,0x78},{0x38,0x44,0x44,0x44,0x38},
{0x7C,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7C},{0x7C,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},
{4,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
{0x44,0x64,0x54,0x4C,0x44},{0,8,0x36,0x41,0},{0,0,0x7F,0,0},{0,0x41,0x36,8,0},{8,4,8,0x10,8} };

/* --------------------------------------------------------------- sockets */
#define MAX_SOCK 256
static int g_socks[MAX_SOCK];
static int g_nsock = 0;
static int sock_store(int fd) {
    for (int i = 0; i < g_nsock; i++) if (g_socks[i] < 0) { g_socks[i] = fd; return i; }
    if (g_nsock < MAX_SOCK) { g_socks[g_nsock] = fd; return g_nsock++; }
    return -1;
}
static int sock_fd(int id) { return (id >= 0 && id < g_nsock) ? g_socks[id] : -1; }

/* ------------------------------------------------------------- threading */
typedef struct { pthread_mutex_t m; bool used; } LockSlot;
static LockSlot g_locks[64];
static int g_nlocks = 0;
static int64_t g_counters[64];
static int g_ncounters = 0;

/* --------------------------------------------------------------- Ember DB */
#include "emberdb.inc"

/* ------------------------------------------------------------------- UI */
#include "uikit.inc"

/* ----------------------------------------------------------------- HTTP */
#include "httpd.inc"

/* ======================================================================= */
/*  dispatch                                                               */
/* ======================================================================= */

/* One line of standard input with no length limit; NULL at end of input. Caller frees. */
static char *read_stdin_line(void) {
    size_t cap = 256, len = 0; char *buf = malloc(cap);
    int ch, any = 0;
    while ((ch = fgetc(stdin)) != EOF) {
        any = 1;
        if (ch == '\n') break;
        if (len + 2 > cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)ch;
    }
    if (!any) { free(buf); return NULL; }
    while (len && buf[len-1] == '\r') len--;
    buf[len] = 0;
    return buf;
}

static Value native_dispatch(VM *vm, int id, Value *args, int nargs, bool *ok);

/* Every native runs inside a scope that holds on to whatever it allocates,
   so a collection triggered half way through cannot sweep its own workings
   out from under it. See vm_native_enter in vm.c. */
Value vm_native_call(VM *vm, int id, Value *args, int nargs, bool *ok) {
    int mark = vm_native_enter(vm);
    Value r = native_dispatch(vm, id, args, nargs, ok);
    vm_native_leave(vm, mark, r);
    return r;
}

static Value native_dispatch(VM *vm, int id, Value *args, int nargs, bool *ok) {
    *ok = true;
    switch (id) {
    /* ------------------------------------------------------------- io */
    case NF_IO_SAY: { char *s = vm_value_text(vm, A(0), false); printf("%s\n", s); free(s); return v_nil(); }
    case NF_IO_PRINT: { char *s = vm_value_text(vm, A(0), false); fputs(s, stdout); free(s); return v_nil(); }
    case NF_IO_WARN: { char *s = vm_value_text(vm, A(0), false); fprintf(stderr, "%s\n", s); free(s); return v_nil(); }
    case NF_IO_ASK: {
        char *p = vm_value_text(vm, A(0), false);
        fputs(p, stdout); fflush(stdout); free(p);
        char *line = read_stdin_line();
        if (!line) return mktext(vm, "");
        Value v = mktext(vm, line); free(line); return v;
    }
    case NF_IO_READ_LINE: {
        char *line = read_stdin_line();
        if (!line) return v_nil();
        Value v = mktext(vm, line); free(line); return v;
    }
    case NF_IO_ASK_NUM: case NF_IO_ASK_INT: {
        char *p = vm_value_text(vm, A(0), false);
        fputs(p, stdout); fflush(stdout); free(p);
        char *line = read_stdin_line();
        if (!line) return v_nil();
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        char *end = NULL;
        Value out = v_nil();
        if (id == NF_IO_ASK_INT) {
            long long r = strtoll(s, &end, 10);
            if (end != s) { while (*end == ' ' || *end == '\t') end++; if (!*end) out = v_int(r); }
        } else {
            double r = strtod(s, &end);
            if (end != s) { while (*end == ' ' || *end == '\t') end++; if (!*end) out = v_num(r); }
        }
        free(line);
        return out;
    }
    case NF_IO_ASK_YES: {
        char *p = vm_value_text(vm, A(0), false);
        fputs(p, stdout); fflush(stdout); free(p);
        char *line = read_stdin_line();
        if (!line) return v_bool(false);
        char *s = line; while (*s == ' ') s++;
        bool yes = (tolower((unsigned char)s[0]) == 'y') || strncasecmp(s, "true", 4) == 0 || s[0] == '1';
        free(line);
        return v_bool(yes);
    }
    case NF_IO_READ_ALL: {
        size_t cap = 4096, len = 0; char *buf = malloc(cap);
        int ch;
        while ((ch = fgetc(stdin)) != EOF) {
            if (len + 2 > cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = (char)ch;
        }
        buf[len] = 0;
        Value v = mktextn(vm, buf, (int)len); free(buf); return v;
    }
    case NF_IO_FLUSH: fflush(stdout); return v_nil();

    /* ----------------------------------------------------------- math */
    case NF_MATH_SQRT: return v_num(sqrt(NUM(0)));
    case NF_MATH_ABS: { Value v = A(0); return v.tag == V_INT ? v_int(v.as.i < 0 ? -v.as.i : v.as.i) : v_num(fabs(v_tonum(v))); }
    case NF_MATH_FLOOR: return v_int((int64_t)floor(NUM(0)));
    case NF_MATH_CEIL: return v_int((int64_t)ceil(NUM(0)));
    case NF_MATH_ROUND: return v_int((int64_t)llround(NUM(0)));
    case NF_MATH_POW: return v_num(pow(NUM(0), NUM(1)));
    case NF_MATH_SIN: return v_num(sin(NUM(0)));
    case NF_MATH_COS: return v_num(cos(NUM(0)));
    case NF_MATH_TAN: return v_num(tan(NUM(0)));
    case NF_MATH_ATAN2: return v_num(atan2(NUM(0), NUM(1)));
    case NF_MATH_LOG: return v_num(log(NUM(0)));
    case NF_MATH_LOG2: return v_num(log2(NUM(0)));
    case NF_MATH_LOG10: return v_num(log10(NUM(0)));
    case NF_MATH_EXP: return v_num(exp(NUM(0)));
    case NF_MATH_MIN: { Value a = A(0), b = A(1);
        if (a.tag == V_INT && b.tag == V_INT) return v_int(a.as.i < b.as.i ? a.as.i : b.as.i);
        return v_num(fmin(v_tonum(a), v_tonum(b))); }
    case NF_MATH_MAX: { Value a = A(0), b = A(1);
        if (a.tag == V_INT && b.tag == V_INT) return v_int(a.as.i > b.as.i ? a.as.i : b.as.i);
        return v_num(fmax(v_tonum(a), v_tonum(b))); }
    case NF_MATH_CLAMP: { double x = NUM(0), lo = NUM(1), hi = NUM(2);
        double r = x < lo ? lo : x > hi ? hi : x;
        return (A(0).tag == V_INT && A(1).tag == V_INT && A(2).tag == V_INT) ? v_int((int64_t)r) : v_num(r); }
    case NF_MATH_PI: return v_num(3.14159265358979323846);
    case NF_MATH_E: return v_num(2.71828182845904523536);
    case NF_MATH_INF: return v_num(INFINITY);
    case NF_MATH_IS_NAN: return v_bool(isnan(NUM(0)));
    case NF_MATH_HYPOT: return v_num(hypot(NUM(0), NUM(1)));

    /* ----------------------------------------------------------- text */
    case NF_TEXT_LEN: return v_int(IS_OBJ(A(0), O_TEXT) ? AS_TEXT(A(0))->len : 0);
    case NF_TEXT_CHARS: {
        ObjList *l = vm_list(vm);
        const char *s = txt(A(0));
        int n = (int)strlen(s), i = 0;
        while (i < n) {
            int k = 1;
            unsigned char c = (unsigned char)s[i];
            if (c >= 0xF0) k = 4; else if (c >= 0xE0) k = 3; else if (c >= 0xC0) k = 2;
            if (i + k > n) k = 1;
            vec_push(&l->items, mktextn(vm, s + i, k));
            i += k;
        }
        return v_obj((Obj *)l);
    }
    case NF_TEXT_UPPER: case NF_TEXT_LOWER: {
        const char *s = txt(A(0));
        int n = (int)strlen(s);
        char *b = malloc((size_t)n + 1);
        for (int i = 0; i < n; i++) b[i] = (char)(id == NF_TEXT_UPPER ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]));
        b[n] = 0;
        Value v = mktextn(vm, b, n);
        free(b);
        return v;
    }
    case NF_TEXT_TRIM: {
        const char *s = txt(A(0));
        int n = (int)strlen(s), a = 0;
        while (a < n && isspace((unsigned char)s[a])) a++;
        while (n > a && isspace((unsigned char)s[n-1])) n--;
        return mktextn(vm, s + a, n - a);
    }
    case NF_TEXT_SPLIT: {
        const char *s = txt(A(0)), *sep = txt(A(1));
        ObjList *l = vm_list(vm);
        if (!*sep) { vec_push(&l->items, mktext(vm, s)); return v_obj((Obj *)l); }
        size_t sl = strlen(sep);
        const char *p = s, *q;
        while ((q = strstr(p, sep))) { vec_push(&l->items, mktextn(vm, p, (int)(q - p))); p = q + sl; }
        vec_push(&l->items, mktext(vm, p));
        return v_obj((Obj *)l);
    }
    case NF_TEXT_LINES: {
        const char *s = txt(A(0));
        ObjList *l = vm_list(vm);
        const char *p = s;
        for (;;) {
            const char *q = strchr(p, '\n');
            if (!q) { if (*p || l->items.len == 0) vec_push(&l->items, mktext(vm, p)); break; }
            int len = (int)(q - p);
            if (len && p[len-1] == '\r') len--;
            vec_push(&l->items, mktextn(vm, p, len));
            p = q + 1;
        }
        return v_obj((Obj *)l);
    }
    case NF_TEXT_CONTAINS: return v_bool(strstr(txt(A(0)), txt(A(1))) != NULL);
    case NF_TEXT_STARTS: { const char *s = txt(A(0)), *p = txt(A(1)); return v_bool(strncmp(s, p, strlen(p)) == 0); }
    case NF_TEXT_ENDS: {
        const char *s = txt(A(0)), *p = txt(A(1));
        size_t ls = strlen(s), lp = strlen(p);
        return v_bool(lp <= ls && strcmp(s + ls - lp, p) == 0);
    }
    case NF_TEXT_REPLACE: {
        const char *s = txt(A(0)), *from = txt(A(1)), *to = txt(A(2));
        if (!*from) return A(0);
        StrBuf b; sb_init(&b);
        size_t lf = strlen(from);
        const char *p = s, *q;
        while ((q = strstr(p, from))) { sb_put(&b, p, (size_t)(q - p)); sb_puts(&b, to); p = q + lf; }
        sb_puts(&b, p);
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }
    case NF_TEXT_SLICE: {
        const char *s = txt(A(0));
        int n = (int)strlen(s);
        int64_t a = INT(1), b = INT(2);
        if (a < 0) a += n;
        if (b < 0) b += n;
        if (a < 0) a = 0;
        if (b > n) b = n;
        if (b < a) b = a;
        return mktextn(vm, s + a, (int)(b - a));
    }
    case NF_TEXT_INDEX_OF: { const char *s = txt(A(0)); const char *q = strstr(s, txt(A(1))); return v_int(q ? (int64_t)(q - s) : -1); }
    case NF_TEXT_REPEAT: {
        const char *s = txt(A(0));
        int64_t k = INT(1);
        if (k < 0) k = 0;
        size_t ls = strlen(s);
        char *b = malloc(ls * (size_t)k + 1);
        for (int64_t i = 0; i < k; i++) memcpy(b + (size_t)i * ls, s, ls);
        b[ls * (size_t)k] = 0;
        Value v = mktextn(vm, b, (int)(ls * (size_t)k));
        free(b);
        return v;
    }
    case NF_TEXT_PAD_LEFT: case NF_TEXT_PAD_RIGHT: {
        const char *s = txt(A(0));
        int width = (int)INT(1);
        const char *pad = nargs > 2 ? txt(A(2)) : " ";
        if (!*pad) pad = " ";
        int n = (int)strlen(s);
        StrBuf b; sb_init(&b);
        if (id == NF_TEXT_PAD_RIGHT) sb_puts(&b, s);
        while ((int)b.len + (id == NF_TEXT_PAD_RIGHT ? 0 : n) < width) sb_puts(&b, pad);
        if (id == NF_TEXT_PAD_LEFT) sb_puts(&b, s);
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }
    case NF_TEXT_TO_INT: {
        const char *s = txt(A(0));
        char *end = NULL;
        long long r = strtoll(s, &end, 10);
        if (end == s || !end) return v_nil();
        while (*end == ' ') end++;
        return *end ? v_nil() : v_int(r);
    }
    case NF_TEXT_TO_NUM: {
        const char *s = txt(A(0));
        char *end = NULL;
        double r = strtod(s, &end);
        if (end == s || !end) return v_nil();
        while (*end == ' ') end++;
        return *end ? v_nil() : v_num(r);
    }
    case NF_TEXT_REVERSE: {
        const char *s = txt(A(0));
        int n = (int)strlen(s);
        char *b = malloc((size_t)n + 1);
        for (int i = 0; i < n; i++) b[i] = s[n - 1 - i];
        b[n] = 0;
        Value v = mktextn(vm, b, n);
        free(b);
        return v;
    }
    case NF_TEXT_CODE_AT: { const char *s = txt(A(0)); int64_t i = INT(1);
        return (i >= 0 && i < (int64_t)strlen(s)) ? v_int((unsigned char)s[i]) : v_int(-1); }
    case NF_TEXT_CHAR_AT: { const char *s = txt(A(0)); int64_t i = INT(1);
        return (i >= 0 && i < (int64_t)strlen(s)) ? mktextn(vm, s + i, 1) : mktext(vm, ""); }
    case NF_TEXT_IS_EMPTY: return v_bool(strlen(txt(A(0))) == 0);
    case NF_TEXT_BYTES: {
        const char *s = txt(A(0));
        ObjList *l = vm_list(vm);
        for (int i = 0; s[i]; i++) vec_push(&l->items, v_int((unsigned char)s[i]));
        return v_obj((Obj *)l);
    }
    case NF_TEXT_TO_TEXT: return A(0);

    /* ----------------------------------------------------------- list */
    case NF_LIST_LEN: return v_int(IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0))->items.len : 0);
    case NF_LIST_PUSH: { if (IS_OBJ(A(0), O_LIST)) vec_push(&AS_LIST(A(0))->items, A(1)); return v_nil(); }
    case NF_LIST_POP: {
        if (!IS_OBJ(A(0), O_LIST)) return v_nil();
        ObjList *l = AS_LIST(A(0));
        if (!l->items.len) return v_nil();
        return l->items.items[--l->items.len];
    }
    case NF_LIST_INSERT: {
        if (!IS_OBJ(A(0), O_LIST)) return v_nil();
        ObjList *l = AS_LIST(A(0));
        int64_t i = INT(1);
        if (i < 0) i = 0;
        if (i > l->items.len) i = l->items.len;
        vec_push(&l->items, v_nil());
        for (int k = l->items.len - 1; k > i; k--) l->items.items[k] = l->items.items[k - 1];
        l->items.items[i] = A(2);
        return v_nil();
    }
    case NF_LIST_REMOVE: {
        if (!IS_OBJ(A(0), O_LIST)) return v_nil();
        ObjList *l = AS_LIST(A(0));
        int64_t i = INT(1);
        if (i < 0) i += l->items.len;
        if (i < 0 || i >= l->items.len) return v_nil();
        Value out = l->items.items[i];
        for (int k = (int)i; k < l->items.len - 1; k++) l->items.items[k] = l->items.items[k + 1];
        l->items.len--;
        return out;
    }
    case NF_LIST_CLEAR: { if (IS_OBJ(A(0), O_LIST)) AS_LIST(A(0))->items.len = 0; return v_nil(); }
    case NF_LIST_CONTAINS: {
        if (!IS_OBJ(A(0), O_LIST)) return v_bool(false);
        ObjList *l = AS_LIST(A(0));
        vec_foreach(i, &l->items) if (vm_values_equal(l->items.items[i], A(1))) return v_bool(true);
        return v_bool(false);
    }
    case NF_LIST_INDEX_OF: {
        if (!IS_OBJ(A(0), O_LIST)) return v_int(-1);
        ObjList *l = AS_LIST(A(0));
        vec_foreach(i, &l->items) if (vm_values_equal(l->items.items[i], A(1))) return v_int(i);
        return v_int(-1);
    }
    case NF_LIST_FIRST: { ObjList *l = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        return (l && l->items.len) ? l->items.items[0] : v_nil(); }
    case NF_LIST_LAST: { ObjList *l = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        return (l && l->items.len) ? l->items.items[l->items.len - 1] : v_nil(); }
    case NF_LIST_SLICE: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (src) {
            int64_t a = INT(1), b = INT(2);
            if (a < 0) a += src->items.len;
            if (b < 0) b += src->items.len;
            if (a < 0) a = 0;
            if (b > src->items.len) b = src->items.len;
            for (int64_t i = a; i < b; i++) vec_push(&out->items, src->items.items[i]);
        }
        return v_obj((Obj *)out);
    }
    case NF_LIST_REVERSE: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (src) for (int i = src->items.len - 1; i >= 0; i--) vec_push(&out->items, src->items.items[i]);
        return v_obj((Obj *)out);
    }
    case NF_LIST_SORT: case NF_LIST_SORT_BY: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (!src) return v_obj((Obj *)out);
        vec_foreach(i, &src->items) vec_push(&out->items, src->items.items[i]);
        /* insertion sort keeps the comparison callback simple and stable */
        for (int i = 1; i < out->items.len; i++) {
            Value key = out->items.items[i];
            int k = i - 1;
            while (k >= 0) {
                bool greater;
                if (id == NF_LIST_SORT_BY && nargs > 1) {
                    /* the callback answers "does a come before b" */
                    Value cargs[2] = { key, out->items.items[k] };
                    Value r = vm_call_value(vm, A(1), cargs, 2);
                    greater = v_truthy(r);
                } else {
                    Value a2 = out->items.items[k];
                    if ((a2.tag == V_INT || a2.tag == V_NUM) && (key.tag == V_INT || key.tag == V_NUM))
                        greater = v_tonum(a2) > v_tonum(key);
                    else if (IS_OBJ(a2, O_TEXT) && IS_OBJ(key, O_TEXT))
                        greater = strcmp(AS_TEXT(a2)->chars, AS_TEXT(key)->chars) > 0;
                    else greater = false;
                }
                if (!greater) break;
                out->items.items[k + 1] = out->items.items[k];
                k--;
            }
            out->items.items[k + 1] = key;
        }
        return v_obj((Obj *)out);
    }
    case NF_LIST_MAP: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (src) vec_foreach(i, &src->items) {
            Value a1 = src->items.items[i];
            vec_push(&out->items, vm_call_value(vm, A(1), &a1, 1));
        }
        return v_obj((Obj *)out);
    }
    case NF_LIST_FILTER: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (src) vec_foreach(i, &src->items) {
            Value a1 = src->items.items[i];
            if (v_truthy(vm_call_value(vm, A(1), &a1, 1))) vec_push(&out->items, a1);
        }
        return v_obj((Obj *)out);
    }
    case NF_LIST_REDUCE: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        Value acc = A(1);
        if (src) vec_foreach(i, &src->items) {
            Value cargs[2] = { acc, src->items.items[i] };
            acc = vm_call_value(vm, A(2), cargs, 2);
        }
        return acc;
    }
    case NF_LIST_EACH: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        if (src) vec_foreach(i, &src->items) {
            Value a1 = src->items.items[i];
            vm_call_value(vm, A(1), &a1, 1);
        }
        return v_nil();
    }
    case NF_LIST_ANY: case NF_LIST_ALL: case NF_LIST_COUNT: case NF_LIST_FIND: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        int64_t count = 0;
        if (src) vec_foreach(i, &src->items) {
            Value a1 = src->items.items[i];
            bool r = v_truthy(vm_call_value(vm, A(1), &a1, 1));
            if (r) {
                if (id == NF_LIST_ANY) return v_bool(true);
                if (id == NF_LIST_FIND) return a1;
                count++;
            } else if (id == NF_LIST_ALL) return v_bool(false);
        }
        if (id == NF_LIST_ANY) return v_bool(false);
        if (id == NF_LIST_ALL) return v_bool(true);
        if (id == NF_LIST_FIND) return v_nil();
        return v_int(count);
    }
    case NF_LIST_JOIN: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        const char *sep = txt(A(1));
        StrBuf b; sb_init(&b);
        if (src) vec_foreach(i, &src->items) {
            if (i) sb_puts(&b, sep);
            char *s = vm_value_text(vm, src->items.items[i], false);
            sb_puts(&b, s);
            free(s);
        }
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }
    case NF_LIST_SUM: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        bool allint = true;
        double total = 0;
        if (src) vec_foreach(i, &src->items) {
            Value v = src->items.items[i];
            if (v.tag != V_INT) allint = false;
            total += v_tonum(v);
        }
        return allint ? v_int((int64_t)total) : v_num(total);
    }
    case NF_LIST_MIN: case NF_LIST_MAX: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        if (!src || !src->items.len) return v_nil();
        Value best = src->items.items[0];
        vec_foreach(i, &src->items) {
            Value v = src->items.items[i];
            bool better;
            if (IS_OBJ(v, O_TEXT) && IS_OBJ(best, O_TEXT))
                better = id == NF_LIST_MIN ? strcmp(AS_TEXT(v)->chars, AS_TEXT(best)->chars) < 0
                                           : strcmp(AS_TEXT(v)->chars, AS_TEXT(best)->chars) > 0;
            else better = id == NF_LIST_MIN ? v_tonum(v) < v_tonum(best) : v_tonum(v) > v_tonum(best);
            if (better) best = v;
        }
        return best;
    }
    case NF_LIST_COPY: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (src) vec_foreach(i, &src->items) vec_push(&out->items, src->items.items[i]);
        return v_obj((Obj *)out);
    }
    case NF_LIST_CONCAT: {
        ObjList *out = vm_list(vm);
        if (IS_OBJ(A(0), O_LIST)) vec_foreach(i, &AS_LIST(A(0))->items) vec_push(&out->items, AS_LIST(A(0))->items.items[i]);
        if (IS_OBJ(A(1), O_LIST)) vec_foreach(i, &AS_LIST(A(1))->items) vec_push(&out->items, AS_LIST(A(1))->items.items[i]);
        return v_obj((Obj *)out);
    }
    case NF_LIST_TAKE: case NF_LIST_DROP: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        int64_t k = INT(1);
        if (src) vec_foreach(i, &src->items) {
            bool keep = id == NF_LIST_TAKE ? (i < k) : (i >= k);
            if (keep) vec_push(&out->items, src->items.items[i]);
        }
        return v_obj((Obj *)out);
    }
    case NF_LIST_IS_EMPTY: return v_bool(!IS_OBJ(A(0), O_LIST) || AS_LIST(A(0))->items.len == 0);
    case NF_LIST_FLATTEN: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (src) vec_foreach(i, &src->items) {
            Value v = src->items.items[i];
            if (IS_OBJ(v, O_LIST)) vec_foreach(k, &AS_LIST(v)->items) vec_push(&out->items, AS_LIST(v)->items.items[k]);
            else vec_push(&out->items, v);
        }
        return v_obj((Obj *)out);
    }

    /* ------------------------------------------------------------ map */
    case NF_MAP_LEN: return v_int(IS_OBJ(A(0), O_MAP) ? AS_MAP(A(0))->keys.len : 0);
    case NF_MAP_GET: { if (!IS_OBJ(A(0), O_MAP)) return v_nil();
        bool found = false; Value v = vm_map_get(AS_MAP(A(0)), A(1), &found); return found ? v : v_nil(); }
    case NF_MAP_SET: { if (IS_OBJ(A(0), O_MAP)) vm_map_set(vm, AS_MAP(A(0)), A(1), A(2)); return v_nil(); }
    case NF_MAP_HAS: { if (!IS_OBJ(A(0), O_MAP)) return v_bool(false);
        bool found = false; vm_map_get(AS_MAP(A(0)), A(1), &found); return v_bool(found); }
    case NF_MAP_REMOVE: { if (!IS_OBJ(A(0), O_MAP)) return v_nil();
        Value out = v_nil(); vm_map_remove(AS_MAP(A(0)), A(1), &out); return out; }
    case NF_MAP_KEYS: case NF_MAP_VALUES: {
        ObjList *l = vm_list(vm);
        if (IS_OBJ(A(0), O_MAP)) {
            ObjMap *m = AS_MAP(A(0));
            ValueVec *src = id == NF_MAP_KEYS ? &m->keys : &m->vals;
            vec_foreach(i, src) vec_push(&l->items, src->items[i]);
        }
        return v_obj((Obj *)l);
    }
    case NF_MAP_CLEAR: { if (IS_OBJ(A(0), O_MAP)) { AS_MAP(A(0))->keys.len = 0; AS_MAP(A(0))->vals.len = 0; } return v_nil(); }
    case NF_MAP_IS_EMPTY: return v_bool(!IS_OBJ(A(0), O_MAP) || AS_MAP(A(0))->keys.len == 0);

    /* ------------------------------------------------------------ set */
    case NF_SET_ADD: {
        if (!IS_OBJ(A(0), O_SET)) return v_nil();
        ObjSet *s = AS_SET(A(0));
        vec_foreach(i, &s->items) if (vm_values_equal(s->items.items[i], A(1))) return v_nil();
        vec_push(&s->items, A(1));
        return v_nil();
    }
    case NF_SET_HAS: {
        if (!IS_OBJ(A(0), O_SET)) return v_bool(false);
        ObjSet *s = AS_SET(A(0));
        vec_foreach(i, &s->items) if (vm_values_equal(s->items.items[i], A(1))) return v_bool(true);
        return v_bool(false);
    }
    case NF_SET_REMOVE: {
        if (!IS_OBJ(A(0), O_SET)) return v_bool(false);
        ObjSet *s = AS_SET(A(0));
        vec_foreach(i, &s->items) if (vm_values_equal(s->items.items[i], A(1))) {
            for (int k = i; k < s->items.len - 1; k++) s->items.items[k] = s->items.items[k + 1];
            s->items.len--;
            return v_bool(true);
        }
        return v_bool(false);
    }
    case NF_SET_LEN: return v_int(IS_OBJ(A(0), O_SET) ? AS_SET(A(0))->items.len : 0);
    case NF_SET_ITEMS: {
        ObjList *l = vm_list(vm);
        if (IS_OBJ(A(0), O_SET)) vec_foreach(i, &AS_SET(A(0))->items) vec_push(&l->items, AS_SET(A(0))->items.items[i]);
        return v_obj((Obj *)l);
    }

    /* --------------------------------------------------------- future */
    case NF_FUT_AWAIT: {
        if (!IS_OBJ(A(0), O_FUTURE)) return A(0);
        ObjFuture *f = AS_FUTURE(A(0));
        if (!f->done && f->fn >= 0) { f->result = vm_call_function(vm, f->fn, f->args.items, f->args.len); f->done = true; }
        return f->result;
    }
    case NF_FUT_DONE: return v_bool(IS_OBJ(A(0), O_FUTURE) ? AS_FUTURE(A(0))->done : true);

    /* ----------------------------------------------------------- chan */
    case NF_CHAN_SEND: { if (IS_OBJ(A(0), O_CHAN)) vec_push(&AS_CHAN(A(0))->buffer, A(1)); return v_nil(); }
    case NF_CHAN_RECV: case NF_CHAN_TRY_RECV: {
        if (!IS_OBJ(A(0), O_CHAN)) return v_nil();
        ObjChan *c = AS_CHAN(A(0));
        if (!c->buffer.len) return v_nil();
        Value v = c->buffer.items[0];
        for (int i = 0; i < c->buffer.len - 1; i++) c->buffer.items[i] = c->buffer.items[i + 1];
        c->buffer.len--;
        return v;
    }
    case NF_CHAN_CLOSE: { if (IS_OBJ(A(0), O_CHAN)) AS_CHAN(A(0))->closed = true; return v_nil(); }
    case NF_CHAN_LEN: return v_int(IS_OBJ(A(0), O_CHAN) ? AS_CHAN(A(0))->buffer.len : 0);

    /* -------------------------------------------------------- builtins */
    case NF_B_LEN: {
        Value v = A(0);
        if (IS_OBJ(v, O_LIST)) return v_int(AS_LIST(v)->items.len);
        if (IS_OBJ(v, O_TEXT)) return v_int(AS_TEXT(v)->len);
        if (IS_OBJ(v, O_MAP)) return v_int(AS_MAP(v)->keys.len);
        if (IS_OBJ(v, O_SET)) return v_int(AS_SET(v)->items.len);
        if (IS_OBJ(v, O_TENSOR)) return v_int(AS_TENSOR(v)->count);
        return v_int(0);
    }
    case NF_B_TO_TEXT: { char *s = vm_value_text(vm, A(0), false); Value v = mktext(vm, s); free(s); return v; }
    case NF_B_TO_INT: {
        Value v = A(0);
        if (IS_OBJ(v, O_TEXT)) return v_int(strtoll(AS_TEXT(v)->chars, NULL, 10));
        return v_int(v_toint(v));
    }
    case NF_B_TO_NUM: {
        Value v = A(0);
        if (IS_OBJ(v, O_TEXT)) return v_num(strtod(AS_TEXT(v)->chars, NULL));
        return v_num(v_tonum(v));
    }
    case NF_B_TYPE_OF: {
        Value v = A(0);
        const char *n = "Nil";
        switch (v.tag) {
            case V_INT: n = "Int"; break;
            case V_NUM: n = "Num"; break;
            case V_BOOL: n = "Bool"; break;
            case V_NIL: n = "Nil"; break;
            case V_OBJ:
                switch (v.as.o->kind) {
                    case O_TEXT: n = "Text"; break; case O_LIST: n = "List"; break;
                    case O_MAP: n = "Map"; break; case O_SET: n = "Set"; break;
                    case O_CLOSURE: n = "Fn"; break;
                    case O_INSTANCE: n = ((ObjInstance *)v.as.o)->tname; break;
                    case O_VARIANT: n = ((ObjVariant *)v.as.o)->vname; break;
                    case O_RESULT: n = "Result"; break; case O_FUTURE: n = "Future"; break;
                    case O_CHAN: n = "Chan"; break; case O_TENSOR: n = "Tensor"; break;
                    case O_IMAGE: n = "Image"; break; default: n = "Any";
                }
                break;
        }
        return mktext(vm, n);
    }
    case NF_B_ASSERT: {
        if (!v_truthy(A(0))) {
            const char *m = nargs > 1 ? txt(A(1)) : "assertion failed";
            vm_runtime_error(vm, "%s", m);
            *ok = false;
        }
        return v_nil();
    }
    case NF_B_PANIC: { vm_runtime_error(vm, "%s", txt(A(0))); *ok = false; return v_nil(); }
    case NF_B_CLONE: {
        Value v = A(0);
        if (IS_OBJ(v, O_LIST)) {
            ObjList *out = vm_list(vm);
            vec_foreach(i, &AS_LIST(v)->items) vec_push(&out->items, AS_LIST(v)->items.items[i]);
            return v_obj((Obj *)out);
        }
        if (IS_OBJ(v, O_MAP)) {
            ObjMap *out = vm_map(vm);
            ObjMap *m = AS_MAP(v);
            vec_foreach(i, &m->keys) vm_map_set(vm, out, m->keys.items[i], m->vals.items[i]);
            return v_obj((Obj *)out);
        }
        if (IS_OBJ(v, O_INSTANCE)) return v_obj((Obj *)vm_instance_copy(vm, AS_INSTANCE(v)));
        return v;
    }
    case NF_B_CHAR_OF: {
        int64_t cp = INT(0);
        char buf[4];
        int n = 0;
        if (cp < 0 || cp > 0x10FFFF) cp = 0xFFFD;
        if (cp < 0x80) buf[n++] = (char)cp;
        else if (cp < 0x800) {
            buf[n++] = (char)(0xC0 | (cp >> 6));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[n++] = (char)(0xE0 | (cp >> 12));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[n++] = (char)(0xF0 | (cp >> 18));
            buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        }
        return mktextn(vm, buf, n);
    }
    case NF_B_HASH: return v_int((int64_t)(vm_value_hash(A(0)) & 0x7fffffffffffffffULL));
    case NF_B_SET: {
        ObjSet *st = vm_set(vm);
        Value v = A(0);
        if (IS_OBJ(v, O_LIST)) {
            vec_foreach(i, &AS_LIST(v)->items) {
                Value item = AS_LIST(v)->items.items[i];
                bool found = false;
                vec_foreach(k, &st->items) if (vm_values_equal(st->items.items[k], item)) found = true;
                if (!found) vec_push(&st->items, item);
            }
        }
        return v_obj((Obj *)st);
    }
    case NF_B_RANGE_LIST: {
        ObjList *l = vm_list(vm);
        for (int64_t i = INT(0); i < INT(1); i++) vec_push(&l->items, v_int(i));
        return v_obj((Obj *)l);
    }

    /* ------------------------------------------------------------- fs */
    case NF_FS_READ: { size_t n = 0; char *s = read_file(txt(A(0)), &n);
        if (!s) return v_nil();
        Value v = mktextn(vm, s, (int)n); free(s); return v; }
    case NF_FS_WRITE: { const char *p = txt(A(0)); const char *d = txt(A(1));
        return v_bool(write_file_bytes(p, d, strlen(d))); }
    case NF_FS_APPEND: { FILE *f = fopen(txt(A(0)), "ab"); if (!f) return v_bool(false);
        const char *d = txt(A(1)); fwrite(d, 1, strlen(d), f); fclose(f); return v_bool(true); }
    case NF_FS_EXISTS: { struct stat st; return v_bool(stat(txt(A(0)), &st) == 0); }
    case NF_FS_REMOVE: return v_bool(remove(txt(A(0))) == 0);
    case NF_FS_LIST: {
        int n = 0;
        char **names = list_dir(txt(A(0)), &n, NULL);
        Value v = names ? list_of_strings(vm, names, n) : v_obj((Obj *)vm_list(vm));
        if (names) { for (int i = 0; i < n; i++) free(names[i]); free(names); }
        return v;
    }
    case NF_FS_MKDIR: return v_bool(make_dir_all(txt(A(0))));
    case NF_FS_IS_DIR: return v_bool(dir_exists(txt(A(0))));
    case NF_FS_IS_FILE: return v_bool(file_exists(txt(A(0))));
    case NF_FS_SIZE: { struct stat st; return v_int(stat(txt(A(0)), &st) == 0 ? (int64_t)st.st_size : -1); }
    case NF_FS_COPY: {
        size_t n = 0;
        char *s = read_file(txt(A(0)), &n);
        if (!s) return v_bool(false);
        bool r = write_file_bytes(txt(A(1)), s, n);
        free(s);
        return v_bool(r);
    }
    case NF_FS_RENAME: return v_bool(rename(txt(A(0)), txt(A(1))) == 0);
    case NF_FS_READ_BYTES: {
        size_t n = 0;
        char *s = read_file(txt(A(0)), &n);
        if (!s) return v_nil();
        ObjList *l = vm_list(vm);
        for (size_t i = 0; i < n; i++) vec_push(&l->items, v_int((unsigned char)s[i]));
        free(s);
        return v_obj((Obj *)l);
    }
    case NF_FS_WRITE_BYTES: {
        if (!IS_OBJ(A(1), O_LIST)) return v_bool(false);
        ObjList *l = AS_LIST(A(1));
        unsigned char *buf = malloc((size_t)l->items.len + 1);
        vec_foreach(i, &l->items) buf[i] = (unsigned char)v_toint(l->items.items[i]);
        bool r = write_file_bytes(txt(A(0)), buf, (size_t)l->items.len);
        free(buf);
        return v_bool(r);
    }

    /* ----------------------------------------------------------- path */
    case NF_PATH_JOIN: {
        const char *a = txt(A(0)), *b = txt(A(1));
        StrBuf s; sb_init(&s);
        sb_puts(&s, a);
        if (s.len && s.data[s.len-1] != '/' && *b != '/') sb_putc(&s, '/');
        sb_puts(&s, b);
        Value v = mktextn(vm, s.data, (int)s.len);
        sb_free(&s);
        return v;
    }
    case NF_PATH_DIR: { const char *p = txt(A(0)); const char *q = strrchr(p, '/');
        return q ? mktextn(vm, p, (int)(q - p)) : mktext(vm, "."); }
    case NF_PATH_BASE: return mktext(vm, path_basename(txt(A(0))));
    case NF_PATH_EXT: return mktext(vm, path_ext(txt(A(0))));
    case NF_PATH_ABS: { char buf[4096]; const char *p = txt(A(0));
        if (p[0] == '/') return mktext(vm, p);
        if (!getcwd(buf, sizeof buf)) return mktext(vm, p);
        StrBuf s; sb_init(&s); sb_puts(&s, buf); sb_putc(&s, '/'); sb_puts(&s, p);
        Value v = mktextn(vm, s.data, (int)s.len); sb_free(&s); return v; }

    /* ----------------------------------------------------------- time */
    case NF_TIME_NOW: return v_num(now_seconds());
    case NF_TIME_MS: return v_int((int64_t)(now_seconds() * 1000.0));
    case NF_TIME_CLOCK: return v_num(now_seconds());
    case NF_TIME_SLEEP: {
        double secs = NUM(0);
        sprfst_unlock();
        struct timespec ts;
        ts.tv_sec = (time_t)secs;
        ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
        sprfst_lock();
        return v_nil();
    }
    case NF_TIME_FORMAT: {
        time_t t = (time_t)NUM(0);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char buf[256];
        const char *fmt = nargs > 1 ? txt(A(1)) : "%Y-%m-%d %H:%M:%S";
        strftime(buf, sizeof buf, fmt, &tmv);
        return mktext(vm, buf);
    }
    case NF_TIME_PARTS: {
        time_t t = (time_t)(nargs ? NUM(0) : now_seconds());
        struct tm tmv;
        localtime_r(&t, &tmv);
        ObjMap *m = vm_map(vm);
        vm_map_set(vm, m, mktext(vm, "year"), v_int(tmv.tm_year + 1900));
        vm_map_set(vm, m, mktext(vm, "month"), v_int(tmv.tm_mon + 1));
        vm_map_set(vm, m, mktext(vm, "day"), v_int(tmv.tm_mday));
        vm_map_set(vm, m, mktext(vm, "hour"), v_int(tmv.tm_hour));
        vm_map_set(vm, m, mktext(vm, "minute"), v_int(tmv.tm_min));
        vm_map_set(vm, m, mktext(vm, "second"), v_int(tmv.tm_sec));
        vm_map_set(vm, m, mktext(vm, "weekday"), v_int(tmv.tm_wday));
        return v_obj((Obj *)m);
    }

    /* ----------------------------------------------------------- rand */
    case NF_RAND_SEED: g_rand_state = (uint64_t)INT(0) * 6364136223846793005ULL + 1442695040888963407ULL; return v_nil();
    case NF_RAND_INT: { int64_t lo = INT(0), hi = INT(1);
        if (hi <= lo) return v_int(lo);
        return v_int(lo + (int64_t)(rnd64() % (uint64_t)(hi - lo))); }
    case NF_RAND_NUM: return v_num(rnd_unit());
    case NF_RAND_CHOICE: {
        if (!IS_OBJ(A(0), O_LIST) || !AS_LIST(A(0))->items.len) return v_nil();
        ObjList *l = AS_LIST(A(0));
        return l->items.items[rnd64() % (uint64_t)l->items.len];
    }
    case NF_RAND_SHUFFLE: {
        ObjList *src = IS_OBJ(A(0), O_LIST) ? AS_LIST(A(0)) : NULL;
        ObjList *out = vm_list(vm);
        if (src) {
            vec_foreach(i, &src->items) vec_push(&out->items, src->items.items[i]);
            for (int i = out->items.len - 1; i > 0; i--) {
                int j = (int)(rnd64() % (uint64_t)(i + 1));
                Value t = out->items.items[i];
                out->items.items[i] = out->items.items[j];
                out->items.items[j] = t;
            }
        }
        return v_obj((Obj *)out);
    }
    case NF_RAND_NORMAL: {
        double u1 = rnd_unit(), u2 = rnd_unit();
        if (u1 < 1e-12) u1 = 1e-12;
        double z = sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979323846 * u2);
        return v_num(NUM(0) + z * NUM(1));
    }

    /* ------------------------------------------------------------ sys */
    case NF_SYS_ARGS: return list_of_strings(vm, g_argv, g_argc);
    case NF_SYS_ENV: { const char *e = getenv(txt(A(0))); return e ? mktext(vm, e) : v_nil(); }
    case NF_SYS_SET_ENV: setenv(txt(A(0)), txt(A(1)), 1); return v_nil();
    case NF_SYS_EXIT: { vm->exit_code = (int)INT(0); vm_runtime_error(vm, "__exit__"); *ok = false;
        fflush(stdout); exit((int)INT(0)); }
    case NF_SYS_PLATFORM:
#if defined(__APPLE__)
        return mktext(vm, "macos");
#elif defined(__linux__)
        return mktext(vm, "linux");
#else
        return mktext(vm, "unknown");
#endif
    case NF_SYS_ARCH:
#if defined(__aarch64__) || defined(__arm64__)
        return mktext(vm, "arm64");
#elif defined(__x86_64__)
        return mktext(vm, "x86_64");
#else
        return mktext(vm, "unknown");
#endif
    case NF_SYS_CPUS: return v_int((int64_t)sysconf(_SC_NPROCESSORS_ONLN));
    case NF_SYS_RUN: {
        sprfst_unlock();
        FILE *p = popen(txt(A(0)), "r");
        StrBuf b; sb_init(&b);
        if (p) {
            char buf[1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, p)) > 0) sb_put(&b, buf, n);
            pclose(p);
        }
        sprfst_lock();
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }
    case NF_SYS_MEMORY: return v_int((int64_t)vm->bytes_allocated);

    /* ----------------------------------------------------------- json */
    case NF_JSON_PARSE: {
        JP j = { txt(A(0)), vm, true };
        Value v = jp_value(&j);
        return j.ok ? v : v_nil();
    }
    case NF_JSON_WRITE: case NF_JSON_PRETTY: {
        StrBuf b; sb_init(&b);
        json_write_value(vm, A(0), &b, id == NF_JSON_PRETTY ? 1 : 0, 0);
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }

    /* ------------------------------------------------------------ csv */
    case NF_CSV_PARSE: {
        const char *s = txt(A(0));
        ObjList *rows = vm_list(vm);
        ObjList *row = vm_list(vm);
        StrBuf cell; sb_init(&cell);
        bool inq = false;
        for (const char *p = s; ; p++) {
            char c = *p;
            if (inq) {
                if (c == '"' && p[1] == '"') { sb_putc(&cell, '"'); p++; }
                else if (c == '"') inq = false;
                else if (!c) break;
                else sb_putc(&cell, c);
                continue;
            }
            if (c == '"') { inq = true; continue; }
            if (c == ',' || c == '\n' || c == 0) {
                vec_push(&row->items, mktextn(vm, cell.data ? cell.data : "", (int)cell.len));
                cell.len = 0;
                if (cell.data) cell.data[0] = 0;
                if (c != ',') {
                    vec_push(&rows->items, v_obj((Obj *)row));
                    if (!c) break;
                    row = vm_list(vm);
                }
                continue;
            }
            if (c == '\r') continue;
            sb_putc(&cell, c);
        }
        sb_free(&cell);
        return v_obj((Obj *)rows);
    }
    case NF_CSV_WRITE: {
        StrBuf b; sb_init(&b);
        if (IS_OBJ(A(0), O_LIST)) {
            ObjList *rows = AS_LIST(A(0));
            vec_foreach(r, &rows->items) {
                if (!IS_OBJ(rows->items.items[r], O_LIST)) continue;
                ObjList *row = AS_LIST(rows->items.items[r]);
                vec_foreach(c, &row->items) {
                    if (c) sb_putc(&b, ',');
                    char *s = vm_value_text(vm, row->items.items[c], false);
                    bool need = strchr(s, ',') || strchr(s, '"') || strchr(s, '\n');
                    if (need) {
                        sb_putc(&b, '"');
                        for (char *q = s; *q; q++) { if (*q == '"') sb_putc(&b, '"'); sb_putc(&b, *q); }
                        sb_putc(&b, '"');
                    } else sb_puts(&b, s);
                    free(s);
                }
                sb_putc(&b, '\n');
            }
        }
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }

    /* ----------------------------------------------------------- hash */
    case NF_HASH_SHA256: {
        const char *s = txt(A(0));
        char hex[65];
        sha256_hex(s, strlen(s), hex);
        return mktext(vm, hex);
    }
    case NF_HASH_CRC32: { const char *s = txt(A(0)); return v_int((int64_t)crc32_bytes((const unsigned char *)s, strlen(s), 0)); }
    case NF_HASH_FNV: { const char *s = txt(A(0)); return v_int((int64_t)(hash_bytes(s, strlen(s)) & 0x7fffffffffffffffULL)); }
    case NF_HASH_B64ENC: {
        const unsigned char *s = (const unsigned char *)txt(A(0));
        size_t n = strlen((const char *)s);
        StrBuf b; sb_init(&b);
        for (size_t i = 0; i < n; i += 3) {
            unsigned v = (unsigned)s[i] << 16;
            if (i + 1 < n) v |= (unsigned)s[i+1] << 8;
            if (i + 2 < n) v |= s[i+2];
            sb_putc(&b, B64[(v >> 18) & 63]);
            sb_putc(&b, B64[(v >> 12) & 63]);
            sb_putc(&b, i + 1 < n ? B64[(v >> 6) & 63] : '=');
            sb_putc(&b, i + 2 < n ? B64[v & 63] : '=');
        }
        Value r = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return r;
    }
    case NF_HASH_B64DEC: {
        const char *s = txt(A(0));
        int rev[256];
        for (int i = 0; i < 256; i++) rev[i] = -1;
        for (int i = 0; i < 64; i++) rev[(unsigned char)B64[i]] = i;
        StrBuf b; sb_init(&b);
        int acc = 0, bits = 0;
        for (const char *p = s; *p; p++) {
            int c = rev[(unsigned char)*p];
            if (c < 0) continue;
            acc = (acc << 6) | c;
            bits += 6;
            if (bits >= 8) { bits -= 8; sb_putc(&b, (char)((acc >> bits) & 0xff)); }
        }
        Value r = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return r;
    }
    case NF_HASH_HMAC: {
        const char *key = txt(A(0)), *msg = txt(A(1));
        unsigned char k[64] = { 0 };
        size_t kl = strlen(key);
        if (kl > 64) { unsigned char d[32]; sha256_raw((const unsigned char *)key, kl, d); memcpy(k, d, 32); }
        else memcpy(k, key, kl);
        unsigned char ipad[64], opad[64];
        for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
        size_t ml = strlen(msg);
        unsigned char *inner = malloc(64 + ml);
        memcpy(inner, ipad, 64);
        memcpy(inner + 64, msg, ml);
        unsigned char d1[32];
        sha256_raw(inner, 64 + ml, d1);
        free(inner);
        unsigned char outer[96];
        memcpy(outer, opad, 64);
        memcpy(outer + 64, d1, 32);
        unsigned char d2[32];
        sha256_raw(outer, 96, d2);
        char hex[65];
        sha256_hex(outer, 96, hex);
        return mktext(vm, hex);
    }

    /* ------------------------------------------------------------ net */
    case NF_NET_LISTEN: {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return v_int(-1);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)INT(0));
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return v_int(-1); }
        if (listen(fd, 64) < 0) { close(fd); return v_int(-1); }
        return v_int(sock_store(fd));
    }
    case NF_NET_ACCEPT: {
        int fd = sock_fd((int)INT(0));
        if (fd < 0) return v_int(-1);
        sprfst_unlock();
        int c = accept(fd, NULL, NULL);
        sprfst_lock();
        return v_int(c < 0 ? -1 : sock_store(c));
    }
    case NF_NET_CONNECT: {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port[16];
        snprintf(port, sizeof port, "%lld", (long long)INT(1));
        sprfst_unlock();
        int gai = getaddrinfo(txt(A(0)), port, &hints, &res);
        sprfst_lock();
        if (gai != 0 || !res) return v_int(-1);
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); return v_int(-1); }
        sprfst_unlock();
        int r = connect(fd, res->ai_addr, res->ai_addrlen);
        sprfst_lock();
        freeaddrinfo(res);
        if (r < 0) { close(fd); return v_int(-1); }
        return v_int(sock_store(fd));
    }
    case NF_NET_SEND: {
        int fd = sock_fd((int)INT(0));
        if (fd < 0) return v_int(-1);
        const char *d = txt(A(1));
        size_t n = strlen(d);
        ssize_t w = send(fd, d, n, 0);
        return v_int((int64_t)w);
    }
    case NF_NET_RECV: {
        int fd = sock_fd((int)INT(0));
        if (fd < 0) return mktext(vm, "");
        int want = (int)INT(1);
        if (want <= 0 || want > 1 << 20) want = 4096;
        char *buf = malloc((size_t)want + 1);
        sprfst_unlock();
        ssize_t n = recv(fd, buf, (size_t)want, 0);
        sprfst_lock();
        if (n < 0) n = 0;
        Value v = mktextn(vm, buf, (int)n);
        free(buf);
        return v;
    }
    case NF_NET_CLOSE: {
        int i = (int)INT(0);
        int fd = sock_fd(i);
        if (fd >= 0) { close(fd); g_socks[i] = -1; }
        return v_nil();
    }
    case NF_NET_RESOLVE: {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        sprfst_unlock();
        int gai = getaddrinfo(txt(A(0)), NULL, &hints, &res);
        sprfst_lock();
        if (gai != 0 || !res) return v_nil();
        char buf[64];
        struct sockaddr_in *in4 = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof buf);
        freeaddrinfo(res);
        return mktext(vm, buf);
    }
    case NF_NET_UDP_OPEN: {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return v_int(-1);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)INT(0));
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return v_int(-1); }
        return v_int(sock_store(fd));
    }
    case NF_NET_UDP_SEND: {
        int fd = sock_fd((int)INT(0));
        if (fd < 0) return v_int(-1);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)INT(2));
        inet_pton(AF_INET, txt(A(1)), &addr.sin_addr);
        const char *d = txt(A(3));
        ssize_t w = sendto(fd, d, strlen(d), 0, (struct sockaddr *)&addr, sizeof addr);
        return v_int((int64_t)w);
    }
    case NF_NET_UDP_RECV: {
        int fd = sock_fd((int)INT(0));
        if (fd < 0) return mktext(vm, "");
        int want = (int)INT(1);
        if (want <= 0) want = 2048;
        char *buf = malloc((size_t)want + 1);
        sprfst_unlock();
        ssize_t n = recvfrom(fd, buf, (size_t)want, 0, NULL, NULL);
        sprfst_lock();
        if (n < 0) n = 0;
        Value v = mktextn(vm, buf, (int)n);
        free(buf);
        return v;
    }
    case NF_NET_HOST: { char buf[256]; if (gethostname(buf, sizeof buf) != 0) return mktext(vm, "localhost"); return mktext(vm, buf); }

    /* ----------------------------------------------------------- http */
    case NF_HTTP_GET: case NF_HTTP_POST: {
        const char *url = txt(A(0));
        const char *scheme_end = strstr(url, "://");
        const char *hostpart = scheme_end ? scheme_end + 3 : url;
        bool https = strncmp(url, "https", 5) == 0;
        if (https) {
            /* TLS is provided by the platform transport layer; not built in yet */
            return v_nil();
        }
        char host[256] = { 0 };
        int port = 80;
        const char *slash = strchr(hostpart, '/');
        const char *path = slash ? slash : "/";
        size_t hl = slash ? (size_t)(slash - hostpart) : strlen(hostpart);
        if (hl >= sizeof host) hl = sizeof host - 1;
        memcpy(host, hostpart, hl);
        char *colon = strchr(host, ':');
        if (colon) { *colon = 0; port = atoi(colon + 1); }
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portbuf[16];
        snprintf(portbuf, sizeof portbuf, "%d", port);
        sprfst_unlock();
        int gai = getaddrinfo(host, portbuf, &hints, &res);
        if (gai != 0 || !res) { sprfst_lock(); return v_nil(); }
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            if (fd >= 0) close(fd);
            freeaddrinfo(res);
            sprfst_lock();
            return v_nil();
        }
        freeaddrinfo(res);
        StrBuf req; sb_init(&req);
        const char *body = (id == NF_HTTP_POST && nargs > 1) ? txt(A(1)) : "";
        sb_printf(&req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SPRFST/%s\r\nConnection: close\r\n",
                  id == NF_HTTP_GET ? "GET" : "POST", path, host, SPRFST_VERSION);
        if (id == NF_HTTP_POST)
            sb_printf(&req, "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(body));
        sb_puts(&req, "\r\n");
        if (id == NF_HTTP_POST) sb_puts(&req, body);
        send(fd, req.data, req.len, 0);
        sb_free(&req);
        StrBuf resp; sb_init(&resp);
        char buf[4096];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof buf, 0)) > 0) sb_put(&resp, buf, (size_t)n);
        close(fd);
        sprfst_lock();
        const char *sep = resp.data ? strstr(resp.data, "\r\n\r\n") : NULL;
        Value v = sep ? mktext(vm, sep + 4) : mktextn(vm, resp.data ? resp.data : "", (int)resp.len);
        sb_free(&resp);
        return v;
    }
    case NF_HTTP_SERVE: return http_serve_native(vm, args, nargs, ok);
    case NF_HTTP_STOP: g_http_running = false; return v_nil();
    case NF_HTTP_URLENC: {
        const char *s = txt(A(0));
        StrBuf b; sb_init(&b);
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') sb_putc(&b, (char)*p);
            else sb_printf(&b, "%%%02X", *p);
        }
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }
    case NF_HTTP_URLDEC: {
        const char *s = txt(A(0));
        StrBuf b; sb_init(&b);
        for (const char *p = s; *p; p++) {
            if (*p == '%' && p[1] && p[2]) {
                int hi = isdigit((unsigned char)p[1]) ? p[1] - '0' : tolower(p[1]) - 'a' + 10;
                int lo = isdigit((unsigned char)p[2]) ? p[2] - '0' : tolower(p[2]) - 'a' + 10;
                sb_putc(&b, (char)(hi * 16 + lo));
                p += 2;
            } else if (*p == '+') sb_putc(&b, ' ');
            else sb_putc(&b, *p);
        }
        Value v = mktextn(vm, b.data ? b.data : "", (int)b.len);
        sb_free(&b);
        return v;
    }

    /* --------------------------------------------------------- threads */
    case NF_THREAD_SPAWN: {
        ObjFuture *f = vm_future(vm);
        if (IS_OBJ(A(0), O_CLOSURE)) f->fn = AS_CLOSURE(A(0))->fn;
        Value v = v_obj((Obj *)f);
        vec_push(&vm->task_queue, v);
        return v;
    }
    case NF_THREAD_CPUS: return v_int((int64_t)sysconf(_SC_NPROCESSORS_ONLN));
    case NF_THREAD_SLEEP: {
        double secs = NUM(0);
        sprfst_unlock();
        struct timespec ts = { (time_t)secs, (long)((secs - (double)(time_t)secs) * 1e9) };
        nanosleep(&ts, NULL);
        sprfst_lock();
        return v_nil();
    }
    case NF_THREAD_LOCK: {
        if (g_nlocks >= 64) return v_int(-1);
        pthread_mutex_init(&g_locks[g_nlocks].m, NULL);
        g_locks[g_nlocks].used = true;
        return v_int(g_nlocks++);
    }
    case NF_THREAD_ACQ: { int i = (int)INT(0); if (i >= 0 && i < g_nlocks) pthread_mutex_lock(&g_locks[i].m); return v_nil(); }
    case NF_THREAD_REL: { int i = (int)INT(0); if (i >= 0 && i < g_nlocks) pthread_mutex_unlock(&g_locks[i].m); return v_nil(); }
    case NF_THREAD_COUNTER: { if (g_ncounters >= 64) return v_int(-1); g_counters[g_ncounters] = INT(0); return v_int(g_ncounters++); }
    case NF_THREAD_ATOMIC: {
        int i = (int)INT(0);
        if (i < 0 || i >= g_ncounters) return v_int(0);
        return v_int(__sync_add_and_fetch(&g_counters[i], INT(1)));
    }
    case NF_THREAD_CHAN: return v_obj((Obj *)vm_chan(vm, (int)INT(0)));

    /* ------------------------------------------------------------ task */
    case NF_TASK_SLEEP: {
        double secs = NUM(0);
        sprfst_unlock();
        struct timespec ts = { (time_t)secs, (long)((secs - (double)(time_t)secs) * 1e9) };
        nanosleep(&ts, NULL);
        sprfst_lock();
        return v_nil();
    }
    case NF_TASK_YIELD: sched_yield(); return v_nil();
    case NF_TASK_RUN: {
        vec_foreach(i, &vm->task_queue) {
            Value t = vm->task_queue.items[i];
            if (IS_OBJ(t, O_FUTURE) && !AS_FUTURE(t)->done) {
                ObjFuture *f = AS_FUTURE(t);
                if (f->fn >= 0) { f->result = vm_call_function(vm, f->fn, f->args.items, f->args.len); }
                f->done = true;
            }
        }
        vm->task_queue.len = 0;
        return v_nil();
    }
    case NF_TASK_COUNT: {
        int n = 0;
        vec_foreach(i, &vm->task_queue)
            if (IS_OBJ(vm->task_queue.items[i], O_FUTURE) && !AS_FUTURE(vm->task_queue.items[i])->done) n++;
        return v_int(n);
    }

    /* -------------------------------------------------------------- db */
    case NF_DB_OPEN: return v_int(db_open(txt(A(0))));
    case NF_DB_EXEC: return v_int(db_exec(vm, (int)INT(0), txt(A(1)), NULL));
    case NF_DB_QUERY: { Value rows = v_nil(); db_exec(vm, (int)INT(0), txt(A(1)), &rows);
                        return rows.tag == V_NIL ? v_obj((Obj *)vm_list(vm)) : rows; }
    case NF_DB_CLOSE: db_close((int)INT(0)); return v_nil();
    case NF_DB_BEGIN: db_begin((int)INT(0)); return v_nil();
    case NF_DB_COMMIT: db_commit((int)INT(0)); return v_nil();
    case NF_DB_ROLLBACK: db_rollback((int)INT(0)); return v_nil();
    case NF_DB_TABLES: return db_tables(vm, (int)INT(0));

    /* ---------------------------------------------------------- tensor */
    case NF_T_NEW: {
        int shape[4] = { 1, 1, 1, 1 };
        int nd = 0;
        if (IS_OBJ(A(0), O_LIST)) {
            ObjList *s = AS_LIST(A(0));
            nd = s->items.len > 4 ? 4 : s->items.len;
            for (int i = 0; i < nd; i++) shape[i] = (int)v_toint(s->items.items[i]);
        }
        ObjTensor *t = vm_tensor(vm, nd ? nd : 1, shape);
        if (IS_OBJ(A(1), O_LIST)) {
            ObjList *d = AS_LIST(A(1));
            for (int i = 0; i < t->count && i < d->items.len; i++) t->data[i] = v_tonum(d->items.items[i]);
        }
        return v_obj((Obj *)t);
    }
    case NF_T_ZEROS: {
        int shape[4] = { 1, 1, 1, 1 };
        int nd = 0;
        if (IS_OBJ(A(0), O_LIST)) {
            ObjList *s = AS_LIST(A(0));
            nd = s->items.len > 4 ? 4 : s->items.len;
            for (int i = 0; i < nd; i++) shape[i] = (int)v_toint(s->items.items[i]);
        }
        return v_obj((Obj *)vm_tensor(vm, nd ? nd : 1, shape));
    }
    case NF_T_RANDOM: {
        int shape[4] = { 1, 1, 1, 1 };
        int nd = 0;
        if (IS_OBJ(A(0), O_LIST)) {
            ObjList *s = AS_LIST(A(0));
            nd = s->items.len > 4 ? 4 : s->items.len;
            for (int i = 0; i < nd; i++) shape[i] = (int)v_toint(s->items.items[i]);
        }
        ObjTensor *t = vm_tensor(vm, nd ? nd : 1, shape);
        double scale = nargs > 1 ? NUM(1) : 1.0;
        for (int i = 0; i < t->count; i++) t->data[i] = (rnd_unit() * 2.0 - 1.0) * scale;
        return v_obj((Obj *)t);
    }
    case NF_T_SHAPE: {
        ObjList *l = vm_list(vm);
        if (IS_OBJ(A(0), O_TENSOR)) { ObjTensor *t = AS_TENSOR(A(0)); for (int i = 0; i < t->ndim; i++) vec_push(&l->items, v_int(t->shape[i])); }
        return v_obj((Obj *)l);
    }
    case NF_T_GET: { if (!IS_OBJ(A(0), O_TENSOR)) return v_num(0);
        ObjTensor *t = AS_TENSOR(A(0)); int64_t i = INT(1);
        return (i >= 0 && i < t->count) ? v_num(t->data[i]) : v_num(0); }
    case NF_T_SET: { if (!IS_OBJ(A(0), O_TENSOR)) return v_nil();
        ObjTensor *t = AS_TENSOR(A(0)); int64_t i = INT(1);
        if (i >= 0 && i < t->count) t->data[i] = NUM(2);
        return v_nil(); }
    case NF_T_ADD: case NF_T_SUB: case NF_T_MUL: {
        if (!IS_OBJ(A(0), O_TENSOR) || !IS_OBJ(A(1), O_TENSOR)) return v_nil();
        ObjTensor *x = AS_TENSOR(A(0)), *y = AS_TENSOR(A(1));
        ObjTensor *o = vm_tensor(vm, x->ndim, x->shape);
        for (int i = 0; i < o->count; i++) {
            double b = y->count ? y->data[i % y->count] : 0;
            o->data[i] = id == NF_T_ADD ? x->data[i] + b : id == NF_T_SUB ? x->data[i] - b : x->data[i] * b;
        }
        return v_obj((Obj *)o);
    }
    case NF_T_SCALE: {
        if (!IS_OBJ(A(0), O_TENSOR)) return v_nil();
        ObjTensor *x = AS_TENSOR(A(0));
        ObjTensor *o = vm_tensor(vm, x->ndim, x->shape);
        double k = NUM(1);
        for (int i = 0; i < o->count; i++) o->data[i] = x->data[i] * k;
        return v_obj((Obj *)o);
    }
    case NF_T_MATMUL: {
        if (!IS_OBJ(A(0), O_TENSOR) || !IS_OBJ(A(1), O_TENSOR)) return v_nil();
        ObjTensor *x = AS_TENSOR(A(0)), *y = AS_TENSOR(A(1));
        int m = x->ndim >= 2 ? x->shape[0] : 1;
        int k = x->ndim >= 2 ? x->shape[1] : x->count;
        int n = y->ndim >= 2 ? y->shape[1] : 1;
        int k2 = y->ndim >= 2 ? y->shape[0] : y->count;
        if (k != k2) { vm_runtime_error(vm, "matmul shape mismatch: [%d x %d] times [%d x %d]", m, k, k2, n); *ok = false; return v_nil(); }
        int shape[2] = { m, n };
        ObjTensor *o = vm_tensor(vm, 2, shape);
        for (int i = 0; i < m; i++)
            for (int j = 0; j < n; j++) {
                double s = 0;
                for (int t = 0; t < k; t++) s += x->data[i * k + t] * y->data[t * n + j];
                o->data[i * n + j] = s;
            }
        return v_obj((Obj *)o);
    }
    case NF_T_TRANSPOSE: {
        if (!IS_OBJ(A(0), O_TENSOR)) return v_nil();
        ObjTensor *x = AS_TENSOR(A(0));
        int r = x->ndim >= 2 ? x->shape[0] : 1, c = x->ndim >= 2 ? x->shape[1] : x->count;
        int shape[2] = { c, r };
        ObjTensor *o = vm_tensor(vm, 2, shape);
        for (int i = 0; i < r; i++) for (int j = 0; j < c; j++) o->data[j * r + i] = x->data[i * c + j];
        return v_obj((Obj *)o);
    }
    case NF_T_RELU: case NF_T_SIGMOID: case NF_T_TANH: {
        if (!IS_OBJ(A(0), O_TENSOR)) return v_nil();
        ObjTensor *x = AS_TENSOR(A(0));
        ObjTensor *o = vm_tensor(vm, x->ndim, x->shape);
        for (int i = 0; i < o->count; i++) {
            double v = x->data[i];
            o->data[i] = id == NF_T_RELU ? (v > 0 ? v : 0) : id == NF_T_SIGMOID ? 1.0 / (1.0 + exp(-v)) : tanh(v);
        }
        return v_obj((Obj *)o);
    }
    case NF_T_SOFTMAX: {
        if (!IS_OBJ(A(0), O_TENSOR)) return v_nil();
        ObjTensor *x = AS_TENSOR(A(0));
        ObjTensor *o = vm_tensor(vm, x->ndim, x->shape);
        int row = x->ndim >= 2 ? x->shape[x->ndim - 1] : x->count;
        for (int base = 0; base < x->count; base += row) {
            double mx = -INFINITY;
            for (int i = 0; i < row; i++) if (x->data[base+i] > mx) mx = x->data[base+i];
            double sum = 0;
            for (int i = 0; i < row; i++) { o->data[base+i] = exp(x->data[base+i] - mx); sum += o->data[base+i]; }
            if (sum == 0) sum = 1;
            for (int i = 0; i < row; i++) o->data[base+i] /= sum;
        }
        return v_obj((Obj *)o);
    }
    case NF_T_SUM: case NF_T_MEAN: {
        if (!IS_OBJ(A(0), O_TENSOR)) return v_num(0);
        ObjTensor *x = AS_TENSOR(A(0));
        double s = 0;
        for (int i = 0; i < x->count; i++) s += x->data[i];
        return v_num(id == NF_T_SUM ? s : (x->count ? s / x->count : 0));
    }
    case NF_T_ARGMAX: {
        if (!IS_OBJ(A(0), O_TENSOR)) return v_int(-1);
        ObjTensor *x = AS_TENSOR(A(0));
        int best = 0;
        for (int i = 1; i < x->count; i++) if (x->data[i] > x->data[best]) best = i;
        return v_int(best);
    }
    case NF_T_TOLIST: {
        ObjList *l = vm_list(vm);
        if (IS_OBJ(A(0), O_TENSOR)) { ObjTensor *t = AS_TENSOR(A(0)); for (int i = 0; i < t->count; i++) vec_push(&l->items, v_num(t->data[i])); }
        return v_obj((Obj *)l);
    }
    case NF_T_DOT: {
        if (!IS_OBJ(A(0), O_TENSOR) || !IS_OBJ(A(1), O_TENSOR)) return v_num(0);
        ObjTensor *x = AS_TENSOR(A(0)), *y = AS_TENSOR(A(1));
        double s = 0;
        int n = x->count < y->count ? x->count : y->count;
        for (int i = 0; i < n; i++) s += x->data[i] * y->data[i];
        return v_num(s);
    }

    /* -------------------------------------------------------------- ui */
    case NF_UI_WINDOW: return v_int(ui_window(vm, txt(A(0)), (int)INT(1), (int)INT(2)));
    case NF_UI_NODE:   return v_int(ui_node(vm, (int)INT(0), txt(A(1)), txt(A(2))));
    case NF_UI_SET:    ui_set(vm, (int)INT(0), txt(A(1)), A(2)); return v_nil();
    case NF_UI_GET:    return ui_get(vm, (int)INT(0), txt(A(1)));
    case NF_UI_ON:     ui_on(vm, (int)INT(0), txt(A(1)), A(2)); return v_nil();
    case NF_UI_RUN:    ui_run(vm); return v_nil();
    case NF_UI_QUIT:   ui_quit(); return v_nil();
    case NF_UI_RENDER: { char *s = ui_describe(vm); Value v = mktext(vm, s); free(s); return v; }
    case NF_UI_EMIT:   ui_emit(vm, (int)INT(0), txt(A(1))); return v_nil();
    case NF_UI_BACKEND: return mktext(vm, ui_backend_name());

    /* ------------------------------------------------------------ draw */
    case NF_GFX_CANVAS: return v_obj((Obj *)vm_image(vm, (int)INT(0), (int)INT(1)));
    case NF_GFX_CLEAR: {
        if (!IS_OBJ(A(0), O_IMAGE)) return v_nil();
        ObjImage *im = AS_IMAGE(A(0));
        uint32_t c = (uint32_t)INT(1) | 0xff000000u;
        for (int i = 0; i < im->w * im->h; i++) im->px[i] = c;
        return v_nil();
    }
    case NF_GFX_PIXEL: {
        if (!IS_OBJ(A(0), O_IMAGE)) return v_nil();
        ObjImage *im = AS_IMAGE(A(0));
        int x = (int)INT(1), y = (int)INT(2);
        if (x >= 0 && y >= 0 && x < im->w && y < im->h) im->px[y * im->w + x] = (uint32_t)INT(3) | 0xff000000u;
        return v_nil();
    }
    case NF_GFX_LINE: {
        if (!IS_OBJ(A(0), O_IMAGE)) return v_nil();
        ObjImage *im = AS_IMAGE(A(0));
        int x0 = (int)INT(1), y0 = (int)INT(2), x1 = (int)INT(3), y1 = (int)INT(4);
        uint32_t c = (uint32_t)INT(5) | 0xff000000u;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            if (x0 >= 0 && y0 >= 0 && x0 < im->w && y0 < im->h) im->px[y0 * im->w + x0] = c;
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
        return v_nil();
    }
    case NF_GFX_RECT: {
        if (!IS_OBJ(A(0), O_IMAGE)) return v_nil();
        ObjImage *im = AS_IMAGE(A(0));
        int x = (int)INT(1), y = (int)INT(2), w = (int)INT(3), h = (int)INT(4);
        uint32_t c = (uint32_t)INT(5) | 0xff000000u;
        for (int j = y; j < y + h; j++)
            for (int i = x; i < x + w; i++)
                if (i >= 0 && j >= 0 && i < im->w && j < im->h) im->px[j * im->w + i] = c;
        return v_nil();
    }
    case NF_GFX_CIRCLE: {
        if (!IS_OBJ(A(0), O_IMAGE)) return v_nil();
        ObjImage *im = AS_IMAGE(A(0));
        int cx = (int)INT(1), cy = (int)INT(2), r = (int)INT(3);
        uint32_t c = (uint32_t)INT(4) | 0xff000000u;
        for (int j = -r; j <= r; j++)
            for (int i = -r; i <= r; i++) {
                if (i * i + j * j > r * r) continue;
                int x = cx + i, y = cy + j;
                if (x >= 0 && y >= 0 && x < im->w && y < im->h) im->px[y * im->w + x] = c;
            }
        return v_nil();
    }
    case NF_GFX_TEXT: {
        if (!IS_OBJ(A(0), O_IMAGE)) return v_nil();
        ObjImage *im = AS_IMAGE(A(0));
        int x = (int)INT(1), y = (int)INT(2);
        const char *s = txt(A(3));
        uint32_t c = (uint32_t)INT(4) | 0xff000000u;
        for (int ci = 0; s[ci]; ci++) {
            unsigned char ch = (unsigned char)s[ci];
            if (ch < 32 || ch > 126) ch = '?';
            const unsigned char *g = FONT5x7[ch - 32];
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (g[col] & (1 << row)) {
                        int px = x + ci * 6 + col, py = y + row;
                        if (px >= 0 && py >= 0 && px < im->w && py < im->h) im->px[py * im->w + px] = c;
                    }
        }
        return v_nil();
    }
    case NF_GFX_SAVE: {
        if (!IS_OBJ(A(0), O_IMAGE)) return v_bool(false);
        ObjImage *im = AS_IMAGE(A(0));
        return v_bool(write_png(txt(A(1)), im->w, im->h, im->px));
    }
    case NF_GFX_RGB: return v_int(((INT(0) & 255) << 16) | ((INT(1) & 255) << 8) | (INT(2) & 255));
    case NF_GFX_SIZE: {
        ObjList *l = vm_list(vm);
        if (IS_OBJ(A(0), O_IMAGE)) { vec_push(&l->items, v_int(AS_IMAGE(A(0))->w)); vec_push(&l->items, v_int(AS_IMAGE(A(0))->h)); }
        return v_obj((Obj *)l);
    }

    default:
        vm_runtime_error(vm, "native function %d is not implemented in this build", id);
        *ok = false;
        return v_nil();
    }
}

void vm_native_shutdown(void) {
    for (int i = 0; i < g_nsock; i++) if (g_socks[i] >= 0) close(g_socks[i]);
    db_shutdown();
}
