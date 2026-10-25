#include "sprfst/sema.h"
#include "sprfst/natives.h"
#include <ctype.h>

/* ====================================================================== */
/*  scopes                                                                 */
/* ====================================================================== */

static Scope *scope_push(Sema *s, FnDecl *fn) {
    Scope *sc = NEW(s->arena, Scope);
    sc->parent = s->scope;
    sc->fn = fn ? fn : (s->scope ? s->scope->fn : NULL);
    sc->is_unsafe = s->scope ? s->scope->is_unsafe : false;
    vec_init(&sc->syms);
    s->scope = sc;
    return sc;
}

static void warn_unused(Sema *s, Scope *sc) {
    vec_foreach(i, &sc->syms) {
        Symbol *sym = sc->syms.items[i];
        if (sym->used || sym->name[0] == '_') continue;
        if (sym->kind != SYM_VAR && sym->kind != SYM_PARAM) continue;
        if (sym->is_self) continue;
        Diag *d = diag_new(s->db, DIAG_WARNING, "W0101", "`%s` is never used", sym->name);
        diag_label(s->db, d, sym->span, true, "declared here, never read");
        diag_fix(s->db, d, "remove it, or rename it to `_%s` to say it is intentional", sym->name);
    }
}

static void scope_pop(Sema *s) {
    if (!s->scope) return;
    warn_unused(s, s->scope);
    s->scope = s->scope->parent;
}

static Symbol *scope_find(Scope *sc, const char *name) {
    vec_foreach(i, &sc->syms)
        if (sc->syms.items[i]->name == name || strcmp(sc->syms.items[i]->name, name) == 0)
            return sc->syms.items[i];
    return NULL;
}

static Symbol *sym_new(Sema *s, SymKind k, const char *name, Type *t, Span sp) {
    Symbol *sym = NEW(s->arena, Symbol);
    sym->kind = k; sym->name = name; sym->type = t; sym->span = sp; sym->slot = -1;
    return sym;
}

static Symbol *declare(Sema *s, SymKind k, const char *name, Type *t, Span sp) {
    Symbol *prev = scope_find(s->scope, name);
    if (prev && name[0] != '_' && strcmp(name, "_") != 0) {
        Diag *d = diag_new(s->db, DIAG_ERROR, "E0110", "`%s` is already defined in this scope", name);
        diag_label(s->db, d, sp, true, "defined again here");
        diag_label(s->db, d, prev->span, false, "first defined here");
        diag_fix(s->db, d, "use a different name, or remove one of the definitions");
    }
    Symbol *sym = sym_new(s, k, name, t, sp);
    sym->owner_fn = s->cur_fn;
    if ((k == SYM_VAR || k == SYM_PARAM) && s->cur_fn) {
        sym->slot = s->cur_fn->local_count++;
        while (s->cur_fn->local_names.len <= sym->slot) vec_push(&s->cur_fn->local_names, "");
        s->cur_fn->local_names.items[sym->slot] = name;
    }
    vec_push(&s->scope->syms, sym);
    return sym;
}

/* ------------------------------------------------------------ ownership
   `own T` says a value has exactly one owner. Handing it to something
   else that wants ownership moves it, and the old name stops working.
   The check is deliberately simple: a name is moved from the point the
   move is written onwards. */
static void move_from(Sema *s, Expr *src) {
    if (!src || src->kind != EX_IDENT) return;
    Symbol *sym = sema_lookup(s, src->as.ident.name);
    if (!sym || !sym->is_owned || sym->moved) return;
    sym->moved = true;
    sym->move_span = src->span;
    (void)s;
}

/* Moves caused by passing an owned value to a parameter that wants to own it. */
static void move_call_args(Sema *s, FnDecl *fn, ExprVec *args) {
    if (!fn) return;
    int shown = 0;
    vec_foreach(i, &fn->params) {
        Param *p = &fn->params.items[i];
        if (p->is_self) continue;
        if (shown < args->len && p->type && p->type->kind == TE_OWN)
            move_from(s, args->items[shown]);
        shown++;
    }
}

/* find a symbol, inserting capture symbols when crossing a function boundary */
static Symbol *lookup_through(Sema *s, Scope *from, const char *name) {
    FnDecl *fn = from ? from->fn : NULL;
    for (Scope *sc = from; sc; sc = sc->parent) {
        Symbol *sym = scope_find(sc, name);
        if (sym) {
            if ((sym->kind == SYM_VAR || sym->kind == SYM_PARAM || sym->kind == SYM_CAPTURE) &&
                sym->owner_fn && fn && sym->owner_fn != fn) {
                /* capture into `fn` */
                Symbol *cap = sym_new(s, SYM_CAPTURE, name, sym->type, sym->span);
                cap->owner_fn = fn;
                cap->is_mut = false;
                cap->used = true;
                cap->slot = fn->local_count++;
                while (fn->local_names.len <= cap->slot) vec_push(&fn->local_names, "");
                fn->local_names.items[cap->slot] = name;
                /* record where the value comes from on the enclosing frame */
                Symbol *src = sym;
                src->used = true;
                /* the capture belongs to the function, not to whichever block
                   happens to mention it, or it disappears when that block ends */
                Scope *target = from;
                while (target->parent && target->parent->fn == fn) target = target->parent;
                vec_push(&target->syms, cap);
                cap->value = NULL;
                cap->fn = NULL;
                cap->td = NULL;
                cap->mod = NULL;
                cap->variant_tag = src->slot;     /* source slot in the outer frame */
                cap->native_mod = NULL;
                return cap;
            }
            return sym;
        }
        if (sc->is_module) break;
    }
    return NULL;
}

Symbol *sema_lookup(Sema *s, const char *name) {
    Symbol *sym = lookup_through(s, s->scope, name);
    if (sym) return sym;
    /* module scope then global scope */
    if (s->module && s->module->scope) {
        sym = scope_find(s->module->scope, name);
        if (sym) return sym;
    }
    if (s->global_scope) return scope_find(s->global_scope, name);
    return NULL;
}

/* ====================================================================== */
/*  signature parser for the native table                                  */
/* ====================================================================== */

typedef struct { const char *p; Sema *s; } SigP;

static void sig_ws(SigP *q) { while (*q->p == ' ') q->p++; }
static Type *sig_type(SigP *q);

static Type *sig_atom(SigP *q) {
    Sema *s = q->s;
    TypeTable *tt = s->tt;
    sig_ws(q);
    if (*q->p == '[') {
        q->p++;
        Type *a = sig_type(q);
        sig_ws(q);
        if (*q->p == ':') {
            q->p++;
            Type *v = sig_type(q);
            sig_ws(q);
            if (*q->p == ']') q->p++;
            return type_map(tt, a, v);
        }
        if (*q->p == ']') q->p++;
        return type_list(tt, a);
    }
    if (*q->p == '{') {
        q->p++;
        Type *a = sig_type(q);
        sig_ws(q);
        if (*q->p == '}') q->p++;
        return type_set(tt, a);
    }
    if (strncmp(q->p, "fn(", 3) == 0) {
        q->p += 3;
        TypeVec ps; vec_init(&ps);
        sig_ws(q);
        while (*q->p && *q->p != ')') {
            vec_push(&ps, sig_type(q));
            sig_ws(q);
            if (*q->p == ',') { q->p++; sig_ws(q); }
        }
        if (*q->p == ')') q->p++;
        sig_ws(q);
        Type *ret = tt->t_nil;
        if (q->p[0] == '-' && q->p[1] == '>') { q->p += 2; ret = sig_type(q); }
        return type_fn(tt, ps, ret);
    }
    char buf[32]; int n = 0;
    while (isalnum((unsigned char)*q->p) || *q->p == '_') { if (n < 31) buf[n++] = *q->p; q->p++; }
    buf[n] = 0;
    Type *base;
    if      (strcmp(buf, "Int") == 0)  base = tt->t_int;
    else if (strcmp(buf, "Num") == 0)  base = tt->t_num;
    else if (strcmp(buf, "Text") == 0) base = tt->t_text;
    else if (strcmp(buf, "Bool") == 0) base = tt->t_bool;
    else if (strcmp(buf, "Byte") == 0) base = tt->t_byte;
    else if (strcmp(buf, "Nil") == 0)  base = tt->t_nil;
    else if (strcmp(buf, "Any") == 0)  base = tt->t_any;
    else if (strcmp(buf, "Self") == 0) base = tt->t_any;
    else if (n == 1 && isupper((unsigned char)buf[0]))
        base = type_generic(tt, intern(s->in, str_cstr(buf)), buf[0]);
    else {
        /* generic container with <...> */
        sig_ws(q);
        TypeVec args; vec_init(&args);
        if (*q->p == '<') {
            q->p++;
            while (*q->p && *q->p != '>') {
                vec_push(&args, sig_type(q));
                sig_ws(q);
                if (*q->p == ',') { q->p++; sig_ws(q); }
            }
            if (*q->p == '>') q->p++;
        }
        if (strcmp(buf, "List") == 0) return type_list(tt, args.len ? args.items[0] : tt->t_any);
        if (strcmp(buf, "Set") == 0)  return type_set(tt, args.len ? args.items[0] : tt->t_any);
        if (strcmp(buf, "Map") == 0)  return type_map(tt, args.len ? args.items[0] : tt->t_any,
                                                      args.len > 1 ? args.items[1] : tt->t_any);
        if (strcmp(buf, "Chan") == 0) return type_chan(tt, args.len ? args.items[0] : tt->t_any);
        if (strcmp(buf, "Future") == 0) return type_future(tt, args.len ? args.items[0] : tt->t_any);
        if (strcmp(buf, "Result") == 0) return type_result(tt, args.len ? args.items[0] : tt->t_any,
                                                           args.len > 1 ? args.items[1] : tt->t_text);
        base = tt->t_any;
    }
    sig_ws(q);
    return base;
}

static Type *sig_type(SigP *q) {
    Type *t = sig_atom(q);
    sig_ws(q);
    while (*q->p == '?') { q->p++; t = type_maybe(q->s->tt, t); sig_ws(q); }
    return t;
}

Type *sema_parse_signature(Sema *s, const char *sig, TypeVec *params_out) {
    SigP q = { sig, s };
    sig_ws(&q);
    if (*q.p == '(') q.p++;
    sig_ws(&q);
    while (*q.p && *q.p != ')') {
        Type *t = sig_type(&q);
        if (params_out) vec_push(params_out, t);
        sig_ws(&q);
        if (*q.p == ',') { q.p++; sig_ws(&q); }
    }
    if (*q.p == ')') q.p++;
    sig_ws(&q);
    if (q.p[0] == '-' && q.p[1] == '>') { q.p += 2; return sig_type(&q); }
    return s->tt->t_nil;
}

/* ====================================================================== */
/*  type expression resolution                                             */
/* ====================================================================== */

static Type *resolve_type(Sema *s, TypeExpr *te);

static Type *builtin_named(Sema *s, const char *name, TypeExprVec *args) {
    TypeTable *tt = s->tt;
    Type *a0 = (args && args->len > 0) ? resolve_type(s, args->items[0]) : NULL;
    Type *a1 = (args && args->len > 1) ? resolve_type(s, args->items[1]) : NULL;
    if (strcmp(name, "Int") == 0)  return tt->t_int;
    if (strcmp(name, "Num") == 0)  return tt->t_num;
    if (strcmp(name, "Text") == 0) return tt->t_text;
    if (strcmp(name, "Bool") == 0) return tt->t_bool;
    if (strcmp(name, "Byte") == 0) return tt->t_byte;
    if (strcmp(name, "Nil") == 0)  return tt->t_nil;
    if (strcmp(name, "Any") == 0)  return tt->t_any;
    if (strcmp(name, "List") == 0) return type_list(tt, a0 ? a0 : tt->t_any);
    if (strcmp(name, "Set") == 0)  return type_set(tt, a0 ? a0 : tt->t_any);
    if (strcmp(name, "Map") == 0)  return type_map(tt, a0 ? a0 : tt->t_any, a1 ? a1 : tt->t_any);
    if (strcmp(name, "Maybe") == 0) return type_maybe(tt, a0 ? a0 : tt->t_any);
    if (strcmp(name, "Result") == 0) return type_result(tt, a0 ? a0 : tt->t_any, a1 ? a1 : tt->t_text);
    if (strcmp(name, "Future") == 0) return type_future(tt, a0 ? a0 : tt->t_any);
    if (strcmp(name, "Chan") == 0) return type_chan(tt, a0 ? a0 : tt->t_any);
    if (strcmp(name, "Range") == 0) return type_range(tt, a0 ? a0 : tt->t_int);
    return NULL;
}

static void suggest_similar(Sema *s, Diag *d, const char *name, const char *kindword) {
    /* cheap edit-distance suggestion over visible symbols */
    const char *best = NULL;
    int bestd = 1000;
    Scope *scopes[3] = { s->scope, s->module ? s->module->scope : NULL, s->global_scope };
    for (int k = 0; k < 3; k++) {
        for (Scope *sc = scopes[k]; sc; sc = sc->parent) {
            vec_foreach(i, &sc->syms) {
                const char *cand = sc->syms.items[i]->name;
                int la = (int)strlen(name), lb = (int)strlen(cand);
                if (la > 24 || lb > 24) continue;
                int dp[26][26];
                for (int x = 0; x <= la; x++) dp[x][0] = x;
                for (int y = 0; y <= lb; y++) dp[0][y] = y;
                for (int x = 1; x <= la; x++)
                    for (int y = 1; y <= lb; y++) {
                        int c = tolower((unsigned char)name[x - 1]) == tolower((unsigned char)cand[y - 1]) ? 0 : 1;
                        int m = dp[x - 1][y] + 1;
                        if (dp[x][y - 1] + 1 < m) m = dp[x][y - 1] + 1;
                        if (dp[x - 1][y - 1] + c < m) m = dp[x - 1][y - 1] + c;
                        dp[x][y] = m;
                    }
                if (dp[la][lb] < bestd) { bestd = dp[la][lb]; best = cand; }
            }
            if (sc->is_module) break;
        }
    }
    if (best && bestd <= 2) diag_fix(s->db, d, "did you mean `%s`?", best);
    else diag_fix(s->db, d, "check the spelling, or declare the %s before using it", kindword);
}

static Type *resolve_type(Sema *s, TypeExpr *te) {
    TypeTable *tt = s->tt;
    if (!te) return tt->t_unknown;
    if (te->resolved) return te->resolved;
    Type *r = NULL;
    switch (te->kind) {
        case TE_INFER: r = tt->t_unknown; break;
        case TE_SELF:  r = s->cur_type && s->cur_type->type ? s->cur_type->type : tt->t_any; break;
        case TE_MAYBE: r = type_maybe(tt, resolve_type(s, te->inner)); break;
        case TE_LIST:  r = type_list(tt, resolve_type(s, te->inner)); break;
        case TE_MAP:   r = type_map(tt, resolve_type(s, te->key), resolve_type(s, te->inner)); break;
        case TE_CHAN:  r = type_chan(tt, te->inner ? resolve_type(s, te->inner) : tt->t_any); break;
        case TE_REF:   r = type_ref(tt, resolve_type(s, te->inner), te->mut); break;
        case TE_OWN:   { r = resolve_type(s, te->inner); break; }
        case TE_WEAK:  { r = resolve_type(s, te->inner); break; }
        case TE_FN: {
            TypeVec ps; vec_init(&ps);
            vec_foreach(i, &te->args) vec_push(&ps, resolve_type(s, te->args.items[i]));
            r = type_fn(tt, ps, te->ret ? resolve_type(s, te->ret) : tt->t_nil);
            break;
        }
        case TE_NAME: {
            const char *name = te->name;
            if (te->path.len > 1) {
                /* module.Type */
                Symbol *msym = sema_lookup(s, te->path.items[0]);
                if (msym && msym->kind == SYM_MODULE && msym->mod && msym->mod->scope) {
                    Symbol *t2 = scope_find(msym->mod->scope, name);
                    if (t2 && t2->kind == SYM_TYPE) { r = t2->type; break; }
                }
            }
            Symbol *sym = sema_lookup(s, name);
            if (sym && sym->kind == SYM_TYPE) {
                r = sym->type;
                if (te->args.len && r && (r->kind == TY_DATA || r->kind == TY_OBJECT || r->kind == TY_ENUM)) {
                    Type *inst = type_new(tt, r->kind);
                    *inst = *r;
                    vec_init(&inst->params);
                    vec_foreach(i, &te->args) vec_push(&inst->params, resolve_type(s, te->args.items[i]));
                    r = inst;
                }
                break;
            }
            if (sym && sym->kind == SYM_GENERIC) { r = sym->type; break; }
            r = builtin_named(s, name, &te->args);
            if (!r) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0120", "Unknown type `%s`", name);
                diag_label(s->db, d, te->span, true, "this type does not exist yet");
                suggest_similar(s, d, name, "type");
                diag_note(s->db, d, "built-in types: Int Num Text Bool Byte Nil Any [T] [K: V] T?");
                r = tt->t_error;
            }
            break;
        }
    }
    te->resolved = r;
    return r;
}

/* ====================================================================== */
/*  expression checking                                                    */
/* ====================================================================== */

static Type *check_expr(Sema *s, Expr *e);
static void check_block(Sema *s, Block *b, bool new_scope);
static void check_stmt(Sema *s, Stmt *st);

static void err_type_mismatch(Sema *s, Span sp, Type *want, Type *got, const char *what) {
    Diag *d = diag_new(s->db, DIAG_ERROR, "E0201", "Type mismatch");
    diag_label(s->db, d, sp, true, "this is %s", type_text(s->arena, got));
    diag_given_wanted(s->db, d, type_text(s->arena, got), type_text(s->arena, want));
    if (want->kind == TY_TEXT && type_numeric(got))
        diag_fix(s->db, d, "convert the value with  to_text(...)");
    else if (type_numeric(want) && got->kind == TY_TEXT)
        diag_fix(s->db, d, "parse the text with  .to_int()  or  .to_num()");
    else if (want->kind == TY_INT && got->kind == TY_NUM)
        diag_fix(s->db, d, "round it with  math.round(...)  or cast with  as Int");
    else if (want->kind == TY_MAYBE)
        diag_fix(s->db, d, "wrap the value, or use `nil` when there is nothing");
    else if (got->kind == TY_MAYBE)
        diag_fix(s->db, d, "unwrap it with  value ?? fallback  or  match on nil");
    if (what) diag_note(s->db, d, "%s", what);
}

static bool expect_type(Sema *s, Span sp, Type *want, Type *got, const char *what) {
    if (!want || !got) return true;
    if (type_assignable(want, got)) return true;
    if (want->kind == TY_ERROR || got->kind == TY_ERROR) return true;
    err_type_mismatch(s, sp, want, got, what);
    return false;
}

static const char *op_text(TokKind k) { return tok_name(k); }

static Type *check_binary(Sema *s, Expr *e) {
    TypeTable *tt = s->tt;
    Type *l = check_expr(s, e->as.binary.lhs);
    Type *r = check_expr(s, e->as.binary.rhs);
    TokKind op = e->as.binary.op;
    if (l->kind == TY_ERROR || r->kind == TY_ERROR) return tt->t_error;

    switch (op) {
        case T_PLUS:
            if (l->kind == TY_TEXT || r->kind == TY_TEXT) {
                if (l->kind != TY_TEXT || r->kind != TY_TEXT) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0202", "Cannot join %s and %s with `+`",
                                       type_text(s->arena, l), type_text(s->arena, r));
                    diag_label(s->db, d, e->span, true, "these two sides have different types");
                    diag_given_wanted(s->db, d,
                        arena_vsprintf(s->arena, "%s + %s", type_text(s->arena, l), type_text(s->arena, r)),
                        "Text + Text");
                    diag_fix(s->db, d, "convert with  to_text(...)");
                    diag_fix(s->db, d, "or build the text with interpolation:  \"{a}{b}\"");
                    return tt->t_text;
                }
                return tt->t_text;
            }
            if (l->kind == TY_LIST && r->kind == TY_LIST) return l;
            /* fall through */
        case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT: case T_POW: {
            if (l->kind == TY_GENERIC || r->kind == TY_GENERIC)
                return l->kind == TY_GENERIC ? l : r;
            /* `Any` stays dynamic: the runtime checks it */
            if (l->kind == TY_ANY || r->kind == TY_ANY) {
                if (type_numeric(l) || type_numeric(r) || (l->kind == TY_ANY && r->kind == TY_ANY))
                    return tt->t_any;
            }
            if (!type_numeric(l) || !type_numeric(r)) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0203", "`%s` needs numbers", op_text(op));
                diag_label(s->db, d, e->span, true, "%s %s %s", type_text(s->arena, l), op_text(op), type_text(s->arena, r));
                diag_given_wanted(s->db, d,
                    arena_vsprintf(s->arena, "%s %s %s", type_text(s->arena, l), op_text(op), type_text(s->arena, r)),
                    "Int or Num on both sides");
                if (l->kind == TY_TEXT || r->kind == TY_TEXT)
                    diag_fix(s->db, d, "parse the text first with  .to_num()");
                return tt->t_error;
            }
            if (op == T_SLASH || op == T_POW) return tt->t_num;
            if (l->kind == TY_NUM || r->kind == TY_NUM) return tt->t_num;
            return tt->t_int;
        }
        case T_EQ: case T_NE: {
            if (!type_assignable(l, r) && !type_assignable(r, l)) {
                Diag *d = diag_new(s->db, DIAG_WARNING, "W0204", "Comparing %s with %s is always %s",
                                   type_text(s->arena, l), type_text(s->arena, r), op == T_EQ ? "false" : "true");
                diag_label(s->db, d, e->span, true, "these types can never be equal");
            }
            return tt->t_bool;
        }
        case T_LT: case T_LE: case T_GT: case T_GE: {
            bool ok = (type_numeric(l) && type_numeric(r)) || (l->kind == TY_TEXT && r->kind == TY_TEXT);
            if (!ok) {
                if (l->kind == TY_GENERIC || r->kind == TY_GENERIC || l->kind == TY_ANY || r->kind == TY_ANY)
                    return tt->t_bool;
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0205", "Cannot order %s and %s",
                                   type_text(s->arena, l), type_text(s->arena, r));
                diag_label(s->db, d, e->span, true, "`%s` works on numbers and text", op_text(op));
            }
            return tt->t_bool;
        }
        case T_AMP: case T_BAR: case T_CARET: case T_SHL: case T_SHR: {
            if (l->kind != TY_INT || r->kind != TY_INT) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0206", "`%s` needs Int values", op_text(op));
                diag_label(s->db, d, e->span, true, "%s %s %s", type_text(s->arena, l), op_text(op), type_text(s->arena, r));
                diag_given_wanted(s->db, d, type_text(s->arena, l->kind == TY_INT ? r : l), "Int");
            }
            return tt->t_int;
        }
        default: return tt->t_error;
    }
}

/* resolve a method on a user defined type, walking the inheritance chain */
static FnDecl *find_method(Type *t, const char *name, TypeDecl **owner) {
    TypeDecl *d = t ? t->decl : NULL;
    int guard = 0;
    while (d && guard++ < 64) {
        vec_foreach(i, &d->methods) {
            FnDecl *m = d->methods.items[i];
            if (strcmp(m->name, name) == 0) { if (owner) *owner = d; return m; }
        }
        if (!d->type || !d->type->ret || !d->type->ret->decl) break;
        d = d->type->ret->decl;   /* base class link */
    }
    return NULL;
}

static FieldDecl *find_field(Type *t, const char *name, TypeDecl **owner, int *index_out) {
    TypeDecl *d = t ? t->decl : NULL;
    int guard = 0;
    int base_offset = 0;
    while (d && guard++ < 64) {
        vec_foreach(i, &d->fields) {
            if (strcmp(d->fields.items[i].name, name) == 0) {
                if (owner) *owner = d;
                if (index_out) *index_out = d->fields.items[i].index + base_offset;
                return &d->fields.items[i];
            }
        }
        if (!d->type || !d->type->ret || !d->type->ret->decl) break;
        base_offset += d->fields.len;
        d = d->type->ret->decl;
    }
    return NULL;
}

static const char *builtin_tag(Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
        case TY_TEXT:   return "@Text";
        case TY_LIST:   return "@List";
        case TY_MAP:    return "@Map";
        case TY_SET:    return "@Set";
        case TY_FUTURE: return "@Future";
        case TY_CHAN:   return "@Chan";
        default: return NULL;
    }
}

static void seed_env_from_recv(GenericEnv *env, Sema *s, Type *recv) {
    if (!recv) return;
    if (recv->kind == TY_LIST || recv->kind == TY_SET || recv->kind == TY_CHAN || recv->kind == TY_FUTURE) {
        env->names[env->n] = intern(s->in, str_cstr("T"));
        env->bound[env->n++] = recv->elem ? recv->elem : s->tt->t_any;
    } else if (recv->kind == TY_MAP) {
        env->names[env->n] = intern(s->in, str_cstr("K"));
        env->bound[env->n++] = recv->key ? recv->key : s->tt->t_any;
        env->names[env->n] = intern(s->in, str_cstr("V"));
        env->bound[env->n++] = recv->elem ? recv->elem : s->tt->t_any;
    }
}

static Type *check_call_args(Sema *s, Span sp, const char *what, TypeVec *params, ExprVec *args,
                             Type *ret, GenericEnv *env, bool variadic_ok) {
    int np = params->len, na = args->len;
    /* trailing parameters written `T?` may be left out */
    if (na < np) {
        int required = np;
        while (required > na && params->items[required - 1] &&
               params->items[required - 1]->kind == TY_MAYBE) required--;
        if (required <= na) np = na;
    }
    if (na != np && !variadic_ok) {
        Diag *d = diag_new(s->db, DIAG_ERROR, "E0210", "%s takes %d argument%s, but %d %s given",
                           what, np, np == 1 ? "" : "s", na, na == 1 ? "was" : "were");
        diag_label(s->db, d, sp, true, "%d given here", na);
        StrBuf b; sb_init(&b);
        sb_puts(&b, "(");
        for (int i = 0; i < np; i++) { if (i) sb_puts(&b, ", "); sb_puts(&b, type_text(s->arena, params->items[i])); }
        sb_puts(&b, ")");
        diag_given_wanted(s->db, d, arena_vsprintf(s->arena, "%d argument%s", na, na == 1 ? "" : "s"),
                          arena_strdup(s->arena, b.data ? b.data : "()"));
        sb_free(&b);
    }
    int n = na < np ? na : np;
    for (int i = 0; i < n; i++) {
        Type *pt = params->items[i];
        /* a lambda argument learns its parameter types from the call */
        Type *saved_exp = s->expected_fn;
        if (pt && args->items[i] && args->items[i]->kind == EX_LAMBDA) {
            Type *hint = env ? type_substitute(s->tt, pt, env) : pt;
            if (hint && hint->kind == TY_FN) s->expected_fn = hint;
        }
        Type *at = check_expr(s, args->items[i]);
        s->expected_fn = saved_exp;
        if (env) {
            type_unify(s->tt, pt, at, env);
            pt = type_substitute(s->tt, pt, env);
        }
        if (!type_assignable(pt, at)) {
            Diag *d = diag_new(s->db, DIAG_ERROR, "E0211", "Wrong type for argument %d of %s", i + 1, what);
            diag_label(s->db, d, args->items[i]->span, true, "this is %s", type_text(s->arena, at));
            diag_given_wanted(s->db, d, type_text(s->arena, at), type_text(s->arena, pt));
            if (pt->kind == TY_TEXT) diag_fix(s->db, d, "convert the value with  to_text(...)");
            if (type_numeric(pt) && at->kind == TY_TEXT) diag_fix(s->db, d, "parse it with  .to_num()");
        }
    }
    for (int i = n; i < na; i++) check_expr(s, args->items[i]);
    if (env && ret) return type_substitute(s->tt, ret, env);
    return ret;
}

static Type *check_native_call(Sema *s, Span sp, int nid, Type *recv, ExprVec *args, Expr *site) {
    const NativeFn *nf = &SPRFST_NATIVES[nid];
    TypeVec params; vec_init(&params);
    Type *ret = sema_parse_signature(s, nf->sig, &params);
    GenericEnv env = { 0 };
    seed_env_from_recv(&env, s, recv);
    char what[128];
    snprintf(what, sizeof what, "%s%s%s", nf->module[0] == '@' ? "" : nf->module,
             nf->module[0] && nf->module[0] != '@' ? "." : "", nf->name);
    Type *r = check_call_args(s, sp, what, &params, args, ret, &env, false);
    vec_free(&params);
    if (site) site->as.method.builtin = nid;
    return r ? r : s->tt->t_nil;
}

static Type *check_method(Sema *s, Expr *e) {
    TypeTable *tt = s->tt;
    Expr *recv = e->as.method.recv;
    const char *name = e->as.method.name;

    /* module.function(...) */
    if (recv->kind == EX_IDENT) {
        Symbol *msym = sema_lookup(s, recv->as.ident.name);
        if (msym && msym->kind == SYM_NATIVE_MOD) {
            msym->used = true;
            recv->as.ident.sym = msym;
            int nid = natives_lookup(msym->native_mod, name);
            if (nid < 0) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0220", "`%s` has no function called `%s`",
                                   msym->native_mod, name);
                diag_label(s->db, d, e->as.method.name_span, true, "not found in this module");
                StrBuf b; sb_init(&b);
                int shown = 0;
                for (int i = 0; i < NF_COUNT && shown < 8; i++)
                    if (strcmp(SPRFST_NATIVES[i].module, msym->native_mod) == 0) {
                        if (shown++) sb_puts(&b, ", ");
                        sb_puts(&b, SPRFST_NATIVES[i].name);
                    }
                if (b.len) diag_note(s->db, d, "available: %s%s", b.data, shown >= 8 ? ", ..." : "");
                sb_free(&b);
                return tt->t_error;
            }
            e->as.method.builtin = nid;
            e->as.method.recv = NULL;      /* module call: no receiver value */
            return check_native_call(s, e->span, nid, NULL, &e->as.method.args, e);
        }
        if (msym && msym->kind == SYM_MODULE && msym->mod && msym->mod->scope) {
            msym->used = true;
            Symbol *f = scope_find(msym->mod->scope, name);
            if (f && f->kind == SYM_FN && f->fn) {
                e->as.method.resolved = f->fn;
                e->as.method.recv = NULL;
                TypeVec params; vec_init(&params);
                vec_foreach(i, &f->fn->params) vec_push(&params, f->fn->params.items[i].type ?
                            resolve_type(s, f->fn->params.items[i].type) : tt->t_any);
                GenericEnv env = { 0 };
                Type *r = check_call_args(s, e->span, f->fn->name, &params, &e->as.method.args,
                                          f->type ? f->type->ret : tt->t_nil, &env, false);
                vec_free(&params);
                return r;
            }
            Diag *d = diag_new(s->db, DIAG_ERROR, "E0221", "`%s` has no function called `%s`",
                               msym->mod->name, name);
            diag_label(s->db, d, e->as.method.name_span, true, "not found in this module");
            return tt->t_error;
        }
        /* Type.static_method(...) and enum variant construction */
        if (msym && msym->kind == SYM_TYPE && msym->td) {
            msym->used = true;
            TypeDecl *td = msym->td;
            if (td->kind == TD_ENUM) {
                vec_foreach(i, &td->variants) {
                    VariantDecl *v = &td->variants.items[i];
                    if (strcmp(v->name, name) == 0) {
                        TypeVec params; vec_init(&params);
                        vec_foreach(k, &v->payload) vec_push(&params, resolve_type(s, v->payload.items[k]));
                        check_call_args(s, e->span, v->name, &params, &e->as.method.args, NULL, NULL, false);
                        vec_free(&params);
                        e->as.method.builtin = -2;          /* variant construction */
                        e->as.method.resolved = NULL;
                        e->as.method.recv = NULL;
                        e->type = msym->type;
                        return msym->type;
                    }
                }
            }
            FnDecl *m = find_method(msym->type, name, NULL);
            if (m && (m->is_static || m->is_init)) {
                e->as.method.resolved = m;
                e->as.method.recv = NULL;
                TypeVec params; vec_init(&params);
                vec_foreach(i, &m->params) if (!m->params.items[i].is_self)
                    vec_push(&params, m->params.items[i].type ? resolve_type(s, m->params.items[i].type) : tt->t_any);
                GenericEnv env = { 0 };
                Type *r = check_call_args(s, e->span, m->name, &params, &e->as.method.args,
                                          m->is_init ? msym->type : (m->ret ? resolve_type(s, m->ret) : tt->t_nil),
                                          &env, false);
                vec_free(&params);
                return r;
            }
        }
    }

    Type *rt = check_expr(s, recv);
    if (rt->kind == TY_ERROR) return tt->t_error;
    bool was_optional = e->as.method.optional;
    if (was_optional && rt->kind == TY_MAYBE) rt = rt->elem;

    /* builtin method */
    const char *tag = builtin_tag(rt);
    if (tag) {
        int nid = natives_lookup(tag, name);
        if (nid >= 0) {
            Type *r = check_native_call(s, e->span, nid, rt, &e->as.method.args, e);
            return was_optional ? type_maybe(tt, r) : r;
        }
    }
    if (rt->kind == TY_ANY) {
        /* a global builtin keeps its precise result type even on Any */
        int gid = natives_lookup("", name);
        if (gid >= 0) {
            const NativeFn *nf = &SPRFST_NATIVES[gid];
            TypeVec all; vec_init(&all);
            Type *ret = sema_parse_signature(s, nf->sig, &all);
            if (all.len >= 1) {
                TypeVec rest; vec_init(&rest);
                for (int i = 1; i < all.len; i++) vec_push(&rest, all.items[i]);
                GenericEnv anyenv = { 0 };
                if (all.items[0] && all.items[0]->kind == TY_GENERIC && rt) {
                    anyenv.names[anyenv.n] = all.items[0]->name;
                    anyenv.bound[anyenv.n++] = rt;
                }
                Type *r = check_call_args(s, e->span, nf->name, &rest, &e->as.method.args, ret,
                                          anyenv.n ? &anyenv : NULL, false);
                vec_free(&rest); vec_free(&all);
                e->as.method.builtin = gid;
                return was_optional ? type_maybe(tt, r ? r : tt->t_nil) : (r ? r : tt->t_nil);
            }
            vec_free(&all);
        }
        vec_foreach(i, &e->as.method.args) check_expr(s, e->as.method.args.items[i]);
        /* dynamic dispatch, resolved at runtime */
        e->as.method.builtin = -3;
        return was_optional ? type_maybe(tt, tt->t_any) : tt->t_any;
    }

    /* user type method */
    TypeDecl *owner = NULL;
    FnDecl *m = find_method(rt, name, &owner);
    if (!m && rt->kind == TY_MAYBE) {
        Diag *d = diag_new(s->db, DIAG_ERROR, "E0222", "`%s` might be nothing", "value");
        diag_label(s->db, d, recv->span, true, "this is %s", type_text(s->arena, rt));
        diag_fix(s->db, d, "use  value?.%s(...)  to skip the call when it is nil", name);
        diag_fix(s->db, d, "or unwrap it first with  value ?? fallback");
        return tt->t_error;
    }
    if (!m) {
        /* universal call syntax: a free function whose first parameter fits
           the receiver may be written as a method, so  x.scale(2)  means
           scale(x, 2).  Real methods always win. */
        Symbol *fsym = sema_lookup(s, name);
        if (fsym && fsym->kind == SYM_FN && fsym->fn && fsym->fn->params.len >= 1) {
            FnDecl *target = fsym->fn;
            fsym->used = true;
            ExprVec call_args; vec_init(&call_args);
            NameVec call_names; vec_init(&call_names);
            vec_push(&call_args, recv);
            vec_push(&call_names, (const char *)NULL);
            vec_foreach(i, &e->as.method.args) {
                vec_push(&call_args, e->as.method.args.items[i]);
                vec_push(&call_names, (const char *)NULL);
            }
            Expr *callee = NEW(s->arena, Expr);
            memset(callee, 0, sizeof *callee);
            callee->kind = EX_IDENT;
            callee->span = e->as.method.name_span;
            callee->as.ident.name = name;
            callee->as.ident.sym = fsym;
            e->kind = EX_CALL;
            memset(&e->as, 0, sizeof e->as);
            e->as.call.callee = callee;
            e->as.call.args = call_args;
            e->as.call.names = call_names;
            Type *r = check_expr(s, e);
            (void)target;
            return was_optional ? type_maybe(tt, r) : r;
        }
        /* and every global builtin, so  x.to_text()  means  to_text(x) */
        int gid = natives_lookup("", name);
        if (gid >= 0) {
            const NativeFn *nf = &SPRFST_NATIVES[gid];
            TypeVec all; vec_init(&all);
            Type *ret = sema_parse_signature(s, nf->sig, &all);
            if (all.len >= 1) {
                TypeVec rest; vec_init(&rest);
                for (int i = 1; i < all.len; i++) vec_push(&rest, all.items[i]);
                GenericEnv genv = { 0 };
                seed_env_from_recv(&genv, s, rt);
                Type *r = check_call_args(s, e->span, nf->name, &rest, &e->as.method.args, ret, &genv, false);
                vec_free(&rest);
                vec_free(&all);
                e->as.method.builtin = gid;
                return was_optional ? type_maybe(tt, r ? r : tt->t_nil) : (r ? r : tt->t_nil);
            }
            vec_free(&all);
        }
    }
    if (!m) {
        Diag *d = diag_new(s->db, DIAG_ERROR, "E0223", "%s has no method called `%s`",
                           type_text(s->arena, rt), name);
        diag_label(s->db, d, e->as.method.name_span, true, "unknown method");
        if (rt->decl) {
            StrBuf b; sb_init(&b);
            int shown = 0;
            vec_foreach(i, &rt->decl->methods) {
                if (shown++) sb_puts(&b, ", ");
                sb_puts(&b, rt->decl->methods.items[i]->name);
                if (shown >= 8) break;
            }
            if (b.len) diag_note(s->db, d, "`%s` has: %s", rt->decl->name, b.data);
            sb_free(&b);
        }
        return tt->t_error;
    }
    if (!m->body && !m->expr_body) {
        /* declared by a trait with no default: decide at runtime */
        e->as.method.resolved = NULL;
        e->as.method.builtin = -3;
        TypeVec aps; vec_init(&aps);
        vec_foreach(i, &m->params) if (!m->params.items[i].is_self)
            vec_push(&aps, m->params.items[i].type ? resolve_type(s, m->params.items[i].type) : tt->t_any);
        Type *ar = check_call_args(s, e->span, name, &aps, &e->as.method.args,
                                   m->ret ? resolve_type(s, m->ret) : tt->t_nil, NULL, false);
        vec_free(&aps);
        return was_optional ? type_maybe(tt, ar ? ar : tt->t_nil) : (ar ? ar : tt->t_nil);
    }
    e->as.method.resolved = m;
    TypeVec params; vec_init(&params);
    vec_foreach(i, &m->params) if (!m->params.items[i].is_self)
        vec_push(&params, m->params.items[i].type ? resolve_type(s, m->params.items[i].type) : tt->t_any);
    GenericEnv env = { 0 };
    if (rt->params.len && rt->decl) {
        vec_foreach(i, &rt->decl->generics) {
            if (i < rt->params.len && env.n < 16) {
                env.names[env.n] = rt->decl->generics.items[i].name;
                env.bound[env.n++] = rt->params.items[i];
            }
        }
    }
    TypeDecl *saved = s->cur_type;
    s->cur_type = owner ? owner : rt->decl;
    Type *ret = m->ret ? resolve_type(s, m->ret) : tt->t_nil;
    s->cur_type = saved;
    Type *r = check_call_args(s, e->span, m->name, &params, &e->as.method.args, ret, &env, false);
    vec_free(&params);
    if (m->is_task) r = type_future(tt, r);
    return was_optional ? type_maybe(tt, r) : r;
}

static Type *check_field(Sema *s, Expr *e) {
    TypeTable *tt = s->tt;
    Expr *obj = e->as.field.obj;
    const char *name = e->as.field.name;

    if (obj->kind == EX_IDENT) {
        Symbol *sym = sema_lookup(s, obj->as.ident.name);
        if (sym && sym->kind == SYM_TYPE && sym->td) {
            sym->used = true;
            obj->as.ident.sym = sym;
            TypeDecl *td = sym->td;
            if (td->kind == TD_ENUM) {
                vec_foreach(i, &td->variants) {
                    if (strcmp(td->variants.items[i].name, name) == 0) {
                        e->as.field.field_index = td->variants.items[i].tag;
                        e->kind = EX_FIELD;
                        return sym->type;
                    }
                }
            }
            vec_foreach(i, &td->fields) {
                if (td->fields.items[i].is_static && strcmp(td->fields.items[i].name, name) == 0) {
                    e->as.field.field_index = -2 - i;
                    return resolve_type(s, td->fields.items[i].type);
                }
            }
            Diag *d = diag_new(s->db, DIAG_ERROR, "E0230", "`%s` has no member called `%s`", td->name, name);
            diag_label(s->db, d, e->as.field.name_span, true, "unknown member");
            return tt->t_error;
        }
        if (sym && sym->kind == SYM_NATIVE_MOD) {
            sym->used = true;
            Diag *d = diag_new(s->db, DIAG_ERROR, "E0231", "`%s.%s` must be called", sym->native_mod, name);
            diag_label(s->db, d, e->span, true, "module members are functions");
            diag_fix(s->db, d, "add parentheses:  %s.%s()", sym->native_mod, name);
            return tt->t_error;
        }
    }

    Type *ot = check_expr(s, obj);
    if (ot->kind == TY_ERROR) return tt->t_error;
    bool opt = e->as.field.optional;
    if (opt && ot->kind == TY_MAYBE) ot = ot->elem;

    if (ot->kind == TY_MAYBE) {
        Diag *d = diag_new(s->db, DIAG_ERROR, "E0232", "This value might be nothing");
        diag_label(s->db, d, obj->span, true, "this is %s", type_text(s->arena, ot));
        diag_fix(s->db, d, "use  value?.%s  to read it only when it exists", name);
        diag_fix(s->db, d, "or give a fallback with  (value ?? other).%s", name);
        return tt->t_error;
    }
    if (ot->kind == TY_ANY) return tt->t_any;

    int index = -1;
    TypeDecl *owner = NULL;
    FieldDecl *f = find_field(ot, name, &owner, &index);
    if (f) {
        e->as.field.field_index = index;
        e->is_lvalue = true;
        Type *ft = resolve_type(s, f->type);
        if (ot->params.len && ot->decl) {
            GenericEnv env = { 0 };
            vec_foreach(i, &ot->decl->generics)
                if (i < ot->params.len && env.n < 16) {
                    env.names[env.n] = ot->decl->generics.items[i].name;
                    env.bound[env.n++] = ot->params.items[i];
                }
            ft = type_substitute(tt, ft, &env);
        }
        return opt ? type_maybe(tt, ft) : ft;
    }
    /* method used as a value */
    FnDecl *m = find_method(ot, name, NULL);
    if (m) {
        TypeVec ps; vec_init(&ps);
        vec_foreach(i, &m->params) if (!m->params.items[i].is_self)
            vec_push(&ps, m->params.items[i].type ? resolve_type(s, m->params.items[i].type) : tt->t_any);
        return type_fn(tt, ps, m->ret ? resolve_type(s, m->ret) : tt->t_nil);
    }
    Diag *d = diag_new(s->db, DIAG_ERROR, "E0233", "%s has no field called `%s`",
                       type_text(s->arena, ot), name);
    diag_label(s->db, d, e->as.field.name_span, true, "unknown field");
    if (ot->decl) {
        StrBuf b; sb_init(&b);
        int shown = 0;
        vec_foreach(i, &ot->decl->fields) {
            if (shown++) sb_puts(&b, ", ");
            sb_puts(&b, ot->decl->fields.items[i].name);
            if (shown >= 10) break;
        }
        if (b.len) diag_note(s->db, d, "`%s` has: %s", ot->decl->name, b.data);
        sb_free(&b);
    }
    return tt->t_error;
}

static void check_pattern(Sema *s, Pattern *p, Type *subject);


/* Rewrite `Circle(2.0)` / `Empty` into the variant construction node that the
   lowering stage understands. */
static Type *rewrite_variant(Sema *s, Expr *e, Symbol *vs, ExprVec args, Span name_span) {
    TypeDecl *td = vs->td;
    VariantDecl *v = NULL;
    if (td) vec_foreach(i, &td->variants)
        if (strcmp(td->variants.items[i].name, vs->name) == 0) v = &td->variants.items[i];
    TypeVec params; vec_init(&params);
    if (v) vec_foreach(k, &v->payload) vec_push(&params, resolve_type(s, v->payload.items[k]));
    check_call_args(s, e->span, vs->name, &params, &args, NULL, NULL, false);
    vec_free(&params);
    e->kind = EX_METHOD;
    memset(&e->as, 0, sizeof e->as);
    e->as.method.name      = vs->name;
    e->as.method.name_span = name_span;
    e->as.method.args      = args;
    e->as.method.builtin   = -2;
    e->type = vs->type;
    return vs->type;
}

static Type *check_match(Sema *s, Expr *e) {
    TypeTable *tt = s->tt;
    Type *subject = check_expr(s, e->as.match.subject);
    Type *result = NULL;
    bool has_else = false;
    bool covered[64] = { false };

    bool cov_ok = false, cov_err = false, cov_nil = false, cov_some = false;
    vec_foreach(i, &e->as.match.arms) {
        MatchArm *arm = &e->as.match.arms.items[i];
        scope_push(s, s->cur_fn);
        check_pattern(s, arm->pat, subject);
        if (arm->pat->kind == PAT_WILDCARD && !arm->guard) has_else = true;
        if (arm->pat->kind == PAT_BIND && !arm->guard) has_else = true;
        if (arm->pat->name && !arm->guard) {
            if (strcmp(arm->pat->name, "Ok") == 0) cov_ok = true;
            else if (strcmp(arm->pat->name, "Err") == 0) cov_err = true;
            else if (strcmp(arm->pat->name, "nil") == 0) cov_nil = true;
        }
        if (arm->pat->kind == PAT_LITERAL && !arm->guard) cov_nil = true;
        if ((arm->pat->kind == PAT_BIND || arm->pat->kind == PAT_VARIANT) && !arm->guard) cov_some = true;
        if (arm->pat->kind == PAT_VARIANT && subject->kind == TY_ENUM && subject->decl) {
            vec_foreach(k, &subject->decl->variants)
                if (strcmp(subject->decl->variants.items[k].name, arm->pat->name) == 0 && k < 64)
                    covered[k] = true;
        }
        if (arm->guard) {
            Type *g = check_expr(s, arm->guard);
            if (g->kind != TY_BOOL && g->kind != TY_ERROR)
                expect_type(s, arm->guard->span, tt->t_bool, g, "a match guard must be a Bool");
        }
        Type *at = NULL;
        if (arm->body) at = check_expr(s, arm->body);
        else if (arm->bbody) { check_block(s, arm->bbody, false); at = tt->t_nil; }
        scope_pop(s);
        result = result ? type_common(tt, result, at) : at;
    }
    if (!has_else) {
        if (subject->kind == TY_ENUM && subject->decl) {
            StrBuf missing; sb_init(&missing);
            int nmiss = 0;
            vec_foreach(k, &subject->decl->variants) {
                if (k < 64 && !covered[k]) {
                    if (nmiss++) sb_puts(&missing, ", ");
                    sb_puts(&missing, subject->decl->variants.items[k].name);
                }
            }
            if (nmiss) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0240", "This match does not cover every case");
                diag_label(s->db, d, e->span, true, "missing: %s", missing.data);
                diag_fix(s->db, d, "add  when %s -> ...", subject->decl->variants.items[0].name);
                diag_fix(s->db, d, "or finish with  else -> ...");
            }
            sb_free(&missing);
        } else if (subject->kind == TY_RESULT) {
            if (!cov_ok || !cov_err) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0240", "This match does not cover every case");
                diag_label(s->db, d, e->span, true, "missing: %s", !cov_ok ? "Ok" : "Err");
                diag_fix(s->db, d, "add  when %s(value) -> ...", !cov_ok ? "Ok" : "Err");
            }
        } else if (subject->kind == TY_MAYBE) {
            if (!cov_nil || !cov_some) {
                Diag *d = diag_new(s->db, DIAG_WARNING, "W0241", "This match may not cover nothing");
                diag_label(s->db, d, e->span, true, "add `when nil ->` or `else ->`");
            }
        } else if (subject->kind != TY_BOOL) {
            Diag *d = diag_new(s->db, DIAG_WARNING, "W0241", "This match has no `else` arm");
            diag_label(s->db, d, e->span, true, "add `else ->` so every value is handled");
        }
    }
    return result ? result : tt->t_nil;
}

static void check_pattern(Sema *s, Pattern *p, Type *subject) {
    TypeTable *tt = s->tt;
    if (!p) return;
    p->vtype = subject;
    switch (p->kind) {
        case PAT_WILDCARD: break;
        case PAT_BIND: {
            Symbol *sym = declare(s, SYM_VAR, p->name, subject, p->span);
            sym->used = true;
            p->sym = sym;
            break;
        }
        case PAT_LITERAL: {
            Type *lt = check_expr(s, p->lit);
            if (!type_assignable(subject, lt) && !type_assignable(lt, subject) &&
                subject->kind != TY_ANY && lt->kind != TY_ERROR) {
                Diag *d = diag_new(s->db, DIAG_WARNING, "W0242", "This pattern can never match");
                diag_label(s->db, d, p->span, true, "%s cannot equal %s",
                           type_text(s->arena, subject), type_text(s->arena, lt));
            }
            break;
        }
        case PAT_RANGE: {
            check_expr(s, p->lo);
            check_expr(s, p->hi);
            break;
        }
        case PAT_TYPE: {
            resolve_type(s, p->type);
            break;
        }
        case PAT_LIST: {
            Type *elem = subject->kind == TY_LIST ? subject->elem : tt->t_any;
            vec_foreach(i, &p->subs) check_pattern(s, p->subs.items[i], elem);
            if (p->rest) {
                Symbol *sym = declare(s, SYM_VAR, p->rest, type_list(tt, elem), p->span);
                sym->used = true;
            }
            break;
        }
        case PAT_DATA: {
            vec_foreach(i, &p->field_names) {
                int idx = -1;
                FieldDecl *f = find_field(subject, p->field_names.items[i], NULL, &idx);
                Type *ft = f ? resolve_type(s, f->type) : tt->t_any;
                if (!f) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0243", "%s has no field `%s`",
                                       type_text(s->arena, subject), p->field_names.items[i]);
                    diag_label(s->db, d, p->span, true, "unknown field in pattern");
                }
                if (i < p->subs.len) {
                    p->subs.items[i]->lo = NULL;
                    check_pattern(s, p->subs.items[i], ft);
                    p->subs.items[i]->hi = NULL;
                    /* remember the field index for codegen in `vtype`-adjacent slot */
                    p->subs.items[i]->name = p->subs.items[i]->name;
                    p->subs.items[i]->sym = p->subs.items[i]->sym;
                    p->subs.items[i]->vtype = ft;
                    p->subs.items[i]->span = p->span;
                    p->subs.items[i]->rest = NULL;
                    p->subs.items[i]->lit = NULL;
                    p->subs.items[i]->type = NULL;
                    /* store index inside the sub pattern's path vector */
                    vec_clear(&p->subs.items[i]->path);
                    char buf[16];
                    snprintf(buf, sizeof buf, "%d", idx);
                    vec_push(&p->subs.items[i]->path, intern(s->in, str_cstr(buf)));
                }
            }
            break;
        }
        case PAT_VARIANT: {
            /* Result Ok/Err and user enums */
            const char *vname = p->name;
            if (subject->kind == TY_RESULT) {
                if (strcmp(vname, "Ok") == 0) {
                    if (p->subs.len) check_pattern(s, p->subs.items[0], subject->elem);
                    break;
                }
                if (strcmp(vname, "Err") == 0) {
                    if (p->subs.len) check_pattern(s, p->subs.items[0], subject->err);
                    break;
                }
            }
            if (subject->kind == TY_ENUM && subject->decl) {
                VariantDecl *found = NULL;
                vec_foreach(i, &subject->decl->variants)
                    if (strcmp(subject->decl->variants.items[i].name, vname) == 0)
                        found = &subject->decl->variants.items[i];
                if (!found) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0244", "`%s` is not a case of %s",
                                       vname, subject->decl->name);
                    diag_label(s->db, d, p->span, true, "unknown case");
                    StrBuf b; sb_init(&b);
                    vec_foreach(i, &subject->decl->variants) {
                        if (i) sb_puts(&b, ", ");
                        sb_puts(&b, subject->decl->variants.items[i].name);
                    }
                    diag_note(s->db, d, "cases are: %s", b.data ? b.data : "");
                    sb_free(&b);
                    break;
                }
                p->sym = NULL;
                vec_foreach(i, &p->subs) {
                    Type *pt = i < found->payload.len ? resolve_type(s, found->payload.items[i]) : tt->t_any;
                    check_pattern(s, p->subs.items[i], pt);
                }
                /* remember tag for codegen */
                char buf[16];
                snprintf(buf, sizeof buf, "%d", found->tag);
                vec_clear(&p->path);
                vec_push(&p->path, intern(s->in, str_cstr(buf)));
                break;
            }
            /* a capitalised bare name that is not an enum case: treat as binding */
            if (!p->subs.len) {
                Symbol *sym = declare(s, SYM_VAR, p->name, subject, p->span);
                sym->used = true;
                p->sym = sym;
                p->kind = PAT_BIND;
            }
            break;
        }
    }
}

static bool is_assignable_target(Sema *s, Expr *t) {
    switch (t->kind) {
        case EX_IDENT: {
            Symbol *sym = t->as.ident.sym;
            if (!sym) return true;
            if (sym->kind == SYM_CONST) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0250", "`%s` is a constant", sym->name);
                diag_label(s->db, d, t->span, true, "constants never change");
                diag_label(s->db, d, sym->span, false, "declared here");
                diag_fix(s->db, d, "declare it with `var` if it has to change");
                return false;
            }
            if (!sym->is_mut) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0251", "`%s` cannot change", sym->name);
                diag_label(s->db, d, t->span, true, "assigned here");
                diag_label(s->db, d, sym->span, false, "`let` makes a fixed binding");
                diag_given_wanted(s->db, d, "let (fixed)", "var (changeable)");
                diag_fix(s->db, d, "change the declaration to  var %s = ...", sym->name);
                return false;
            }
            sym->assigned = true;
            return true;
        }
        case EX_FIELD: case EX_INDEX: return true;
        default: {
            Diag *d = diag_new(s->db, DIAG_ERROR, "E0252", "This cannot be assigned to");
            diag_label(s->db, d, t->span, true, "only names, fields and items can be assigned");
            return false;
        }
    }
}

static Type *check_expr(Sema *s, Expr *e) {
    TypeTable *tt = s->tt;
    if (!e) return tt->t_unknown;
    Type *t = tt->t_unknown;
    switch (e->kind) {
        case EX_INT:  t = tt->t_int; break;
        case EX_NUM:  t = tt->t_num; break;
        case EX_BYTE: t = tt->t_byte; break;
        case EX_BOOL: t = tt->t_bool; break;
        case EX_NIL:  t = tt->t_nil; break;
        case EX_TEXT: t = tt->t_text; break;
        case EX_INTERP: {
            vec_foreach(i, &e->as.str.parts) check_expr(s, e->as.str.parts.items[i]);
            t = tt->t_text;
            break;
        }
        case EX_SELF: {
            Symbol *sym = sema_lookup(s, intern(s->in, str_cstr("self")));
            if (!sym) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0260", "`self` is only available inside a method");
                diag_label(s->db, d, e->span, true, "used outside of an object");
                t = tt->t_error;
            } else {
                sym->used = true;
                t = sym->type;
            }
            break;
        }
        case EX_IDENT: {
            const char *name = e->as.ident.name;
            Symbol *sym = sema_lookup(s, name);
            if (!sym) {
                /* Ok / Err constructors */
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0261", "`%s` is not defined", name);
                diag_label(s->db, d, e->span, true, "used here before it exists");
                suggest_similar(s, d, name, "value");
                t = tt->t_error;
                break;
            }
            sym->used = true;
            e->as.ident.sym = sym;
            if (sym->kind == SYM_VARIANT) {
                ExprVec none; vec_init(&none);
                t = rewrite_variant(s, e, sym, none, e->span);
                break;
            }
            if (sym->moved) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0301", "`%s` was moved away", name);
                diag_label(s->db, d, e->span, true, "used after the move");
                diag_label(s->db, d, sym->move_span, false, "ownership moved here");
                diag_note(s->db, d, "`own` values have exactly one owner at a time");
                diag_fix(s->db, d, "clone it first:  let copy = clone(%s)", name);
                diag_fix(s->db, d, "or borrow it instead:  fn use(x: ref %s)", type_text(s->arena, sym->type));
            }
            if (sym->kind == SYM_FN && sym->fn) {
                TypeVec ps; vec_init(&ps);
                vec_foreach(i, &sym->fn->params)
                    vec_push(&ps, sym->fn->params.items[i].type ? resolve_type(s, sym->fn->params.items[i].type) : tt->t_any);
                Type *ft = type_fn(tt, ps, sym->fn->ret ? resolve_type(s, sym->fn->ret) : tt->t_nil);
                ft->fndecl = sym->fn;
                t = ft;
            } else t = sym->type ? sym->type : tt->t_unknown;
            e->is_lvalue = (sym->kind == SYM_VAR || sym->kind == SYM_GLOBAL || sym->kind == SYM_PARAM);
            break;
        }
        case EX_UNARY: {
            Type *ot = check_expr(s, e->as.unary.operand);
            if (e->as.unary.op == T_NOT) {
                if (ot->kind != TY_BOOL && ot->kind != TY_ERROR && ot->kind != TY_ANY) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0262", "`not` needs a Bool");
                    diag_label(s->db, d, e->span, true, "this is %s", type_text(s->arena, ot));
                    diag_given_wanted(s->db, d, type_text(s->arena, ot), "Bool");
                    if (ot->kind == TY_MAYBE) diag_fix(s->db, d, "compare with nil:  value == nil");
                }
                t = tt->t_bool;
            } else {
                if (!type_numeric(ot) && ot->kind != TY_ERROR) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0263", "`-` needs a number");
                    diag_label(s->db, d, e->span, true, "this is %s", type_text(s->arena, ot));
                    diag_given_wanted(s->db, d, type_text(s->arena, ot), "Int or Num");
                }
                t = ot;
            }
            break;
        }
        case EX_BINARY:  t = check_binary(s, e); break;
        case EX_LOGICAL: {
            Type *l = check_expr(s, e->as.binary.lhs);
            Type *r = check_expr(s, e->as.binary.rhs);
            if (l->kind != TY_BOOL && l->kind != TY_ERROR && l->kind != TY_ANY)
                expect_type(s, e->as.binary.lhs->span, tt->t_bool, l, "`and` / `or` work on Bool values");
            if (r->kind != TY_BOOL && r->kind != TY_ERROR && r->kind != TY_ANY)
                expect_type(s, e->as.binary.rhs->span, tt->t_bool, r, "`and` / `or` work on Bool values");
            t = tt->t_bool;
            break;
        }
        case EX_ASSIGN: {
            Type *vt = check_expr(s, e->as.assign.value);
            Type *tt2 = check_expr(s, e->as.assign.target);
            is_assignable_target(s, e->as.assign.target);
            if (e->as.assign.op == T_ASSIGN) {
                expect_type(s, e->as.assign.value->span, tt2, vt, NULL);
            } else {
                if (!type_numeric(tt2) && !(tt2->kind == TY_TEXT && e->as.assign.op == T_PLUSEQ)) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0264", "`%s` needs a number", op_text(e->as.assign.op));
                    diag_label(s->db, d, e->span, true, "this is %s", type_text(s->arena, tt2));
                } else expect_type(s, e->as.assign.value->span, tt2, vt, NULL);
            }
            t = tt->t_nil;
            break;
        }
        case EX_CALL: {
            Expr *callee = e->as.call.callee;
            /* Ok(...) / Err(...) */
            if (callee->kind == EX_IDENT) {
                const char *n = callee->as.ident.name;
                if (strcmp(n, "Ok") == 0 && !sema_lookup(s, n)) {
                    Type *inner = e->as.call.args.len ? check_expr(s, e->as.call.args.items[0]) : tt->t_nil;
                    t = type_result(tt, inner, tt->t_text);
                    e->as.call.callee->as.ident.sym = NULL;
                    e->comptime_known = false;
                    e->type = t;
                    e->as.call.names.len = e->as.call.names.len;
                    return t;
                }
                if (strcmp(n, "Err") == 0 && !sema_lookup(s, n)) {
                    Type *inner = e->as.call.args.len ? check_expr(s, e->as.call.args.items[0]) : tt->t_text;
                    t = type_result(tt, tt->t_any, inner);
                    e->type = t;
                    return t;
                }
            }
            if (callee->kind == EX_IDENT) {
                Symbol *vsym = sema_lookup(s, callee->as.ident.name);
                if (vsym && vsym->kind == SYM_VARIANT) {
                    ExprVec cargs = e->as.call.args;
                    t = rewrite_variant(s, e, vsym, cargs, callee->span);
                    break;
                }
            }
            Type *ct = check_expr(s, callee);
            if (ct->kind == TY_ERROR) { vec_foreach(i, &e->as.call.args) check_expr(s, e->as.call.args.items[i]); t = tt->t_error; break; }
            if (ct->kind == TY_TYPEREF && ct->decl) {
                /* Point(1, 2) positional construction */
                TypeVec ps; vec_init(&ps);
                vec_foreach(i, &ct->decl->fields) vec_push(&ps, resolve_type(s, ct->decl->fields.items[i].type));
                check_call_args(s, e->span, ct->decl->name, &ps, &e->as.call.args, NULL, NULL, false);
                vec_free(&ps);
                t = ct->decl->type;
                break;
            }
            if (ct->kind != TY_FN) {
                if (ct->kind == TY_ANY) {
                    vec_foreach(i, &e->as.call.args) check_expr(s, e->as.call.args.items[i]);
                    t = tt->t_any;
                    break;
                }
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0270", "This is not a function");
                diag_label(s->db, d, callee->span, true, "this is %s", type_text(s->arena, ct));
                diag_fix(s->db, d, "remove the parentheses to use the value itself");
                vec_foreach(i, &e->as.call.args) check_expr(s, e->as.call.args.items[i]);
                t = tt->t_error;
                break;
            }
            const char *what = "this function";
            if (callee->kind == EX_IDENT) what = callee->as.ident.name;
            GenericEnv env = { 0 };
            t = check_call_args(s, e->span, what, &ct->params, &e->as.call.args, ct->ret, &env, false);
            move_call_args(s, ct->fndecl, &e->as.call.args);
            if (ct->fndecl && ct->fndecl->is_task) t = type_future(tt, t);
            break;
        }
        case EX_METHOD: t = check_method(s, e); break;
        case EX_FIELD:  t = check_field(s, e); break;
        case EX_INDEX: {
            Type *ot = check_expr(s, e->as.index.obj);
            Type *it = check_expr(s, e->as.index.index);
            e->is_lvalue = true;
            if (ot->kind == TY_LIST) {
                if (!type_numeric(it)) expect_type(s, e->as.index.index->span, tt->t_int, it, "lists are indexed by Int");
                t = ot->elem ? ot->elem : tt->t_any;
            } else if (ot->kind == TY_MAP) {
                expect_type(s, e->as.index.index->span, ot->key, it, NULL);
                t = type_maybe(tt, ot->elem ? ot->elem : tt->t_any);
            } else if (ot->kind == TY_TEXT) {
                t = tt->t_text;
            } else if (ot->kind == TY_ANY) {
                t = tt->t_any;
            } else if (ot->kind != TY_ERROR) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0271", "%s cannot be indexed", type_text(s->arena, ot));
                diag_label(s->db, d, e->span, true, "indexing works on lists, maps and text");
                t = tt->t_error;
            }
            break;
        }
        case EX_LIST: {
            Type *elem = NULL;
            vec_foreach(i, &e->as.list.items) {
                Type *it = check_expr(s, e->as.list.items.items[i]);
                elem = elem ? type_common(tt, elem, it) : it;
            }
            t = type_list(tt, elem ? elem : tt->t_unknown);
            break;
        }
        case EX_MAP: {
            Type *kt = NULL, *vt = NULL;
            vec_foreach(i, &e->as.map.keys) {
                Type *k = check_expr(s, e->as.map.keys.items[i]);
                Type *v = check_expr(s, e->as.map.vals.items[i]);
                kt = kt ? type_common(tt, kt, k) : k;
                vt = vt ? type_common(tt, vt, v) : v;
            }
            if (kt && !type_hashable(kt)) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0272", "%s cannot be a map key", type_text(s->arena, kt));
                diag_label(s->db, d, e->span, true, "keys must be Text, Int, Num, Bool or an enum");
            }
            t = type_map(tt, kt ? kt : tt->t_text, vt ? vt : tt->t_unknown);
            break;
        }
        case EX_STRUCT: {
            Type *st = resolve_type(s, e->as.strct.type);
            if (st->kind == TY_ERROR) { t = st; break; }
            if (st->kind != TY_DATA && st->kind != TY_OBJECT) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0273", "%s is not a data or object type",
                                   type_text(s->arena, st));
                diag_label(s->db, d, e->span, true, "cannot build this with { ... }");
                t = tt->t_error;
                break;
            }
            TypeDecl *td = st->decl;
            bool *seen = NEWN(s->arena, bool, td->fields.len + 1);
            vec_foreach(i, &e->as.strct.fields) {
                FieldInit *fi = &e->as.strct.fields.items[i];
                int idx = -1;
                FieldDecl *f = find_field(st, fi->name, NULL, &idx);
                if (!f) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0274", "`%s` has no field called `%s`",
                                       td->name, fi->name);
                    diag_label(s->db, d, fi->span, true, "unknown field");
                    StrBuf b; sb_init(&b);
                    vec_foreach(k, &td->fields) { if (k) sb_puts(&b, ", "); sb_puts(&b, td->fields.items[k].name); }
                    if (b.len) diag_note(s->db, d, "fields are: %s", b.data);
                    sb_free(&b);
                    check_expr(s, fi->value);
                    continue;
                }
                if (idx >= 0 && idx < td->fields.len) seen[idx] = true;
                Type *ft = resolve_type(s, f->type);
                Type *vt = check_expr(s, fi->value);
                if (!type_assignable(ft, vt)) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0275", "Wrong type for field `%s`", fi->name);
                    diag_label(s->db, d, fi->value->span, true, "this is %s", type_text(s->arena, vt));
                    diag_given_wanted(s->db, d, type_text(s->arena, vt), type_text(s->arena, ft));
                }
            }
            StrBuf missing; sb_init(&missing);
            int nmiss = 0;
            vec_foreach(i, &td->fields) {
                if (!seen[i] && !td->fields.items[i].deflt && !td->fields.items[i].is_static) {
                    if (nmiss++) sb_puts(&missing, ", ");
                    sb_puts(&missing, td->fields.items[i].name);
                }
            }
            if (nmiss) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0276", "`%s` is missing %d field%s",
                                   td->name, nmiss, nmiss == 1 ? "" : "s");
                diag_label(s->db, d, e->span, true, "missing: %s", missing.data);
                diag_fix(s->db, d, "add them:  %s { %s: ... }", td->name, missing.data);
            }
            sb_free(&missing);
            t = st;
            break;
        }
        case EX_LAMBDA: {
            FnDecl *fn = e->as.lambda.fn;
            Type *want = s->expected_fn;
            s->expected_fn = NULL;
            if (!fn->name) fn->name = arena_vsprintf(s->arena, "lambda#%d", ++s->anon_counter);
            Scope *saved_scope = s->scope;
            FnDecl *saved_fn = s->cur_fn;
            Type *saved_ret = s->cur_ret;
            s->cur_fn = fn;
            fn->local_count = 0;
            scope_push(s, fn);
            TypeVec ps; vec_init(&ps);
            vec_foreach(i, &fn->params) {
                Param *p = &fn->params.items[i];
                Type *pt = p->type ? resolve_type(s, p->type)
                         : (want && i < want->params.len ? want->params.items[i] : tt->t_any);
                p->sym = declare(s, SYM_PARAM, p->name, pt, p->span);
                p->sym->is_mut = p->mut;
                vec_push(&ps, pt);
            }
            Type *ret = fn->ret ? resolve_type(s, fn->ret) : tt->t_unknown;
            s->cur_ret = ret;
            if (fn->expr_body) {
                Type *bt = check_expr(s, fn->expr_body);
                if (!fn->ret) ret = bt;
            } else if (fn->body) {
                check_block(s, fn->body, false);
                if (!fn->ret) ret = tt->t_nil;
            }
            s->cur_ret = ret;
            /* capture symbols recorded in this scope */
            vec_foreach(i, &s->scope->syms) {
                Symbol *sym = s->scope->syms.items[i];
                if (sym->kind == SYM_CAPTURE) vec_push(&e->as.lambda.captures, sym);
            }
            scope_pop(s);
            s->scope = saved_scope;
            s->cur_fn = saved_fn;
            s->cur_ret = saved_ret;
            Type *ft = type_fn(tt, ps, ret ? ret : tt->t_nil);
            ft->fndecl = fn;
            fn->type = ft;
            vec_push(&s->all_fns, fn);
            t = ft;
            break;
        }
        case EX_IF: {
            Type *c = check_expr(s, e->as.iff.cond);
            if (c->kind != TY_BOOL && c->kind != TY_ERROR && c->kind != TY_ANY) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0280", "`if` needs a Bool");
                diag_label(s->db, d, e->as.iff.cond->span, true, "this is %s", type_text(s->arena, c));
                diag_given_wanted(s->db, d, type_text(s->arena, c), "Bool");
                if (c->kind == TY_MAYBE) diag_fix(s->db, d, "compare it:  if value != nil { ... }");
                if (type_numeric(c)) diag_fix(s->db, d, "compare it:  if value != 0 { ... }");
            }
            Type *a = NULL, *b = NULL;
            scope_push(s, s->cur_fn);
            check_block(s, e->as.iff.then_b, false);
            if (e->as.iff.then_b->stmts.len) {
                Stmt *last = e->as.iff.then_b->stmts.items[e->as.iff.then_b->stmts.len - 1];
                if (last->kind == ST_EXPR) a = last->as.expr->type;
            }
            scope_pop(s);
            if (e->as.iff.else_b) {
                scope_push(s, s->cur_fn);
                check_block(s, e->as.iff.else_b, false);
                if (e->as.iff.else_b->stmts.len) {
                    Stmt *last = e->as.iff.else_b->stmts.items[e->as.iff.else_b->stmts.len - 1];
                    if (last->kind == ST_EXPR) b = last->as.expr->type;
                }
                scope_pop(s);
            }
            t = (a && b) ? type_common(tt, a, b) : tt->t_nil;
            break;
        }
        case EX_MATCH: t = check_match(s, e); break;
        case EX_BLOCK: {
            scope_push(s, s->cur_fn);
            check_block(s, e->as.block.block, false);
            scope_pop(s);
            t = tt->t_nil;
            break;
        }
        case EX_RANGE: {
            Type *lo = check_expr(s, e->as.range.lo);
            Type *hi = check_expr(s, e->as.range.hi);
            if (!type_numeric(lo) || !type_numeric(hi)) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0281", "Ranges need numbers");
                diag_label(s->db, d, e->span, true, "%s .. %s", type_text(s->arena, lo), type_text(s->arena, hi));
            }
            t = type_range(tt, tt->t_int);
            break;
        }
        case EX_CAST: {
            Type *vt = check_expr(s, e->as.cast.value);
            Type *to = resolve_type(s, e->as.cast.type);
            if (!type_castable(to, vt)) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0282", "Cannot convert %s to %s",
                                   type_text(s->arena, vt), type_text(s->arena, to));
                diag_label(s->db, d, e->span, true, "this conversion is not possible");
                diag_given_wanted(s->db, d, type_text(s->arena, vt), type_text(s->arena, to));
            }
            t = to;
            break;
        }
        case EX_IS: {
            check_expr(s, e->as.is.value);
            resolve_type(s, e->as.is.type);
            t = tt->t_bool;
            break;
        }
        case EX_TRY: {
            Type *vt = check_expr(s, e->as.wrap.value);
            if (vt->kind == TY_RESULT) {
                t = vt->elem;
                if (s->cur_ret && s->cur_ret->kind != TY_RESULT && s->cur_ret->kind != TY_UNKNOWN) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0290", "`try` can only be used in a function that gives a Result");
                    diag_label(s->db, d, e->span, true, "this propagates an error");
                    diag_given_wanted(s->db, d, type_text(s->arena, s->cur_ret), "Result<T, E>");
                    diag_fix(s->db, d, "change the return type to  -> Result<%s, Text>", type_text(s->arena, vt->elem));
                    diag_fix(s->db, d, "or handle it here with  match value { when Ok(v) -> ... }");
                }
            } else if (vt->kind == TY_MAYBE) {
                t = vt->elem;
            } else if (vt->kind != TY_ERROR) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0291", "`try` needs a Result or a Maybe");
                diag_label(s->db, d, e->span, true, "this is %s", type_text(s->arena, vt));
                t = vt;
            }
            break;
        }
        case EX_FORCE: {
            Type *vt = check_expr(s, e->as.wrap.value);
            if (vt->kind == TY_MAYBE) t = vt->elem;
            else if (vt->kind == TY_RESULT) t = vt->elem;
            else t = vt;
            break;
        }
        case EX_AWAIT: {
            Type *vt = check_expr(s, e->as.wrap.value);
            if (vt->kind == TY_FUTURE) t = vt->elem;
            else if (vt->kind == TY_CHAN) t = type_maybe(tt, vt->elem);
            else if (vt->kind != TY_ERROR) {
                Diag *d = diag_new(s->db, DIAG_WARNING, "W0292", "`await` on a value that is not a task");
                diag_label(s->db, d, e->span, true, "this is %s", type_text(s->arena, vt));
                diag_note(s->db, d, "await works on Future<T> from `spawn` or a `task fn`");
                t = vt;
            } else t = vt;
            break;
        }
        case EX_SPAWN: {
            Type *vt = check_expr(s, e->as.wrap.value);
            t = vt->kind == TY_FUTURE ? vt : type_future(tt, vt);
            break;
        }
        case EX_COALESCE: {
            Type *a = check_expr(s, e->as.coalesce.value);
            Type *b = check_expr(s, e->as.coalesce.fallback);
            /* an empty [] or [:] fallback takes the type of the left side */
            Type *inner = a->kind == TY_MAYBE || a->kind == TY_RESULT ? a->elem : NULL;
            if (inner && b && type_assignable(inner, b)) b = inner;
            if (a->kind == TY_MAYBE) t = type_common(tt, a->elem, b);
            else if (a->kind == TY_RESULT) t = type_common(tt, a->elem, b);
            else {
                Diag *d = diag_new(s->db, DIAG_WARNING, "W0293", "`??` on a value that is never nil");
                diag_label(s->db, d, e->as.coalesce.value->span, true, "this is %s", type_text(s->arena, a));
                diag_fix(s->db, d, "remove the `?? ...` part");
                t = a;
            }
            break;
        }
        case EX_NEW: {
            Type *nt = resolve_type(s, e->as.nw.type);
            if (nt->kind == TY_OBJECT || nt->kind == TY_DATA) {
                FnDecl *init = find_method(nt, intern(s->in, str_cstr("init")), NULL);
                if (init) {
                    TypeVec ps; vec_init(&ps);
                    vec_foreach(i, &init->params) if (!init->params.items[i].is_self)
                        vec_push(&ps, init->params.items[i].type ? resolve_type(s, init->params.items[i].type) : tt->t_any);
                    check_call_args(s, e->span, arena_vsprintf(s->arena, "%s.init", nt->decl->name),
                                    &ps, &e->as.nw.args, NULL, NULL, false);
                    vec_free(&ps);
                } else {
                    TypeVec ps; vec_init(&ps);
                    vec_foreach(i, &nt->decl->fields)
                        if (!nt->decl->fields.items[i].is_static)
                            vec_push(&ps, resolve_type(s, nt->decl->fields.items[i].type));
                    if (e->as.nw.args.len)
                        check_call_args(s, e->span, nt->decl->name, &ps, &e->as.nw.args, NULL, NULL, false);
                    vec_free(&ps);
                }
            } else if (nt->kind != TY_ERROR) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0294", "`new` needs an object or data type");
                diag_label(s->db, d, e->span, true, "this is %s", type_text(s->arena, nt));
            }
            t = nt;
            break;
        }
        case EX_COMPTIME: {
            t = check_expr(s, e->as.wrap.value);
            e->comptime_known = true;
            break;
        }
        default: t = tt->t_any; break;
    }
    e->type = t;
    return t;
}

/* ====================================================================== */
/*  statements                                                             */
/* ====================================================================== */

static bool block_always_exits(Block *b) {
    if (!b || !b->stmts.len) return false;
    Stmt *last = b->stmts.items[b->stmts.len - 1];
    switch (last->kind) {
        case ST_GIVE: case ST_FAIL: case ST_BREAK: return true;
        case ST_IF:
            return block_always_exits(last->as.iff.then_b) && last->as.iff.else_b &&
                   block_always_exits(last->as.iff.else_b);
        default: return false;
    }
}

static void check_stmt(Sema *s, Stmt *st) {
    TypeTable *tt = s->tt;
    if (!st) return;
    switch (st->kind) {
        case ST_EXPR: {
            Type *t = check_expr(s, st->as.expr);
            Expr *e = st->as.expr;
            if (t && t->kind == TY_RESULT && e->kind != EX_ASSIGN) {
                Diag *d = diag_new(s->db, DIAG_WARNING, "W0295", "This Result is ignored");
                diag_label(s->db, d, e->span, true, "the error case is never checked");
                diag_fix(s->db, d, "handle it with  match ... { when Ok(v) -> ... when Err(e) -> ... }");
                diag_fix(s->db, d, "or propagate it with  try ...");
            }
            break;
        }
        case ST_LET: {
            Type *declared = st->as.let.type ? resolve_type(s, st->as.let.type) : NULL;
            Type *init = NULL;
            if (st->as.let.init) {
                init = check_expr(s, st->as.let.init);
                if (declared && !type_assignable(declared, init)) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0201", "Type mismatch");
                    diag_label(s->db, d, st->as.let.init->span, true, "this is %s", type_text(s->arena, init));
                    if (st->as.let.type) diag_label(s->db, d, st->as.let.type->span, false, "declared as %s", type_text(s->arena, declared));
                    diag_given_wanted(s->db, d, type_text(s->arena, init), type_text(s->arena, declared));
                    if (declared->kind == TY_TEXT) diag_fix(s->db, d, "convert the value with  to_text(...)");
                    else if (type_numeric(declared) && init->kind == TY_TEXT)
                        diag_fix(s->db, d, "parse the text with  .to_%s()", declared->kind == TY_INT ? "int" : "num");
                    else diag_fix(s->db, d, "change the declared type to %s", type_text(s->arena, init));
                }
            }
            Type *final = declared ? declared : (init ? init : tt->t_unknown);
            if (!declared && init && init->kind == TY_NIL) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0130", "Cannot infer a type from `nil`");
                diag_label(s->db, d, st->span, true, "nil alone does not say what this holds");
                diag_fix(s->db, d, "write the type:  let %s: Text? = nil", st->as.let.name ? st->as.let.name : "x");
                final = type_maybe(tt, tt->t_any);
            }
            if (final->kind == TY_UNKNOWN && init) final = init;
            if (st->as.let.pat) {
                check_pattern(s, st->as.let.pat, final);
            } else {
                Symbol *sym = declare(s, st->as.let.is_const ? SYM_CONST : SYM_VAR,
                                      st->as.let.name, final, st->span);
                sym->is_mut = st->as.let.mutable_;
                sym->value = st->as.let.init;
                st->as.let.sym = sym;
                /* `own T` means this binding is the single owner */
                sym->is_owned = st->as.let.type && st->as.let.type->kind == TE_OWN;
                if (sym->is_owned) move_from(s, st->as.let.init);
                if (st->as.let.is_const && st->as.let.init && !st->as.let.init->comptime_known) {
                    /* constants must be simple values */
                    Expr *iv = st->as.let.init;
                    if (iv->kind != EX_INT && iv->kind != EX_NUM && iv->kind != EX_TEXT &&
                        iv->kind != EX_BOOL && iv->kind != EX_NIL && iv->kind != EX_BINARY &&
                        iv->kind != EX_LIST && iv->kind != EX_MAP && iv->kind != EX_UNARY) {
                        Diag *d = diag_new(s->db, DIAG_ERROR, "E0131", "A `const` must be known while compiling");
                        diag_label(s->db, d, iv->span, true, "this is computed while the program runs");
                        diag_fix(s->db, d, "use `let` for values computed at run time");
                    }
                }
            }
            break;
        }
        case ST_GIVE: {
            Type *vt = st->as.give.value ? check_expr(s, st->as.give.value) : tt->t_nil;
            if (s->cur_ret && s->cur_ret->kind != TY_UNKNOWN) {
                if (!type_assignable(s->cur_ret, vt)) {
                    Diag *d = diag_new(s->db, DIAG_ERROR, "E0300", "This function gives back the wrong type");
                    diag_label(s->db, d, st->as.give.value ? st->as.give.value->span : st->span, true,
                               "this is %s", type_text(s->arena, vt));
                    if (s->cur_fn && s->cur_fn->ret)
                        diag_label(s->db, d, s->cur_fn->ret->span, false, "declared to give %s", type_text(s->arena, s->cur_ret));
                    diag_given_wanted(s->db, d, type_text(s->arena, vt), type_text(s->arena, s->cur_ret));
                    if (s->cur_ret->kind == TY_TEXT) diag_fix(s->db, d, "convert the value with  to_text(...)");
                    if (s->cur_ret->kind == TY_RESULT) diag_fix(s->db, d, "wrap it:  give Ok(value)");
                }
            } else if (s->cur_fn && !s->cur_fn->ret && st->as.give.value && s->cur_fn->is_lambda) {
                s->cur_ret = vt;
            }
            break;
        }
        case ST_IF: break;
        case ST_WHILE: {
            Type *c = check_expr(s, st->as.whil.cond);
            if (c->kind != TY_BOOL && c->kind != TY_ERROR && c->kind != TY_ANY)
                expect_type(s, st->as.whil.cond->span, tt->t_bool, c, "`while` needs a Bool");
            s->loop_depth++;
            scope_push(s, s->cur_fn);
            check_block(s, st->as.whil.body, false);
            scope_pop(s);
            s->loop_depth--;
            break;
        }
        case ST_LOOP: {
            s->loop_depth++;
            scope_push(s, s->cur_fn);
            check_block(s, st->as.loop.body, false);
            scope_pop(s);
            s->loop_depth--;
            break;
        }
        case ST_FOR: {
            Type *it = check_expr(s, st->as.forr.iter);
            Type *elem = tt->t_any;
            if (it->kind == TY_LIST || it->kind == TY_SET) elem = it->elem ? it->elem : tt->t_any;
            else if (it->kind == TY_RANGE) elem = tt->t_int;
            else if (it->kind == TY_MAP) elem = it->key ? it->key : tt->t_any;
            else if (it->kind == TY_TEXT) elem = tt->t_text;
            else if (it->kind == TY_CHAN) elem = it->elem ? it->elem : tt->t_any;
            else if (it->kind != TY_ERROR && it->kind != TY_ANY) {
                Diag *d = diag_new(s->db, DIAG_ERROR, "E0310", "%s cannot be looped over", type_text(s->arena, it));
                diag_label(s->db, d, st->as.forr.iter->span, true, "`for` works on lists, maps, sets, text and ranges");
                diag_fix(s->db, d, "use a range:  for i in 0..10 { ... }");
            }
            s->loop_depth++;
            scope_push(s, s->cur_fn);
            Symbol *v = declare(s, SYM_VAR, st->as.forr.var, elem, st->span);
            st->as.forr.sym = v;
            if (st->as.forr.pat && it->kind == TY_MAP) {
                Symbol *v2 = declare(s, SYM_VAR, st->as.forr.pat->name, it->elem ? it->elem : tt->t_any, st->span);
                st->as.forr.pat->sym = v2;
            } else if (st->as.forr.pat) {
                Symbol *v2 = declare(s, SYM_VAR, st->as.forr.pat->name, tt->t_int, st->span);
                st->as.forr.pat->sym = v2;
            }
            check_block(s, st->as.forr.body, false);
            scope_pop(s);
            s->loop_depth--;
            break;
        }
        case ST_BREAK: case ST_SKIP: break;
        case ST_BLOCK:
            scope_push(s, s->cur_fn);
            check_block(s, st->as.block.block, false);
            scope_pop(s);
            break;
        case ST_DEFER:
            scope_push(s, s->cur_fn);
            check_block(s, st->as.block.block, false);
            scope_pop(s);
            break;
        case ST_UNSAFE:
            s->unsafe_depth++;
            scope_push(s, s->cur_fn);
            s->scope->is_unsafe = true;
            check_block(s, st->as.block.block, false);
            scope_pop(s);
            s->unsafe_depth--;
            break;
        case ST_FAIL: {
            Type *vt = check_expr(s, st->as.fail.value);
            if (vt->kind != TY_TEXT && vt->kind != TY_ERROR && vt->kind != TY_ANY)
                expect_type(s, st->as.fail.value->span, tt->t_text, vt, "`fail` takes a message");
            break;
        }
        case ST_TRY: {
            scope_push(s, s->cur_fn);
            check_block(s, st->as.tryc.body, false);
            scope_pop(s);
            if (st->as.tryc.handler) {
                scope_push(s, s->cur_fn);
                if (st->as.tryc.err_name)
                    st->as.tryc.err_sym = declare(s, SYM_VAR, st->as.tryc.err_name, tt->t_text, st->span);
                check_block(s, st->as.tryc.handler, false);
                scope_pop(s);
            }
            break;
        }
        case ST_DECL: break;   /* nested declarations are hoisted earlier */
        default: break;
    }
}

static void check_block(Sema *s, Block *b, bool new_scope) {
    if (!b) return;
    if (new_scope) scope_push(s, s->cur_fn);
    bool exited = false;
    Span exit_span = SPAN_NONE;
    vec_foreach(i, &b->stmts) {
        Stmt *st = b->stmts.items[i];
        if (exited) {
            Diag *d = diag_new(s->db, DIAG_WARNING, "W0320", "This code can never run");
            diag_label(s->db, d, st->span, true, "unreachable");
            diag_label(s->db, d, exit_span, false, "the function already left here");
            exited = false;  /* report once per block */
        }
        check_stmt(s, st);
        if (st->kind == ST_GIVE || st->kind == ST_FAIL || st->kind == ST_BREAK || st->kind == ST_SKIP) {
            exited = true;
            exit_span = st->span;
        }
    }
    if (new_scope) scope_pop(s);
}

/* ====================================================================== */
/*  declarations                                                           */
/* ====================================================================== */

static void check_fn_body(Sema *s, FnDecl *fn, Type *self_type) {
    if (!fn->body && !fn->expr_body) return;
    TypeTable *tt = s->tt;
    FnDecl *saved_fn = s->cur_fn;
    Type *saved_ret = s->cur_ret;
    s->cur_fn = fn;
    fn->local_count = 0;
    scope_push(s, fn);
    vec_foreach(i, &fn->generics) {
        GenericParam *g = &fn->generics.items[i];
        Symbol *gs = sym_new(s, SYM_GENERIC, g->name, type_generic(tt, g->name, i), g->span);
        gs->used = true;
        vec_push(&s->scope->syms, gs);
    }
    vec_foreach(i, &fn->params) {
        Param *p = &fn->params.items[i];
        Type *pt;
        if (p->is_self) pt = self_type ? self_type : tt->t_any;
        else pt = p->type ? resolve_type(s, p->type) : tt->t_any;
        p->sym = declare(s, SYM_PARAM, p->name, pt, p->span);
        p->sym->is_mut = p->mut || p->is_self;
        p->sym->is_self = p->is_self;
        p->sym->is_owned = p->type && p->type->kind == TE_OWN;
        if (p->is_self) p->sym->used = true;
    }
    Type *ret = fn->ret ? resolve_type(s, fn->ret) : tt->t_nil;
    if (fn->is_init) ret = self_type;
    s->cur_ret = ret;
    if (fn->expr_body) {
        Type *bt = check_expr(s, fn->expr_body);
        if (fn->ret && !type_assignable(ret, bt)) {
            Diag *d = diag_new(s->db, DIAG_ERROR, "E0300", "This function gives back the wrong type");
            diag_label(s->db, d, fn->expr_body->span, true, "this is %s", type_text(s->arena, bt));
            diag_given_wanted(s->db, d, type_text(s->arena, bt), type_text(s->arena, ret));
        }
    } else {
        check_block(s, fn->body, false);
        /* a function that promises a value must give one */
        if (fn->ret && ret->kind != TY_NIL && !block_always_exits(fn->body) && !fn->is_init) {
            Diag *d = diag_new(s->db, DIAG_ERROR, "E0302", "`%s` does not give a value on every path", fn->name);
            diag_label(s->db, d, fn->ret->span, true, "declared to give %s", type_text(s->arena, ret));
            diag_label(s->db, d, fn->body->span, false, "this body can finish without `give`");
            diag_fix(s->db, d, "add a `give ...` at the end");
            diag_fix(s->db, d, "or add an `else` branch that gives a value");
        }
    }
    scope_pop(s);
    s->cur_fn = saved_fn;
    s->cur_ret = saved_ret;
}

/* ---- pass 1: create the type symbols ---- */
static void declare_types(Sema *s, Module *m) {
    s->module = m;
    s->scope = m->scope;
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind != D_TYPE) continue;
        TypeDecl *td = d->as.type;
        TypeKind k = td->kind == TD_DATA ? TY_DATA : td->kind == TD_OBJECT ? TY_OBJECT :
                     td->kind == TD_TRAIT ? TY_TRAIT : td->kind == TD_ENUM ? TY_ENUM : TY_ANY;
        if (td->kind == TD_ALIAS) continue;
        Type *t = type_new(s->tt, k);
        t->name = td->name;
        t->decl = td;
        td->type = t;
        Symbol *sym = declare(s, SYM_TYPE, td->name, t, td->name_span);
        sym->td = td;
        sym->used = td->is_pub;
        td->sym = sym;
    }
    /* aliases after real types so they can refer to them */
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind != D_TYPE || d->as.type->kind != TD_ALIAS) continue;
        TypeDecl *td = d->as.type;
        Type *t = resolve_type(s, td->aliased);
        td->type = t;
        Symbol *sym = declare(s, SYM_TYPE, td->name, t, td->name_span);
        sym->td = td;
        sym->used = true;
    }
}

/* ---- pass 2: resolve imports ---- */
static void resolve_imports(Sema *s, Module *m) {
    s->module = m;
    s->scope = m->scope;
    int nmods = 0;
    const char **mods = natives_module_names(&nmods);
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind != D_USE) continue;
        NameVec *path = &d->as.use.path;
        if (!path->len) continue;
        const char *last = path->items[path->len - 1];
        const char *alias = d->as.use.alias ? d->as.use.alias : last;
        bool is_std = strcmp(path->items[0], "std") == 0;
        bool found = false;
        if (is_std && path->len >= 2) {
            for (int k = 0; k < nmods; k++) {
                if (strcmp(mods[k], last) == 0) {
                    Symbol *sym = declare(s, SYM_NATIVE_MOD, alias, s->tt->t_any, d->span);
                    sym->native_mod = mods[k];
                    sym->used = false;
                    found = true;
                    break;
                }
            }
        }
        if (found) continue;
        /* user module (dotted name match) */
        StrBuf nb; sb_init(&nb);
        vec_foreach(k, path) { if (k) sb_putc(&nb, '.'); sb_puts(&nb, path->items[k]); }
        vec_foreach(k, &s->modules) {
            Module *om = s->modules.items[k];
            if (om == m) continue;
            if (strcmp(om->name, nb.data) == 0 || strcmp(path_basename(om->name), last) == 0) {
                Symbol *sym = declare(s, SYM_MODULE, alias, s->tt->t_any, d->span);
                sym->mod = om;
                sym->used = false;
                found = true;
                vec_push(&m->imports, om);
                break;
            }
        }
        if (!found) {
            Diag *dg = diag_new(s->db, DIAG_ERROR, "E0140", "Cannot find module `%s`", nb.data);
            diag_label(s->db, dg, d->span, true, "this module is not available");
            StrBuf avail; sb_init(&avail);
            for (int k = 0; k < nmods && k < 10; k++) { if (k) sb_puts(&avail, ", "); sb_puts(&avail, mods[k]); }
            diag_note(s->db, dg, "standard modules: %s, ...", avail.data ? avail.data : "");
            diag_fix(s->db, dg, "write  use std.io  for the standard library");
            diag_fix(s->db, dg, "or add the package with  sprfst add <name>");
            sb_free(&avail);
        }
        sb_free(&nb);
    }
}

/* ---- pass 3: fill in type members and function signatures ---- */

/* ---------------------------------------------------------------- apps */
/* `state x = v` entries become module globals; `on <event> { ... }` blocks
   become real zero-argument functions so they are compiled, type checked
   and debuggable exactly like any other function. */

static void app_declare_state(Sema *s, UiNode *n) {
    if (strcmp(n->kind, "state") == 0) {
        const char *nm = n->prop_names.len ? n->prop_names.items[0] : n->label;
        if (!nm) return;
        Symbol *sym = declare(s, SYM_GLOBAL, nm, s->tt->t_unknown, n->span);
        sym->is_mut  = true;
        sym->slot    = s->globals.len;
        sym->used    = true;
        vec_push(&s->globals, sym);
        Decl *g = NEW(s->arena, Decl);
        g->kind = D_LET;
        g->span = n->span;
        vec_init(&g->attrs);
        g->as.konst.name     = nm;
        g->as.konst.value    = n->prop_values.len ? n->prop_values.items[0] : NULL;
        g->as.konst.sym      = sym;
        g->as.konst.mutable_ = true;
        vec_push(&s->all_consts, g);
        return;
    }
    vec_foreach(i, &n->children) app_declare_state(s, n->children.items[i]);
}

static void app_declare_handlers(Sema *s, UiNode *n, const char *app_name, int *counter) {
    vec_foreach(i, &n->handler_names) {
        FnDecl *fn = NEW(s->arena, FnDecl);
        memset(fn, 0, sizeof *fn);
        vec_init(&fn->generics); vec_init(&fn->params); vec_init(&fn->attrs);
        StrBuf nm; sb_init(&nm);
        sb_printf(&nm, "%s.on %s #%d", app_name ? app_name : "app", n->handler_names.items[i], (*counter)++);
        fn->name      = intern(s->in, str_cstr(nm.data));
        sb_free(&nm);
        fn->span      = n->span;
        fn->name_span = n->span;
        fn->body      = n->handler_bodies.items[i];
        fn->ir_index  = -1;
        TypeVec ps; vec_init(&ps);
        /* `on change(text) { ... }` takes the value the event carries */
        const char *argname = i < n->handler_params.len ? n->handler_params.items[i] : NULL;
        if (argname) {
            Param p; memset(&p, 0, sizeof p);
            p.name = argname;
            p.type = NULL;                      /* typed Any */
            p.span = n->span;
            vec_push(&fn->params, p);
            vec_push(&ps, s->tt->t_any);
        }
        fn->type = type_fn(s->tt, ps, s->tt->t_nil);
        if (i < n->handler_fns.len) n->handler_fns.items[i] = fn;
        else vec_push(&n->handler_fns, fn);
        vec_push(&s->all_fns, fn);
    }
    vec_foreach(i, &n->children) app_declare_handlers(s, n->children.items[i], app_name, counter);
}

static void app_check_props(Sema *s, UiNode *n) {
    if (strcmp(n->kind, "state") == 0) return;
    vec_foreach(i, &n->prop_values) check_expr(s, n->prop_values.items[i]);
    vec_foreach(i, &n->children) app_check_props(s, n->children.items[i]);
}

static void app_check_handlers(Sema *s, UiNode *n) {
    vec_foreach(i, &n->handler_fns)
        if (n->handler_fns.items[i]) check_fn_body(s, n->handler_fns.items[i], NULL);
    vec_foreach(i, &n->children) app_check_handlers(s, n->children.items[i]);
}

static void app_check_state(Sema *s, UiNode *n) {
    if (strcmp(n->kind, "state") == 0) {
        if (!n->prop_values.len) return;
        const char *nm = n->prop_names.len ? n->prop_names.items[0] : n->label;
        Symbol *sym = nm ? sema_lookup(s, nm) : NULL;
        Type *vt = check_expr(s, n->prop_values.items[0]);
        if (sym && sym->type && sym->type->kind == TY_UNKNOWN) sym->type = vt;
        return;
    }
    vec_foreach(i, &n->children) app_check_state(s, n->children.items[i]);
}

/* Bring a type's own generic parameters into scope. */
static void push_type_generics(Sema *s, TypeDecl *td) {
    scope_push(s, s->cur_fn);
    vec_foreach(i, &td->generics) {
        GenericParam *g = &td->generics.items[i];
        Symbol *gs = sym_new(s, SYM_GENERIC, g->name, type_generic(s->tt, g->name, i), g->span);
        gs->used = true;
        vec_push(&s->scope->syms, gs);
    }
}

static void declare_members(Sema *s, Module *m) {
    s->module = m;
    s->scope = m->scope;
    TypeTable *tt = s->tt;

    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind != D_TYPE) continue;
        TypeDecl *td = d->as.type;
        if (td->kind == TD_ALIAS) continue;
        s->cur_type = td;
        push_type_generics(s, td);
        /* base class link stored in type->ret */
        if (td->base) {
            Symbol *b = sema_lookup(s, td->base);
            if (b && b->kind == SYM_TYPE && b->type && b->type->kind == TY_OBJECT) {
                td->type->ret = b->type;
                b->used = true;
            } else {
                Diag *dg = diag_new(s->db, DIAG_ERROR, "E0150", "Cannot extend `%s`", td->base);
                diag_label(s->db, dg, td->span, true, "only objects can be extended");
            }
        }
        vec_foreach(k, &td->traits) {
            Symbol *tr = sema_lookup(s, td->traits.items[k]);
            if (!tr || tr->kind != SYM_TYPE || !tr->type || tr->type->kind != TY_TRAIT) {
                Diag *dg = diag_new(s->db, DIAG_ERROR, "E0151", "`%s` is not a trait", td->traits.items[k]);
                diag_label(s->db, dg, td->span, true, "used as a trait here");
            } else tr->used = true;
        }
        vec_foreach(k, &td->fields) resolve_type(s, td->fields.items[k].type);
        vec_foreach(k, &td->variants) {
            vec_foreach(j, &td->variants.items[k].payload)
                resolve_type(s, td->variants.items[k].payload.items[j]);
        }
        vec_foreach(k, &td->methods) {
            FnDecl *fn = td->methods.items[k];
            fn->owner = td;
            /* Resolve the signature here, while the module that declares
               the type is the one in scope.  Otherwise it is resolved at
               the first call site instead, in whatever module that turns
               out to be, and a method that mentions its own type -- the
               usual `fn kids(self) -> [Node]` -- cannot be seen from
               another module. */
            scope_push(s, NULL);
            vec_foreach(g, &fn->generics) {
                GenericParam *gp = &fn->generics.items[g];
                Symbol *gs = sym_new(s, SYM_GENERIC, gp->name,
                                     type_generic(tt, gp->name, g), gp->span);
                gs->used = true;
                vec_push(&s->scope->syms, gs);
            }
            vec_foreach(j, &fn->params) {
                Param *p = &fn->params.items[j];
                if (!p->is_self && p->type) resolve_type(s, p->type);
            }
            if (fn->ret) resolve_type(s, fn->ret);
            scope_pop(s);
            vec_push(&s->all_fns, fn);
        }
        scope_pop(s);
        /* variants can be written bare:  Circle(2.0)  as well as  Shape.Circle(2.0) */
        if (td->kind == TD_ENUM) {
            vec_foreach(k, &td->variants) {
                VariantDecl *v = &td->variants.items[k];
                if (sema_lookup(s, v->name)) continue;       /* never shadow */
                Symbol *vs = declare(s, SYM_VARIANT, v->name, td->type, v->span);
                vs->td = td;
                vs->variant_tag = v->tag;
                vs->used = true;
            }
        }
        s->cur_type = NULL;
    }

    /* impl blocks attach methods to their target type */
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind != D_IMPL) continue;
        ImplDecl *im = d->as.impl;
        Type *target = resolve_type(s, im->target);
        if (!target || !target->decl) {
            Diag *dg = diag_new(s->db, DIAG_ERROR, "E0152", "`impl` needs a known type");
            diag_label(s->db, dg, im->span, true, "unknown target type");
            continue;
        }
        if (im->trait_name) {
            bool already = false;
            vec_foreach(k, &target->decl->traits)
                if (strcmp(target->decl->traits.items[k], im->trait_name) == 0) already = true;
            if (!already) vec_push(&target->decl->traits, im->trait_name);
            /* a trait's default methods become methods of the type unless the
               type (or this impl block) provides its own */
            Symbol *trsym = sema_lookup(s, im->trait_name);
            if (trsym && trsym->td) {
                vec_foreach(k, &trsym->td->methods) {
                    FnDecl *def = trsym->td->methods.items[k];
                    if (!def->body && !def->expr_body) continue;       /* abstract */
                    bool have = false;
                    vec_foreach(j, &target->decl->methods)
                        if (strcmp(target->decl->methods.items[j]->name, def->name) == 0) have = true;
                    vec_foreach(j, &im->methods)
                        if (strcmp(im->methods.items[j]->name, def->name) == 0) have = true;
                    if (!have) vec_push(&target->decl->methods, def);
                }
            }
        }
        vec_foreach(k, &im->methods) {
            FnDecl *fn = im->methods.items[k];
            fn->owner = target->decl;
            fn->is_method = true;
            vec_push(&target->decl->methods, fn);
            vec_push(&s->all_fns, fn);
        }
    }

    /* top level functions and constants */
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind == D_FN) {
            FnDecl *fn = d->as.fn;
            scope_push(s, s->cur_fn);
            vec_foreach(gi, &fn->generics) {
                GenericParam *g = &fn->generics.items[gi];
                Symbol *gs = sym_new(s, SYM_GENERIC, g->name, type_generic(tt, g->name, gi), g->span);
                gs->used = true;
                vec_push(&s->scope->syms, gs);
            }
            TypeVec ps; vec_init(&ps);
            vec_foreach(k, &fn->params)
                vec_push(&ps, fn->params.items[k].type ? resolve_type(s, fn->params.items[k].type) : tt->t_any);
            Type *ret = fn->ret ? resolve_type(s, fn->ret) : tt->t_nil;
            scope_pop(s);
            Type *ft = type_fn(tt, ps, ret);
            ft->fndecl = fn;
            ft->is_task = fn->is_task;
            fn->type = ft;
            if (fn->is_test || fn->is_bench) {
                /* a test is named by its description, not by an identifier,
                   so it never takes a name away from your code */
                vec_push(&s->all_fns, fn);
                continue;
            }
            Symbol *sym = declare(s, SYM_FN, fn->name, ft, fn->name_span);
            sym->fn = fn;
            sym->used = fn->is_pub || fn->is_test || fn->is_bench || strcmp(fn->name, "main") == 0;
            fn->sym = sym;
            vec_push(&s->all_fns, fn);
        } else if (d->kind == D_CONST || d->kind == D_LET) {
            Type *dt = d->as.konst.type ? resolve_type(s, d->as.konst.type) : NULL;
            Symbol *sym = declare(s, d->kind == D_CONST ? SYM_CONST : SYM_GLOBAL,
                                  d->as.konst.name, dt ? dt : tt->t_unknown, d->span);
            sym->is_mut = d->as.konst.mutable_;
            sym->value = d->as.konst.value;
            sym->slot = s->globals.len;
            sym->used = d->is_pub;
            d->as.konst.sym = sym;
            vec_push(&s->globals, sym);
            vec_push(&s->all_consts, d);
        }
    }

    /* app blocks */
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind != D_APP) continue;
        app_declare_state(s, d->as.app.root);
        int counter = 0;
        app_declare_handlers(s, d->as.app.root, d->as.app.name, &counter);
    }
}

/* ---- pass 4: check bodies ---- */
static void check_module_bodies(Sema *s, Module *m) {
    s->module = m;
    s->scope = m->scope;

    /* global initialisers */
    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind != D_CONST && d->kind != D_LET) continue;
        Type *vt = check_expr(s, d->as.konst.value);
        Symbol *sym = d->as.konst.sym;
        if (sym->type->kind == TY_UNKNOWN) sym->type = vt;
        else expect_type(s, d->as.konst.value->span, sym->type, vt, NULL);
    }

    vec_foreach(i, &m->decls) {
        Decl *d = m->decls.items[i];
        if (d->kind == D_FN) {
            check_fn_body(s, d->as.fn, NULL);
        } else if (d->kind == D_TYPE) {
            TypeDecl *td = d->as.type;
            if (td->kind == TD_ALIAS) continue;
            s->cur_type = td;
            push_type_generics(s, td);
            vec_foreach(k, &td->fields) {
                FieldDecl *f = &td->fields.items[k];
                if (f->deflt) {
                    Type *vt = check_expr(s, f->deflt);
                    Type *ft = f->type ? resolve_type(s, f->type) : vt;
                    if (f->type) expect_type(s, f->deflt->span, ft, vt, NULL);
                }
            }
            vec_foreach(k, &td->methods) {
                FnDecl *mth = td->methods.items[k];
                /* a default method borrowed from a trait is checked once,
                   with `self` typed as the trait */
                if (mth->owner && mth->owner != td) continue;
                check_fn_body(s, mth, td->type);
            }
            /* trait conformance */
            vec_foreach(k, &td->traits) {
                Symbol *tr = sema_lookup(s, td->traits.items[k]);
                if (!tr || !tr->td) continue;
                vec_foreach(j, &tr->td->methods) {
                    FnDecl *req = tr->td->methods.items[j];
                    if (req->body || req->expr_body) continue;   /* default method */
                    if (!find_method(td->type, req->name, NULL)) {
                        Diag *dg = diag_new(s->db, DIAG_ERROR, "E0160",
                                            "`%s` does not implement `%s` from trait `%s`",
                                            td->name, req->name, tr->td->name);
                        diag_label(s->db, dg, td->name_span, true, "missing method");
                        diag_label(s->db, dg, req->span, false, "required here");
                        StrBuf sig; sb_init(&sig);
                        sb_printf(&sig, "fn %s(self", req->name);
                        vec_foreach(q, &req->params)
                            if (!req->params.items[q].is_self)
                                sb_printf(&sig, ", %s: %s", req->params.items[q].name,
                                          typeexpr_to_text(s->arena, req->params.items[q].type));
                        sb_printf(&sig, ") -> %s", req->ret ? typeexpr_to_text(s->arena, req->ret) : "Nil");
                        diag_fix(s->db, dg, "add:  %s { ... }", sig.data);
                        sb_free(&sig);
                    }
                }
            }
            scope_pop(s);
            s->cur_type = NULL;
        } else if (d->kind == D_IMPL) {
            Type *target = resolve_type(s, d->as.impl->target);
            s->cur_type = target ? target->decl : NULL;
            vec_foreach(k, &d->as.impl->methods)
                check_fn_body(s, d->as.impl->methods.items[k], target);
            s->cur_type = NULL;
        } else if (d->kind == D_APP) {
            app_check_state(s, d->as.app.root);
            app_check_props(s, d->as.app.root);
            app_check_handlers(s, d->as.app.root);
        }
    }
    m->analyzed = true;
}

/* ====================================================================== */
/*  driver                                                                 */
/* ====================================================================== */

Sema *sema_new(Arena *a, Interner *in, DiagBag *db, TypeTable *tt) {
    Sema *s = NEW(a, Sema);
    s->arena = a; s->in = in; s->db = db; s->tt = tt;
    vec_init(&s->modules); vec_init(&s->all_fns); vec_init(&s->globals); vec_init(&s->all_consts);

    Scope *g = NEW(a, Scope);
    vec_init(&g->syms);
    g->is_module = true;
    s->global_scope = g;
    s->scope = g;

    /* built-in types */
    struct { const char *n; Type *t; } prims[] = {
        { "Int", tt->t_int }, { "Num", tt->t_num }, { "Text", tt->t_text },
        { "Bool", tt->t_bool }, { "Byte", tt->t_byte }, { "Nil", tt->t_nil },
        { "Any", tt->t_any },
    };
    for (size_t i = 0; i < sizeof(prims) / sizeof(prims[0]); i++) {
        Symbol *sym = sym_new(s, SYM_TYPE, internc(in, prims[i].n), prims[i].t, SPAN_NONE);
        sym->used = true;
        vec_push(&g->syms, sym);
    }
    /* built-in free functions */
    for (int i = 0; i < NF_COUNT; i++) {
        if (SPRFST_NATIVES[i].module[0] != 0) continue;
        TypeVec ps; vec_init(&ps);
        Type *ret = sema_parse_signature(s, SPRFST_NATIVES[i].sig, &ps);
        Type *ft = type_fn(tt, ps, ret);
        Symbol *sym = sym_new(s, SYM_FN, internc(in, SPRFST_NATIVES[i].name), ft, SPAN_NONE);
        sym->slot = i;          /* native id */
        sym->fn = NULL;
        sym->used = true;
        vec_push(&g->syms, sym);
    }
    return s;
}

void sema_add_module(Sema *s, Module *m) {
    Scope *sc = NEW(s->arena, Scope);
    vec_init(&sc->syms);
    sc->parent = s->global_scope;
    sc->is_module = true;
    m->scope = sc;
    vec_push(&s->modules, m);
}

bool sema_check(Sema *s) {
    vec_foreach(i, &s->modules) declare_types(s, s->modules.items[i]);
    vec_foreach(i, &s->modules) resolve_imports(s, s->modules.items[i]);
    vec_foreach(i, &s->modules) declare_members(s, s->modules.items[i]);
    vec_foreach(i, &s->modules) check_module_bodies(s, s->modules.items[i]);
    s->scope = s->global_scope;
    return !diagbag_has_errors(s->db);
}

/* ------------------------------------------------------- IDE completions */
static void comp_add(CompletionVec *out, const char *label, const char *detail,
                     const char *doc, const char *kind) {
    CompletionItem it = { label, detail, doc, kind };
    vec_push(out, it);
}

void sema_collect_completions(Sema *s, Module *m, int offset, CompletionVec *out) {
    static const char *kw[] = { "let", "var", "const", "fn", "give", "object", "data", "trait",
        "enum", "impl", "match", "when", "if", "else", "loop", "while", "for", "in", "break",
        "skip", "task", "spawn", "await", "chan", "try", "catch", "fail", "defer", "module",
        "use", "export", "pub", "test", "app", "self", "new", "true", "false", "nil", "and",
        "or", "not", "as", "is", "unsafe", "static", "init", "drop", "extends", "macro" };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
        comp_add(out, kw[i], "keyword", NULL, "keyword");

    int nmods = 0;
    const char **mods = natives_module_names(&nmods);
    for (int i = 0; i < nmods; i++)
        comp_add(out, mods[i], "standard module", NULL, "module");

    for (int i = 0; i < NF_COUNT; i++) {
        const NativeFn *nf = &SPRFST_NATIVES[i];
        if (nf->module[0] == '@') continue;
        char *label = nf->module[0] ? arena_vsprintf(s->arena, "%s.%s", nf->module, nf->name)
                                    : arena_strdup(s->arena, nf->name);
        comp_add(out, label, nf->sig, nf->doc, "function");
    }
    if (m && m->scope) {
        vec_foreach(i, &m->scope->syms) {
            Symbol *sym = m->scope->syms.items[i];
            const char *kind = sym->kind == SYM_FN ? "function" : sym->kind == SYM_TYPE ? "type" : "variable";
            comp_add(out, sym->name, type_text(s->arena, sym->type), NULL, kind);
        }
    }
}
