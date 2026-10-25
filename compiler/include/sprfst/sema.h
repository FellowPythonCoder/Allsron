#ifndef SPRFST_SEMA_H
#define SPRFST_SEMA_H

#include "sprfst/ast.h"
#include "sprfst/types.h"

typedef enum {
    SYM_VAR, SYM_PARAM, SYM_CAPTURE, SYM_GLOBAL, SYM_CONST,
    SYM_FN, SYM_TYPE, SYM_MODULE, SYM_NATIVE_MOD, SYM_VARIANT, SYM_GENERIC
} SymKind;

struct Symbol {
    SymKind     kind;
    const char *name;
    Type       *type;
    Span        span;
    bool        is_mut, used, assigned, moved, is_self, is_owned;
    int         slot;          /* local slot / global index / capture index */
    FnDecl     *fn;
    TypeDecl   *td;
    Module     *mod;
    const char *native_mod;
    Expr       *value;         /* const initialiser */
    int         variant_tag;
    Span        move_span;
    FnDecl     *owner_fn;
};

typedef struct Scope {
    struct Scope *parent;
    SymbolVec     syms;
    FnDecl       *fn;
    bool          is_loop;
    bool          is_unsafe;
    bool          is_module;
} Scope;

typedef struct {
    Arena      *arena;
    Interner   *in;
    DiagBag    *db;
    TypeTable  *tt;
    Scope      *scope;
    Scope      *global_scope;
    Module     *module;
    ModuleVec   modules;
    FnDecl     *cur_fn;
    Type       *cur_ret;
    TypeDecl   *cur_type;
    int         loop_depth, unsafe_depth, task_depth;
    FnVec       all_fns;       /* every function, in code generation order  */
    SymbolVec   globals;       /* module level variables and constants      */
    DeclVec     all_consts;
    bool        in_lambda;
    int         anon_counter;
    bool        strict_unused;
    Type       *expected_fn;   /* contextual type for the lambda being checked */
} Sema;

Sema *sema_new(Arena *a, Interner *in, DiagBag *db, TypeTable *tt);
void  sema_add_module(Sema *s, Module *m);
bool  sema_check(Sema *s);
Type *sema_parse_signature(Sema *s, const char *sig, TypeVec *params_out);
Symbol *sema_lookup(Sema *s, const char *name);

/* helpers used by the IDE services (completion / hover) */
typedef struct {
    const char *label;
    const char *detail;
    const char *doc;
    const char *kind;
} CompletionItem;
typedef struct { CompletionItem *items; int len, cap; } CompletionVec;
void sema_collect_completions(Sema *s, Module *m, int offset, CompletionVec *out);

#endif
