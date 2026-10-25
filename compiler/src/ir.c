/* ========================================================================
   SPRFST — lowering the checked AST into SPIR
   ======================================================================== */
#include "sprfst/vm.h"
#include "sprfst/natives.h"

const char *const OP_NAMES[OP_COUNT] = {
#define X(o) #o,
    SPRFST_OPS(X)
#undef X
};

typedef struct { int *items; int len, cap; } IntVec;

typedef struct {
    IntVec breaks, continues;
    const char *label;
} LoopCtx;

typedef struct {
    Arena     *a;
    Sema      *sema;
    IRProgram *p;
    DiagBag   *db;
    IRFunc    *fn;
    FnDecl    *decl;
    int        next_reg, max_reg;
    LoopCtx    loops[32];
    int        nloops;
    BlockVec   defers;
    int        cur_line;
    int        cur_file;
} Lower;

/* ------------------------------------------------------------ utilities */
static int emit(Lower *L, OpCode op, int a, int b, int c) {
    Instr in = { (uint16_t)op, a, b, c, L->cur_line };
    vec_push(&L->fn->code, in);
    return L->fn->code.len - 1;
}
static void patch(Lower *L, int idx, int target) { L->fn->code.items[idx].a = target; }
static void patch_b(Lower *L, int idx, int target) { L->fn->code.items[idx].b = target; }
static int here(Lower *L) { return L->fn->code.len; }

static int reg_alloc(Lower *L) {
    int r = L->next_reg++;
    if (L->next_reg > L->max_reg) L->max_reg = L->next_reg;
    return r;
}
static int reg_mark(Lower *L) { return L->next_reg; }
static void reg_release(Lower *L, int mark) { L->next_reg = mark; }

static int const_add(Lower *L, Value v) {
    vec_foreach(i, &L->fn->consts) {
        Value c = L->fn->consts.items[i];
        if (c.tag != v.tag) continue;
        if (v.tag == V_INT && c.as.i == v.as.i) return i;
        if (v.tag == V_NUM && c.as.n == v.as.n) return i;
        if (v.tag == V_BOOL && c.as.b == v.as.b) return i;
        if (v.tag == V_NIL) return i;
    }
    vec_push(&L->fn->consts, v);
    return L->fn->consts.len - 1;
}

/* text constants are stored as C strings in a side table using O_TEXT objects
   created lazily by the VM; we keep the raw pointer in the constant pool. */
static int const_text(Lower *L, const char *s) {
    vec_foreach(i, &L->fn->consts) {
        Value c = L->fn->consts.items[i];
        if (c.tag == V_OBJ && c.as.o && c.as.o->kind == O_TEXT) {
            ObjText *t = (ObjText *)c.as.o;
            if (strcmp(t->chars, s) == 0) return i;
        }
    }
    ObjText *t = NEW(L->a, ObjText);
    t->obj.kind = O_TEXT;
    t->obj.marked = true;          /* constants are permanent roots */
    t->chars = (char *)s;
    t->len = (int)strlen(s);
    t->hash = hash_bytes(s, (size_t)t->len);
    Value v = v_obj((Obj *)t);
    vec_push(&L->fn->consts, v);
    return L->fn->consts.len - 1;
}

static void set_line(Lower *L, Span sp) {
    if (sp.file < 0) return;
    int line = 1, col = 1;
    sourcemap_pos(L->p->sm, sp, &line, &col);
    L->cur_line = line;
    L->cur_file = sp.file;
}

static int lower_expr(Lower *L, Expr *e);
static void lower_expr_to(Lower *L, Expr *e, int dest);
static void lower_block(Lower *L, Block *b);
static void lower_block_value(Lower *L, Block *b, int dest);
static void lower_stmt(Lower *L, Stmt *st);

/* ------------------------------------------------------- symbol access */
static void load_symbol(Lower *L, Symbol *sym, int dest) {
    if (!sym) { emit(L, OP_NIL, dest, 0, 0); return; }
    switch (sym->kind) {
        case SYM_VAR: case SYM_PARAM:
            if (sym->slot != dest) emit(L, OP_MOVE, dest, sym->slot, 0);
            break;
        case SYM_CAPTURE:
            /* the prologue copied the captured value into a local slot */
            if (sym->slot != dest) emit(L, OP_MOVE, dest, sym->slot, 0);
            break;
        case SYM_GLOBAL: case SYM_CONST:
            emit(L, OP_GETGLOBAL, dest, sym->slot, 0);
            break;
        case SYM_FN:
            if (sym->fn && sym->fn->ir_index >= 0)
                emit(L, OP_CLOSURE, dest, sym->fn->ir_index, 0);
            else
                emit(L, OP_CONST, dest, const_add(L, v_int(sym->slot)), 0);
            break;
        default:
            emit(L, OP_NIL, dest, 0, 0);
    }
}

static void store_symbol(Lower *L, Symbol *sym, int src) {
    if (!sym) return;
    switch (sym->kind) {
        case SYM_VAR: case SYM_PARAM:
            if (sym->slot != src) emit(L, OP_MOVE, sym->slot, src, 0);
            break;
        case SYM_GLOBAL: case SYM_CONST:
            emit(L, OP_SETGLOBAL, sym->slot, src, 0);
            break;
        default: break;
    }
}

/* --------------------------------------------------------- expressions */
static int type_id_of(Lower *L, Type *t) {
    if (!t || !t->decl) return -1;
    vec_foreach(i, &L->p->types)
        if (L->p->types.items[i]->decl == t->decl) return i;
    return -1;
}

static void lower_args(Lower *L, ExprVec *args, int base) {
    vec_foreach(i, args) {
        int r = base + i;
        if (r >= L->next_reg) { L->next_reg = r + 1; if (L->next_reg > L->max_reg) L->max_reg = L->next_reg; }
        lower_expr_to(L, args->items[i], r);
    }
}

static void lower_logical(Lower *L, Expr *e, int dest) {
    bool is_and = (e->as.binary.op == T_AND);
    lower_expr_to(L, e->as.binary.lhs, dest);
    int jmp = emit(L, is_and ? OP_BRFALSE : OP_BRTRUE, 0, dest, 0);
    lower_expr_to(L, e->as.binary.rhs, dest);
    patch(L, jmp, here(L));
}

static void lower_interp(Lower *L, Expr *e, int dest) {
    int mark = reg_mark(L);
    int acc = dest;
    int tmp = reg_alloc(L);
    bool first = true;
    int nparts = e->as.str.parts.len;
    for (int i = 0; i <= nparts; i++) {
        const char *chunk = i < e->as.str.chunks.len ? e->as.str.chunks.items[i] : "";
        if (chunk && *chunk) {
            emit(L, OP_CONST, tmp, const_text(L, chunk), 0);
            if (first) { emit(L, OP_MOVE, acc, tmp, 0); first = false; }
            else emit(L, OP_CONCAT, acc, acc, tmp);
        }
        if (i < nparts) {
            int pr = reg_alloc(L);
            lower_expr_to(L, e->as.str.parts.items[i], pr);
            emit(L, OP_TOTEXT, tmp, pr, 0);
            L->next_reg = pr;
            if (first) { emit(L, OP_MOVE, acc, tmp, 0); first = false; }
            else emit(L, OP_CONCAT, acc, acc, tmp);
        }
    }
    if (first) emit(L, OP_CONST, acc, const_text(L, ""), 0);
    reg_release(L, mark);
}

static OpCode binop_of(TokKind k) {
    switch (k) {
        case T_PLUS: return OP_ADD;
        case T_MINUS: return OP_SUB;
        case T_STAR: return OP_MUL;
        case T_SLASH: return OP_DIV;
        case T_PERCENT: return OP_MOD;
        case T_POW: return OP_POW;
        case T_EQ: return OP_EQ;
        case T_NE: return OP_NE;
        case T_LT: return OP_LT;
        case T_LE: return OP_LE;
        case T_GT: return OP_GT;
        case T_GE: return OP_GE;
        case T_AMP: return OP_BAND;
        case T_BAR: return OP_BOR;
        case T_CARET: return OP_BXOR;
        case T_SHL: return OP_SHL;
        case T_SHR: return OP_SHR;
        default: return OP_NOP;
    }
}

static void lower_match(Lower *L, Expr *e, int dest);
static void lower_if_expr(Lower *L, Expr *e, int dest);

/* Fields in layout order: a type's own fields first, then its base's.
   find_field in the analyser uses exactly this order. */
static int collect_fields(TypeDecl *td, FieldDecl **out, int cap) {
    int n = 0, guard = 0;
    for (TypeDecl *d = td; d && guard++ < 64; ) {
        vec_foreach(i, &d->fields) if (n < cap) out[n++] = &d->fields.items[i];
        if (!d->type || !d->type->ret || !d->type->ret->decl) break;
        d = d->type->ret->decl;
    }
    return n;
}

static void lower_struct_literal(Lower *L, Expr *e, int dest) {
    Type *t = e->type;
    int tid = type_id_of(L, t);
    TypeDecl *td = t ? t->decl : NULL;
    FieldDecl *flds[128];
    int n = td ? collect_fields(td, flds, 128) : 0;
    int base = L->next_reg;
    for (int i = 0; i < n; i++) {
        int r = reg_alloc(L);
        Expr *init = NULL;
        vec_foreach(k, &e->as.strct.fields)
            if (strcmp(e->as.strct.fields.items[k].name, flds[i]->name) == 0)
                init = e->as.strct.fields.items[k].value;
        if (init) lower_expr_to(L, init, r);
        else if (flds[i]->deflt) lower_expr_to(L, flds[i]->deflt, r);
        else emit(L, OP_NIL, r, 0, 0);
    }
    emit(L, OP_NEWOBJ, dest, tid, (base << 8) | (n & 0xff));
    L->next_reg = base;
}

static void lower_new(Lower *L, Expr *e, int dest) {
    Type *t = e->type;
    int tid = type_id_of(L, t);
    TypeDecl *td = t ? t->decl : NULL;
    if (!td) { emit(L, OP_NIL, dest, 0, 0); return; }
    IRType *irt = (tid >= 0) ? L->p->types.items[tid] : NULL;

    /* default-initialise every field */
    int base = L->next_reg;
    for (int i = 0; i < td->fields.len; i++) {
        int r = reg_alloc(L);
        if (td->fields.items[i].deflt) lower_expr_to(L, td->fields.items[i].deflt, r);
        else emit(L, OP_NIL, r, 0, 0);
    }
    emit(L, OP_NEWOBJ, dest, tid, (base << 8) | (td->fields.len & 0xff));
    L->next_reg = base;

    if (irt && irt->init_fn >= 0) {
        int abase = L->next_reg;
        int selfr = reg_alloc(L);
        emit(L, OP_MOVE, selfr, dest, 0);
        lower_args(L, &e->as.nw.args, abase + 1);
        int nargs = e->as.nw.args.len + 1;
        if (L->next_reg < abase + nargs) L->next_reg = abase + nargs;
        if (L->next_reg > L->max_reg) L->max_reg = L->next_reg;
        int tmp = reg_alloc(L);
        emit(L, OP_CALL, tmp, irt->init_fn, (abase << 8) | (nargs & 0xff));
        L->next_reg = abase;
    } else if (e->as.nw.args.len) {
        /* positional field initialisation */
        for (int i = 0; i < e->as.nw.args.len && i < td->fields.len; i++) {
            int r = reg_alloc(L);
            lower_expr_to(L, e->as.nw.args.items[i], r);
            emit(L, OP_SETFIELD, dest, i, r);
            L->next_reg = r;
        }
    }
}

static void lower_call(Lower *L, Expr *e, int dest) {
    Expr *callee = e->as.call.callee;
    /* Ok(...) / Err(...) */
    if (callee->kind == EX_IDENT && !callee->as.ident.sym) {
        const char *n = callee->as.ident.name;
        if (strcmp(n, "Ok") == 0 || strcmp(n, "Err") == 0) {
            int r = L->next_reg;
            if (e->as.call.args.len) { int rr = reg_alloc(L); lower_expr_to(L, e->as.call.args.items[0], rr); r = rr; }
            else { int rr = reg_alloc(L); emit(L, OP_NIL, rr, 0, 0); r = rr; }
            emit(L, strcmp(n, "Ok") == 0 ? OP_OK : OP_ERR, dest, r, 0);
            return;
        }
    }
    if (callee->kind == EX_IDENT && callee->as.ident.sym) {
        Symbol *sym = callee->as.ident.sym;
        if (sym->kind == SYM_FN && !sym->fn) {
            /* native builtin */
            int base = L->next_reg;
            lower_args(L, &e->as.call.args, base);
            emit(L, OP_NATIVE, dest, sym->slot, (base << 8) | (e->as.call.args.len & 0xff));
            L->next_reg = base;
            return;
        }
        if (sym->kind == SYM_FN && sym->fn && sym->fn->ir_index >= 0) {
            int base = L->next_reg;
            lower_args(L, &e->as.call.args, base);
            emit(L, OP_CALL, dest, sym->fn->ir_index, (base << 8) | (e->as.call.args.len & 0xff));
            L->next_reg = base;
            return;
        }
    }
    /* call a value (closure / lambda / function stored in a variable) */
    int cmark = reg_mark(L);
    int cr = reg_alloc(L);
    lower_expr_to(L, callee, cr);
    int base = L->next_reg;
    lower_args(L, &e->as.call.args, base);
    emit(L, OP_CALLV, dest, cr, (base << 8) | (e->as.call.args.len & 0xff));
    reg_release(L, cmark);
}

/* `a?.b()` calls the method only when `a` is there, and is nil when it
   is not. Without this the method runs on nil, and a native that hands
   back a default — "" from a text method, 0 from a number one — makes
   the whole chain look like it succeeded. The `??` that follows then
   never fires, which is the opposite of what the reader expects. */
static void lower_method_with(Lower *L, Expr *e, int dest, int recv_reg);

static void lower_method(Lower *L, Expr *e, int dest) {
    if (!e->as.method.optional || !e->as.method.recv) {
        lower_method_with(L, e, dest, -1);
        return;
    }
    int mark = reg_mark(L);
    int r = reg_alloc(L);
    lower_expr_to(L, e->as.method.recv, r);
    int cond = reg_alloc(L);
    emit(L, OP_ISNIL, cond, r, 0);
    int br = emit(L, OP_BRTRUE, 0, cond, 0);
    lower_method_with(L, e, dest, r);
    int done = emit(L, OP_JUMP, 0, 0, 0);
    patch(L, br, here(L));
    emit(L, OP_NIL, dest, 0, 0);
    patch(L, done, here(L));
    reg_release(L, mark);
}

static void lower_method_with(Lower *L, Expr *e, int dest, int recv_reg) {
    int nid = e->as.method.builtin;
    /* enum variant construction  Color.Custom(1,2,3) */
    if (nid == -2) {
        Type *t = e->type;
        int tag = 0;
        if (t && t->decl) {
            vec_foreach(i, &t->decl->variants)
                if (strcmp(t->decl->variants.items[i].name, e->as.method.name) == 0)
                    tag = t->decl->variants.items[i].tag;
        }
        int base = L->next_reg;
        lower_args(L, &e->as.method.args, base);
        int nameidx = const_text(L, e->as.method.name);
        emit(L, OP_VARIANT, dest, (tag << 16) | (nameidx & 0xffff), (base << 8) | (e->as.method.args.len & 0xff));
        L->next_reg = base;
        return;
    }
    if (e->as.method.resolved) {
        FnDecl *m = e->as.method.resolved;
        int base = L->next_reg;
        int nargs = 0;
        if (e->as.method.recv) {
            int sr = reg_alloc(L);
            if (recv_reg >= 0) emit(L, OP_MOVE, sr, recv_reg, 0);
            else lower_expr_to(L, e->as.method.recv, sr);
            nargs = 1;
        }
        lower_args(L, &e->as.method.args, base + nargs);
        nargs += e->as.method.args.len;
        if (L->next_reg < base + nargs) L->next_reg = base + nargs;
        if (L->next_reg > L->max_reg) L->max_reg = L->next_reg;
        emit(L, OP_CALL, dest, m->ir_index, (base << 8) | (nargs & 0xff));
        L->next_reg = base;
        return;
    }
    if (nid >= 0) {
        int base = L->next_reg;
        int nargs = 0;
        if (e->as.method.recv) {
            int sr = reg_alloc(L);
            if (recv_reg >= 0) emit(L, OP_MOVE, sr, recv_reg, 0);
            else lower_expr_to(L, e->as.method.recv, sr);
            nargs = 1;
        }
        lower_args(L, &e->as.method.args, base + nargs);
        nargs += e->as.method.args.len;
        if (L->next_reg < base + nargs) L->next_reg = base + nargs;
        if (L->next_reg > L->max_reg) L->max_reg = L->next_reg;
        emit(L, OP_NATIVE, dest, nid, (base << 8) | (nargs & 0xff));
        L->next_reg = base;
        return;
    }
    /* dynamic dispatch by name */
    int base = L->next_reg;
    int sr = reg_alloc(L);
    if (e->as.method.recv) {
        if (recv_reg >= 0) emit(L, OP_MOVE, sr, recv_reg, 0);
        else lower_expr_to(L, e->as.method.recv, sr);
    } else emit(L, OP_NIL, sr, 0, 0);
    lower_args(L, &e->as.method.args, base + 1);
    int nargs = 1 + e->as.method.args.len;
    if (L->next_reg < base + nargs) L->next_reg = base + nargs;
    if (L->next_reg > L->max_reg) L->max_reg = L->next_reg;
    emit(L, OP_METHOD, dest, const_text(L, e->as.method.name), (base << 8) | (nargs & 0xff));
    L->next_reg = base;
}

static void lower_assign(Lower *L, Expr *e, int dest) {
    Expr *target = e->as.assign.target;
    TokKind op = e->as.assign.op;
    int mark = reg_mark(L);

    if (op != T_ASSIGN) {
        /* a += b  ==>  a = a + b */
        int cur = reg_alloc(L);
        lower_expr_to(L, target, cur);
        int rhs = reg_alloc(L);
        lower_expr_to(L, e->as.assign.value, rhs);
        OpCode bop = op == T_PLUSEQ ? OP_ADD : op == T_MINUSEQ ? OP_SUB :
                     op == T_STAREQ ? OP_MUL : op == T_SLASHEQ ? OP_DIV : OP_MOD;
        if (op == T_PLUSEQ && target->type && target->type->kind == TY_TEXT) bop = OP_CONCAT;
        emit(L, bop, cur, cur, rhs);
        /* store */
        if (target->kind == EX_IDENT) store_symbol(L, target->as.ident.sym, cur);
        else if (target->kind == EX_FIELD) {
            int o = reg_alloc(L);
            lower_expr_to(L, target->as.field.obj, o);
            emit(L, OP_SETFIELD, o, target->as.field.field_index, cur);
        } else if (target->kind == EX_INDEX) {
            int o = reg_alloc(L), ix = reg_alloc(L);
            lower_expr_to(L, target->as.index.obj, o);
            lower_expr_to(L, target->as.index.index, ix);
            emit(L, OP_SETINDEX, o, ix, cur);
        }
        if (dest >= 0) emit(L, OP_MOVE, dest, cur, 0);
        reg_release(L, mark);
        return;
    }

    int val = reg_alloc(L);
    lower_expr_to(L, e->as.assign.value, val);
    if (target->kind == EX_IDENT) {
        store_symbol(L, target->as.ident.sym, val);
    } else if (target->kind == EX_FIELD) {
        int o = reg_alloc(L);
        lower_expr_to(L, target->as.field.obj, o);
        emit(L, OP_SETFIELD, o, target->as.field.field_index, val);
    } else if (target->kind == EX_INDEX) {
        int o = reg_alloc(L), ix = reg_alloc(L);
        lower_expr_to(L, target->as.index.obj, o);
        lower_expr_to(L, target->as.index.index, ix);
        emit(L, OP_SETINDEX, o, ix, val);
    }
    if (dest >= 0) emit(L, OP_MOVE, dest, val, 0);
    reg_release(L, mark);
}

static void lower_try(Lower *L, Expr *e, int dest) {
    Type *vt = e->as.wrap.value->type;
    int mark = reg_mark(L);
    int r = reg_alloc(L);
    lower_expr_to(L, e->as.wrap.value, r);
    if (vt && vt->kind == TY_RESULT) {
        int ok = reg_alloc(L);
        emit(L, OP_ISOK, ok, r, 0);
        int br = emit(L, OP_BRTRUE, 0, ok, 0);
        emit(L, OP_RET, r, 0, 0);          /* propagate the whole Result */
        patch(L, br, here(L));
        emit(L, OP_UNWRAP, dest, r, 0);
    } else {
        int isnil = reg_alloc(L);
        emit(L, OP_ISNIL, isnil, r, 0);
        int br = emit(L, OP_BRFALSE, 0, isnil, 0);
        emit(L, OP_RETNIL, 0, 0, 0);
        patch(L, br, here(L));
        emit(L, OP_MOVE, dest, r, 0);
    }
    reg_release(L, mark);
}

static void lower_expr_to(Lower *L, Expr *e, int dest) {
    if (!e) { emit(L, OP_NIL, dest, 0, 0); return; }
    set_line(L, e->span);
    switch (e->kind) {
        case EX_INT:  emit(L, OP_CONST, dest, const_add(L, v_int(e->as.ival)), 0); break;
        case EX_BYTE: emit(L, OP_CONST, dest, const_add(L, v_int(e->as.ival)), 0); break;
        case EX_NUM:  emit(L, OP_CONST, dest, const_add(L, v_num(e->as.nval)), 0); break;
        case EX_BOOL: emit(L, OP_CONST, dest, const_add(L, v_bool(e->as.bval)), 0); break;
        case EX_NIL:  emit(L, OP_NIL, dest, 0, 0); break;
        case EX_TEXT: emit(L, OP_CONST, dest, const_text(L, e->as.str.text), 0); break;
        case EX_INTERP: lower_interp(L, e, dest); break;
        case EX_IDENT: load_symbol(L, e->as.ident.sym, dest); break;
        case EX_SELF: emit(L, OP_MOVE, dest, 0, 0); break;   /* self is always slot 0 */
        case EX_UNARY: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, e->as.unary.operand, r);
            emit(L, e->as.unary.op == T_NOT ? OP_NOT : OP_NEG, dest, r, 0);
            reg_release(L, mark);
            break;
        }
        case EX_BINARY: {
            int mark = reg_mark(L);
            int a = reg_alloc(L), b = reg_alloc(L);
            lower_expr_to(L, e->as.binary.lhs, a);
            lower_expr_to(L, e->as.binary.rhs, b);
            OpCode op = binop_of(e->as.binary.op);
            if (op == OP_ADD && e->type && e->type->kind == TY_TEXT) op = OP_CONCAT;
            emit(L, op, dest, a, b);
            reg_release(L, mark);
            break;
        }
        case EX_LOGICAL: lower_logical(L, e, dest); break;
        case EX_ASSIGN:  lower_assign(L, e, dest); break;
        case EX_CALL:    lower_call(L, e, dest); break;
        case EX_METHOD:  lower_method(L, e, dest); break;
        case EX_FIELD: {
            if (e->as.field.obj && e->as.field.obj->kind == EX_IDENT &&
                e->as.field.obj->as.ident.sym &&
                e->as.field.obj->as.ident.sym->kind == SYM_TYPE) {
                /* enum unit variant */
                int tag = e->as.field.field_index;
                int nameidx = const_text(L, e->as.field.name);
                emit(L, OP_VARIANT, dest, (tag << 16) | (nameidx & 0xffff), (L->next_reg << 8) | 0);
                break;
            }
            int mark = reg_mark(L);
            int o = reg_alloc(L);
            lower_expr_to(L, e->as.field.obj, o);
            if (e->as.field.optional) {
                int cond = reg_alloc(L);
                emit(L, OP_ISNIL, cond, o, 0);
                int br = emit(L, OP_BRTRUE, 0, cond, 0);
                emit(L, OP_GETFIELD, dest, o, e->as.field.field_index);
                int done = emit(L, OP_JUMP, 0, 0, 0);
                patch(L, br, here(L));
                emit(L, OP_NIL, dest, 0, 0);
                patch(L, done, here(L));
            } else {
                emit(L, OP_GETFIELD, dest, o, e->as.field.field_index);
            }
            reg_release(L, mark);
            break;
        }
        case EX_INDEX: {
            int mark = reg_mark(L);
            int o = reg_alloc(L), i = reg_alloc(L);
            lower_expr_to(L, e->as.index.obj, o);
            lower_expr_to(L, e->as.index.index, i);
            emit(L, OP_GETINDEX, dest, o, i);
            reg_release(L, mark);
            break;
        }
        case EX_LIST: {
            int base = L->next_reg;
            vec_foreach(i, &e->as.list.items) {
                int r = reg_alloc(L);
                lower_expr_to(L, e->as.list.items.items[i], r);
            }
            emit(L, OP_NEWLIST, dest, base, e->as.list.items.len);
            L->next_reg = base;
            break;
        }
        case EX_MAP: {
            int base = L->next_reg;
            vec_foreach(i, &e->as.map.keys) {
                int k = reg_alloc(L), v = reg_alloc(L);
                lower_expr_to(L, e->as.map.keys.items[i], k);
                lower_expr_to(L, e->as.map.vals.items[i], v);
            }
            emit(L, OP_NEWMAP, dest, base, e->as.map.keys.len);
            L->next_reg = base;
            break;
        }
        case EX_STRUCT: lower_struct_literal(L, e, dest); break;
        case EX_NEW:    lower_new(L, e, dest); break;
        case EX_LAMBDA: {
            FnDecl *fn = e->as.lambda.fn;
            if (fn->ir_index >= 0) {
                IRFunc *tf = L->p->funcs.items[fn->ir_index];
                int ncap = e->as.lambda.captures.len;
                if (ncap > 0 && tf->ncaps == 0) {
                    tf->ncaps = ncap;
                    tf->cap_src  = arena_alloc(L->p->arena, sizeof(int) * (size_t)ncap);
                    tf->cap_slot = arena_alloc(L->p->arena, sizeof(int) * (size_t)ncap);
                    for (int ci = 0; ci < ncap; ci++) {
                        Symbol *cs = e->as.lambda.captures.items[ci];
                        tf->cap_src[ci]  = cs->variant_tag;   /* register in this frame */
                        tf->cap_slot[ci] = cs->slot;          /* local slot over there  */
                    }
                }
            }
            emit(L, OP_CLOSURE, dest, fn->ir_index, 0);
            break;
        }
        case EX_IF:    lower_if_expr(L, e, dest); break;
        case EX_MATCH: lower_match(L, e, dest); break;
        case EX_BLOCK: lower_block_value(L, e->as.block.block, dest); break;
        case EX_RANGE: {
            int mark = reg_mark(L);
            int a = reg_alloc(L), b = reg_alloc(L);
            lower_expr_to(L, e->as.range.lo, a);
            lower_expr_to(L, e->as.range.hi, b);
            emit(L, OP_RANGE, dest, a, (b << 1) | (e->as.range.inclusive ? 1 : 0));
            reg_release(L, mark);
            break;
        }
        case EX_CAST: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, e->as.cast.value, r);
            int kind = e->type ? (int)e->type->kind : 0;
            emit(L, OP_CAST, dest, r, kind);
            reg_release(L, mark);
            break;
        }
        case EX_IS: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, e->as.is.value, r);
            Type *t = e->as.is.type ? e->as.is.type->resolved : NULL;
            int kind = t ? (int)t->kind : 0;
            int tid = t ? type_id_of(L, t) : -1;
            emit(L, OP_ISTYPE, dest, r, (kind << 16) | (tid & 0xffff));
            reg_release(L, mark);
            break;
        }
        case EX_TRY: lower_try(L, e, dest); break;
        case EX_FORCE: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, e->as.wrap.value, r);
            emit(L, OP_UNWRAP, dest, r, 1);     /* c=1 -> fail when empty */
            reg_release(L, mark);
            break;
        }
        case EX_AWAIT: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, e->as.wrap.value, r);
            emit(L, OP_AWAIT, dest, r, 0);
            reg_release(L, mark);
            break;
        }
        case EX_SPAWN: {
            /* spawn f(args)  ->  build a closure + args then OP_SPAWN */
            Expr *inner = e->as.wrap.value;
            int mark = reg_mark(L);
            if (inner->kind == EX_CALL) {
                int cr = reg_alloc(L);
                lower_expr_to(L, inner->as.call.callee, cr);
                int base = L->next_reg;
                lower_args(L, &inner->as.call.args, base);
                emit(L, OP_SPAWN, dest, cr, (base << 8) | (inner->as.call.args.len & 0xff));
            } else if (inner->kind == EX_METHOD && inner->as.method.resolved) {
                int cr = reg_alloc(L);
                emit(L, OP_CLOSURE, cr, inner->as.method.resolved->ir_index, 0);
                int base = L->next_reg;
                int nargs = 0;
                if (inner->as.method.recv) { int sr = reg_alloc(L); lower_expr_to(L, inner->as.method.recv, sr); nargs = 1; }
                lower_args(L, &inner->as.method.args, base + nargs);
                nargs += inner->as.method.args.len;
                emit(L, OP_SPAWN, dest, cr, (base << 8) | (nargs & 0xff));
            } else {
                int cr = reg_alloc(L);
                lower_expr_to(L, inner, cr);
                emit(L, OP_SPAWN, dest, cr, (L->next_reg << 8) | 0);
            }
            reg_release(L, mark);
            break;
        }
        case EX_COALESCE: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, e->as.coalesce.value, r);
            Type *vt = e->as.coalesce.value->type;
            int cond = reg_alloc(L);
            if (vt && vt->kind == TY_RESULT) {
                emit(L, OP_ISOK, cond, r, 0);
                int br = emit(L, OP_BRFALSE, 0, cond, 0);
                emit(L, OP_UNWRAP, dest, r, 0);
                int done = emit(L, OP_JUMP, 0, 0, 0);
                patch(L, br, here(L));
                lower_expr_to(L, e->as.coalesce.fallback, dest);
                patch(L, done, here(L));
            } else {
                emit(L, OP_ISNIL, cond, r, 0);
                int br = emit(L, OP_BRTRUE, 0, cond, 0);
                emit(L, OP_MOVE, dest, r, 0);
                int done = emit(L, OP_JUMP, 0, 0, 0);
                patch(L, br, here(L));
                lower_expr_to(L, e->as.coalesce.fallback, dest);
                patch(L, done, here(L));
            }
            reg_release(L, mark);
            break;
        }
        case EX_COMPTIME: lower_expr_to(L, e->as.wrap.value, dest); break;
        default: emit(L, OP_NIL, dest, 0, 0); break;
    }
}

static int lower_expr(Lower *L, Expr *e) {
    int r = reg_alloc(L);
    lower_expr_to(L, e, r);
    return r;
}

static void lower_if_expr(Lower *L, Expr *e, int dest) {
    int mark = reg_mark(L);
    int c = reg_alloc(L);
    lower_expr_to(L, e->as.iff.cond, c);
    int br = emit(L, OP_BRFALSE, 0, c, 0);
    reg_release(L, mark);
    lower_block_value(L, e->as.iff.then_b, dest);
    if (e->as.iff.else_b) {
        int done = emit(L, OP_JUMP, 0, 0, 0);
        patch(L, br, here(L));
        lower_block_value(L, e->as.iff.else_b, dest);
        patch(L, done, here(L));
    } else {
        if (dest >= 0) {
            int done = emit(L, OP_JUMP, 0, 0, 0);
            patch(L, br, here(L));
            emit(L, OP_NIL, dest, 0, 0);
            patch(L, done, here(L));
        } else patch(L, br, here(L));
    }
}

/* ------------------------------------------------------------- patterns */
/* Emits a test; on failure jumps to `fail_jumps` (collected for patching). */
static void lower_pattern_test(Lower *L, Pattern *p, int subject, IntVec *fails) {
    if (!p) return;
    switch (p->kind) {
        case PAT_WILDCARD: break;
        case PAT_BIND:
            if (p->sym) emit(L, OP_MOVE, p->sym->slot, subject, 0);
            break;
        case PAT_LITERAL: {
            int mark = reg_mark(L);
            int lit = reg_alloc(L), cmp = reg_alloc(L);
            lower_expr_to(L, p->lit, lit);
            emit(L, OP_EQ, cmp, subject, lit);
            int j = emit(L, OP_BRFALSE, 0, cmp, 0);
            vec_push(fails, j);
            reg_release(L, mark);
            break;
        }
        case PAT_RANGE: {
            int mark = reg_mark(L);
            int lo = reg_alloc(L), hi = reg_alloc(L), cmp = reg_alloc(L);
            lower_expr_to(L, p->lo, lo);
            lower_expr_to(L, p->hi, hi);
            emit(L, OP_GE, cmp, subject, lo);
            vec_push(fails, emit(L, OP_BRFALSE, 0, cmp, 0));
            emit(L, p->name ? OP_LE : OP_LT, cmp, subject, hi);
            vec_push(fails, emit(L, OP_BRFALSE, 0, cmp, 0));
            reg_release(L, mark);
            break;
        }
        case PAT_TYPE: {
            int mark = reg_mark(L);
            int cmp = reg_alloc(L);
            Type *t = p->type ? p->type->resolved : NULL;
            emit(L, OP_ISTYPE, cmp, subject, ((t ? (int)t->kind : 0) << 16) | (type_id_of(L, t) & 0xffff));
            vec_push(fails, emit(L, OP_BRFALSE, 0, cmp, 0));
            reg_release(L, mark);
            break;
        }
        case PAT_VARIANT: {
            /* Result Ok/Err */
            Type *st = p->vtype;
            int mark = reg_mark(L);
            if (st && st->kind == TY_RESULT) {
                int ok = reg_alloc(L);
                emit(L, OP_ISOK, ok, subject, 0);
                bool want_ok = strcmp(p->name, "Ok") == 0;
                vec_push(fails, emit(L, want_ok ? OP_BRFALSE : OP_BRTRUE, 0, ok, 0));
                if (p->subs.len) {
                    int inner = reg_alloc(L);
                    emit(L, OP_UNWRAP, inner, subject, 0);
                    lower_pattern_test(L, p->subs.items[0], inner, fails);
                }
                reg_release(L, mark);
                break;
            }
            int tag = p->path.len ? atoi(p->path.items[0]) : 0;
            int got = reg_alloc(L), want = reg_alloc(L), cmp = reg_alloc(L);
            emit(L, OP_GETTAG, got, subject, 0);
            emit(L, OP_CONST, want, const_add(L, v_int(tag)), 0);
            emit(L, OP_EQ, cmp, got, want);
            vec_push(fails, emit(L, OP_BRFALSE, 0, cmp, 0));
            vec_foreach(i, &p->subs) {
                int sub = reg_alloc(L);
                emit(L, OP_PAYLOAD, sub, subject, i);
                lower_pattern_test(L, p->subs.items[i], sub, fails);
            }
            reg_release(L, mark);
            break;
        }
        case PAT_DATA: {
            vec_foreach(i, &p->subs) {
                Pattern *sub = p->subs.items[i];
                int idx = sub->path.len ? atoi(sub->path.items[0]) : i;
                int r = reg_alloc(L);
                emit(L, OP_GETFIELD, r, subject, idx);
                lower_pattern_test(L, sub, r, fails);
            }
            break;
        }
        case PAT_LIST: {
            int mark = reg_mark(L);
            int len = reg_alloc(L), want = reg_alloc(L), cmp = reg_alloc(L);
            emit(L, OP_LEN, len, subject, 0);
            emit(L, OP_CONST, want, const_add(L, v_int(p->subs.len)), 0);
            emit(L, p->rest ? OP_GE : OP_EQ, cmp, len, want);
            vec_push(fails, emit(L, OP_BRFALSE, 0, cmp, 0));
            vec_foreach(i, &p->subs) {
                int ix = reg_alloc(L), item = reg_alloc(L);
                emit(L, OP_CONST, ix, const_add(L, v_int(i)), 0);
                emit(L, OP_GETINDEX, item, subject, ix);
                lower_pattern_test(L, p->subs.items[i], item, fails);
            }
            reg_release(L, mark);
            break;
        }
    }
}

static void lower_match(Lower *L, Expr *e, int dest) {
    int mark = reg_mark(L);
    int subj = reg_alloc(L);
    lower_expr_to(L, e->as.match.subject, subj);
    IntVec ends; vec_init(&ends);
    vec_foreach(i, &e->as.match.arms) {
        MatchArm *arm = &e->as.match.arms.items[i];
        IntVec fails; vec_init(&fails);
        lower_pattern_test(L, arm->pat, subj, &fails);
        if (arm->guard) {
            int g = reg_alloc(L);
            lower_expr_to(L, arm->guard, g);
            vec_push(&fails, emit(L, OP_BRFALSE, 0, g, 0));
        }
        if (arm->body) lower_expr_to(L, arm->body, dest >= 0 ? dest : reg_alloc(L));
        else if (arm->bbody) { lower_block(L, arm->bbody); if (dest >= 0) emit(L, OP_NIL, dest, 0, 0); }
        vec_push(&ends, emit(L, OP_JUMP, 0, 0, 0));
        int next = here(L);
        vec_foreach(k, &fails) patch(L, fails.items[k], next);
        vec_free(&fails);
    }
    if (dest >= 0) emit(L, OP_NIL, dest, 0, 0);
    int done = here(L);
    vec_foreach(i, &ends) patch(L, ends.items[i], done);
    vec_free(&ends);
    reg_release(L, mark);
}

/* ------------------------------------------------------------ statements */
static void run_defers(Lower *L) {
    for (int i = L->defers.len - 1; i >= 0; i--) lower_block(L, L->defers.items[i]);
}

static void lower_stmt(Lower *L, Stmt *st) {
    if (!st) return;
    set_line(L, st->span);
    switch (st->kind) {
        case ST_EXPR: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, st->as.expr, r);
            reg_release(L, mark);
            break;
        }
        case ST_LET: {
            if (st->as.let.sym) {
                Symbol *sym = st->as.let.sym;
                if (sym->kind == SYM_GLOBAL || sym->kind == SYM_CONST) {
                    int mark = reg_mark(L);
                    int r = reg_alloc(L);
                    if (st->as.let.init) lower_expr_to(L, st->as.let.init, r);
                    else emit(L, OP_NIL, r, 0, 0);
                    emit(L, OP_SETGLOBAL, sym->slot, r, 0);
                    reg_release(L, mark);
                } else {
                    if (st->as.let.init) lower_expr_to(L, st->as.let.init, sym->slot);
                    else emit(L, OP_NIL, sym->slot, 0, 0);
                }
            } else if (st->as.let.pat) {
                int mark = reg_mark(L);
                int r = reg_alloc(L);
                if (st->as.let.init) lower_expr_to(L, st->as.let.init, r);
                IntVec fails; vec_init(&fails);
                lower_pattern_test(L, st->as.let.pat, r, &fails);
                int ok = here(L);
                vec_foreach(k, &fails) patch(L, fails.items[k], ok);
                vec_free(&fails);
                reg_release(L, mark);
            }
            break;
        }
        case ST_GIVE: {
            if (st->as.give.value) {
                int mark = reg_mark(L);
                int r = reg_alloc(L);
                lower_expr_to(L, st->as.give.value, r);
                run_defers(L);
                emit(L, OP_RET, r, 0, 0);
                reg_release(L, mark);
            } else {
                run_defers(L);
                emit(L, OP_RETNIL, 0, 0, 0);
            }
            break;
        }
        case ST_WHILE: {
            int top = here(L);
            int mark = reg_mark(L);
            int c = reg_alloc(L);
            lower_expr_to(L, st->as.whil.cond, c);
            int br = emit(L, OP_BRFALSE, 0, c, 0);
            reg_release(L, mark);
            LoopCtx *lc = &L->loops[L->nloops++];
            vec_init(&lc->breaks); vec_init(&lc->continues); lc->label = NULL;
            lower_block(L, st->as.whil.body);
            emit(L, OP_JUMP, top, 0, 0);
            int end = here(L);
            patch(L, br, end);
            vec_foreach(i, &lc->breaks) patch(L, lc->breaks.items[i], end);
            vec_foreach(i, &lc->continues) patch(L, lc->continues.items[i], top);
            vec_free(&lc->breaks); vec_free(&lc->continues);
            L->nloops--;
            break;
        }
        case ST_LOOP: {
            int top = here(L);
            LoopCtx *lc = &L->loops[L->nloops++];
            vec_init(&lc->breaks); vec_init(&lc->continues); lc->label = st->as.loop.label;
            lower_block(L, st->as.loop.body);
            emit(L, OP_JUMP, top, 0, 0);
            int end = here(L);
            vec_foreach(i, &lc->breaks) patch(L, lc->breaks.items[i], end);
            vec_foreach(i, &lc->continues) patch(L, lc->continues.items[i], top);
            vec_free(&lc->breaks); vec_free(&lc->continues);
            L->nloops--;
            break;
        }
        case ST_FOR: {
            int mark = reg_mark(L);
            int src = reg_alloc(L);
            lower_expr_to(L, st->as.forr.iter, src);
            int it = reg_alloc(L);
            emit(L, OP_ITERNEW, it, src, 0);
            int top = here(L);
            int valslot = st->as.forr.sym ? st->as.forr.sym->slot : reg_alloc(L);
            int next = emit(L, OP_ITERNEXT, valslot, it, 0);   /* c patched with exit */
            if (st->as.forr.pat && st->as.forr.pat->sym)
                emit(L, OP_ITERKEY, st->as.forr.pat->sym->slot, it, 0);
            LoopCtx *lc = &L->loops[L->nloops++];
            vec_init(&lc->breaks); vec_init(&lc->continues); lc->label = st->as.forr.label;
            lower_block(L, st->as.forr.body);
            emit(L, OP_JUMP, top, 0, 0);
            int end = here(L);
            L->fn->code.items[next].c = end;
            vec_foreach(i, &lc->breaks) patch(L, lc->breaks.items[i], end);
            vec_foreach(i, &lc->continues) patch(L, lc->continues.items[i], top);
            vec_free(&lc->breaks); vec_free(&lc->continues);
            L->nloops--;
            reg_release(L, mark);
            break;
        }
        case ST_BREAK: {
            if (L->nloops > 0) {
                int j = emit(L, OP_JUMP, 0, 0, 0);
                vec_push(&L->loops[L->nloops - 1].breaks, j);
            }
            break;
        }
        case ST_SKIP: {
            if (L->nloops > 0) {
                int j = emit(L, OP_JUMP, 0, 0, 0);
                vec_push(&L->loops[L->nloops - 1].continues, j);
            }
            break;
        }
        case ST_BLOCK: lower_block(L, st->as.block.block); break;
        case ST_DEFER: vec_push(&L->defers, st->as.block.block); break;
        case ST_UNSAFE: lower_block(L, st->as.block.block); break;
        case ST_FAIL: {
            int mark = reg_mark(L);
            int r = reg_alloc(L);
            lower_expr_to(L, st->as.fail.value, r);
            emit(L, OP_FAIL, r, 0, 0);
            reg_release(L, mark);
            break;
        }
        case ST_TRY: {
            int tp = emit(L, OP_TRYPUSH, 0, 0, 0);
            lower_block(L, st->as.tryc.body);
            emit(L, OP_TRYPOP, 0, 0, 0);
            int done = emit(L, OP_JUMP, 0, 0, 0);
            patch(L, tp, here(L));
            if (st->as.tryc.err_sym) L->fn->code.items[tp].b = st->as.tryc.err_sym->slot + 1;
            if (st->as.tryc.handler) lower_block(L, st->as.tryc.handler);
            patch(L, done, here(L));
            break;
        }
        case ST_DECL: break;
        default: break;
    }
}

static void lower_block(Lower *L, Block *b) {
    if (!b) return;
    vec_foreach(i, &b->stmts) lower_stmt(L, b->stmts.items[i]);
}

/* A block used as a value: the last expression statement is its result. */
static void lower_block_value(Lower *L, Block *b, int dest) {
    if (!b || !b->stmts.len) { if (dest >= 0) emit(L, OP_NIL, dest, 0, 0); return; }
    int last = b->stmts.len - 1;
    for (int i = 0; i < last; i++) lower_stmt(L, b->stmts.items[i]);
    Stmt *st = b->stmts.items[last];
    if (st->kind == ST_EXPR && dest >= 0) lower_expr_to(L, st->as.expr, dest);
    else {
        lower_stmt(L, st);
        if (dest >= 0) emit(L, OP_NIL, dest, 0, 0);
    }
}

/* ------------------------------------------------------------- functions */
static IRFunc *new_func(Lower *L, const char *name, FnDecl *decl) {
    IRFunc *f = NEW(L->a, IRFunc);
    f->name = name;
    f->qualname = name;
    f->decl = decl;
    f->file_id = -1;
    vec_init(&f->code);
    vec_init(&f->consts);
    vec_push(&L->p->funcs, f);
    return f;
}

static void lower_function(Lower *L, FnDecl *decl) {
    if (decl->ir_index < 0) return;
    IRFunc *f = L->p->funcs.items[decl->ir_index];
    IRFunc *saved_fn = L->fn;
    FnDecl *saved_decl = L->decl;
    int saved_next = L->next_reg, saved_max = L->max_reg;
    BlockVec saved_defers = L->defers;
    vec_init(&L->defers);

    L->fn = f;
    L->decl = decl;
    L->next_reg = decl->local_count;
    L->max_reg = decl->local_count;
    f->nparams = 0;
    vec_foreach(i, &decl->params) { (void)i; f->nparams++; }
    f->is_task = decl->is_task;
    f->is_test = decl->is_test;
    f->is_bench = decl->is_bench;
    f->is_method = decl->is_method;
    f->doc = decl->doc;
    vec_foreach(li, &decl->local_names) vec_push(&f->local_names, decl->local_names.items[li]);
    if (decl->span.file >= 0) {
        int line = 1, col = 1;
        sourcemap_pos(L->p->sm, decl->span, &line, &col);
        f->line = line;
        f->file_id = decl->span.file;
        L->cur_line = line;
    }

    /* prologue: lift every captured value into its local slot */
    for (int ci = 0; ci < f->ncaps; ci++)
        emit(L, OP_GETCAP, f->cap_slot ? f->cap_slot[ci] : ci, ci, 0);

    if (decl->expr_body) {
        int r = reg_alloc(L);
        lower_expr_to(L, decl->expr_body, r);
        emit(L, OP_RET, r, 0, 0);
    } else {
        lower_block(L, decl->body);
        run_defers(L);
        emit(L, OP_RETNIL, 0, 0, 0);
    }
    f->nregs = L->max_reg + 2;

    L->fn = saved_fn;
    L->decl = saved_decl;
    L->next_reg = saved_next;
    L->max_reg = saved_max;
    vec_free(&L->defers);
    L->defers = saved_defers;
}

/* ------------------------------------------------------------------ app */
static void collect_app_handlers(Lower *L, UiNode *n, FnVec *out) {
    vec_foreach(i, &n->handler_fns) if (n->handler_fns.items[i]) vec_push(out, n->handler_fns.items[i]);
    vec_foreach(i, &n->children) collect_app_handlers(L, n->children.items[i], out);
}

static void lower_ui_node(Lower *L, UiNode *n, int parent_reg, int refresh_fn);

static void lower_ui_children(Lower *L, UiNode *n, int self_reg, int refresh_fn) {
    vec_foreach(i, &n->children) lower_ui_node(L, n->children.items[i], self_reg, refresh_fn);
}

static void lower_ui_node(Lower *L, UiNode *n, int parent_reg, int refresh_fn) {
    if (strcmp(n->kind, "state") == 0) return;   /* handled as globals */
    int mark = reg_mark(L);
    int idr = reg_alloc(L);
    if (strcmp(n->kind, "window") == 0) {
        int a = reg_alloc(L), b = reg_alloc(L), c = reg_alloc(L);
        const char *title = n->label ? n->label : "SPRFST";
        Expr *te = NULL, *we = NULL, *he = NULL;
        vec_foreach(i, &n->prop_names) {
            if (strcmp(n->prop_names.items[i], "title") == 0) te = n->prop_values.items[i];
            if (strcmp(n->prop_names.items[i], "width") == 0) we = n->prop_values.items[i];
            if (strcmp(n->prop_names.items[i], "height") == 0) he = n->prop_values.items[i];
        }
        if (te) lower_expr_to(L, te, a); else emit(L, OP_CONST, a, const_text(L, title), 0);
        if (we) lower_expr_to(L, we, b); else emit(L, OP_CONST, b, const_add(L, v_int(760)), 0);
        if (he) lower_expr_to(L, he, c); else emit(L, OP_CONST, c, const_add(L, v_int(520)), 0);
        emit(L, OP_NATIVE, idr, NF_UI_WINDOW, (a << 8) | 3);
    } else {
        int a = reg_alloc(L), b = reg_alloc(L), c = reg_alloc(L);
        emit(L, OP_MOVE, a, parent_reg, 0);
        emit(L, OP_CONST, b, const_text(L, n->kind), 0);
        emit(L, OP_CONST, c, const_text(L, n->label ? n->label : ""), 0);
        emit(L, OP_NATIVE, idr, NF_UI_NODE, (a << 8) | 3);
    }
    /* static properties */
    vec_foreach(i, &n->prop_names) {
        const char *pn = n->prop_names.items[i];
        if (strcmp(n->kind, "window") == 0 &&
            (strcmp(pn, "title") == 0 || strcmp(pn, "width") == 0 || strcmp(pn, "height") == 0)) continue;
        int m2 = reg_mark(L);
        int a = reg_alloc(L), b = reg_alloc(L), c = reg_alloc(L);
        emit(L, OP_MOVE, a, idr, 0);
        emit(L, OP_CONST, b, const_text(L, pn), 0);
        lower_expr_to(L, n->prop_values.items[i], c);
        int t = reg_alloc(L);
        emit(L, OP_NATIVE, t, NF_UI_SET, (a << 8) | 3);
        reg_release(L, m2);
    }
    /* handlers */
    vec_foreach(i, &n->handler_names) {
        FnDecl *h = i < n->handler_fns.len ? n->handler_fns.items[i] : NULL;
        if (!h || h->ir_index < 0) continue;
        int m2 = reg_mark(L);
        int a = reg_alloc(L), b = reg_alloc(L), c = reg_alloc(L);
        emit(L, OP_MOVE, a, idr, 0);
        emit(L, OP_CONST, b, const_text(L, n->handler_names.items[i]), 0);
        emit(L, OP_CLOSURE, c, h->ir_index, 0);
        int t = reg_alloc(L);
        emit(L, OP_NATIVE, t, NF_UI_ON, (a << 8) | 3);
        reg_release(L, m2);
    }
    lower_ui_children(L, n, idr, refresh_fn);
    reg_release(L, mark);
}

/* dynamic properties are re-applied after every event */
static void lower_ui_refresh(Lower *L, UiNode *n, int *counter) {
    if (strcmp(n->kind, "state") == 0) return;
    int myid = (*counter)++;
    vec_foreach(i, &n->prop_names) {
        Expr *v = n->prop_values.items[i];
        if (!v || v->kind == EX_TEXT || v->kind == EX_INT || v->kind == EX_NUM || v->kind == EX_BOOL) continue;
        int mark = reg_mark(L);
        int a = reg_alloc(L), b = reg_alloc(L), c = reg_alloc(L);
        emit(L, OP_CONST, a, const_add(L, v_int(myid)), 0);
        emit(L, OP_CONST, b, const_text(L, n->prop_names.items[i]), 0);
        lower_expr_to(L, v, c);
        int t = reg_alloc(L);
        emit(L, OP_NATIVE, t, NF_UI_SET, (a << 8) | 3);
        reg_release(L, mark);
    }
    if (n->label && strchr(n->label, '{')) { /* interpolated labels refresh too */ }
    vec_foreach(i, &n->children) lower_ui_refresh(L, n->children.items[i], counter);
}

/* --------------------------------------------------------------- driver */
IRProgram *ir_lower(Arena *a, Sema *sema, SourceMap *sm, DiagBag *db) {
    IRProgram *p = NEW(a, IRProgram);
    p->arena = a;
    p->sm = sm;
    p->entry = -1;
    vec_init(&p->funcs); vec_init(&p->types); vec_init(&p->globals); vec_init(&p->global_names);

    Lower L = { 0 };
    L.a = a; L.sema = sema; L.p = p; L.db = db;
    vec_init(&L.defers);

    /* 1. types */
    vec_foreach(mi, &sema->modules) {
        Module *m = sema->modules.items[mi];
        vec_foreach(di, &m->decls) {
            Decl *d = m->decls.items[di];
            if (d->kind != D_TYPE || d->as.type->kind == TD_ALIAS) continue;
            TypeDecl *td = d->as.type;
            IRType *t = NEW(a, IRType);
            t->name = td->name;
            t->decl = td;
            t->init_fn = -1;
            t->drop_fn = -1;
            t->base = -1;
            {
                FieldDecl *flds[128];
                int nf = collect_fields(td, flds, 128);
                t->nfields = nf;
                t->field_names = NEWN(a, const char *, nf + 1);
                for (int fi = 0; fi < nf; fi++) t->field_names[fi] = flds[fi]->name;
            }
            vec_push(&p->types, t);
        }
    }

    /* 2. function table (indices first, bodies after) */
    vec_foreach(i, &sema->all_fns) {
        FnDecl *fn = sema->all_fns.items[i];
        if (fn->ir_index >= 0) continue;
        const char *qual = fn->owner ? arena_vsprintf(a, "%s.%s", fn->owner->name, fn->name) : fn->name;
        IRFunc *f = new_func(&L, fn->name, fn);
        f->qualname = qual;
        fn->ir_index = p->funcs.len - 1;
    }
    /* link init/drop/methods onto the types */
    vec_foreach(i, &p->types) {
        IRType *t = p->types.items[i];
        TypeDecl *td = t->decl;
        t->nmethods = td->methods.len;
        t->method_ids = NEWN(a, int, td->methods.len + 1);
        t->method_names = NEWN(a, const char *, td->methods.len + 1);
        vec_foreach(k, &td->methods) {
            FnDecl *m = td->methods.items[k];
            t->method_ids[k] = m->ir_index;
            t->method_names[k] = m->name;
            if (m->is_init) t->init_fn = m->ir_index;
            if (m->is_drop) t->drop_fn = m->ir_index;
        }
        if (td->type && td->type->ret && td->type->ret->decl) {
            vec_foreach(k, &p->types)
                if (p->types.items[k]->decl == td->type->ret->decl) t->base = k;
        }
    }

    /* 3. globals */
    vec_foreach(i, &sema->globals) {
        Symbol *g = sema->globals.items[i];
        vec_push(&p->globals, v_nil());
        vec_push(&p->global_names, g->name);
    }

    /* 4. the <start> function: global initialisers, then main */
    IRFunc *start = new_func(&L, "<start>", NULL);
    if (sema->modules.len) start->file_id = sema->modules.items[0]->file_id;
    int start_idx = p->funcs.len - 1;
    L.fn = start;
    L.next_reg = 0; L.max_reg = 2;
    vec_foreach(ci, &sema->all_consts) {
        Decl *d = sema->all_consts.items[ci];
        Symbol *sym = d->as.konst.sym;
        if (!sym) continue;
        int mark = reg_mark(&L);
        int r = reg_alloc(&L);
        lower_expr_to(&L, d->as.konst.value, r);
        emit(&L, OP_SETGLOBAL, sym->slot, r, 0);
        reg_release(&L, mark);
    }
    start->nregs = L.max_reg + 2;

    /* 5. function bodies */
    vec_foreach(i, &sema->all_fns) lower_function(&L, sema->all_fns.items[i]);


    /* 7. app declarations */
    vec_foreach(mi, &sema->modules) {
        Module *m = sema->modules.items[mi];
        vec_foreach(di, &m->decls) {
            Decl *d = m->decls.items[di];
            if (d->kind != D_APP) continue;
            /* refresh function */
            IRFunc *refresh = new_func(&L, "<ui-refresh>", NULL);
            int refresh_idx = p->funcs.len - 1;
            L.fn = refresh; L.next_reg = 0; L.max_reg = 4;
            int counter = 0;
            /* the window is node 0 at runtime: mirror the build order exactly */
            vec_foreach(ci, &d->as.app.root->children)
                lower_ui_refresh(&L, d->as.app.root->children.items[ci], &counter);
            emit(&L, OP_RETNIL, 0, 0, 0);
            refresh->nregs = L.max_reg + 2;

            IRFunc *build = new_func(&L, "<ui-main>", NULL);
            int build_idx = p->funcs.len - 1;
            L.fn = build; L.next_reg = 0; L.max_reg = 4;
            int rootreg = reg_alloc(&L);
            emit(&L, OP_CONST, rootreg, const_add(&L, v_int(0)), 0);
            vec_foreach(ci, &d->as.app.root->children)
                lower_ui_node(&L, d->as.app.root->children.items[ci], rootreg, refresh_idx);
            /* register the refresh hook: ui.on(0, "refresh", closure) */
            {
                int a2 = reg_alloc(&L), b2 = reg_alloc(&L), c2 = reg_alloc(&L);
                emit(&L, OP_CONST, a2, const_add(&L, v_int(0)), 0);
                emit(&L, OP_CONST, b2, const_text(&L, "refresh"), 0);
                emit(&L, OP_CLOSURE, c2, refresh_idx, 0);
                int t = reg_alloc(&L);
                emit(&L, OP_NATIVE, t, NF_UI_ON, (a2 << 8) | 3);
                emit(&L, OP_NATIVE, t, NF_UI_RUN, (L.next_reg << 8) | 0);
            }
            emit(&L, OP_RETNIL, 0, 0, 0);
            build->nregs = L.max_reg + 2;
            if (p->entry < 0) p->entry = build_idx;
        }
    }

    /* 8. entry point */
    vec_foreach(i, &sema->all_fns) {
        FnDecl *fn = sema->all_fns.items[i];
        if (!fn->owner && fn->name && strcmp(fn->name, "main") == 0 && fn->ir_index >= 0)
            p->entry = fn->ir_index;
    }
    p->funcs.items[start_idx]->nregs = start->nregs;
    /* <start> runs first and then calls the entry */
    L.fn = start;
    L.next_reg = 0;
    if (p->entry >= 0) {
        int r = 0;
        emit(&L, OP_CALL, r, p->entry, (1 << 8) | 0);
    }
    emit(&L, OP_RETNIL, 0, 0, 0);
    p->entry = start_idx;
    return p;
}

/* ----------------------------------------------------------- IR printing */
void ir_dump(IRProgram *p, StrBuf *out) {
    sb_printf(out, "; SPRFST SPIR — %d functions, %d types, %d globals\n\n",
              p->funcs.len, p->types.len, p->globals.len);
    vec_foreach(i, &p->types) {
        IRType *t = p->types.items[i];
        sb_printf(out, "type %%%d %s  fields=%d methods=%d init=%d drop=%d\n",
                  i, t->name, t->nfields, t->nmethods, t->init_fn, t->drop_fn);
    }
    if (p->types.len) sb_puts(out, "\n");
    vec_foreach(fi, &p->funcs) {
        IRFunc *f = p->funcs.items[fi];
        sb_printf(out, "fn @%d %s  regs=%d params=%d consts=%d\n",
                  fi, f->qualname ? f->qualname : f->name, f->nregs, f->nparams, f->consts.len);
        vec_foreach(i, &f->code) {
            Instr *in = &f->code.items[i];
            sb_printf(out, "  %4d  %-12s a=%-5d b=%-5d c=%-5d", i, OP_NAMES[in->op], in->a, in->b, in->c);
            if (in->op == OP_CONST && in->b >= 0 && in->b < f->consts.len) {
                Value v = f->consts.items[in->b];
                if (v.tag == V_INT) sb_printf(out, "  ; %lld", (long long)v.as.i);
                else if (v.tag == V_NUM) sb_printf(out, "  ; %g", v.as.n);
                else if (v.tag == V_BOOL) sb_printf(out, "  ; %s", v.as.b ? "true" : "false");
                else if (v.tag == V_OBJ && v.as.o->kind == O_TEXT) sb_printf(out, "  ; \"%s\"", ((ObjText *)v.as.o)->chars);
            }
            if (in->op == OP_NATIVE) sb_printf(out, "  ; %s.%s", SPRFST_NATIVES[in->b].module, SPRFST_NATIVES[in->b].name);
            sb_printf(out, "   @line %d\n", in->line);
        }
        sb_puts(out, "\n");
    }
}
