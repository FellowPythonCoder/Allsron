#ifndef SPRFST_PARSER_H
#define SPRFST_PARSER_H

#include "sprfst/ast.h"
#include "sprfst/lexer.h"

typedef struct {
    Token      *toks;
    int         ntok, pos;
    Arena      *arena;
    Interner   *interner;
    DiagBag    *diags;
    SourceFile *file;
    int         no_struct;   /* >0: a `{` starts a block, not a struct literal */
    int         loop_depth;
    int         panic;
    VEC(MacroDecl *) macros;
} Parser;

Module *parse_module(Arena *a, Interner *in, DiagBag *db, SourceFile *f);
Expr   *parse_expression_source(Arena *a, Interner *in, DiagBag *db, SourceFile *f);

#endif
