/* ========================================================================
   sprfst fmt — the SPRFST formatter

   Conservative on purpose: it never rewrites your expressions, it fixes
   layout.  Indentation follows block depth, spacing around operators and
   separators is normalised, trailing whitespace goes away, runs of blank
   lines collapse to one, and the file ends with exactly one newline.
   Comments and string contents are preserved byte for byte.
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include <ctype.h>

typedef struct {
    int  depth;
    bool changed;
} FmtState;

/* one pass over a single logical line, outside of strings and comments */
static void fmt_line(const char *in, StrBuf *out, int depth) {
    for (int i = 0; i < depth; i++) sb_puts(out, "    ");

    size_t n = strlen(in);
    size_t i = 0;
    while (i < n && isspace((unsigned char)in[i])) i++;

    bool prev_space = false;
    for (; i < n; i++) {
        char c = in[i];

        /* text literals and comments pass through untouched */
        if (c == '"') {
            sb_putc(out, c);
            i++;
            while (i < n) {
                if (in[i] == '\\' && i + 1 < n) { sb_putc(out, in[i]); sb_putc(out, in[i+1]); i += 2; continue; }
                sb_putc(out, in[i]);
                if (in[i] == '"') break;
                i++;
            }
            prev_space = false;
            continue;
        }
        if (c == '\'') {
            sb_putc(out, c);
            i++;
            while (i < n) {
                if (in[i] == '\\' && i + 1 < n) { sb_putc(out, in[i]); sb_putc(out, in[i+1]); i += 2; continue; }
                sb_putc(out, in[i]);
                if (in[i] == '\'') break;
                i++;
            }
            prev_space = false;
            continue;
        }
        if (c == '~' && i + 1 < n && in[i+1] == '~') {
            /* doc or normal comment: keep the rest of the line verbatim */
            while (i < n && isspace((unsigned char)in[n-1])) n--;
            sb_puts(out, in + i);
            return;
        }

        if (isspace((unsigned char)c)) {
            if (!prev_space && out->len && out->data[out->len-1] != ' ') { sb_putc(out, ' '); prev_space = true; }
            continue;
        }
        prev_space = false;

        /* no space before these */
        if (c == ',' || c == ')' || c == ']') {
            while (out->len && out->data[out->len-1] == ' ') out->len--;
            if (out->data) out->data[out->len] = 0;
            sb_putc(out, c);
            if (c == ',' && i + 1 < n && !isspace((unsigned char)in[i+1])) sb_putc(out, ' ');
            continue;
        }
        if (c == ':') {
            while (out->len && out->data[out->len-1] == ' ') out->len--;
            if (out->data) out->data[out->len] = 0;
            sb_putc(out, c);
            if (i + 1 < n && !isspace((unsigned char)in[i+1]) && in[i+1] != ']') sb_putc(out, ' ');
            continue;
        }
        if (c == '.') {
            while (out->len && out->data[out->len-1] == ' ') out->len--;
            if (out->data) out->data[out->len] = 0;
            sb_putc(out, c);
            continue;
        }
        if (c == '(' || c == '[') {
            sb_putc(out, c);
            while (i + 1 < n && in[i+1] == ' ') i++;
            continue;
        }
        if (c == '{') {
            /* one space before an opening block, never after `(` or `[` */
            char last = out->len ? out->data[out->len-1] : 0;
            if (last && last != ' ' && last != '(' && last != '[' && last != '{') sb_putc(out, ' ');
            sb_putc(out, c);
            if (i + 1 < n && !isspace((unsigned char)in[i+1]) && in[i+1] != '}') sb_putc(out, ' ');
            continue;
        }
        if (c == '}') {
            char last = out->len ? out->data[out->len-1] : 0;
            if (last && last != ' ' && last != '{') sb_putc(out, ' ');
            sb_putc(out, c);
            continue;
        }

        /* operator runs get one space on each side, with sensible exceptions */
        if (strchr("=+-*/%<>!&|^?", c)) {
            /* every two character operator the lexer knows; one missing here
               would be split in half and the file would stop compiling */
            static const char *ops[] = { "==", "!=", "<=", ">=", "**", "<<", ">>",
                                         "??", "=>", "->", "|>", "+=", "-=", "*=",
                                         "/=", "%=", NULL };
            char run[4] = { c, 0, 0, 0 };
            size_t j = i + 1;
            for (int k = 0; ops[k]; k++)
                if (in[i] == ops[k][0] && i + 1 < n && in[i+1] == ops[k][1]) {
                    run[1] = ops[k][1];
                    run[2] = 0;
                    j = i + 2;
                    break;
                }
            int rl = (int)strlen(run);
            char last = out->len ? out->data[out->len-1] : 0;
            char prev = last;
            {   /* last meaningful character before this operator */
                size_t q = out->len;
                while (q && out->data[q-1] == ' ') q--;
                prev = q ? out->data[q-1] : 0;
            }
            bool unary = (rl == 1 && (run[0] == '-' || run[0] == '+')) &&
                         (!prev || strchr("(,[{=+-*/%<>!&|^", prev) != NULL);
            /* a `?` that is not `??` is postfix: no spaces at all */
            if (rl == 1 && run[0] == '?') {
                while (out->len && out->data[out->len-1] == ' ') out->len--;
                if (out->data) out->data[out->len] = 0;
                sb_putc(out, '?');
                i = j - 1;
                continue;
            }
            if (unary) {
                if (last && last != ' ' && last != '(' && last != '[') sb_putc(out, ' ');
                sb_puts(out, run);
                while (j < n && in[j] == ' ') j++;
                i = j - 1;
                continue;
            }
            if (last && last != ' ') sb_putc(out, ' ');
            sb_puts(out, run);
            while (j < n && in[j] == ' ') j++;
            if (j < n) sb_putc(out, ' ');
            i = j - 1;
            continue;
        }
        sb_putc(out, c);
    }
    /* strip any trailing space we produced */
    while (out->len && (out->data[out->len-1] == ' ' || out->data[out->len-1] == '\t')) out->len--;
    if (out->data) out->data[out->len] = 0;
}

/* Count the net block depth change of a line, ignoring strings/comments. */
static void delta_depth(const char *s, int *open_before, int *net) {
    bool in_str = false, in_chr = false;
    int depth = 0, first_close = 0;
    bool seen_open = false;
    for (const char *p = s; *p; p++) {
        if (in_str) { if (*p == '\\' && p[1]) p++; else if (*p == '"') in_str = false; continue; }
        if (in_chr) { if (*p == '\\' && p[1]) p++; else if (*p == '\'') in_chr = false; continue; }
        if (*p == '"') { in_str = true; continue; }
        if (*p == '\'') { in_chr = true; continue; }
        if (*p == '~' && p[1] == '~') break;
        if (*p == '{' || *p == '[' || *p == '(') { depth++; seen_open = true; }
        else if (*p == '}' || *p == ']' || *p == ')') { depth--; if (!seen_open && depth < 0) first_close++; }
    }
    *net = depth;
    *open_before = first_close;
}

char *sprfst_format_source(const char *src, bool *changed) {
    StrBuf out; sb_init(&out);
    int depth = 0;
    int blank_run = 0;
    const char *p = src;
    bool first = true;

    while (*p || first) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = malloc(len + 1);
        memcpy(line, p, len);
        line[len] = 0;

        /* blank lines: keep at most one in a row, none at the start */
        bool blank = true;
        for (size_t k = 0; k < len; k++) if (!isspace((unsigned char)line[k])) { blank = false; break; }
        if (blank) {
            blank_run++;
            if (blank_run <= 1 && out.len) sb_putc(&out, '\n');
        } else {
            blank_run = 0;
            int closes = 0, net = 0;
            delta_depth(line, &closes, &net);
            int indent = depth - closes;
            if (indent < 0) indent = 0;
            StrBuf lb; sb_init(&lb);
            fmt_line(line, &lb, indent);
            sb_puts(&out, lb.data ? lb.data : "");
            sb_putc(&out, '\n');
            sb_free(&lb);
            depth += net;
            if (depth < 0) depth = 0;
        }
        free(line);
        first = false;
        if (!eol) break;
        p = eol + 1;
    }
    /* exactly one trailing newline */
    while (out.len >= 2 && out.data[out.len-1] == '\n' && out.data[out.len-2] == '\n') out.len--;
    if (out.data) out.data[out.len] = 0;
    if (!out.len) { sb_putc(&out, '\n'); }

    if (changed) *changed = strcmp(src, out.data ? out.data : "") != 0;
    return out.data ? out.data : strdup("\n");
}

static int format_one(const char *path, bool check_only, bool quiet) {
    size_t n = 0;
    char *src = read_file(path, &n);
    if (!src) {
        fprintf(stderr, "  %serror%s  cannot read %s\n", C_RED, C_RESET, path);
        return 1;
    }
    bool changed = false;
    char *out = sprfst_format_source(src, &changed);
    int rc = 0;
    if (changed) {
        if (check_only) {
            fprintf(stderr, "  %sneeds formatting%s  %s\n", C_AMBER, C_RESET, path);
            rc = 1;
        } else {
            write_file_bytes(path, out, strlen(out));
            if (!quiet) fprintf(stderr, "  %sformatted%s  %s\n", C_GREEN, C_RESET, path);
        }
    } else if (!quiet && !check_only) {
        fprintf(stderr, "  %sunchanged%s  %s\n", C_DIM, C_RESET, path);
    }
    free(src);
    free(out);
    return rc;
}

static int format_tree(const char *dir, bool check_only, bool quiet) {
    int rc = 0, n = 0;
    char **names = list_dir(dir, &n, NULL);
    if (!names) return 0;
    for (int i = 0; i < n; i++) {
        char p[1600];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (dir_exists(p)) {
            if (strcmp(names[i], "build") && strcmp(names[i], "packages") && names[i][0] != '.')
                rc |= format_tree(p, check_only, quiet);
        } else if (strcmp(path_ext(names[i]), "spf") == 0) {
            rc |= format_one(p, check_only, quiet);
        }
        free(names[i]);
    }
    free(names);
    return rc;
}

int cmd_fmt(int argc, char **argv) {
    bool check_only = false, quiet = false;
    const char *target = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) check_only = true;
        else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) quiet = true;
        else if (argv[i][0] != '-') target = argv[i];
    }
    if (!target) {
        Arena a;
        arena_init(&a);
        char *root = project_find_root(&a, ".");
        char src[1200];
        snprintf(src, sizeof src, "%s/src", root ? root : ".");
        int rc = format_tree(dir_exists(src) ? src : (root ? root : "."), check_only, quiet);
        arena_free(&a);
        return rc;
    }
    if (dir_exists(target)) return format_tree(target, check_only, quiet);
    return format_one(target, check_only, quiet);
}
