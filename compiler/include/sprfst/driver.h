#ifndef SPRFST_DRIVER_H
#define SPRFST_DRIVER_H

#include "sprfst/common.h"
#include "sprfst/ast.h"
#include "sprfst/sema.h"
#include "sprfst/vm.h"

/* A Build owns one compilation: sources, diagnostics, types, IR. */
typedef struct {
    Arena       arena;
    Interner   *in;
    SourceMap  *sm;
    DiagBag    *db;
    TypeTable  *tt;
    Sema       *sema;
    IRProgram  *prog;
    OptStats    opt;
    int         opt_level;
    bool        color;
    bool        json_diags;
    bool        quiet;
    char       *root;          /* project root (dir with project.sprfst) or src dir */
    double      t_parse, t_check, t_lower;
    int         nfiles;
} Build;

void  build_init(Build *b, int opt_level);
void  build_dispose(Build *b);

/* Load `path` and, recursively, every user module it imports. */
bool  build_load(Build *b, const char *path);
/* Semantic analysis over everything loaded. */
bool  build_check(Build *b);
/* Lower to SPIR and optimise. */
bool  build_lower(Build *b);
/* Everything at once. Returns false if any error was reported. */
bool  build_compile(Build *b, const char *path);
/* Print whatever diagnostics were collected. */
void  build_report(Build *b);

/* project.sprfst ------------------------------------------------------- */
typedef struct {
    char name[128];
    char version[32];
    char entry[512];
    char author[128];
    char description[256];
    char deps[32][160];
    int  ndeps;
    bool found;
} Project;

Project project_load(const char *dir);
char   *project_find_root(Arena *a, const char *start);

#endif
