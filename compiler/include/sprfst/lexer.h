/* ========================================================================
   SPRFST — lexer
   ======================================================================== */
#ifndef SPRFST_LEXER_H
#define SPRFST_LEXER_H

#include "sprfst/common.h"

/* X(enum, text, is_keyword) */
#define SPRFST_TOKENS(X)                                                      \
    X(T_EOF,        "end of file",   0)                                       \
    X(T_NEWLINE,    "line break",    0)                                       \
    X(T_IDENT,      "name",          0)                                       \
    X(T_INT,        "integer",       0)                                       \
    X(T_NUM,        "number",        0)                                       \
    X(T_TEXT,       "text",          0)                                       \
    X(T_BYTE,       "byte",          0)                                       \
    X(T_DOC,        "doc comment",   0)                                       \
    /* keywords */                                                            \
    X(T_MODULE,     "module",        1)                                       \
    X(T_USE,        "use",           1)                                       \
    X(T_EXPORT,     "export",        1)                                       \
    X(T_PUB,        "pub",           1)                                       \
    X(T_LET,        "let",           1)                                       \
    X(T_VAR,        "var",           1)                                       \
    X(T_CONST,      "const",         1)                                       \
    X(T_TYPE,       "type",          1)                                       \
    X(T_ALIAS,      "alias",         1)                                       \
    X(T_FN,         "fn",            1)                                       \
    X(T_GIVE,       "give",          1)                                       \
    X(T_OBJECT,     "object",        1)                                       \
    X(T_DATA,       "data",          1)                                       \
    X(T_TRAIT,      "trait",         1)                                       \
    X(T_ENUM,       "enum",          1)                                       \
    X(T_IMPL,       "impl",          1)                                       \
    X(T_HAS,        "has",           1)                                       \
    X(T_EXTENDS,    "extends",       1)                                       \
    X(T_WHEN,       "when",          1)                                       \
    X(T_MATCH,      "match",         1)                                       \
    X(T_IF,         "if",            1)                                       \
    X(T_ELSE,       "else",          1)                                       \
    X(T_LOOP,       "loop",          1)                                       \
    X(T_WHILE,      "while",         1)                                       \
    X(T_FOR,        "for",           1)                                       \
    X(T_IN,         "in",            1)                                       \
    X(T_BREAK,      "break",         1)                                       \
    X(T_SKIP,       "skip",          1)                                       \
    X(T_TASK,       "task",          1)                                       \
    X(T_SPAWN,      "spawn",         1)                                       \
    X(T_AWAIT,      "await",         1)                                       \
    X(T_CHAN,       "chan",          1)                                       \
    X(T_TRY,        "try",           1)                                       \
    X(T_CATCH,      "catch",         1)                                       \
    X(T_FAIL,       "fail",          1)                                       \
    X(T_DEFER,      "defer",         1)                                       \
    X(T_UNSAFE,     "unsafe",        1)                                       \
    X(T_EXTERN,     "extern",        1)                                       \
    X(T_SELF,       "self",          1)                                       \
    X(T_INIT,       "init",          1)                                       \
    X(T_DROP,       "drop",          1)                                       \
    X(T_STATIC,     "static",        1)                                       \
    X(T_NEW,        "new",           1)                                       \
    X(T_TRUE,       "true",          1)                                       \
    X(T_FALSE,      "false",         1)                                       \
    X(T_NIL,        "nil",           1)                                       \
    X(T_AND,        "and",           1)                                       \
    X(T_OR,         "or",            1)                                       \
    X(T_NOT,        "not",           1)                                       \
    X(T_AS,         "as",            1)                                       \
    X(T_IS,         "is",            1)                                       \
    X(T_TEST,       "test",          1)                                       \
    X(T_BENCH,      "bench",         1)                                       \
    X(T_MACRO,      "macro",         1)                                       \
    X(T_COMPTIME,   "comptime",      1)                                       \
    X(T_WHERE,      "where",         1)                                       \
    X(T_REF,        "ref",           1)                                       \
    X(T_MUT,        "mut",           1)                                       \
    X(T_OWN,        "own",           1)                                       \
    X(T_WEAK,       "weak",          1)                                       \
    X(T_APP,        "app",           1)                                       \
    X(T_ON,         "on",            1)                                       \
    /* punctuation / operators */                                             \
    X(T_LPAREN,     "(",             0)                                       \
    X(T_RPAREN,     ")",             0)                                       \
    X(T_LBRACE,     "{",             0)                                       \
    X(T_RBRACE,     "}",             0)                                       \
    X(T_LBRACKET,   "[",             0)                                       \
    X(T_RBRACKET,   "]",             0)                                       \
    X(T_COMMA,      ",",             0)                                       \
    X(T_SEMI,       ";",             0)                                       \
    X(T_COLON,      ":",             0)                                       \
    X(T_DOT,        ".",             0)                                       \
    X(T_RANGE,      "..",            0)                                       \
    X(T_RANGE_IN,   "...",           0)                                       \
    X(T_ARROW,      "->",            0)                                       \
    X(T_FATARROW,   "=>",            0)                                       \
    X(T_PIPE,       "|>",            0)                                       \
    X(T_AT,         "@",             0)                                       \
    X(T_QUESTION,   "?",             0)                                       \
    X(T_QQ,         "??",            0)                                       \
    X(T_QDOT,       "?.",            0)                                       \
    X(T_BANG,       "!",             0)                                       \
    X(T_ASSIGN,     "=",             0)                                       \
    X(T_PLUSEQ,     "+=",            0)                                       \
    X(T_MINUSEQ,    "-=",            0)                                       \
    X(T_STAREQ,     "*=",            0)                                       \
    X(T_SLASHEQ,    "/=",            0)                                       \
    X(T_PCTEQ,      "%=",            0)                                       \
    X(T_PLUS,       "+",             0)                                       \
    X(T_MINUS,      "-",             0)                                       \
    X(T_STAR,       "*",             0)                                       \
    X(T_POW,        "**",            0)                                       \
    X(T_SLASH,      "/",             0)                                       \
    X(T_PERCENT,    "%",             0)                                       \
    X(T_EQ,         "==",            0)                                       \
    X(T_NE,         "!=",            0)                                       \
    X(T_LT,         "<",             0)                                       \
    X(T_LE,         "<=",            0)                                       \
    X(T_GT,         ">",             0)                                       \
    X(T_GE,         ">=",            0)                                       \
    X(T_AMP,        "&",             0)                                       \
    X(T_BAR,        "|",             0)                                       \
    X(T_CARET,      "^",             0)                                       \
    X(T_SHL,        "<<",            0)                                       \
    X(T_SHR,        ">>",            0)                                       \
    X(T_UNDERSCORE, "_",             0)

typedef enum {
#define X(e, t, k) e,
    SPRFST_TOKENS(X)
#undef X
    T_COUNT
} TokKind;

typedef struct {
    TokKind     kind;
    Span        span;
    Str         text;      /* raw source slice */
    const char *name;      /* interned identifier / keyword text */
    int64_t     ival;
    double      nval;
    char       *sval;      /* decoded text literal (arena) */
    bool        has_interp;/* text literal contains {..} holes */
    bool        nl_before; /* a line break preceded this token */
} Token;

typedef struct {
    SourceFile *file;
    Interner   *interner;
    Arena      *arena;
    DiagBag    *diags;
    const char *p, *end, *start;
    int         depth;      /* () and [] nesting: newlines suppressed inside */
    bool        prev_joins; /* previous token wants continuation */
    VEC(Token)  tokens;
} Lexer;

const char *tok_name(TokKind k);
bool        tok_is_keyword(TokKind k);
TokKind     keyword_lookup(const char *s, size_t n);

/* Tokenize an entire file. Returns a NUL-ish terminated token vector
   (always ends with T_EOF). Lexical errors are reported to diags. */
Token *lex_file(Arena *a, Interner *in, DiagBag *db, SourceFile *f, int *out_count);

/* Decode a text literal body (between quotes) into a buffer. */
char *lex_decode_text(Arena *a, Str raw, bool *has_interp);

#endif
