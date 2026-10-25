/* ========================================================================
   sprfst docs — documentation generator

   Walks the analysed module graph and writes one Markdown page per module
   plus an index.  Everything comes from the real AST: signatures, doc
   comments (`~~`), fields, variants, traits and tests.
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include "sprfst/parser.h"
#include "sprfst/natives.h"
#include <unistd.h>

static void write_doc_text(StrBuf *b, const char *doc, const char *prefix) {
    if (!doc || !*doc) return;
    const char *p = doc;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len && (p[len-1] == ' ' || p[len-1] == '\r')) len--;
        sb_puts(b, prefix);
        sb_put(b, p, len);
        sb_putc(b, '\n');
        if (!eol) break;
        p = eol + 1;
    }
    sb_putc(b, '\n');
}

static void fn_signature(Arena *a, StrBuf *b, FnDecl *fn) {
    sb_printf(b, "%s%s(", fn->is_task ? "task " : "fn ", fn->name);
    int shown = 0;
    vec_foreach(i, &fn->params) {
        Param *p = &fn->params.items[i];
        if (shown++) sb_puts(b, ", ");
        if (p->is_self) { sb_puts(b, "self"); continue; }
        sb_printf(b, "%s: %s", p->name, p->type ? typeexpr_to_text(a, p->type) : "Any");
    }
    sb_puts(b, ")");
    if (fn->ret) sb_printf(b, " -> %s", typeexpr_to_text(a, fn->ret));
}

static void document_module(Build *b, Module *m, StrBuf *out) {
    Arena *a = &b->arena;
    sb_printf(out, "# %s\n\n", m->name ? m->name : path_basename(m->path));
    /* show the path relative to the project, not the whole machine */
    const char *shown = m->path ? m->path : "";
    const char *dot = strstr(shown, "/./");
    if (dot) shown = dot + 3;
    else {
        char cwd[1200];
        const char *base = (b->root && b->root[0] == '/') ? b->root
                         : (getcwd(cwd, sizeof cwd) ? cwd : NULL);
        if (base) {
            size_t bl = strlen(base);
            if (strncmp(shown, base, bl) == 0) shown += bl;
        }
    }
    while (*shown == '/') shown++;
    sb_printf(out, "`%s`\n\n", shown);
    write_doc_text(out, m->doc, "");

    /* imports */
    int nuse = 0;
    vec_foreach(i, &m->decls) if (m->decls.items[i]->kind == D_USE) nuse++;
    if (nuse) {
        sb_puts(out, "## Uses\n\n");
        vec_foreach(i, &m->decls) {
            Decl *d = m->decls.items[i];
            if (d->kind != D_USE) continue;
            sb_puts(out, "- `");
            vec_foreach(k, &d->as.use.path) { if (k) sb_putc(out, '.'); sb_puts(out, d->as.use.path.items[k]); }
            sb_puts(out, "`\n");
        }
        sb_putc(out, '\n');
    }

    /* types */
    int ntypes = 0;
    vec_foreach(i, &m->decls) if (m->decls.items[i]->kind == D_TYPE) ntypes++;
    if (ntypes) {
        sb_puts(out, "## Types\n\n");
        vec_foreach(i, &m->decls) {
            Decl *d = m->decls.items[i];
            if (d->kind != D_TYPE) continue;
            TypeDecl *td = d->as.type;
            const char *kw = td->kind == TD_OBJECT ? "object" : td->kind == TD_DATA ? "data" :
                             td->kind == TD_ENUM ? "enum" : td->kind == TD_TRAIT ? "trait" : "type";
            sb_printf(out, "### %s `%s`\n\n", kw, td->name);
            write_doc_text(out, td->doc, "");
            if (td->base) sb_printf(out, "Extends `%s`.\n\n", td->base);
            if (td->traits.len) {
                sb_puts(out, "Implements ");
                vec_foreach(k, &td->traits) sb_printf(out, "%s`%s`", k ? ", " : "", td->traits.items[k]);
                sb_puts(out, ".\n\n");
            }
            if (td->fields.len) {
                sb_puts(out, "| field | type | notes |\n|---|---|---|\n");
                vec_foreach(k, &td->fields) {
                    FieldDecl *f = &td->fields.items[k];
                    sb_printf(out, "| `%s` | `%s` | %s |\n", f->name,
                              f->type ? typeexpr_to_text(a, f->type) : "Any",
                              f->doc ? f->doc : (f->deflt ? "has a default" : ""));
                }
                sb_putc(out, '\n');
            }
            if (td->variants.len) {
                sb_puts(out, "| variant | payload |\n|---|---|\n");
                vec_foreach(k, &td->variants) {
                    VariantDecl *v = &td->variants.items[k];
                    sb_printf(out, "| `%s` | ", v->name);
                    if (!v->payload.len) sb_puts(out, "—");
                    vec_foreach(j, &v->payload) sb_printf(out, "%s`%s`", j ? ", " : "", typeexpr_to_text(a, v->payload.items[j]));
                    sb_puts(out, " |\n");
                }
                sb_putc(out, '\n');
            }
            if (td->methods.len) {
                sb_puts(out, "**Methods**\n\n");
                vec_foreach(k, &td->methods) {
                    FnDecl *fn = td->methods.items[k];
                    sb_puts(out, "- `");
                    fn_signature(a, out, fn);
                    sb_puts(out, "`");
                    if (fn->doc) sb_printf(out, " — %s", fn->doc);
                    sb_putc(out, '\n');
                }
                sb_putc(out, '\n');
            }
        }
    }

    /* functions */
    int nfn = 0;
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind == D_FN && !d->as.fn->is_test && !d->as.fn->is_bench) nfn++;
    }
    if (nfn) {
        sb_puts(out, "## Functions\n\n");
        vec_foreach(i, &m->decls) {
            Decl *d = m->decls.items[i];
            if (d->kind != D_FN) continue;
            FnDecl *fn = d->as.fn;
            if (fn->is_test || fn->is_bench) continue;
            sb_puts(out, "### `");
            fn_signature(a, out, fn);
            sb_puts(out, "`\n\n");
            write_doc_text(out, fn->doc, "");
        }
    }

    /* constants */
    int nconst = 0;
    vec_foreach(i, &m->decls) if (m->decls.items[i]->kind == D_CONST) nconst++;
    if (nconst) {
        sb_puts(out, "## Constants\n\n");
        vec_foreach(i, &m->decls) {
            Decl *d = m->decls.items[i];
            if (d->kind != D_CONST) continue;
            sb_printf(out, "- `%s`", d->as.konst.name);
            if (d->as.konst.type) sb_printf(out, ": `%s`", typeexpr_to_text(a, d->as.konst.type));
            if (d->doc) sb_printf(out, " — %s", d->doc);
            sb_putc(out, '\n');
        }
        sb_putc(out, '\n');
    }

    /* tests */
    int ntest = 0;
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind == D_FN && (d->as.fn->is_test || d->as.fn->is_bench)) ntest++;
    }
    if (ntest) {
        sb_puts(out, "## Tests\n\n");
        vec_foreach(i, &m->decls) {
            Decl *d = m->decls.items[i];
            if (d->kind != D_FN) continue;
            if (!d->as.fn->is_test && !d->as.fn->is_bench) continue;
            sb_printf(out, "- %s `%s`\n", d->as.fn->is_bench ? "bench" : "test", d->as.fn->name);
        }
        sb_putc(out, '\n');
    }
}

/* the whole standard library, straight from the native table */
static void document_stdlib(StrBuf *out) {
    sb_puts(out, "# Standard library\n\n");
    sb_puts(out, "Every function below is built into the runtime.\n\n");
    int nmods = 0;
    const char **mods = natives_module_names(&nmods);
    for (int k = 0; k < nmods; k++) {
        sb_printf(out, "## %s\n\n`use std.%s`\n\n| function | signature | does |\n|---|---|---|\n", mods[k], mods[k]);
        for (int i = 0; i < NF_COUNT; i++) {
            const NativeFn *nf = &SPRFST_NATIVES[i];
            if (strcmp(nf->module, mods[k]) != 0) continue;
            sb_printf(out, "| `%s.%s` | `%s` | %s |\n", mods[k], nf->name, nf->sig, nf->doc);
        }
        sb_putc(out, '\n');
    }
    sb_puts(out, "## Built in\n\nAvailable everywhere, no import needed.\n\n| function | signature | does |\n|---|---|---|\n");
    for (int i = 0; i < NF_COUNT; i++) {
        const NativeFn *nf = &SPRFST_NATIVES[i];
        if (nf->module[0]) continue;
        sb_printf(out, "| `%s` | `%s` | %s |\n", nf->name, nf->sig, nf->doc);
    }
    sb_putc(out, '\n');
    sb_puts(out, "## Methods on built in types\n\n| type | method | signature | does |\n|---|---|---|---|\n");
    for (int i = 0; i < NF_COUNT; i++) {
        const NativeFn *nf = &SPRFST_NATIVES[i];
        if (nf->module[0] != '@') continue;
        sb_printf(out, "| `%s` | `%s` | `%s` | %s |\n", nf->module + 1, nf->name, nf->sig, nf->doc);
    }
    sb_putc(out, '\n');
}

int cmd_docs(int argc, char **argv) {
    const char *target = NULL;
    const char *outdir = NULL;
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--out=", 6) == 0) outdir = argv[i] + 6;
        else if (argv[i][0] != '-') target = argv[i];
    }

    Build b;
    build_init(&b, 0);
    char entry[1300];
    if (target && file_exists(target)) snprintf(entry, sizeof entry, "%s", target);
    else {
        char *root = project_find_root(&b.arena, target ? target : ".");
        Project p = project_load(root ? root : ".");
        snprintf(entry, sizeof entry, "%s/%s", root ? root : ".", p.entry);
    }
    build_load(&b, entry);

    /* a project's own library code is documented too, not only what the
       entry point happens to import */
    const char *root = b.root ? b.root : ".";
    const char *folders[2] = { "src", "std" };
    for (int f = 0; f < 2; f++) {
        char dir[1300];
        snprintf(dir, sizeof dir, "%s/%s", root, folders[f]);
        if (!dir_exists(dir)) continue;
        int n = 0;
        char **names = list_dir(dir, &n, NULL);
        for (int i = 0; i < n; i++) {
            if (strcmp(path_ext(names[i]), "spf") == 0) {
                char file[1600];
                snprintf(file, sizeof file, "%s/%s", dir, names[i]);
                build_load(&b, file);
            }
            free(names[i]);
        }
        free(names);
    }

    build_check(&b);                /* types make the docs better; errors do not stop us */

    char dir[1400];
    snprintf(dir, sizeof dir, "%s", outdir ? outdir : "docs");
    make_dir_all(dir);

    StrBuf index; sb_init(&index);
    Project proj = project_load(b.root ? b.root : ".");
    sb_printf(&index, "# %s\n\n", proj.found ? proj.name : "SPRFST documentation");
    if (proj.description[0]) sb_printf(&index, "%s\n\n", proj.description);
    sb_printf(&index, "Generated by SPRFST %s.\n\n## Modules\n\n", SPRFST_VERSION);

    /* the pages come out in whatever order the imports resolved in, which is
       not an order anyone wants to read, and makes the output differ between
       machines. Sort by name so the same project always produces the same docs. */
    int nmod = b.sema->modules.len;
    Module **ordered = calloc((size_t)(nmod > 0 ? nmod : 1), sizeof(Module *));
    for (int i = 0; i < nmod; i++) ordered[i] = b.sema->modules.items[i];
    for (int i = 1; i < nmod; i++) {
        Module *m = ordered[i];
        const char *mn = m->name ? m->name : "";
        int j = i - 1;
        while (j >= 0 && strcmp(ordered[j]->name ? ordered[j]->name : "", mn) > 0) {
            ordered[j + 1] = ordered[j];
            j--;
        }
        ordered[j + 1] = m;
    }

    for (int i = 0; i < nmod; i++) {
        Module *m = ordered[i];
        const char *base = path_basename(m->path ? m->path : "module");
        char name[256];
        snprintf(name, sizeof name, "%s", base);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;

        StrBuf page; sb_init(&page);
        document_module(&b, m, &page);
        char p[2048];
        snprintf(p, sizeof p, "%s/%s.md", dir, name);
        write_file_bytes(p, page.data ? page.data : "", page.len);
        sb_free(&page);

        sb_printf(&index, "- [%s](%s.md)\n", m->name ? m->name : name, name);
    }
    free(ordered);
    sb_puts(&index, "- [Standard library](stdlib.md)\n");

    StrBuf std; sb_init(&std);
    document_stdlib(&std);
    char p[1600];
    snprintf(p, sizeof p, "%s/stdlib.md", dir);
    write_file_bytes(p, std.data ? std.data : "", std.len);
    sb_free(&std);

    snprintf(p, sizeof p, "%s/index.md", dir);
    write_file_bytes(p, index.data ? index.data : "", index.len);

    fprintf(stderr, "  %sdocs%s  %d page%s  %s->  %s/%s\n", C_GREEN, C_RESET,
            b.sema->modules.len + 2, b.sema->modules.len + 2 == 1 ? "" : "s", C_DIM, dir, C_RESET);
    sb_free(&index);
    build_dispose(&b);
    return 0;
}
