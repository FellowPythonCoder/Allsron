#include "sprfst/types.h"

static Type *prim(TypeTable *tt, TypeKind k, const char *name, int size) {
    Type *t = NEW(tt->arena, Type);
    t->kind = k; t->name = name; t->size = size; t->native_mod = -1;
    vec_init(&t->params);
    vec_push(&tt->all, t);
    return t;
}

TypeTable *types_new(Arena *a) {
    TypeTable *tt = NEW(a, TypeTable);
    tt->arena = a;
    vec_init(&tt->all);
    tt->t_nil     = prim(tt, TY_NIL, "Nil", 0);
    tt->t_bool    = prim(tt, TY_BOOL, "Bool", 1);
    tt->t_int     = prim(tt, TY_INT, "Int", 8);
    tt->t_num     = prim(tt, TY_NUM, "Num", 8);
    tt->t_byte    = prim(tt, TY_BYTE, "Byte", 1);
    tt->t_text    = prim(tt, TY_TEXT, "Text", 8);
    tt->t_any     = prim(tt, TY_ANY, "Any", 16);
    tt->t_unknown = prim(tt, TY_UNKNOWN, "_", 0);
    tt->t_error   = prim(tt, TY_ERROR, "<error>", 0);
    tt->t_never   = prim(tt, TY_NIL, "Never", 0);
    return tt;
}

Type *type_new(TypeTable *tt, TypeKind k) {
    Type *t = NEW(tt->arena, Type);
    t->kind = k;
    t->native_mod = -1;
    t->size = 8;
    vec_init(&t->params);
    vec_push(&tt->all, t);
    return t;
}

Type *type_list(TypeTable *tt, Type *elem)  { Type *t = type_new(tt, TY_LIST); t->elem = elem; t->name = "List"; return t; }
Type *type_set(TypeTable *tt, Type *elem)   { Type *t = type_new(tt, TY_SET); t->elem = elem; t->name = "Set"; return t; }
Type *type_map(TypeTable *tt, Type *k, Type *v) { Type *t = type_new(tt, TY_MAP); t->key = k; t->elem = v; t->name = "Map"; return t; }
Type *type_maybe(TypeTable *tt, Type *inner) {
    if (inner && inner->kind == TY_MAYBE) return inner;
    Type *t = type_new(tt, TY_MAYBE); t->elem = inner; t->name = "Maybe"; return t;
}
Type *type_result(TypeTable *tt, Type *ok, Type *err) {
    Type *t = type_new(tt, TY_RESULT); t->elem = ok; t->err = err; t->name = "Result"; return t;
}
Type *type_chan(TypeTable *tt, Type *inner)  { Type *t = type_new(tt, TY_CHAN); t->elem = inner; t->name = "Chan"; return t; }
Type *type_future(TypeTable *tt, Type *inner){ Type *t = type_new(tt, TY_FUTURE); t->elem = inner; t->name = "Future"; return t; }
Type *type_ref(TypeTable *tt, Type *inner, bool mut) { Type *t = type_new(tt, TY_REF); t->elem = inner; t->mut = mut; t->name = "Ref"; return t; }
Type *type_range(TypeTable *tt, Type *inner) { Type *t = type_new(tt, TY_RANGE); t->elem = inner; t->name = "Range"; return t; }

Type *type_fn(TypeTable *tt, TypeVec params, Type *ret) {
    Type *t = type_new(tt, TY_FN);
    t->params = params; t->ret = ret; t->name = "fn";
    return t;
}

Type *type_generic(TypeTable *tt, const char *name, int id) {
    Type *t = type_new(tt, TY_GENERIC);
    t->name = name; t->generic_id = id;
    return t;
}

const char *type_text(Arena *a, Type *t) {
    if (!t) return "?";
    switch (t->kind) {
        case TY_LIST:   return arena_vsprintf(a, "List<%s>", type_text(a, t->elem));
        case TY_SET:    return arena_vsprintf(a, "Set<%s>", type_text(a, t->elem));
        case TY_MAP:    return arena_vsprintf(a, "Map<%s, %s>", type_text(a, t->key), type_text(a, t->elem));
        case TY_MAYBE:  return arena_vsprintf(a, "%s?", type_text(a, t->elem));
        case TY_RESULT: return arena_vsprintf(a, "Result<%s, %s>", type_text(a, t->elem), type_text(a, t->err));
        case TY_CHAN:   return arena_vsprintf(a, "chan<%s>", type_text(a, t->elem));
        case TY_FUTURE: return arena_vsprintf(a, "Future<%s>", type_text(a, t->elem));
        case TY_RANGE:  return arena_vsprintf(a, "Range<%s>", type_text(a, t->elem));
        case TY_REF:    return arena_vsprintf(a, "%sref %s", t->mut ? "mut " : "", type_text(a, t->elem));
        case TY_MODULE: return arena_vsprintf(a, "module %s", t->name ? t->name : "?");
        case TY_TYPEREF:return arena_vsprintf(a, "type %s", t->name ? t->name : "?");
        case TY_FN: {
            StrBuf b; sb_init(&b);
            sb_puts(&b, "fn(");
            vec_foreach(i, &t->params) { if (i) sb_puts(&b, ", "); sb_puts(&b, type_text(a, t->params.items[i])); }
            sb_puts(&b, ")");
            if (t->ret && t->ret->kind != TY_NIL) { sb_puts(&b, " -> "); sb_puts(&b, type_text(a, t->ret)); }
            char *s = arena_strdup(a, b.data ? b.data : "fn()");
            sb_free(&b);
            return s;
        }
        default: {
            if (t->params.len && t->name) {
                StrBuf b; sb_init(&b);
                sb_puts(&b, t->name);
                sb_puts(&b, "<");
                vec_foreach(i, &t->params) { if (i) sb_puts(&b, ", "); sb_puts(&b, type_text(a, t->params.items[i])); }
                sb_puts(&b, ">");
                char *s = arena_strdup(a, b.data);
                sb_free(&b);
                return s;
            }
            return t->name ? t->name : "?";
        }
    }
}

bool type_numeric(Type *t) { return t && (t->kind == TY_INT || t->kind == TY_NUM || t->kind == TY_BYTE); }
bool type_hashable(Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_INT: case TY_TEXT: case TY_BOOL: case TY_BYTE: case TY_NUM: case TY_ENUM: case TY_ANY: return true;
        default: return false;
    }
}
bool type_truthy(Type *t) { return t && (t->kind == TY_BOOL || t->kind == TY_MAYBE || t->kind == TY_ANY); }

Type *type_unwrap_maybe(Type *t) { return (t && t->kind == TY_MAYBE) ? t->elem : t; }

bool type_is_ref_like(Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TEXT: case TY_LIST: case TY_MAP: case TY_SET: case TY_OBJECT:
        case TY_DATA: case TY_FN: case TY_CHAN: case TY_FUTURE: case TY_ANY:
        case TY_ENUM: case TY_RESULT: case TY_MAYBE:
            return true;
        default: return false;
    }
}

bool type_same(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_LIST: case TY_SET: case TY_MAYBE: case TY_CHAN: case TY_FUTURE: case TY_RANGE:
            return type_same(a->elem, b->elem);
        case TY_REF:
            return a->mut == b->mut && type_same(a->elem, b->elem);
        case TY_MAP:
            return type_same(a->key, b->key) && type_same(a->elem, b->elem);
        case TY_RESULT:
            return type_same(a->elem, b->elem) && type_same(a->err, b->err);
        case TY_FN: {
            if (a->params.len != b->params.len) return false;
            vec_foreach(i, &a->params) if (!type_same(a->params.items[i], b->params.items[i])) return false;
            return type_same(a->ret, b->ret);
        }
        case TY_DATA: case TY_OBJECT: case TY_TRAIT: case TY_ENUM:
            if (a->decl != b->decl) return false;
            if (a->params.len != b->params.len) return true;  /* erased generics */
            vec_foreach(i, &a->params) if (!type_same(a->params.items[i], b->params.items[i])) return false;
            return true;
        case TY_GENERIC:
            return a->name == b->name || (a->name && b->name && strcmp(a->name, b->name) == 0);
        default:
            return true;
    }
}

bool type_implements(Type *t, const char *trait_name) {
    if (!t || !trait_name) return false;
    if (t->kind == TY_TRAIT && t->decl && strcmp(t->decl->name, trait_name) == 0) return true;
    TypeDecl *d = t->decl;
    int guard = 0;
    while (d && guard++ < 32) {
        vec_foreach(i, &d->traits)
            if (strcmp(d->traits.items[i], trait_name) == 0) return true;
        if (!d->base) break;
        /* base chain is resolved by sema into decl->type->... ; approximate here */
        break;
    }
    /* built-in trait-ish names understood by the compiler */
    if (strcmp(trait_name, "Any") == 0) return true;
    if (strcmp(trait_name, "Ord") == 0 || strcmp(trait_name, "Eq") == 0)
        return type_numeric(t) || t->kind == TY_TEXT || t->kind == TY_BOOL;
    if (strcmp(trait_name, "Show") == 0) return true;
    if (strcmp(trait_name, "Hash") == 0) return type_hashable(t);
    if (strcmp(trait_name, "Num") == 0) return type_numeric(t);
    return false;
}

static bool object_derives(Type *sub, Type *base) {
    if (!sub || !base) return false;
    TypeDecl *d = sub->decl;
    int guard = 0;
    while (d && guard++ < 64) {
        if (d == base->decl) return true;
        if (!d->type || !d->type->ret) break;   /* ret used as "base type" link */
        Type *b = d->type->ret;
        if (b->decl == base->decl) return true;
        d = b->decl;
    }
    return false;
}

bool type_assignable(Type *to, Type *from) {
    if (!to || !from) return true;
    if (to->kind == TY_ERROR || from->kind == TY_ERROR) return true;
    if (to->kind == TY_UNKNOWN || from->kind == TY_UNKNOWN) return true;
    if (type_same(to, from)) return true;
    if (to->kind == TY_ANY) return true;
    if (from->kind == TY_ANY) return true;          /* Any flows both ways (checked at runtime) */

    /* Int -> Num widening, Byte -> Int */
    if (to->kind == TY_NUM && (from->kind == TY_INT || from->kind == TY_BYTE)) return true;
    if (to->kind == TY_INT && from->kind == TY_BYTE) return true;

    /* T -> T? and nil -> T? */
    if (to->kind == TY_MAYBE) {
        if (from->kind == TY_NIL) return true;
        if (from->kind == TY_MAYBE) return type_assignable(to->elem, from->elem);
        return type_assignable(to->elem, from);
    }
    if (from->kind == TY_NIL && (to->kind == TY_LIST || to->kind == TY_MAP || to->kind == TY_SET ||
                                 to->kind == TY_TEXT || to->kind == TY_OBJECT))
        return false;

    /* Result covariance */
    if (to->kind == TY_RESULT && from->kind == TY_RESULT)
        return type_assignable(to->elem, from->elem) && type_assignable(to->err, from->err);

    /* collections are covariant in SPRFST when the source is a literal of
       unknown element type (empty list), otherwise invariant */
    if (to->kind == TY_LIST && from->kind == TY_LIST)
        return from->elem == NULL || from->elem->kind == TY_UNKNOWN || type_assignable(to->elem, from->elem);
    if (to->kind == TY_SET && from->kind == TY_SET)
        return from->elem == NULL || from->elem->kind == TY_UNKNOWN || type_assignable(to->elem, from->elem);
    if (to->kind == TY_MAP && from->kind == TY_MAP)
        return (from->key == NULL || from->key->kind == TY_UNKNOWN || type_assignable(to->key, from->key)) &&
               (from->elem == NULL || from->elem->kind == TY_UNKNOWN || type_assignable(to->elem, from->elem));

    /* object inheritance + trait conformance */
    if (to->kind == TY_TRAIT && to->decl) return type_implements(from, to->decl->name);
    if (to->kind == TY_OBJECT && from->kind == TY_OBJECT) return object_derives(from, to);

    /* functions: contravariant params, covariant return (kept simple: same arity + assignable) */
    if (to->kind == TY_FN && from->kind == TY_FN) {
        if (to->params.len != from->params.len) return false;
        vec_foreach(i, &to->params)
            if (!type_assignable(from->params.items[i], to->params.items[i])) return false;
        return type_assignable(to->ret, from->ret);
    }
    if (to->kind == TY_GENERIC || from->kind == TY_GENERIC) return true;
    if (to->kind == TY_FUTURE && from->kind == TY_FUTURE) return type_assignable(to->elem, from->elem);
    if (to->kind == TY_CHAN && from->kind == TY_CHAN) return type_assignable(to->elem, from->elem);
    if (to->kind == TY_REF) return type_assignable(to->elem, from->kind == TY_REF ? from->elem : from);
    if (from->kind == TY_REF) return type_assignable(to, from->elem);
    return false;
}

bool type_castable(Type *to, Type *from) {
    if (type_assignable(to, from)) return true;
    if (type_numeric(to) && type_numeric(from)) return true;
    if (to->kind == TY_TEXT) return true;                 /* anything -> Text via as Text */
    if (from->kind == TY_TEXT && type_numeric(to)) return true;
    if (to->kind == TY_ANY || from->kind == TY_ANY) return true;
    if (to->kind == TY_OBJECT && from->kind == TY_OBJECT) return true; /* downcast, checked */
    if (to->kind == TY_BOOL) return true;
    return false;
}

Type *type_common(TypeTable *tt, Type *a, Type *b) {
    if (!a) return b;
    if (!b) return a;
    if (type_same(a, b)) return a;
    if (a->kind == TY_ERROR || b->kind == TY_ERROR) return tt->t_error;
    if (a->kind == TY_NIL) return type_maybe(tt, b);
    if (b->kind == TY_NIL) return type_maybe(tt, a);
    if (type_assignable(a, b)) return a;
    if (type_assignable(b, a)) return b;
    if (type_numeric(a) && type_numeric(b)) return tt->t_num;
    if (a->kind == TY_MAYBE || b->kind == TY_MAYBE) {
        Type *ia = type_unwrap_maybe(a), *ib = type_unwrap_maybe(b);
        return type_maybe(tt, type_common(tt, ia, ib));
    }
    return tt->t_any;
}

/* ------------------------------------------------------------- generics */
static Type **env_slot(GenericEnv *env, const char *name) {
    for (int i = 0; i < env->n; i++)
        if (env->names[i] == name || (env->names[i] && name && strcmp(env->names[i], name) == 0))
            return &env->bound[i];
    if (env->n >= 16) return NULL;
    env->names[env->n] = name;
    env->bound[env->n] = NULL;
    return &env->bound[env->n++];
}

bool type_unify(TypeTable *tt, Type *pattern, Type *actual, GenericEnv *env) {
    if (!pattern || !actual) return true;
    if (pattern->kind == TY_GENERIC) {
        Type **slot = env_slot(env, pattern->name);
        if (!slot) return true;
        if (*slot == NULL) { *slot = actual; return true; }
        if (type_assignable(*slot, actual)) return true;
        if (type_assignable(actual, *slot)) { *slot = actual; return true; }
        *slot = type_common(tt, *slot, actual);
        return true;
    }
    if (pattern->kind != actual->kind) {
        if (pattern->kind == TY_MAYBE) return type_unify(tt, pattern->elem, actual, env);
        return type_assignable(pattern, actual);
    }
    switch (pattern->kind) {
        case TY_LIST: case TY_SET: case TY_MAYBE: case TY_CHAN: case TY_FUTURE: case TY_RANGE:
            return type_unify(tt, pattern->elem, actual->elem, env);
        case TY_MAP:
            return type_unify(tt, pattern->key, actual->key, env) &&
                   type_unify(tt, pattern->elem, actual->elem, env);
        case TY_RESULT:
            return type_unify(tt, pattern->elem, actual->elem, env) &&
                   type_unify(tt, pattern->err, actual->err, env);
        case TY_FN: {
            int n = pattern->params.len < actual->params.len ? pattern->params.len : actual->params.len;
            for (int i = 0; i < n; i++)
                type_unify(tt, pattern->params.items[i], actual->params.items[i], env);
            return type_unify(tt, pattern->ret, actual->ret, env);
        }
        case TY_DATA: case TY_OBJECT: case TY_ENUM: {
            int n = pattern->params.len < actual->params.len ? pattern->params.len : actual->params.len;
            for (int i = 0; i < n; i++)
                type_unify(tt, pattern->params.items[i], actual->params.items[i], env);
            return true;
        }
        default: return true;
    }
}

Type *type_substitute(TypeTable *tt, Type *t, GenericEnv *env) {
    if (!t) return t;
    switch (t->kind) {
        case TY_GENERIC: {
            for (int i = 0; i < env->n; i++)
                if (env->names[i] == t->name || (env->names[i] && t->name && strcmp(env->names[i], t->name) == 0))
                    return env->bound[i] ? env->bound[i] : t;
            return t;
        }
        case TY_LIST:   { Type *e = type_substitute(tt, t->elem, env); return e == t->elem ? t : type_list(tt, e); }
        case TY_SET:    { Type *e = type_substitute(tt, t->elem, env); return e == t->elem ? t : type_set(tt, e); }
        case TY_MAYBE:  { Type *e = type_substitute(tt, t->elem, env); return e == t->elem ? t : type_maybe(tt, e); }
        case TY_CHAN:   { Type *e = type_substitute(tt, t->elem, env); return e == t->elem ? t : type_chan(tt, e); }
        case TY_FUTURE: { Type *e = type_substitute(tt, t->elem, env); return e == t->elem ? t : type_future(tt, e); }
        case TY_RANGE:  { Type *e = type_substitute(tt, t->elem, env); return e == t->elem ? t : type_range(tt, e); }
        case TY_MAP: {
            Type *k = type_substitute(tt, t->key, env), *v = type_substitute(tt, t->elem, env);
            return (k == t->key && v == t->elem) ? t : type_map(tt, k, v);
        }
        case TY_RESULT: {
            Type *o = type_substitute(tt, t->elem, env), *e = type_substitute(tt, t->err, env);
            return (o == t->elem && e == t->err) ? t : type_result(tt, o, e);
        }
        case TY_FN: {
            TypeVec ps; vec_init(&ps);
            bool changed = false;
            vec_foreach(i, &t->params) {
                Type *p = type_substitute(tt, t->params.items[i], env);
                if (p != t->params.items[i]) changed = true;
                vec_push(&ps, p);
            }
            Type *r = type_substitute(tt, t->ret, env);
            if (!changed && r == t->ret) { vec_free(&ps); return t; }
            Type *n = type_fn(tt, ps, r);
            n->fndecl = t->fndecl;
            n->is_task = t->is_task;
            return n;
        }
        case TY_DATA: case TY_OBJECT: case TY_ENUM: {
            if (!t->params.len) return t;
            TypeVec ps; vec_init(&ps);
            bool changed = false;
            vec_foreach(i, &t->params) {
                Type *p = type_substitute(tt, t->params.items[i], env);
                if (p != t->params.items[i]) changed = true;
                vec_push(&ps, p);
            }
            if (!changed) { vec_free(&ps); return t; }
            Type *n = type_new(tt, t->kind);
            *n = *t;
            n->params = ps;
            return n;
        }
        default: return t;
    }
}
