/* ========================================================================
   SPRFST — abstract syntax tree
   ======================================================================== */
#ifndef SPRFST_AST_H
#define SPRFST_AST_H

#include "sprfst/common.h"
#include "sprfst/lexer.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
typedef struct TypeExpr TypeExpr;
typedef struct Pattern Pattern;
typedef struct Block Block;
typedef struct Type Type;      /* semantic type, see types.h */
typedef struct Symbol Symbol;  /* resolved symbol, see sema.h */

/* named vector types (vectors that cross function boundaries need real names) */
typedef struct { Expr **items; int len, cap; } ExprVec;
typedef struct { const char **items; int len, cap; } NameVec;
typedef struct { TypeExpr **items; int len, cap; } TypeExprVec;
typedef struct { Pattern **items; int len, cap; } PatternVec;
typedef struct { Symbol **items; int len, cap; } SymbolVec;
typedef struct { Stmt **items; int len, cap; } StmtVec;
typedef struct { Block **items; int len, cap; } BlockVec;
typedef struct { struct FnDecl **items; int len, cap; } FnVec;
typedef struct { struct UiNode **items; int len, cap; } UiNodeVec;
typedef struct { struct Decl **items; int len, cap; } DeclVec;
typedef struct { struct Module **items; int len, cap; } ModuleVec;

/* ------------------------------------------------------------- type expr */
typedef enum {
    TE_NAME,     /* Int, List<Text>, geo.Point      */
    TE_MAYBE,    /* T?                              */
    TE_LIST,     /* [T]                             */
    TE_MAP,      /* [K: V]                          */
    TE_FN,       /* fn(Int, Text) -> Bool           */
    TE_REF,      /* ref T / mut ref T               */
    TE_OWN,      /* own T                           */
    TE_WEAK,     /* weak T                          */
    TE_CHAN,     /* chan<T>                         */
    TE_SELF,     /* Self                            */
    TE_INFER     /* _ (let the compiler decide)     */
} TypeExprKind;

struct TypeExpr {
    TypeExprKind kind;
    Span         span;
    const char  *name;          /* TE_NAME: last segment  */
    NameVec      path;          /* TE_NAME: module path   */
    TypeExprVec  args;          /* generic args / fn params */
    TypeExpr    *inner;         /* TE_MAYBE/LIST/REF/OWN/WEAK/CHAN element */
    TypeExpr    *key;           /* TE_MAP key */
    TypeExpr    *ret;           /* TE_FN result */
    bool         mut;           /* ref mut */
    Type        *resolved;
};

/* --------------------------------------------------------------- pattern */
typedef enum {
    PAT_WILDCARD,   /* _            */
    PAT_BIND,       /* name         */
    PAT_LITERAL,    /* 1, "x", true */
    PAT_VARIANT,    /* Circle(r) / Color.Red */
    PAT_DATA,       /* Point { x, y } */
    PAT_LIST,       /* [a, b, ..rest] */
    PAT_RANGE,      /* 1..10        */
    PAT_TYPE        /* is Text      */
} PatternKind;

struct Pattern {
    PatternKind kind;
    Span        span;
    const char *name;             /* bind name / variant name */
    NameVec     path;             /* qualified variant */
    PatternVec  subs;
    NameVec     field_names;      /* PAT_DATA */
    Expr       *lit;              /* PAT_LITERAL */
    Expr       *lo, *hi;          /* PAT_RANGE */
    TypeExpr   *type;             /* PAT_TYPE */
    const char *rest;             /* PAT_LIST ..rest binder */
    Symbol     *sym;              /* bound symbol */
    Type       *vtype;
};

/* ------------------------------------------------------------ expression */
typedef enum {
    EX_INT, EX_NUM, EX_TEXT, EX_BYTE, EX_BOOL, EX_NIL,
    EX_IDENT, EX_SELF, EX_PATH,
    EX_UNARY, EX_BINARY, EX_LOGICAL, EX_ASSIGN,
    EX_CALL, EX_METHOD, EX_FIELD, EX_INDEX,
    EX_LIST, EX_MAP, EX_STRUCT, EX_LAMBDA,
    EX_IF, EX_MATCH, EX_BLOCK,
    EX_RANGE, EX_INTERP, EX_CAST, EX_IS,
    EX_TRY, EX_FORCE, EX_AWAIT, EX_SPAWN, EX_COALESCE, EX_OPTCHAIN,
    EX_NEW, EX_COMPTIME, EX_MACROCALL
} ExprKind;

typedef struct { const char *name; Expr *value; Span span; } FieldInit;
typedef struct { FieldInit *items; int len, cap; } FieldInitVec;
typedef struct { Pattern *pat; Expr *guard; Expr *body; Block *bbody; Span span; } MatchArm;
typedef struct { MatchArm *items; int len, cap; } MatchArmVec;

struct Expr {
    ExprKind kind;
    Span     span;
    Type    *type;         /* filled by the type checker */
    bool     is_lvalue;
    bool     comptime_known;

    union {
        int64_t ival;
        double  nval;
        bool    bval;
        struct { const char *text; ExprVec parts; NameVec chunks; } str;
        struct { const char *name; Symbol *sym; } ident;
        struct { NameVec segs; Symbol *sym; } path;
        struct { TokKind op; Expr *operand; } unary;
        struct { TokKind op; Expr *lhs, *rhs; } binary;
        struct { TokKind op; Expr *target, *value; } assign;
        struct { Expr *callee; ExprVec args; NameVec names; } call;
        struct { Expr *recv; const char *name; ExprVec args; Span name_span;
                 struct FnDecl *resolved; int builtin; bool optional; } method;
        struct { Expr *obj; const char *name; Span name_span; int field_index; bool optional; } field;
        struct { Expr *obj, *index; } index;
        struct { ExprVec items; } list;
        struct { ExprVec keys; ExprVec vals; } map;
        struct { TypeExpr *type; FieldInitVec fields; } strct;
        struct { struct FnDecl *fn; SymbolVec captures; } lambda;
        struct { Expr *cond; Block *then_b; Block *else_b; Expr *else_e; } iff;
        struct { Expr *subject; MatchArmVec arms; } match;
        struct { Block *block; } block;
        struct { Expr *lo, *hi; bool inclusive; } range;
        struct { Expr *value; TypeExpr *type; } cast;
        struct { Expr *value; TypeExpr *type; } is;
        struct { Expr *value; } wrap;      /* try / force / await / spawn / comptime */
        struct { Expr *value, *fallback; } coalesce;
        struct { TypeExpr *type; ExprVec args; } nw;
        struct { const char *name; ExprVec args; } macrocall;
    } as;
};

/* ------------------------------------------------------------- statement */
typedef enum {
    ST_EXPR, ST_LET, ST_ASSIGN, ST_GIVE, ST_IF, ST_WHILE, ST_LOOP, ST_FOR,
    ST_MATCH, ST_BREAK, ST_SKIP, ST_BLOCK, ST_DEFER, ST_FAIL, ST_TRY,
    ST_UNSAFE, ST_DECL
} StmtKind;

struct Stmt {
    StmtKind kind;
    Span     span;
    union {
        Expr *expr;
        struct { bool mutable_, is_const; Pattern *pat; const char *name; TypeExpr *type;
                 Expr *init; Symbol *sym; } let;
        struct { Expr *value; } give;
        struct { Expr *cond; Block *then_b; Block *else_b; Stmt *else_if; } iff;
        struct { Expr *cond; Block *body; const char *label; } whil;
        struct { Block *body; const char *label; } loop;
        struct { const char *var; Pattern *pat; Expr *iter; Block *body;
                 Symbol *sym; const char *label; bool parallel; } forr;
        struct { Expr *subject; MatchArmVec arms; } match;
        struct { const char *label; Expr *value; } brk;
        struct { Block *block; } block;
        struct { Block *body; const char *err_name; Block *handler; Symbol *err_sym; } tryc;
        struct { Expr *value; } fail;
        Decl *decl;
    } as;
};

struct Block {
    StmtVec stmts;
    Span        span;
    struct Scope *scope;
};

/* ----------------------------------------------------------- declarations */
typedef struct { const char *name; TypeExpr *type; Expr *deflt; Span span; Symbol *sym;
                 bool is_self; bool mut; } Param;
typedef struct { Param *items; int len, cap; } ParamVec;
typedef struct { const char *name; NameVec bounds; Span span; } GenericParam;
typedef struct { GenericParam *items; int len, cap; } GenericVec;
typedef struct { const char *name; const char *value; Span span; } Attr;
typedef struct { Attr *items; int len, cap; } AttrVec;

typedef struct FnDecl {
    const char *name;
    Span        span, name_span;
    GenericVec  generics;
    ParamVec    params;
    TypeExpr   *ret;
    Block      *body;
    Expr       *expr_body;     /* => form */
    bool        is_task;       /* async */
    bool        is_pub, is_static, is_extern, is_init, is_drop, is_test, is_bench;
    bool        is_lambda;
    bool        is_method;
    const char *doc;
    AttrVec     attrs;
    const char *extern_name;
    struct TypeDecl *owner;    /* enclosing object/data, if any */
    /* filled in later */
    Type       *type;
    Symbol     *sym;
    struct Scope *scope;
    int         ir_index;      /* index in the compiled function table */
    int         local_count;
    NameVec     local_names;   /* slot -> source name, for the debugger */
} FnDecl;

typedef struct { const char *name; TypeExpr *type; Expr *deflt; Span span;
                 bool is_pub, is_static, is_mut, is_has; const char *doc; int index; } FieldDecl;
typedef struct { FieldDecl *items; int len, cap; } FieldVec;

typedef struct { const char *name; TypeExprVec payload; Span span; const char *doc;
                 int tag; Expr *value; } VariantDecl;
typedef struct { VariantDecl *items; int len, cap; } VariantVec;

typedef enum { TD_DATA, TD_OBJECT, TD_TRAIT, TD_ENUM, TD_ALIAS } TypeDeclKind;

typedef struct TypeDecl {
    TypeDeclKind kind;
    const char  *name;
    Span         span, name_span;
    GenericVec   generics;
    FieldVec     fields;
    VariantVec   variants;
    FnVec        methods;
    NameVec      traits;           /* implemented trait names (object) */
    const char       *base;        /* extends */
    TypeExpr         *aliased;
    bool              is_pub;
    const char       *doc;
    AttrVec           attrs;
    Type             *type;
    Symbol           *sym;
} TypeDecl;

typedef struct {
    const char *trait_name;
    TypeExpr   *target;
    FnVec       methods;
    Span        span;
} ImplDecl;

typedef enum {
    D_MODULE, D_USE, D_FN, D_TYPE, D_IMPL, D_CONST, D_LET, D_MACRO, D_APP, D_EXTERN
} DeclKind;

typedef struct {
    const char *name;
    NameVec     params;
    Block      *body;
    Expr       *expr;
    Span        span;
} MacroDecl;

/* `app "Name" { window { ... } }` — declarative UI root */
typedef struct UiNode {
    const char *kind;           /* window / column / button / text ... */
    const char *label;          /* positional text argument */
    Span        span;
    NameVec     prop_names;
    ExprVec     prop_values;
    UiNodeVec   children;
    NameVec     handler_names;   /* on click { ... } */
    NameVec     handler_params;  /* on change(text) { ... }, NULL when absent */
    BlockVec    handler_bodies;
    FnVec       handler_fns;
} UiNode;

struct Decl {
    DeclKind kind;
    Span     span;
    bool     is_pub;
    const char *doc;
    AttrVec  attrs;
    union {
        struct { NameVec path; } module;
        struct { NameVec path; const char *alias; NameVec items; } use;
        FnDecl   *fn;
        TypeDecl *type;
        ImplDecl *impl;
        struct { const char *name; TypeExpr *type; Expr *value; Symbol *sym; bool mutable_; } konst;
        MacroDecl *macro;
        struct { const char *name; UiNode *root; FnDecl *entry; } app;
    } as;
};

typedef struct Module {
    const char *name;          /* dotted module name  */
    const char *path;          /* source file path    */
    int         file_id;
    DeclVec     decls;
    ModuleVec   imports;
    const char *doc;
    struct Scope *scope;
    bool        analyzed;
    uint64_t    content_hash;
} Module;

typedef struct {
    ModuleVec modules;
    Arena    *arena;
} Program;

/* helpers */
const char *ast_expr_kind_name(ExprKind k);
void        ast_dump_module(Module *m, StrBuf *out);
const char *typeexpr_to_text(Arena *a, TypeExpr *t);

#endif
