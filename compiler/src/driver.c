/* ========================================================================
   SPRFST — build driver: source loading, module graph, pipeline timing
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include "sprfst/parser.h"
#include "sprfst/lexer.h"
#include "sprfst/natives.h"
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

void build_init(Build *b, int opt_level) {
    memset(b, 0, sizeof *b);
    arena_init(&b->arena);
    b->in        = interner_new();
    b->sm        = sourcemap_new(&b->arena);
    b->db        = diagbag_new(&b->arena, b->sm);
    b->tt        = types_new(&b->arena);
    b->sema      = sema_new(&b->arena, b->in, b->db, b->tt);
    b->opt_level = opt_level;
    b->color     = term_supports_color(stderr);
}

void build_dispose(Build *b) {
    interner_free(b->in);
    arena_free(&b->arena);
    free(b->root);
    memset(b, 0, sizeof *b);
}

/* ------------------------------------------------------------- project */
static void trim_inplace(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t a = 0;
    while (s[a] && isspace((unsigned char)s[a])) a++;
    if (a) memmove(s, s + a, n - a + 1);
    n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { memmove(s, s + 1, n - 2); s[n - 2] = 0; }
}

Project project_load(const char *dir) {
    Project p;
    memset(&p, 0, sizeof p);
    snprintf(p.name, sizeof p.name, "app");
    snprintf(p.version, sizeof p.version, "0.1.0");
    snprintf(p.entry, sizeof p.entry, "src/main.spf");
    if (!dir) return p;

    char path[1024];
    snprintf(path, sizeof path, "%s/project.sprfst", dir);
    size_t n = 0;
    char *text = read_file(path, &n);
    if (!text) return p;
    p.found = true;

    char section[64] = "";
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", line);
        char *hash = strchr(buf, '#');
        if (hash) *hash = 0;
        trim_inplace(buf);
        if (!*buf) continue;
        if (buf[0] == '[') {
            char *end = strchr(buf, ']');
            if (end) *end = 0;
            snprintf(section, sizeof section, "%.*s", (int)sizeof section - 1, buf + 1);
            continue;
        }
        char *eq = strchr(buf, '=');
        if (!eq) continue;
        *eq = 0;
        char key[128], val[512];
        snprintf(key, sizeof key, "%.*s", (int)sizeof key - 1, buf);
        snprintf(val, sizeof val, "%.*s", (int)sizeof val - 1, eq + 1);
        trim_inplace(key);
        trim_inplace(val);
        if (strcmp(section, "package") == 0 || !*section) {
            if (strcmp(key, "name") == 0) snprintf(p.name, sizeof p.name, "%.*s", (int)sizeof p.name - 1, val);
            else if (strcmp(key, "version") == 0) snprintf(p.version, sizeof p.version, "%.*s", (int)sizeof p.version - 1, val);
            else if (strcmp(key, "entry") == 0) snprintf(p.entry, sizeof p.entry, "%.*s", (int)sizeof p.entry - 1, val);
            else if (strcmp(key, "author") == 0) snprintf(p.author, sizeof p.author, "%.*s", (int)sizeof p.author - 1, val);
            else if (strcmp(key, "description") == 0) snprintf(p.description, sizeof p.description, "%.*s", (int)sizeof p.description - 1, val);
        } else if (strcmp(section, "packages") == 0 && p.ndeps < 32) {
            snprintf(p.deps[p.ndeps++], 160, "%.79s=%.79s", key, val);
        }
    }
    free(text);
    return p;
}

char *project_find_root(Arena *a, const char *start) {
    char cur[1024];
    if (start && *start) snprintf(cur, sizeof cur, "%s", start);
    else if (!getcwd(cur, sizeof cur)) return NULL;
    if (!dir_exists(cur)) {
        char *d = path_dirname(a, cur);
        snprintf(cur, sizeof cur, "%s", d ? d : ".");
    }
    for (int depth = 0; depth < 32; depth++) {
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/project.sprfst", cur);
        if (file_exists(probe)) return arena_strdup(a, cur);
        char *parent = path_dirname(a, cur);
        if (!parent || strcmp(parent, cur) == 0 || !*parent) break;
        snprintf(cur, sizeof cur, "%s", parent);
    }
    return NULL;
}

/* --------------------------------------------------------- module load */
/* Where the standard library lives: $SPRFST_HOME, else next to the binary,
   else the usual install locations. */
static const char *sprfst_home(void) {
    const char *h = getenv("SPRFST_HOME");
    if (h && *h) return h;
    static char guess[1024];
    if (guess[0]) return guess;

    char exe[1024] = { 0 };
#if defined(__APPLE__)
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) exe[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) exe[n] = 0; else exe[0] = 0;
#endif

    char probe[1400];
    if (exe[0]) {
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;                       /* .../build/bin */
        const char *rel[] = { "/..", "/../..", "/../lib/sprfst", "/../Resources", "/../share/sprfst" };
        for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
            snprintf(probe, sizeof probe, "%s%s/std/core.spf", exe, rel[i]);
            if (file_exists(probe)) {
                snprintf(guess, sizeof guess, "%s%s", exe, rel[i]);
                return guess;
            }
        }
    }
    const char *candidates[] = { "/usr/local/lib/sprfst", "/opt/sprfst", "." };
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        snprintf(probe, sizeof probe, "%s/std/core.spf", candidates[i]);
        if (file_exists(probe)) { snprintf(guess, sizeof guess, "%s", candidates[i]); return guess; }
    }
    snprintf(guess, sizeof guess, ".");
    return guess;
}

static bool already_loaded(Build *b, const char *abs) {
    vec_foreach(i, &b->sema->modules)
        if (b->sema->modules.items[i]->path && strcmp(b->sema->modules.items[i]->path, abs) == 0) return true;
    return false;
}

static bool load_recursive(Build *b, const char *path, Span from, bool is_entry);

static bool resolve_import(Build *b, Module *m, Decl *d) {
    NameVec *p = &d->as.use.path;
    if (!p->len) return true;
    /* `use std.io` is built into the runtime; `use std.ai` is a source file */
    if (strcmp(p->items[0], "std") == 0 && p->len >= 2) {
        int nmods = 0;
        const char **mods = natives_module_names(&nmods);
        const char *last = p->items[p->len - 1];
        for (int i = 0; i < nmods; i++)
            if (strcmp(mods[i], last) == 0) return true;
    }

    /* build a relative path from the dotted name */
    StrBuf rel; sb_init(&rel);
    vec_foreach(k, p) { if (k) sb_putc(&rel, '/'); sb_puts(&rel, p->items[k]); }

    char *dir = path_dirname(&b->arena, m->path);
    const char *home = sprfst_home();
    const char *root = b->root ? b->root : (dir ? dir : ".");
    const char *last = p->items[p->len - 1];
    char cand[8][1400];
    int n = 0;
    /* beside the importing file */
    snprintf(cand[n++], sizeof cand[0], "%s/%s.spf", dir ? dir : ".", rel.data);
    snprintf(cand[n++], sizeof cand[0], "%s/%s/mod.spf", dir ? dir : ".", rel.data);
    /* inside a package installed by Forge: its own entry point first,
       then the usual places a package keeps its root module */
    char pkgdir[1300];
    snprintf(pkgdir, sizeof pkgdir, "%s/packages/%s", root, p->items[0]);
    Project pkg = project_load(pkgdir);
    if (pkg.found && pkg.entry[0])
        snprintf(cand[n++], sizeof cand[0], "%.1000s/%.380s", pkgdir, pkg.entry);
    snprintf(cand[n++], sizeof cand[0], "%s/packages/%s/src/%s.spf", root, rel.data, last);
    snprintf(cand[n++], sizeof cand[0], "%s/packages/%s/src/main.spf", root, rel.data);
    snprintf(cand[n++], sizeof cand[0], "%s/packages/%s/%s.spf", root, rel.data, last);
    /* `use std.ai` lives at $SPRFST_HOME/std/ai.spf */
    const char *tail = rel.data;
    if (strncmp(tail, "std/", 4) == 0) tail += 4;
    snprintf(cand[n++], sizeof cand[0], "%s/std/%s.spf", home, tail);
    sb_free(&rel);

    for (int i = 0; i < n; i++)
        if (file_exists(cand[i])) return load_recursive(b, cand[i], d->span, false);
    return true;   /* sema reports the missing module with a good message */
}

static bool load_recursive(Build *b, const char *path, Span from, bool is_entry) {
    (void)from;
    char *abs = path_abs(&b->arena, path);
    if (already_loaded(b, abs ? abs : path)) return true;

    SourceFile *f = sourcemap_load(b->sm, path);
    if (!f) {
        Diag *dg = diag_new(b->db, DIAG_ERROR, "E0001", "Cannot read `%s`", path);
        diag_note(b->db, dg, "%s", strerror(errno));
        if (is_entry) diag_fix(b->db, dg, "check the path, or run  sprfst new <name>  to start a project");
        return false;
    }
    Module *m = parse_module(&b->arena, b->in, b->db, f);
    if (!m) return false;
    m->path = abs ? abs : arena_strdup(&b->arena, path);
    sema_add_module(b->sema, m);
    b->nfiles++;

    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind == D_USE) resolve_import(b, m, d);
    }
    return true;
}

bool build_load(Build *b, const char *path) {
    double t0 = now_seconds();
    if (!b->root) {
        char *r = project_find_root(&b->arena, path);
        if (r) b->root = strdup(r);
    }
    bool ok = load_recursive(b, path, span_make(0, 0, 0), true);
    b->t_parse = now_seconds() - t0;
    return ok && !diagbag_has_errors(b->db);
}

bool build_check(Build *b) {
    double t0 = now_seconds();
    bool ok = sema_check(b->sema);
    b->t_check = now_seconds() - t0;
    return ok;
}

bool build_lower(Build *b) {
    double t0 = now_seconds();
    b->prog = ir_lower(&b->arena, b->sema, b->sm, b->db);
    if (b->prog && b->opt_level > 0) b->opt = ir_optimize(b->prog, b->opt_level);
    b->t_lower = now_seconds() - t0;
    return b->prog != NULL && !diagbag_has_errors(b->db);
}

bool build_compile(Build *b, const char *path) {
    if (!build_load(b, path)) return false;
    if (!build_check(b)) return false;
    return build_lower(b);
}

void build_report(Build *b) {
    if (b->json_diags) {
        char *json = diagbag_to_json(b->db);
        puts(json);
        free(json);
    } else if (!b->quiet) {
        diagbag_render(b->db, stderr, b->color);
    }
}
