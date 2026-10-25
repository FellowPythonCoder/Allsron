/* ========================================================================
   sprfst debug — the SPRFST debugger

   A real stop-the-world debugger driven by the VM's per instruction hook:
   breakpoints (plain and conditional), step over / into / out, call stack,
   locals with their source names, watch expressions, heap and memory
   figures.  The same engine answers the Studio debug protocol.
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/driver.h"
#include "sprfst/parser.h"
#include <ctype.h>

#define MAX_BP 64
#define MAX_WATCH 16

typedef struct {
    bool  active;
    char  file[256];
    int   line;
    char  cond[256];       /* empty = unconditional */
    int   hits;
} Breakpoint;

typedef struct {
    Build      *build;
    VM         *vm;
    Breakpoint  bps[MAX_BP];
    int         nbps;
    char        watches[MAX_WATCH][128];
    int         nwatch;
    int         mode;          /* 0 run, 1 step into, 2 step over, 3 step out, 4 stopped */
    int         depth_target;
    int         last_line;
    const char *last_file;
    bool        attached;
    bool        quit;
} Debugger;

static Debugger g_dbg;

/* ------------------------------------------------------------- printing */
static void show_source(Debugger *d, const char *file, int line) {
    (void)file;
    SourceMap *sm = d->build->sm;
    Frame *f = &d->vm->frames[d->vm->nframes - 1];
    int fid = f->fn->file_id;
    for (int l = line - 2; l <= line + 2; l++) {
        if (l < 1) continue;
        Str s = sourcemap_line(sm, fid, l);
        if (!s.p) continue;
        if (l == line)
            fprintf(stderr, "  %s%4d >%s %s" STRFMT "%s\n", C_AMBER, l, C_RESET, C_BOLD, STRARG(s), C_RESET);
        else
            fprintf(stderr, "  %s%4d  " STRFMT "%s\n", C_DIM, l, STRARG(s), C_RESET);
    }
}

static void show_stack(Debugger *d) {
    VM *vm = d->vm;
    for (int i = vm->nframes - 1; i >= 0; i--) {
        Frame *f = &vm->frames[i];
        const char *file = "?";
        if (f->fn->file_id >= 0) {
            SourceFile *sf = sourcemap_get(vm->sm, f->fn->file_id);
            if (sf) file = path_basename(sf->path);
        }
        int line = f->ip > 0 && f->ip <= f->fn->code.len ? f->fn->code.items[f->ip - 1].line : f->fn->line;
        fprintf(stderr, "  %s%s%2d%s  %s%s%s  %s%s:%d%s\n",
                i == vm->nframes - 1 ? C_AMBER : C_DIM, i == vm->nframes - 1 ? "▸" : " ",
                i, C_RESET, C_BOLD, f->fn->qualname ? f->fn->qualname : f->fn->name, C_RESET,
                C_DIM, file, line, C_RESET);
    }
}

static void show_vars(Debugger *d) {
    VM *vm = d->vm;
    if (!vm->nframes) return;
    Frame *f = &vm->frames[vm->nframes - 1];
    int shown = 0;
    for (int i = 0; i < f->fn->local_names.len && i < f->fn->nregs; i++) {
        const char *nm = f->fn->local_names.items[i];
        if (!nm || !*nm) continue;
        char *txt = vm_value_text(vm, f->regs[i], true);
        fprintf(stderr, "  %s%-16s%s %s\n", C_AMBER, nm, C_RESET, txt);
        free(txt);
        shown++;
    }
    if (!shown) fprintf(stderr, "  %sno named locals in this frame%s\n", C_DIM, C_RESET);
}

static Value eval_in_frame(Debugger *d, const char *expr, bool *ok) {
    /* names resolve against the current frame's locals */
    *ok = false;
    VM *vm = d->vm;
    if (!vm->nframes) return v_nil();
    Frame *f = &vm->frames[vm->nframes - 1];
    char name[128];
    snprintf(name, sizeof name, "%s", expr);
    char *sp = name;
    while (*sp == ' ') sp++;
    char *end = sp + strlen(sp);
    while (end > sp && isspace((unsigned char)end[-1])) *--end = 0;

    /* plain local */
    for (int i = 0; i < f->fn->local_names.len && i < f->fn->nregs; i++) {
        const char *nm = f->fn->local_names.items[i];
        if (nm && strcmp(nm, sp) == 0) { *ok = true; return f->regs[i]; }
    }
    /* global by name */
    vec_foreach(i, &d->build->prog->global_names) {
        if (strcmp(d->build->prog->global_names.items[i], sp) == 0 && i < d->build->prog->globals.len) {
            *ok = true;
            return d->build->prog->globals.items[i];
        }
    }
    /* integer literal */
    char *endp = NULL;
    long long v = strtoll(sp, &endp, 10);
    if (endp && *endp == 0 && endp != sp) { *ok = true; return v_int(v); }
    return v_nil();
}

static bool cond_true(Debugger *d, const char *cond) {
    /* supported forms: NAME, NAME == VALUE, NAME != VALUE, NAME > VALUE, NAME < VALUE */
    char lhs[128] = { 0 }, op[4] = { 0 }, rhs[128] = { 0 };
    int n = sscanf(cond, "%127s %3s %127s", lhs, op, rhs);
    bool ok = false;
    Value a = eval_in_frame(d, lhs, &ok);
    if (!ok) return true;                 /* unknown name: stop anyway */
    if (n < 3) return v_truthy(a);
    bool rok = false;
    Value b = eval_in_frame(d, rhs, &rok);
    if (!rok) {
        /* text compare */
        char *at = vm_value_text(d->vm, a, false);
        bool eq = strcmp(at, rhs) == 0;
        free(at);
        return strcmp(op, "!=") == 0 ? !eq : eq;
    }
    double x = v_tonum(a), y = v_tonum(b);
    if (strcmp(op, "==") == 0) return vm_values_equal(a, b);
    if (strcmp(op, "!=") == 0) return !vm_values_equal(a, b);
    if (strcmp(op, ">") == 0)  return x > y;
    if (strcmp(op, "<") == 0)  return x < y;
    if (strcmp(op, ">=") == 0) return x >= y;
    if (strcmp(op, "<=") == 0) return x <= y;
    return true;
}

/* --------------------------------------------------------- command loop */
static void dbg_help(void) {
    fprintf(stderr,
"\n  %sdebugger%s\n"
"    %sr%s  run            %sc%s  continue       %ss%s  step into\n"
"    %sn%s  next (over)    %so%s  step out       %sq%s  quit\n"
"    %sb%s  <line>         break here     %sb%s <line> if <cond>\n"
"    %sd%s  <n>            delete break   %sl%s  list breaks\n"
"    %sk%s  call stack     %sv%s  variables      %sp%s <name> print\n"
"    %sw%s  <expr> watch   %sm%s  memory         %s?%s  this help\n\n",
    C_BOLD, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET,
    C_AMBER, C_RESET, C_AMBER, C_RESET, C_AMBER, C_RESET);
}

static void dbg_prompt(Debugger *d, const char *file, int line) {
    char buf[512];
    for (;;) {
        /* watches first */
        for (int i = 0; i < d->nwatch; i++) {
            bool ok = false;
            Value v = eval_in_frame(d, d->watches[i], &ok);
            char *t = vm_value_text(d->vm, v, true);
            fprintf(stderr, "  %swatch%s %s = %s%s%s\n", C_DIM, C_RESET, d->watches[i],
                    ok ? C_BOLD : C_DIM, ok ? t : "not in scope", C_RESET);
            free(t);
        }
        fprintf(stderr, "%s(debug)%s ", C_AMBER, C_RESET);
        fflush(stderr);
        if (!fgets(buf, sizeof buf, stdin)) { d->quit = true; d->mode = 0; return; }
        size_t n = strlen(buf);
        while (n && isspace((unsigned char)buf[n-1])) buf[--n] = 0;
        if (!n) continue;

        char cmd = buf[0];
        const char *arg = buf + 1;
        while (*arg == ' ') arg++;

        switch (cmd) {
            case 'r': case 'c': d->mode = 0; return;
            case 's': d->mode = 1; return;
            case 'n': d->mode = 2; d->depth_target = d->vm->nframes; return;
            case 'o': d->mode = 3; d->depth_target = d->vm->nframes - 1; return;
            case 'q': d->quit = true; d->mode = 0; return;
            case 'k': show_stack(d); break;
            case 'v': show_vars(d); break;
            case 'm':
                fprintf(stderr, "  %sheap%s %zu KB   %speak%s %zu KB   %sobjects%s %d   %sgc runs%s %d   %sinstructions%s %d\n",
                        C_DIM, C_RESET, d->vm->bytes_allocated / 1024, C_DIM, C_RESET, d->vm->peak_bytes / 1024,
                        C_DIM, C_RESET, d->vm->obj_count, C_DIM, C_RESET, d->vm->gc_runs,
                        C_DIM, C_RESET, d->vm->instr_count);
                break;
            case 'p': {
                bool ok = false;
                Value v = eval_in_frame(d, arg, &ok);
                if (!ok) fprintf(stderr, "  %s`%s` is not in scope here%s\n", C_DIM, arg, C_RESET);
                else { char *t = vm_value_text(d->vm, v, true); fprintf(stderr, "  %s = %s\n", arg, t); free(t); }
                break;
            }
            case 'w':
                if (d->nwatch < MAX_WATCH && *arg) {
                    snprintf(d->watches[d->nwatch++], 128, "%.127s", arg);
                    fprintf(stderr, "  %swatching%s %s\n", C_GREEN, C_RESET, arg);
                }
                break;
            case 'b': {
                if (!*arg) { fprintf(stderr, "  usage: b <line> [if <cond>]\n"); break; }
                int ln = atoi(arg);
                const char *ifp = strstr(arg, " if ");
                /* snap forward to a line that has code */
                int best = 0;
                vec_foreach(fi, &d->build->prog->funcs) {
                    IRFunc *f = d->build->prog->funcs.items[fi];
                    vec_foreach(ii, &f->code) {
                        int l = f->code.items[ii].line;
                        if (l >= ln && (best == 0 || l < best)) best = l;
                    }
                }
                if (best && best != ln) {
                    fprintf(stderr, "  %sline %d has no code, using line %d%s\n", C_DIM, ln, best, C_RESET);
                    ln = best;
                }
                if (d->nbps < MAX_BP && ln > 0) {
                    Breakpoint *bp = &d->bps[d->nbps++];
                    memset(bp, 0, sizeof *bp);
                    bp->active = true;
                    bp->line = ln;
                    snprintf(bp->file, sizeof bp->file, "%s", file ? path_basename(file) : "");
                    if (ifp) snprintf(bp->cond, sizeof bp->cond, "%s", ifp + 4);
                    fprintf(stderr, "  %sbreak %d%s at line %d%s%s\n", C_GREEN, d->nbps - 1, C_RESET, ln,
                            bp->cond[0] ? " if " : "", bp->cond);
                }
                break;
            }
            case 'd': {
                int k = atoi(arg);
                if (k >= 0 && k < d->nbps) { d->bps[k].active = false; fprintf(stderr, "  %sdeleted%s %d\n", C_DIM, C_RESET, k); }
                break;
            }
            case 'l':
                for (int i = 0; i < d->nbps; i++)
                    if (d->bps[i].active)
                        fprintf(stderr, "  %s%d%s  %s:%d%s%s  %shits %d%s\n", C_AMBER, i, C_RESET,
                                d->bps[i].file, d->bps[i].line, d->bps[i].cond[0] ? " if " : "", d->bps[i].cond,
                                C_DIM, d->bps[i].hits, C_RESET);
                break;
            case '?': case 'h': dbg_help(); break;
            default:
                fprintf(stderr, "  %sunknown command `%c` — press ? for help%s\n", C_DIM, cmd, C_RESET);
        }
        (void)line;
    }
}

/* ------------------------------------------------------------ VM hook */
static void on_step(VM *vm, Frame *f, int line, const char *file) {
    Debugger *d = &g_dbg;
    (void)f;
    if (d->quit) { vm->had_error = true; snprintf(vm->error_msg, sizeof vm->error_msg, "stopped by the debugger"); return; }
    if (line <= 0) return;

    bool stop = false;
    const char *why = "";

    if (d->mode == 1 && (line != d->last_line || file != d->last_file)) { stop = true; why = "step"; }
    else if (d->mode == 2 && vm->nframes <= d->depth_target && line != d->last_line) { stop = true; why = "next"; }
    else if (d->mode == 3 && vm->nframes <= d->depth_target) { stop = true; why = "returned"; }

    if (!stop) {
        const char *base = file ? path_basename(file) : "";
        for (int i = 0; i < d->nbps; i++) {
            Breakpoint *bp = &d->bps[i];
            if (!bp->active || bp->line != line) continue;
            if (bp->file[0] && base && strcmp(bp->file, base) != 0) continue;
            if (line == d->last_line && file == d->last_file) continue;
            if (bp->cond[0] && !cond_true(d, bp->cond)) continue;
            bp->hits++;
            stop = true;
            why = "breakpoint";
            break;
        }
    }
    d->last_line = line;
    d->last_file = file;
    if (!stop) return;

    fprintf(stderr, "\n  %s%s%s  %s%s:%d%s  in %s%s%s\n", C_AMBER, why, C_RESET,
            C_DIM, file ? path_basename(file) : "?", line, C_RESET,
            C_BOLD, vm->nframes ? vm->frames[vm->nframes-1].fn->qualname : "?", C_RESET);
    show_source(d, file, line);
    dbg_prompt(d, file, line);
}

/* ---------------------------------------------------------------- entry */
int cmd_debug(int argc, char **argv) {
    const char *target = NULL;
    int pending_lines[MAX_BP];
    int npending = 0;
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--break=", 8) == 0 && npending < MAX_BP) pending_lines[npending++] = atoi(argv[i] + 8);
        else if (argv[i][0] != '-') target = argv[i];
    }

    Build b;
    build_init(&b, 0);                 /* no optimisation: line numbers stay honest */
    char entry[1300];
    if (target && file_exists(target)) snprintf(entry, sizeof entry, "%s", target);
    else {
        char *root = project_find_root(&b.arena, target ? target : ".");
        Project p = project_load(root ? root : ".");
        snprintf(entry, sizeof entry, "%s/%s", root ? root : ".", p.entry);
    }
    if (!build_compile(&b, entry)) { build_report(&b); build_dispose(&b); return 1; }
    build_report(&b);

    memset(&g_dbg, 0, sizeof g_dbg);
    g_dbg.build = &b;
    g_dbg.mode = 1;                    /* stop on the first statement */
    for (int i = 0; i < npending; i++) {
        Breakpoint *bp = &g_dbg.bps[g_dbg.nbps++];
        memset(bp, 0, sizeof *bp);
        bp->active = true;
        bp->line = pending_lines[i];
        snprintf(bp->file, sizeof bp->file, "%s", path_basename(entry));
    }

    VM *vm = vm_new(b.prog, b.sm, &b.arena);
    vm->debug_mode = true;
    vm->on_breakpoint = on_step;
    g_dbg.vm = vm;

    fprintf(stderr, "\n  %s%sSPRFST debugger%s  %s%s%s\n", C_BOLD, C_AMBER, C_RESET, C_DIM, entry, C_RESET);
    dbg_help();

    double t0 = now_seconds();
    int code = vm_run(vm, 0, NULL);
    fprintf(stderr, "\n  %sprogram finished%s  exit %d  %s%.0fms, %d instructions, %zu KB peak%s\n",
            C_GREEN, C_RESET, code, C_DIM, (now_seconds() - t0) * 1e3, vm->instr_count, vm->peak_bytes / 1024, C_RESET);
    vm_free(vm);
    vm_native_shutdown();
    build_dispose(&b);
    return code;
}
