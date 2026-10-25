/* ========================================================================
   sprfst lint — style and clarity checks

   Two layers:
     * everything the type checker already knows (unused names, shadowing,
       unreachable code, ...) with `strict` turned on
     * source level checks that need the raw text: long lines, tabs,
       trailing whitespace, deep nesting, left-over markers
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include "sprfst/parser.h"
#include <ctype.h>

typedef struct {
    int warnings;
    int max_line;
    int max_depth;
} LintCfg;

static void lint_source(DiagBag *db, SourceFile *f, LintCfg *cfg, int *count) {
    const char *src = f->src;
    int depth = 0;
    size_t line_start = 0;
    size_t n = f->len;

    for (size_t i = 0; i <= n; i++) {
        char c = i < n ? src[i] : '\n';
        if (c == '{') depth++;
        if (c == '}' && depth > 0) depth--;
        if (c != '\n') continue;

        size_t len = i - line_start;
        const char *s = src + line_start;

        /* long lines */
        if ((int)len > cfg->max_line) {
            Diag *d = diag_new(db, DIAG_WARNING, "W0401", "This line is %d characters long", (int)len);
            diag_label(db, d, span_make(f->id, (int)(line_start + (size_t)cfg->max_line), (int)i), true,
                       "past the %d column guide", cfg->max_line);
            diag_fix(db, d, "split the expression over several lines");
            (*count)++;
        }
        /* tabs */
        for (size_t k = 0; k < len; k++) {
            if (s[k] == '\t') {
                Diag *d = diag_new(db, DIAG_WARNING, "W0402", "This line uses a tab");
                diag_label(db, d, span_make(f->id, (int)(line_start + k), (int)(line_start + k + 1)), true, "tab here");
                diag_fix(db, d, "SPRFST indents with four spaces — run  sprfst fmt");
                (*count)++;
                break;
            }
        }
        /* trailing whitespace */
        if (len && (s[len-1] == ' ' || s[len-1] == '\t')) {
            size_t k = len;
            while (k && (s[k-1] == ' ' || s[k-1] == '\t')) k--;
            Diag *d = diag_new(db, DIAG_WARNING, "W0403", "Trailing whitespace");
            diag_label(db, d, span_make(f->id, (int)(line_start + k), (int)i), true, "remove this");
            diag_fix(db, d, "run  sprfst fmt");
            (*count)++;
        }
        /* leftover markers */
        for (size_t k = 0; k + 4 < len; k++) {
            if ((strncmp(s + k, "TODO", 4) == 0 || strncmp(s + k, "FIXME", 5) == 0 ||
                 strncmp(s + k, "XXX", 3) == 0)) {
                Diag *d = diag_new(db, DIAG_WARNING, "W0404", "Unfinished work is marked here");
                diag_label(db, d, span_make(f->id, (int)(line_start + k), (int)i), true, "left over marker");
                (*count)++;
                break;
            }
        }
        /* deep nesting */
        if (depth > cfg->max_depth) {
            Diag *d = diag_new(db, DIAG_WARNING, "W0405", "This code is %d blocks deep", depth);
            diag_label(db, d, span_make(f->id, (int)line_start, (int)i), true, "hard to follow");
            diag_fix(db, d, "pull the inner part into its own function");
            (*count)++;
            depth = cfg->max_depth;   /* report once per run */
        }
        line_start = i + 1;
    }
}

int cmd_lint(int argc, char **argv) {
    const char *target = NULL;
    bool quiet = false;
    LintCfg cfg = { 0, 100, 5 };
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--max-line=", 11) == 0) cfg.max_line = atoi(argv[i] + 11);
        else if (strncmp(argv[i], "--max-depth=", 12) == 0) cfg.max_depth = atoi(argv[i] + 12);
        else if (strcmp(argv[i], "-q") == 0) quiet = true;
        else if (argv[i][0] != '-') target = argv[i];
    }

    Build b;
    build_init(&b, 0);
    b.sema->strict_unused = true;

    char entry[1300];
    if (target && file_exists(target)) snprintf(entry, sizeof entry, "%s", target);
    else {
        char *root = project_find_root(&b.arena, target ? target : ".");
        Project p = project_load(root ? root : ".");
        snprintf(entry, sizeof entry, "%s/%s", root ? root : ".", p.entry);
        if (!file_exists(entry)) snprintf(entry, sizeof entry, "%s", target ? target : "main.spf");
    }

    build_load(&b, entry);
    build_check(&b);

    int extra = 0;
    vec_foreach(i, &b.sema->modules) {
        Module *m = b.sema->modules.items[i];
        SourceFile *f = sourcemap_get(b.sm, m->file_id);
        if (f) lint_source(b.db, f, &cfg, &extra);
    }

    build_report(&b);
    bool bad = diagbag_has_errors(b.db);
    if (!quiet)
        fprintf(stderr, "  %s%s%s  %d file%s linted\n",
                bad ? C_RED : C_GREEN, bad ? "problems found" : "clean", C_RESET,
                b.nfiles, b.nfiles == 1 ? "" : "s");
    build_dispose(&b);
    return bad ? 1 : 0;
}
