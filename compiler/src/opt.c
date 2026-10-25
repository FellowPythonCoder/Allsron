/* ========================================================================
   SPRFST — SPIR optimizer
   Linear, register based passes:
     1. constant folding        (within straight line regions)
     2. copy propagation        (MOVE chains)
     3. dead code elimination   (pure instructions whose result is never read)
     4. jump threading          (jump -> jump)
     5. compaction              (drop NOPs, renumber every branch target)
   ======================================================================== */
#include "sprfst/vm.h"
#include <math.h>

static bool op_is_pure(OpCode op) {
    switch (op) {
        case OP_CONST: case OP_MOVE: case OP_NIL: case OP_ADD: case OP_SUB:
        case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: case OP_NEG:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
        case OP_NOT: case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL:
        case OP_SHR: case OP_GETFIELD: case OP_GETGLOBAL: case OP_GETCAP:
        case OP_TYPEOF: case OP_ISTYPE: case OP_ISOK: case OP_ISNIL:
        case OP_GETTAG: case OP_PAYLOAD: case OP_LEN: case OP_RANGE:
            return true;
        default: return false;
    }
}

/* which registers does this instruction write? (-1 = none) */
static int op_dest(Instr *in) {
    switch (in->op) {
        case OP_SETFIELD: case OP_SETINDEX: case OP_SETGLOBAL: case OP_JUMP:
        case OP_BRTRUE: case OP_BRFALSE: case OP_RET: case OP_RETNIL:
        case OP_FAIL: case OP_TRYPUSH: case OP_TRYPOP: case OP_HALT:
        case OP_LINE: case OP_DROP: case OP_ASSERT: case OP_NOP:
            return -1;
        default: return in->a;
    }
}

static void mark_targets(IRFunc *f, bool *is_target) {
    vec_foreach(i, &f->code) {
        Instr *in = &f->code.items[i];
        if (in->op == OP_JUMP || in->op == OP_BRTRUE || in->op == OP_BRFALSE || in->op == OP_TRYPUSH) {
            if (in->a >= 0 && in->a <= f->code.len) is_target[in->a] = true;
        }
        if (in->op == OP_ITERNEXT) { if (in->c >= 0 && in->c <= f->code.len) is_target[in->c] = true; }
    }
}

static int fold_function(IRFunc *f) {
    int folded = 0;
    int n = f->code.len;
    bool *is_target = calloc((size_t)n + 2, 1);
    mark_targets(f, is_target);

    /* known[reg] = index into consts, or -1 */
    int nregs = f->nregs + 4;
    int *known = malloc(sizeof(int) * (size_t)nregs);
    for (int i = 0; i < nregs; i++) known[i] = -1;

    for (int i = 0; i < n; i++) {
        if (is_target[i]) for (int r = 0; r < nregs; r++) known[r] = -1;
        Instr *in = &f->code.items[i];
        int d = op_dest(in);
        switch (in->op) {
            case OP_CONST:
                if (in->a < nregs) known[in->a] = in->b;
                continue;
            case OP_MOVE:
                if (in->a < nregs && in->b < nregs) known[in->a] = known[in->b];
                continue;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
                if (in->b >= nregs || in->c >= nregs) break;
                int ka = known[in->b], kb = known[in->c];
                if (ka < 0 || kb < 0) break;
                Value va = f->consts.items[ka], vb = f->consts.items[kb];
                if ((va.tag != V_INT && va.tag != V_NUM) || (vb.tag != V_INT && vb.tag != V_NUM)) break;
                bool both_int = (va.tag == V_INT && vb.tag == V_INT);
                double x = va.tag == V_INT ? (double)va.as.i : va.as.n;
                double y = vb.tag == V_INT ? (double)vb.as.i : vb.as.n;
                Value out;
                bool ok = true;
                switch (in->op) {
                    case OP_ADD: out = both_int ? v_int(va.as.i + vb.as.i) : v_num(x + y); break;
                    case OP_SUB: out = both_int ? v_int(va.as.i - vb.as.i) : v_num(x - y); break;
                    case OP_MUL: out = both_int ? v_int(va.as.i * vb.as.i) : v_num(x * y); break;
                    case OP_DIV: if (y == 0) { ok = false; out = v_nil(); } else out = v_num(x / y); break;
                    case OP_MOD: if (y == 0) { ok = false; out = v_nil(); } else out = both_int ? v_int(va.as.i % vb.as.i) : v_num(fmod(x, y)); break;
                    case OP_LT: out = v_bool(x < y); break;
                    case OP_LE: out = v_bool(x <= y); break;
                    case OP_GT: out = v_bool(x > y); break;
                    case OP_GE: out = v_bool(x >= y); break;
                    case OP_EQ: out = v_bool(x == y); break;
                    case OP_NE: out = v_bool(x != y); break;
                    default: ok = false; out = v_nil();
                }
                if (!ok) break;
                int k = -1;
                vec_foreach(ci, &f->consts) {
                    Value c = f->consts.items[ci];
                    if (c.tag != out.tag) continue;
                    if (out.tag == V_INT && c.as.i == out.as.i) { k = ci; break; }
                    if (out.tag == V_NUM && c.as.n == out.as.n) { k = ci; break; }
                    if (out.tag == V_BOOL && c.as.b == out.as.b) { k = ci; break; }
                }
                if (k < 0) { vec_push(&f->consts, out); k = f->consts.len - 1; }
                in->op = OP_CONST;
                in->b = k;
                in->c = 0;
                if (in->a < nregs) known[in->a] = k;
                folded++;
                continue;
            }
            default: break;
        }
        if (d >= 0 && d < nregs) known[d] = -1;
        if (in->op == OP_CALL || in->op == OP_CALLV || in->op == OP_NATIVE || in->op == OP_METHOD)
            for (int r = 0; r < nregs; r++) known[r] = -1;
    }
    free(known);
    free(is_target);
    return folded;
}

static int remove_self_moves(IRFunc *f) {
    int removed = 0;
    vec_foreach(i, &f->code) {
        Instr *in = &f->code.items[i];
        if (in->op == OP_MOVE && in->a == in->b) { in->op = OP_NOP; removed++; }
    }
    return removed;
}

static int dead_code(IRProgram *p, IRFunc *f) {
    int removed = 0;
    int nregs = f->nregs + 4;
    bool *read = calloc((size_t)nregs, 1);
    /* a register is "read" if any instruction uses it as a source */
    vec_foreach(i, &f->code) {
        Instr *in = &f->code.items[i];
        switch (in->op) {
            case OP_CLOSURE: {
                /* a closure reads the registers it captures from this frame */
                if (p && in->b >= 0 && in->b < p->funcs.len) {
                    IRFunc *tf = p->funcs.items[in->b];
                    for (int k = 0; k < tf->ncaps; k++) {
                        int src = tf->cap_src ? tf->cap_src[k] : k;
                        if (src >= 0 && src < nregs) read[src] = true;
                    }
                }
                break;
            }
            case OP_CONST: case OP_NIL: case OP_GETGLOBAL: case OP_GETCAP:
                break;
            case OP_MOVE: case OP_NEG: case OP_NOT: case OP_TOTEXT: case OP_TYPEOF:
            case OP_ISOK: case OP_ISNIL: case OP_UNWRAP: case OP_GETTAG: case OP_LEN:
            case OP_ITERNEW: case OP_AWAIT: case OP_OK: case OP_ERR: case OP_CAST:
            case OP_ISTYPE: case OP_GETFIELD: case OP_PAYLOAD: case OP_ITERNEXT: case OP_ITERKEY:
                if (in->b >= 0 && in->b < nregs) read[in->b] = true;
                break;
            case OP_SETGLOBAL: case OP_RET: case OP_FAIL: case OP_DROP: case OP_BRTRUE:
            case OP_BRFALSE:
                if (in->op == OP_SETGLOBAL || in->op == OP_BRTRUE || in->op == OP_BRFALSE) {
                    if (in->b >= 0 && in->b < nregs) read[in->b] = true;
                } else if (in->a >= 0 && in->a < nregs) read[in->a] = true;
                break;
            case OP_RANGE: {
                /* c is not a plain register here: it packs the upper bound's
                   register with the inclusive flag, (reg << 1) | inclusive.
                   Decoding it is what keeps the bound alive — read as a plain
                   register it names the wrong one, and the CONST that loads
                   the bound is then removed as dead. The loop that follows
                   runs zero times. */
                if (in->b >= 0 && in->b < nregs) read[in->b] = true;
                int hi = in->c >> 1;
                if (hi >= 0 && hi < nregs) read[hi] = true;
                break;
            }
            case OP_SETFIELD:
                if (in->a >= 0 && in->a < nregs) read[in->a] = true;
                if (in->c >= 0 && in->c < nregs) read[in->c] = true;
                break;
            case OP_SETINDEX:
                if (in->a >= 0 && in->a < nregs) read[in->a] = true;
                if (in->b >= 0 && in->b < nregs) read[in->b] = true;
                if (in->c >= 0 && in->c < nregs) read[in->c] = true;
                break;
            case OP_CALL: case OP_NATIVE: case OP_METHOD: case OP_SPAWN: case OP_CALLV:
            case OP_NEWLIST: case OP_NEWMAP: case OP_NEWSET: case OP_NEWOBJ: case OP_VARIANT: {
                int base = -1, cnt = 0;
                if (in->op == OP_NEWLIST || in->op == OP_NEWMAP || in->op == OP_NEWSET) { base = in->b; cnt = in->c * (in->op == OP_NEWMAP ? 2 : 1); }
                else { base = (in->c >> 8); cnt = in->c & 0xff; }
                for (int r = base; r < base + cnt && r >= 0 && r < nregs; r++) read[r] = true;
                if (in->op == OP_CALLV && in->b >= 0 && in->b < nregs) read[in->b] = true;
                if (in->op == OP_SPAWN && in->b >= 0 && in->b < nregs) read[in->b] = true;
                break;
            }
            default:
                if (in->b >= 0 && in->b < nregs) read[in->b] = true;
                if (in->c >= 0 && in->c < nregs) read[in->c] = true;
                break;
        }
    }
    vec_foreach(i, &f->code) {
        Instr *in = &f->code.items[i];
        if (!op_is_pure((OpCode)in->op)) continue;
        int d = op_dest(in);
        if (d < 0 || d >= nregs) continue;
        if (d < f->nparams) continue;            /* parameters are live on entry */
        if (!read[d]) { in->op = OP_NOP; removed++; }
    }
    free(read);
    return removed;
}

static int thread_jumps(IRFunc *f) {
    int threaded = 0;
    vec_foreach(i, &f->code) {
        Instr *in = &f->code.items[i];
        if (in->op != OP_JUMP && in->op != OP_BRTRUE && in->op != OP_BRFALSE) continue;
        int t = in->a, guard = 0;
        while (t >= 0 && t < f->code.len && f->code.items[t].op == OP_JUMP && guard++ < 64)
            t = f->code.items[t].a;
        while (t >= 0 && t < f->code.len && f->code.items[t].op == OP_NOP) t++;
        if (t != in->a) { in->a = t; threaded++; }
    }
    return threaded;
}

static int compact(IRFunc *f) {
    int n = f->code.len;
    int *map = malloc(sizeof(int) * (size_t)(n + 1));
    int out = 0;
    for (int i = 0; i < n; i++) {
        map[i] = out;
        if (f->code.items[i].op != OP_NOP) out++;
    }
    map[n] = out;
    if (out == n) { free(map); return 0; }
    Instr *dst = f->code.items;
    int w = 0;
    for (int i = 0; i < n; i++) {
        Instr in = f->code.items[i];
        if (in.op == OP_NOP) continue;
        if (in.op == OP_JUMP || in.op == OP_BRTRUE || in.op == OP_BRFALSE || in.op == OP_TRYPUSH) {
            if (in.a >= 0 && in.a <= n) in.a = map[in.a];
        }
        if (in.op == OP_ITERNEXT) { if (in.c >= 0 && in.c <= n) in.c = map[in.c]; }
        dst[w++] = in;
    }
    f->code.len = w;
    free(map);
    return n - w;
}

OptStats ir_optimize(IRProgram *p, int level) {
    OptStats st = { 0 };
    if (level <= 0) return st;
    vec_foreach(i, &p->funcs) {
        IRFunc *f = p->funcs.items[i];
        for (int pass = 0; pass < (level >= 2 ? 3 : 1); pass++) {
            st.folded        += fold_function(f);
            st.moves_removed += remove_self_moves(f);
            st.dead_removed  += dead_code(p, f);
            st.jumps_threaded+= thread_jumps(f);
            st.blocks_removed+= compact(f);
        }
    }
    return st;
}
