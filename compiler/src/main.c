/* ========================================================================
   SPRFST — command line
   Small on purpose: one obvious verb per job.
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include "sprfst/parser.h"
#include "sprfst/natives.h"
#include <unistd.h>
#include <ctype.h>

/* verbs implemented in their own translation units */
int cmd_fmt(int argc, char **argv);
int cmd_lint(int argc, char **argv);
int cmd_docs(int argc, char **argv);
int cmd_debug(int argc, char **argv);
int cmd_forge(const char *verb, int argc, char **argv);
int cmd_studio(int argc, char **argv);

/* ------------------------------------------------------------- options */
typedef struct {
    int   opt_level;
    bool  opt_set;
    bool  json;
    bool  dump_ir;
    bool  timing;
    bool  quiet;
    const char *path;
    int   rest_argc;
    char **rest_argv;
} Opts;


static void banner(void) {
    fprintf(stderr, "%s%sSPRFST%s %s %s(%s)%s\n", C_BOLD, C_AMBER, C_RESET,
            SPRFST_VERSION, C_DIM, SPRFST_CODENAME, C_RESET);
}

static void usage(void) {
    banner();
    fprintf(stderr,
"\n  %susage%s  sprfst <verb> [path] [options]\n\n"
"  %srun%s      <file|.>     compile and run\n"
"  %scheck%s    [path]       type check, no output\n"
"  %sbuild%s    [path]       compile and write build artefacts\n"
"  %stest%s     [path]       run every test\n"
"  %sfmt%s      [path]       format source in place\n"
"  %slint%s     [path]       report style and clarity problems\n"
"  %sdocs%s     [path]       write documentation to docs/\n"
"  %sdebug%s    <file>       run under the debugger\n"
"  %snew%s      <name>       start a project   (new app <name>: a desktop app)\n"
"  %sadd%s      <pkg>        add a package with Forge\n"
"  %sremove%s   <pkg>        remove a package\n"
"  %sinstall%s               fetch everything in project.sprfst\n"
"  %sclean%s                 delete build/\n"
"\n  %soptions%s  -O0..-O3  --json  --ir  --time  --quiet  --version\n\n",
    C_DIM, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_DIM, C_RESET);
}

/* Resolve the source file to work on: an explicit path, or the project entry. */
static char *resolve_entry(Build *b, const char *given, char *buf, size_t cap) {
    if (given && *given && file_exists(given)) { snprintf(buf, cap, "%s", given); return buf; }
    const char *dir = (given && *given && dir_exists(given)) ? given : ".";
    char *root = project_find_root(&b->arena, dir);
    Project p = project_load(root ? root : dir);
    if (p.found) {
        snprintf(buf, cap, "%s/%s", root ? root : dir, p.entry);
        if (file_exists(buf)) { b->root = strdup(root ? root : dir); return buf; }
    }
    const char *tries[] = { "main.spf", "src/main.spf" };
    for (size_t i = 0; i < sizeof tries / sizeof *tries; i++) {
        snprintf(buf, cap, "%s/%s", dir, tries[i]);
        if (file_exists(buf)) return buf;
    }
    if (given && *given) snprintf(buf, cap, "%s", given);
    else snprintf(buf, cap, "main.spf");
    return buf;
}

static void print_timing(Build *b, VM *vm, double run_s) {
    fprintf(stderr, "\n  %s%d file%s  parse %.1fms  check %.1fms  lower %.1fms",
            C_DIM, b->nfiles, b->nfiles == 1 ? " " : "s", b->t_parse * 1e3, b->t_check * 1e3, b->t_lower * 1e3);
    if (vm) fprintf(stderr, "  run %.1fms  %d instr  %zu KB peak  %d gc",
                    run_s * 1e3, vm->instr_count, vm->peak_bytes / 1024, vm->gc_runs);
    fprintf(stderr, "%s\n", C_RESET);
}

/* ----------------------------------------------------------------- run */
static int cmd_run(Opts *o) {
    Build b;
    build_init(&b, o->opt_level);
    b.json_diags = o->json;
    b.quiet = o->quiet;
    char buf[1200];
    char *entry = resolve_entry(&b, o->path, buf, sizeof buf);

    bool ok = build_compile(&b, entry);
    build_report(&b);
    if (!ok) { build_dispose(&b); return 1; }

    if (o->dump_ir) {
        StrBuf s; sb_init(&s);
        ir_dump(b.prog, &s);
        fputs(s.data ? s.data : "", stdout);
        sb_free(&s);
    }

    VM *vm = vm_new(b.prog, b.sm, &b.arena);
    double t0 = now_seconds();
    int code = vm_run(vm, o->rest_argc, o->rest_argv);
    double elapsed = now_seconds() - t0;
    if (o->timing) print_timing(&b, vm, elapsed);
    vm_free(vm);
    vm_native_shutdown();
    build_dispose(&b);
    return code;
}

/* --------------------------------------------------------------- check */
static int cmd_check(Opts *o) {
    Build b;
    build_init(&b, 0);
    b.json_diags = o->json;
    char buf[1200];
    char *entry = resolve_entry(&b, o->path, buf, sizeof buf);
    bool ok = build_load(&b, entry) && build_check(&b);
    build_report(&b);
    if (ok && !o->json && !o->quiet)
        fprintf(stderr, "  %sok%s  %d file%s checked in %.0fms\n", C_GREEN, C_RESET,
                b.nfiles, b.nfiles == 1 ? "" : "s", (b.t_parse + b.t_check) * 1e3);
    if (o->timing) print_timing(&b, NULL, 0);
    build_dispose(&b);
    return ok ? 0 : 1;
}

/* --------------------------------------------------------------- build */
static int cmd_build(Opts *o) {
    Build b;
    build_init(&b, o->opt_set ? o->opt_level : 2);
    b.json_diags = o->json;
    char buf[1200];
    char *entry = resolve_entry(&b, o->path, buf, sizeof buf);
    bool ok = build_compile(&b, entry);
    build_report(&b);
    if (!ok) { build_dispose(&b); return 1; }

    const char *root = b.root ? b.root : ".";
    char outdir[1300];
    snprintf(outdir, sizeof outdir, "%s/build", root);
    make_dir_all(outdir);

    StrBuf s; sb_init(&s);
    ir_dump(b.prog, &s);
    char irpath[1500];
    snprintf(irpath, sizeof irpath, "%s/program.spir", outdir);
    write_file_bytes(irpath, s.data ? s.data : "", s.len);
    sb_free(&s);

    if (!o->quiet) {
        int ninstr = 0;
        vec_foreach(i, &b.prog->funcs) ninstr += b.prog->funcs.items[i]->code.len;
        fprintf(stderr, "  %sbuilt%s  %d function%s, %d instructions  %s->  %s%s\n",
                C_GREEN, C_RESET, b.prog->funcs.len, b.prog->funcs.len == 1 ? "" : "s",
                ninstr, C_DIM, irpath, C_RESET);
        fprintf(stderr, "  %sopt%s    folded %d, moves %d, dead %d, jumps %d, nops %d\n",
                C_DIM, C_RESET, b.opt.folded, b.opt.moves_removed, b.opt.dead_removed,
                b.opt.jumps_threaded, b.opt.blocks_removed);
    }
    if (o->timing) print_timing(&b, NULL, 0);
    build_dispose(&b);
    return 0;
}

/* ---------------------------------------------------------------- test */
static int cmd_test(Opts *o) {
    Build b;
    build_init(&b, o->opt_level);
    b.json_diags = o->json;
    char buf[1200];
    char *entry = resolve_entry(&b, o->path, buf, sizeof buf);

    /* also pick up test files next to the project */
    bool ok = build_load(&b, entry);
    const char *root = b.root ? b.root : ".";
    char tdir[1300];
    snprintf(tdir, sizeof tdir, "%s/tests", root);
    if (dir_exists(tdir)) {
        int n = 0;
        char **names = list_dir(tdir, &n, NULL);
        for (int i = 0; i < n; i++) {
            if (strcmp(path_ext(names[i]), "spf") == 0) {
                char f[1500];
                snprintf(f, sizeof f, "%s/%s", tdir, names[i]);
                build_load(&b, f);
            }
            free(names[i]);
        }
        free(names);
    }
    ok = ok && build_check(&b) && build_lower(&b);
    build_report(&b);
    if (!ok) { build_dispose(&b); return 1; }

    VM *vm = vm_new(b.prog, b.sm, &b.arena);
    int pass = 0, fail = 0;
    double total = 0;
    fprintf(stderr, "\n");
    vec_foreach(i, &b.sema->all_fns) {
        FnDecl *fn = b.sema->all_fns.items[i];
        if (!fn->is_test || fn->ir_index < 0) continue;
        double t0 = now_seconds();
        vm->had_error = false;
        vm_call_function(vm, fn->ir_index, NULL, 0);
        double dt = now_seconds() - t0;
        total += dt;
        if (vm->had_error) {
            fail++;
            fprintf(stderr, "  %sfail%s  %s\n        %s%s%s\n", C_RED, C_RESET, fn->name,
                    C_DIM, vm->error_msg, C_RESET);
        } else {
            pass++;
            fprintf(stderr, "  %spass%s  %s %s%.2fms%s\n", C_GREEN, C_RESET, fn->name, C_DIM, dt * 1e3, C_RESET);
        }
    }
    /* benchmarks */
    vec_foreach(i, &b.sema->all_fns) {
        FnDecl *fn = b.sema->all_fns.items[i];
        if (!fn->is_bench || fn->ir_index < 0) continue;
        int reps = 0;
        double t0 = now_seconds(), spent = 0;
        while (spent < 0.25 && reps < 1000000) {
            vm_call_function(vm, fn->ir_index, NULL, 0);
            reps++;
            spent = now_seconds() - t0;
            if (vm->had_error) break;
        }
        if (vm->had_error)
            fprintf(stderr, "  %sfail%s  %s %s\n", C_RED, C_RESET, fn->name, vm->error_msg);
        else
            fprintf(stderr, "  %sbench%s %s  %s%.0f ops/s  %.3fms each%s\n", C_AMBER, C_RESET, fn->name,
                    C_DIM, reps / (spent > 0 ? spent : 1), spent * 1e3 / (reps ? reps : 1), C_RESET);
    }
    fprintf(stderr, "\n  %s%d passed%s, %s%d failed%s  %s%.0fms%s\n\n",
            pass ? C_GREEN : C_DIM, pass, C_RESET, fail ? C_RED : C_DIM, fail, C_RESET,
            C_DIM, total * 1e3, C_RESET);
    vm_free(vm);
    vm_native_shutdown();
    build_dispose(&b);
    return fail ? 1 : 0;
}

/* ----------------------------------------------------------------- new */
static const char *TEMPLATE_MAIN =
"~~ %s — made with SPRFST\n"
"use std.io\n"
"\n"
"fn main() {\n"
"    io.say(\"hello from %s\")\n"
"\n"
"    let numbers = [3, 1, 4, 1, 5, 9, 2, 6]\n"
"    let total = numbers |> sum\n"
"    io.say(\"the numbers add up to \" + total.to_text())\n"
"}\n"
"\n"
"test \"numbers add up\" {\n"
"    assert([1, 2, 3] |> sum == 6, \"sum should be 6\")\n"
"}\n";

static const char *TEMPLATE_APP_MAIN =
"~~ %s — a desktop app made with SPRFST\n"
"~~ Run it with:  sprfst run .\n"
"~~ State lives in `var`. Text with {holes} redraws by itself when the state changes.\n"
"use std.io\n"
"\n"
"app \"%s\" {\n"
"    var draft = \"\"\n"
"    var todos: [Text] = []\n"
"\n"
"    window {\n"
"        title: \"%s\"\n"
"        width: 460\n"
"        height: 480\n"
"\n"
"        title \"{todos.len()} things to do\"\n"
"        field \"What needs doing?\" {\n"
"            value: draft\n"
"            on change(text) {\n"
"                draft = text\n"
"            }\n"
"        }\n"
"        button \"Add\" {\n"
"            on click {\n"
"                if draft.trim().len() > 0 {\n"
"                    todos.push(draft.trim())\n"
"                    draft = \"\"\n"
"                }\n"
"            }\n"
"        }\n"
"        divider\n"
"        list {\n"
"            items: todos\n"
"        }\n"
"        button \"Clear all\" {\n"
"            on click {\n"
"                todos = []\n"
"            }\n"
"        }\n"
"    }\n"
"}\n";

static const char *TEMPLATE_PROJECT =
"[package]\n"
"name = \"%s\"\n"
"version = \"0.1.0\"\n"
"entry = \"src/main.spf\"\n"
"description = \"A SPRFST project\"\n"
"\n"
"[packages]\n";

static const char *TEMPLATE_IGNORE = "build/\npackages/\n*.spb\n";

static int cmd_new(const char *name, bool is_app) {
    if (!name || !*name) { fprintf(stderr, "  %serror%s  give the project a name:  sprfst new my-app\n", C_RED, C_RESET); return 1; }
    if (dir_exists(name)) { fprintf(stderr, "  %serror%s  `%s` already exists\n", C_RED, C_RESET, name); return 1; }
    char p[1200];
    snprintf(p, sizeof p, "%s/src", name); make_dir_all(p);
    snprintf(p, sizeof p, "%s/tests", name); make_dir_all(p);
    snprintf(p, sizeof p, "%s/assets", name); make_dir_all(p);

    char buf[4096];
    if (is_app) snprintf(buf, sizeof buf, TEMPLATE_APP_MAIN, name, name, name);
    else snprintf(buf, sizeof buf, TEMPLATE_MAIN, name, name);
    snprintf(p, sizeof p, "%s/src/main.spf", name);
    write_file_bytes(p, buf, strlen(buf));

    snprintf(buf, sizeof buf, TEMPLATE_PROJECT, name);
    snprintf(p, sizeof p, "%s/project.sprfst", name);
    write_file_bytes(p, buf, strlen(buf));

    snprintf(p, sizeof p, "%s/.gitignore", name);
    write_file_bytes(p, TEMPLATE_IGNORE, strlen(TEMPLATE_IGNORE));

    fprintf(stderr, "  %screated%s %s\n\n     cd %s\n     sprfst run .\n\n", C_GREEN, C_RESET, name, name);
    return 0;
}

/* --------------------------------------------------------------- clean */
static int cmd_clean(void) {
    Arena a;
    arena_init(&a);
    char *root = project_find_root(&a, ".");
    char p[1200];
    snprintf(p, sizeof p, "%s/build", root ? root : ".");
    bool had = dir_exists(p);
    if (had) remove_dir_all(p);
    fprintf(stderr, "  %sclean%s  %s\n", C_GREEN, C_RESET, had ? p : "nothing to remove");
    arena_free(&a);
    return 0;
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv) {
    g_color = term_supports_color(stderr);
    if (argc < 2) { usage(); return 1; }

    Opts o;
    memset(&o, 0, sizeof o);
    o.opt_level = 1;
    const char *verb = argv[1];

    static char *empty[1] = { NULL };
    o.rest_argv = empty;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { o.rest_argc = argc - i - 1; o.rest_argv = argv + i + 1; break; }
        if (strncmp(a, "-O", 2) == 0 && isdigit((unsigned char)a[2])) { o.opt_level = a[2] - '0'; o.opt_set = true; continue; }
        if (strcmp(a, "--json") == 0) { o.json = true; continue; }
        if (strcmp(a, "--ir") == 0) { o.dump_ir = true; continue; }
        if (strcmp(a, "--time") == 0) { o.timing = true; continue; }
        if (strcmp(a, "--quiet") == 0 || strcmp(a, "-q") == 0) { o.quiet = true; continue; }
        if (strcmp(a, "--no-color") == 0) { g_color = false; continue; }
        if (a[0] != '-' && !o.path) { o.path = a; continue; }
    }
    vm_set_args(o.rest_argc, o.rest_argv);

    if (strcmp(verb, "run") == 0)      return cmd_run(&o);
    if (strcmp(verb, "check") == 0)    return cmd_check(&o);
    if (strcmp(verb, "build") == 0)    return cmd_build(&o);
    if (strcmp(verb, "test") == 0)     return cmd_test(&o);
    if (strcmp(verb, "new") == 0) {
        /* `sprfst new app <name>` starts a desktop app; `sprfst new <name>` a plain project */
        if (o.path && strcmp(o.path, "app") == 0) {
            const char *nm = NULL;
            for (int i = 3; i < argc; i++) if (argv[i][0] != '-') { nm = argv[i]; break; }
            return cmd_new(nm, true);
        }
        return cmd_new(o.path, false);
    }
    if (strcmp(verb, "clean") == 0)    return cmd_clean();
    if (strcmp(verb, "fmt") == 0)      return cmd_fmt(argc - 2, argv + 2);
    if (strcmp(verb, "lint") == 0)     return cmd_lint(argc - 2, argv + 2);
    if (strcmp(verb, "docs") == 0)     return cmd_docs(argc - 2, argv + 2);
    if (strcmp(verb, "debug") == 0)    return cmd_debug(argc - 2, argv + 2);
    if (strcmp(verb, "add") == 0 || strcmp(verb, "remove") == 0 ||
        strcmp(verb, "install") == 0 || strcmp(verb, "package") == 0)
        return cmd_forge(verb, argc - 2, argv + 2);
    if (strcmp(verb, "studio") == 0)   return cmd_studio(argc - 2, argv + 2);
    if (strcmp(verb, "version") == 0 || strcmp(verb, "--version") == 0 || strcmp(verb, "-v") == 0) {
        printf("sprfst %s (%s)\n", SPRFST_VERSION, SPRFST_CODENAME);
        return 0;
    }
    if (strcmp(verb, "help") == 0 || strcmp(verb, "--help") == 0 || strcmp(verb, "-h") == 0) { usage(); return 0; }

    fprintf(stderr, "  %serror%s  there is no verb `%s`\n", C_RED, C_RESET, verb);
    fprintf(stderr, "  %stry%s    sprfst help\n", C_DIM, C_RESET);
    return 1;
}
