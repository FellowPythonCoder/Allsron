/* ========================================================================
   SPRFST — semantic type system
   ======================================================================== */
#ifndef SPRFST_TYPES_H
#define SPRFST_TYPES_H

#include "sprfst/ast.h"

typedef enum {
    TY_UNKNOWN,   /* not yet inferred            */
    TY_ERROR,     /* an error was already reported here */
    TY_NIL,
    TY_BOOL,
    TY_INT,
    TY_NUM,
    TY_BYTE,
    TY_TEXT,
    TY_ANY,
    TY_LIST,
    TY_MAP,
    TY_SET,
    TY_MAYBE,
    TY_RESULT,
    TY_FN,
    TY_DATA,
    TY_OBJECT,
    TY_TRAIT,
    TY_ENUM,
    TY_CHAN,
    TY_FUTURE,
    TY_REF,
    TY_RANGE,
    TY_GENERIC,
    TY_MODULE,
    TY_TYPEREF    /* the type itself used as a value (Color.Red, Point.new) */
} TypeKind;

typedef struct { Type **items; int len, cap; } TypeVec;

struct Type {
    TypeKind    kind;
    const char *name;
    Type       *elem;      /* list/maybe/chan/future/ref/set element */
    Type       *key;       /* map key */
    Type       *err;       /* result error type */
    TypeVec     params;    /* fn parameter types, generic arguments */
    Type       *ret;       /* fn return type */
    TypeDecl   *decl;      /* data / object / trait / enum declaration */
    FnDecl     *fndecl;    /* for fn types that came from a declaration */
    struct Module *module; /* TY_MODULE */
    int         native_mod;/* native module id, -1 otherwise */
    bool        mut;       /* ref mut */
    bool        is_task;   /* async fn */
    int         generic_id;
    int         size;      /* bytes, for the native backend */
};

typedef struct {
    Arena *arena;
    Type  *t_nil, *t_bool, *t_int, *t_num, *t_byte, *t_text, *t_any,
          *t_unknown, *t_error, *t_never;
    TypeVec all;
} TypeTable;

TypeTable *types_new(Arena *a);
Type *type_new(TypeTable *tt, TypeKind k);
Type *type_list(TypeTable *tt, Type *elem);
Type *type_set(TypeTable *tt, Type *elem);
Type *type_map(TypeTable *tt, Type *k, Type *v);
Type *type_maybe(TypeTable *tt, Type *inner);
Type *type_result(TypeTable *tt, Type *ok, Type *err);
Type *type_chan(TypeTable *tt, Type *inner);
Type *type_future(TypeTable *tt, Type *inner);
Type *type_ref(TypeTable *tt, Type *inner, bool mut);
Type *type_range(TypeTable *tt, Type *inner);
Type *type_fn(TypeTable *tt, TypeVec params, Type *ret);
Type *type_generic(TypeTable *tt, const char *name, int id);

const char *type_text(Arena *a, Type *t);        /* "List<Text>" */
bool  type_same(Type *a, Type *b);
bool  type_assignable(Type *to, Type *from);     /* from -> to without a cast */
bool  type_castable(Type *to, Type *from);       /* with `as` */
bool  type_numeric(Type *t);
bool  type_hashable(Type *t);
bool  type_truthy(Type *t);
Type *type_unwrap_maybe(Type *t);
Type *type_common(TypeTable *tt, Type *a, Type *b);  /* join for if/match */
bool  type_implements(Type *t, const char *trait_name);
bool  type_is_ref_like(Type *t);  /* heap managed */

/* generic binding environment used while checking a generic call */
typedef struct {
    const char *names[16];
    Type       *bound[16];
    int         n;
} GenericEnv;

bool type_unify(TypeTable *tt, Type *pattern, Type *actual, GenericEnv *env);
Type *type_substitute(TypeTable *tt, Type *t, GenericEnv *env);

#endif
