#include "sprfst/parser.h"
#include <ctype.h>

/* ---------------------------------------------------------------- utils */
static Token *cur(Parser *P) { return &P->toks[P->pos]; }
static Token *peek(Parser *P, int n) {
    int i = P->pos + n;
    if (i >= P->ntok) i = P->ntok - 1;
    return &P->toks[i];
}
static bool at(Parser *P, TokKind k) { return cur(P)->kind == k; }
static Token *advance(Parser *P) {
    Token *t = cur(P);
    if (P->pos < P->ntok - 1) P->pos++;
    return t;
}
static bool accept(Parser *P, TokKind k) {
    if (at(P, k)) { advance(P); return true; }
    return false;
}
static void skip_newlines(Parser *P) {
    while (at(P, T_NEWLINE) || at(P, T_SEMI)) advance(P);
}
static void skip_docs(Parser *P) {
    while (at(P, T_DOC) || at(P, T_NEWLINE)) advance(P);
}

static void perr(Parser *P, Span sp, const char *code, const char *title, const char *label) {
    if (P->panic) return;
    Diag *d = diag_new(P->diags, DIAG_ERROR, code, "%s", title);
    diag_label(P->diags, d, sp, true, "%s", label ? label : "here");
    P->panic = 1;
}

static void sync_statement(Parser *P) {
    P->panic = 0;
    int depth = 0;
    while (!at(P, T_EOF)) {
        if (at(P, T_NEWLINE) && depth == 0) { advance(P); return; }
        if (at(P, T_LBRACE)) depth++;
        if (at(P, T_RBRACE)) { if (depth == 0) return; depth--; }
        advance(P);
    }
}

static Token *expect(Parser *P, TokKind k, const char *ctx) {
    if (at(P, k)) return advance(P);
    Token *t = cur(P);
    if (!P->panic) {
        Diag *d = diag_new(P->diags, DIAG_ERROR, "E0010", "Expected %s%s%s", "`", tok_name(k), "`");
        diag_label(P->diags, d, t->span, true, "found `%s` here", t->kind == T_NEWLINE ? "line break" : tok_name(t->kind));
        if (ctx) diag_note(P->diags, d, "while parsing %s", ctx);
        if (k == T_RBRACE) diag_fix(P->diags, d, "check that every `{` has a matching `}`");
        if (k == T_RPAREN) diag_fix(P->diags, d, "check that every `(` has a matching `)`");
        P->panic = 1;
    }
    return t;
}

static const char *ident_of(Parser *P, const char *ctx) {
    if (at(P, T_IDENT) || at(P, T_UNDERSCORE) || tok_is_keyword(cur(P)->kind)) {
        Token *t = cur(P);
        /* allow contextual keywords as field/method names */
        if (t->kind != T_IDENT && t->kind != T_UNDERSCORE) {
            if (!t->name) t->name = intern(P->interner, t->text);
        }
        advance(P);
        return t->name ? t->name : intern(P->interner, t->text);
    }
    Token *t = cur(P);
    if (!P->panic) {
        Diag *d = diag_new(P->diags, DIAG_ERROR, "E0011", "Expected a name");
        diag_label(P->diags, d, t->span, true, "found `%s`", tok_name(t->kind));
        if (ctx) diag_note(P->diags, d, "while parsing %s", ctx);
        P->panic = 1;
    }
    return intern(P->interner, str_cstr("<error>"));
}

/* ------------------------------------------------------------ type exprs */
static TypeExpr *parse_type(Parser *P);

static TypeExpr *te_new(Parser *P, TypeExprKind k, Span sp) {
    TypeExpr *t = NEW(P->arena, TypeExpr);
    t->kind = k; t->span = sp;
    vec_init(&t->path); vec_init(&t->args);
    return t;
}

static TypeExpr *parse_type_atom(Parser *P) {
    Token *t = cur(P);
    Span sp = t->span;

    if (accept(P, T_UNDERSCORE)) return te_new(P, TE_INFER, sp);

    if (at(P, T_LBRACKET)) {            /* [T] list or [K: V] map */
        advance(P);
        TypeExpr *first = parse_type(P);
        if (accept(P, T_COLON)) {
            TypeExpr *v = parse_type(P);
            expect(P, T_RBRACKET, "a map type");
            TypeExpr *m = te_new(P, TE_MAP, span_join(sp, cur(P)->span));
            m->key = first; m->inner = v;
            return m;
        }
        expect(P, T_RBRACKET, "a list type");
        TypeExpr *l = te_new(P, TE_LIST, span_join(sp, cur(P)->span));
        l->inner = first;
        return l;
    }
    if (at(P, T_REF) || at(P, T_MUT)) {
        bool mut = accept(P, T_MUT);
        if (!mut) accept(P, T_REF); else accept(P, T_REF);
        TypeExpr *r = te_new(P, TE_REF, sp);
        r->mut = mut;
        r->inner = parse_type_atom(P);
        return r;
    }
    if (accept(P, T_OWN)) {
        TypeExpr *r = te_new(P, TE_OWN, sp);
        r->inner = parse_type_atom(P);
        return r;
    }
    if (accept(P, T_WEAK)) {
        TypeExpr *r = te_new(P, TE_WEAK, sp);
        r->inner = parse_type_atom(P);
        return r;
    }
    if (accept(P, T_CHAN)) {
        TypeExpr *r = te_new(P, TE_CHAN, sp);
        if (accept(P, T_LT)) { r->inner = parse_type(P); expect(P, T_GT, "a channel type"); }
        return r;
    }
    if (at(P, T_FN)) {
        advance(P);
        TypeExpr *f = te_new(P, TE_FN, sp);
        expect(P, T_LPAREN, "a function type");
        if (!at(P, T_RPAREN)) {
            do { skip_newlines(P); vec_push(&f->args, parse_type(P)); skip_newlines(P); } while (accept(P, T_COMMA));
        }
        expect(P, T_RPAREN, "a function type");
        if (accept(P, T_ARROW)) f->ret = parse_type(P);
        return f;
    }
    if (at(P, T_SELF)) { advance(P); return te_new(P, TE_SELF, sp); }

    /* qualified name with optional generic arguments */
    TypeExpr *n = te_new(P, TE_NAME, sp);
    const char *first = ident_of(P, "a type");
    vec_push(&n->path, first);
    while (at(P, T_DOT) && (peek(P, 1)->kind == T_IDENT)) {
        advance(P);
        vec_push(&n->path, ident_of(P, "a type"));
    }
    n->name = n->path.items[n->path.len - 1];
    if (at(P, T_LT)) {
        /* generic arguments */
        advance(P);
        if (!at(P, T_GT)) {
            do { skip_newlines(P); vec_push(&n->args, parse_type(P)); skip_newlines(P); } while (accept(P, T_COMMA));
        }
        if (at(P, T_SHR)) {  /* >> closing two generics */
            cur(P)->kind = T_GT;
            cur(P)->span.start++;
        } else expect(P, T_GT, "generic arguments");
    }
    n->span = span_join(sp, P->toks[P->pos > 0 ? P->pos - 1 : 0].span);
    return n;
}

static TypeExpr *parse_type(Parser *P) {
    TypeExpr *t = parse_type_atom(P);
    while (at(P, T_QUESTION)) {
        Span sp = cur(P)->span;
        advance(P);
        TypeExpr *m = te_new(P, TE_MAYBE, span_join(t->span, sp));
        m->inner = t;
        t = m;
    }
    return t;
}

const char *typeexpr_to_text(Arena *a, TypeExpr *t) {
    if (!t) return "?";
    switch (t->kind) {
        case TE_INFER: return "_";
        case TE_SELF: return "Self";
        case TE_MAYBE: return arena_vsprintf(a, "%s?", typeexpr_to_text(a, t->inner));
        case TE_LIST: return arena_vsprintf(a, "[%s]", typeexpr_to_text(a, t->inner));
        case TE_MAP: return arena_vsprintf(a, "[%s: %s]", typeexpr_to_text(a, t->key), typeexpr_to_text(a, t->inner));
        case TE_REF: return arena_vsprintf(a, "%sref %s", t->mut ? "mut " : "", typeexpr_to_text(a, t->inner));
        case TE_OWN: return arena_vsprintf(a, "own %s", typeexpr_to_text(a, t->inner));
        case TE_WEAK: return arena_vsprintf(a, "weak %s", typeexpr_to_text(a, t->inner));
        case TE_CHAN: return arena_vsprintf(a, "chan<%s>", typeexpr_to_text(a, t->inner));
        case TE_FN: {
            StrBuf b; sb_init(&b);
            sb_puts(&b, "fn(");
            vec_foreach(i, &t->args) { if (i) sb_puts(&b, ", "); sb_puts(&b, typeexpr_to_text(a, t->args.items[i])); }
            sb_puts(&b, ")");
            if (t->ret) { sb_puts(&b, " -> "); sb_puts(&b, typeexpr_to_text(a, t->ret)); }
            char *s = arena_strdup(a, b.data ? b.data : "fn()");
            sb_free(&b);
            return s;
        }
        default: {
            StrBuf b; sb_init(&b);
            vec_foreach(i, &t->path) { if (i) sb_puts(&b, "."); sb_puts(&b, t->path.items[i]); }
            if (t->args.len) {
                sb_puts(&b, "<");
                vec_foreach(i, &t->args) { if (i) sb_puts(&b, ", "); sb_puts(&b, typeexpr_to_text(a, t->args.items[i])); }
                sb_puts(&b, ">");
            }
            char *s = arena_strdup(a, b.data ? b.data : "?");
            sb_free(&b);
            return s;
        }
    }
}

/* ------------------------------------------------------------ expressions */
static Expr *parse_expr(Parser *P);
static Block *parse_block(Parser *P);
static Stmt *parse_stmt(Parser *P);
static FnDecl *parse_fn_rest(Parser *P, bool is_task, bool is_pub, Span start);

static Expr *ex_new(Parser *P, ExprKind k, Span sp) {
    Expr *e = NEW(P->arena, Expr);
    e->kind = k; e->span = sp;
    return e;
}

/* Parse the inside of an interpolated text literal: "a {x} b {y}" */
static Expr *parse_interp(Parser *P, Token *t) {
    Expr *e = ex_new(P, EX_INTERP, t->span);
    vec_init(&e->as.str.parts);
    vec_init(&e->as.str.chunks);
    const char *s = t->sval;
    size_t n = strlen(s);
    StrBuf chunk; sb_init(&chunk);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '{' && i + 1 < n && s[i + 1] == '{') { sb_putc(&chunk, '{'); i++; continue; }
        if (s[i] == '}' && i + 1 < n && s[i + 1] == '}') { sb_putc(&chunk, '}'); i++; continue; }
        if (s[i] == '{') {
            /* find matching } */
            size_t j = i + 1, depth = 1;
            while (j < n && depth) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') { depth--; if (!depth) break; }
                j++;
            }
            vec_push(&e->as.str.chunks, arena_strndup(P->arena, chunk.data ? chunk.data : "", chunk.len));
            chunk.len = 0; if (chunk.data) chunk.data[0] = 0;
            /* sub-parse the expression text */
            size_t sublen = j - i - 1;
            char *sub = arena_strndup(P->arena, s + i + 1, sublen);
            Parser sp2 = *P;
            SourceFile tmp = *P->file;
            int cnt = 0;
            /* tokens for the fragment share the original file for spans (approximate) */
            SourceFile *frag = NEW(P->arena, SourceFile);
            *frag = tmp;
            frag->src = sub;
            frag->len = sublen;
            Token *toks = lex_file(P->arena, P->interner, P->diags, frag, &cnt);
            /* shift the fragment spans so diagnostics point inside the real
               source line instead of at offset zero */
            int base = t->span.start + 1 + (int)i + 1;
            for (int k = 0; k < cnt; k++) {
                toks[k].span.file  = t->span.file;
                toks[k].span.start += base;
                toks[k].span.end   += base;
                if (toks[k].span.end > t->span.end) toks[k].span.end = t->span.end;
                if (toks[k].span.start > t->span.end) toks[k].span.start = t->span.end;
            }
            sp2.toks = toks; sp2.ntok = cnt; sp2.pos = 0; sp2.panic = 0;
            skip_newlines(&sp2);
            Expr *inner = parse_expr(&sp2);
            if (inner && (inner->span.end <= inner->span.start)) inner->span = t->span;
            vec_push(&e->as.str.parts, inner);
            i = j;
        } else {
            sb_putc(&chunk, s[i]);
        }
    }
    vec_push(&e->as.str.chunks, arena_strndup(P->arena, chunk.data ? chunk.data : "", chunk.len));
    sb_free(&chunk);
    return e;
}

static Pattern *parse_pattern(Parser *P);

static MatchArm parse_match_arm(Parser *P) {
    MatchArm arm = { 0 };
    Span sp = cur(P)->span;
    if (accept(P, T_ELSE)) {
        Pattern *p = NEW(P->arena, Pattern);
        p->kind = PAT_WILDCARD; p->span = sp;
        arm.pat = p;
    } else {
        expect(P, T_WHEN, "a match arm");
        arm.pat = parse_pattern(P);
        if (accept(P, T_IF)) { P->no_struct++; arm.guard = parse_expr(P); P->no_struct--; }
    }
    if (accept(P, T_ARROW)) {
        arm.body = parse_expr(P);
    } else if (at(P, T_LBRACE)) {
        arm.bbody = parse_block(P);
    } else {
        expect(P, T_ARROW, "a match arm (use `->` or a block)");
    }
    arm.span = span_join(sp, cur(P)->span);
    return arm;
}

static Expr *parse_match(Parser *P) {
    Span sp = cur(P)->span;
    expect(P, T_MATCH, "a match expression");
    Expr *e = ex_new(P, EX_MATCH, sp);
    P->no_struct++;
    e->as.match.subject = parse_expr(P);
    P->no_struct--;
    vec_init(&e->as.match.arms);
    skip_newlines(P);
    expect(P, T_LBRACE, "a match body");
    skip_newlines(P);
    while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
        MatchArm arm = parse_match_arm(P);
        vec_push(&e->as.match.arms, arm);
        skip_newlines(P);
        accept(P, T_COMMA);
        skip_newlines(P);
        if (P->panic) { sync_statement(P); skip_newlines(P); }
    }
    expect(P, T_RBRACE, "a match body");
    e->span = span_join(sp, P->toks[P->pos - 1].span);
    return e;
}

static Expr *parse_if_expr(Parser *P) {
    Span sp = cur(P)->span;
    expect(P, T_IF, "an if expression");
    Expr *e = ex_new(P, EX_IF, sp);
    P->no_struct++;
    e->as.iff.cond = parse_expr(P);
    P->no_struct--;
    e->as.iff.then_b = parse_block(P);
    skip_newlines(P);
    if (accept(P, T_ELSE)) {
        skip_newlines(P);
        if (at(P, T_IF)) {
            Expr *nested = parse_if_expr(P);
            Block *b = NEW(P->arena, Block);
            vec_init(&b->stmts);
            Stmt *s = NEW(P->arena, Stmt);
            s->kind = ST_EXPR; s->as.expr = nested; s->span = nested->span;
            vec_push(&b->stmts, s);
            b->span = nested->span;
            e->as.iff.else_b = b;
        } else {
            e->as.iff.else_b = parse_block(P);
        }
    }
    e->span = span_join(sp, P->toks[P->pos - 1].span);
    return e;
}

static void parse_call_args(Parser *P, ExprVec *args, NameVec *names) {
    expect(P, T_LPAREN, "an argument list");
    skip_newlines(P);
    if (!at(P, T_RPAREN)) {
        do {
            skip_newlines(P);
            if (at(P, T_RPAREN)) break;
            const char *nm = NULL;
            if ((at(P, T_IDENT)) && peek(P, 1)->kind == T_COLON && peek(P, 2)->kind != T_COLON) {
                nm = cur(P)->name; advance(P); advance(P);
            }
            int save = P->no_struct;
            P->no_struct = 0;
            Expr *a = parse_expr(P);
            P->no_struct = save;
            vec_push(args, a);
            if (names) vec_push(names, nm);
            skip_newlines(P);
        } while (accept(P, T_COMMA));
    }
    skip_newlines(P);
    expect(P, T_RPAREN, "an argument list");
}

/* `{ name: value, shorthand }` after a type name */
static void parse_struct_fields(Parser *P, Expr *e) {
    int save = P->no_struct;
    P->no_struct = 0;
    expect(P, T_LBRACE, "a struct literal");
    skip_newlines(P);
    while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
        FieldInit fi = { 0 };
        fi.span = cur(P)->span;
        fi.name = ident_of(P, "a field initialiser");
        if (accept(P, T_COLON)) fi.value = parse_expr(P);
        else {                                  /* shorthand  Point { x, y } */
            Expr *id = ex_new(P, EX_IDENT, fi.span);
            id->as.ident.name = fi.name;
            fi.value = id;
        }
        vec_push(&e->as.strct.fields, fi);
        skip_newlines(P);
        if (!accept(P, T_COMMA)) { skip_newlines(P); if (at(P, T_RBRACE)) break; }
        skip_newlines(P);
    }
    expect(P, T_RBRACE, "a struct literal");
    e->span = span_join(e->span, P->toks[P->pos - 1].span);
    P->no_struct = save;
}

static Expr *parse_primary(Parser *P) {
    Token *t = cur(P);
    Span sp = t->span;
    switch (t->kind) {
        case T_INT:  { advance(P); Expr *e = ex_new(P, EX_INT, sp); e->as.ival = t->ival; return e; }
        case T_NUM:  { advance(P); Expr *e = ex_new(P, EX_NUM, sp); e->as.nval = t->nval; return e; }
        case T_BYTE: { advance(P); Expr *e = ex_new(P, EX_BYTE, sp); e->as.ival = t->ival; return e; }
        case T_TRUE: { advance(P); Expr *e = ex_new(P, EX_BOOL, sp); e->as.bval = true; return e; }
        case T_FALSE:{ advance(P); Expr *e = ex_new(P, EX_BOOL, sp); e->as.bval = false; return e; }
        case T_NIL:  { advance(P); return ex_new(P, EX_NIL, sp); }
        case T_TEXT: {
            advance(P);
            if (t->has_interp) return parse_interp(P, t);
            Expr *e = ex_new(P, EX_TEXT, sp);
            e->as.str.text = t->sval;
            return e;
        }
        case T_SELF: { advance(P); return ex_new(P, EX_SELF, sp); }
        case T_UNDERSCORE: {
            advance(P);
            Expr *e = ex_new(P, EX_IDENT, sp);
            e->as.ident.name = intern(P->interner, str_cstr("_"));
            return e;
        }
        case T_IDENT: {
            advance(P);
            /* explicit type arguments:  Stack<Int> { }  — look ahead and
               rewind if the `<` turns out to be a comparison */
            if (at(P, T_LT) && !P->no_struct && isupper((unsigned char)t->name[0])) {
                int save = P->pos;
                int saved_panic = P->panic;
                int depth = 0;
                bool looks_generic = false;
                for (int k = P->pos; k < P->ntok && k < P->pos + 64; k++) {
                    TokKind kk = P->toks[k].kind;
                    if (kk == T_LT) depth++;
                    else if (kk == T_GT) {
                        depth--;
                        if (depth == 0) {
                            looks_generic = (k + 1 < P->ntok && P->toks[k + 1].kind == T_LBRACE);
                            break;
                        }
                    } else if (kk == T_IDENT || kk == T_COMMA || kk == T_LBRACKET ||
                               kk == T_RBRACKET || kk == T_QUESTION || kk == T_COLON ||
                               tok_is_keyword(kk)) continue;
                    else break;
                }
                if (looks_generic) {
                    advance(P);                       /* `<` */
                    while (!at(P, T_GT) && !at(P, T_EOF)) {
                        parse_type(P);
                        if (!accept(P, T_COMMA)) break;
                    }
                    expect(P, T_GT, "type arguments");
                } else { P->pos = save; P->panic = saved_panic; }
            }
            /* struct literal:  Point { x: 1 } */
            if (at(P, T_LBRACE) && !P->no_struct && isupper((unsigned char)t->name[0])) {
                Expr *e = ex_new(P, EX_STRUCT, sp);
                TypeExpr *ty = te_new(P, TE_NAME, sp);
                vec_push(&ty->path, t->name);
                ty->name = t->name;
                e->as.strct.type = ty;
                vec_init(&e->as.strct.fields);
                parse_struct_fields(P, e);
                e->span = span_join(sp, P->toks[P->pos - 1].span);
                return e;
            }
            Expr *e = ex_new(P, EX_IDENT, sp);
            e->as.ident.name = t->name;
            return e;
        }
        case T_LPAREN: {
            advance(P);
            skip_newlines(P);
            int save = P->no_struct; P->no_struct = 0;
            Expr *e = parse_expr(P);
            P->no_struct = save;
            skip_newlines(P);
            expect(P, T_RPAREN, "a parenthesised expression");
            return e;
        }
        case T_LBRACKET: {
            advance(P);
            skip_newlines(P);
            int save = P->no_struct; P->no_struct = 0;
            if (accept(P, T_COLON)) {       /* [:] empty map */
                skip_newlines(P);
                expect(P, T_RBRACKET, "an empty map literal");
                Expr *e = ex_new(P, EX_MAP, sp);
                vec_init(&e->as.map.keys); vec_init(&e->as.map.vals);
                P->no_struct = save;
                return e;
            }
            if (at(P, T_RBRACKET)) {        /* [] empty list */
                advance(P);
                Expr *e = ex_new(P, EX_LIST, sp);
                vec_init(&e->as.list.items);
                P->no_struct = save;
                return e;
            }
            Expr *first = parse_expr(P);
            skip_newlines(P);
            if (accept(P, T_COLON)) {       /* map literal */
                Expr *e = ex_new(P, EX_MAP, sp);
                vec_init(&e->as.map.keys); vec_init(&e->as.map.vals);
                skip_newlines(P);
                vec_push(&e->as.map.keys, first);
                vec_push(&e->as.map.vals, parse_expr(P));
                skip_newlines(P);
                while (accept(P, T_COMMA)) {
                    skip_newlines(P);
                    if (at(P, T_RBRACKET)) break;
                    Expr *k = parse_expr(P);
                    expect(P, T_COLON, "a map entry");
                    skip_newlines(P);
                    Expr *v = parse_expr(P);
                    vec_push(&e->as.map.keys, k);
                    vec_push(&e->as.map.vals, v);
                    skip_newlines(P);
                }
                skip_newlines(P);
                expect(P, T_RBRACKET, "a map literal");
                e->span = span_join(sp, P->toks[P->pos - 1].span);
                P->no_struct = save;
                return e;
            }
            Expr *e = ex_new(P, EX_LIST, sp);
            vec_init(&e->as.list.items);
            vec_push(&e->as.list.items, first);
            skip_newlines(P);
            while (accept(P, T_COMMA)) {
                skip_newlines(P);
                if (at(P, T_RBRACKET)) break;
                vec_push(&e->as.list.items, parse_expr(P));
                skip_newlines(P);
            }
            skip_newlines(P);
            expect(P, T_RBRACKET, "a list literal");
            e->span = span_join(sp, P->toks[P->pos - 1].span);
            P->no_struct = save;
            return e;
        }
        case T_FN: {   /* lambda */
            advance(P);
            FnDecl *fn = parse_fn_rest(P, false, false, sp);
            fn->is_lambda = true;
            Expr *e = ex_new(P, EX_LAMBDA, sp);
            e->as.lambda.fn = fn;
            vec_init(&e->as.lambda.captures);
            e->span = span_join(sp, P->toks[P->pos - 1].span);
            return e;
        }
        case T_IF:    return parse_if_expr(P);
        case T_MATCH: return parse_match(P);
        case T_NEW: {
            advance(P);
            Expr *e = ex_new(P, EX_NEW, sp);
            e->as.nw.type = parse_type(P);
            vec_init(&e->as.nw.args);
            if (at(P, T_LPAREN)) parse_call_args(P, &e->as.nw.args, NULL);
            e->span = span_join(sp, P->toks[P->pos - 1].span);
            return e;
        }
        case T_AWAIT: {
            advance(P);
            Expr *e = ex_new(P, EX_AWAIT, sp);
            e->as.wrap.value = parse_expr(P);
            e->span = span_join(sp, e->as.wrap.value->span);
            return e;
        }
        case T_SPAWN: {
            advance(P);
            Expr *e = ex_new(P, EX_SPAWN, sp);
            e->as.wrap.value = parse_expr(P);
            e->span = span_join(sp, e->as.wrap.value->span);
            return e;
        }
        case T_TRY: {
            advance(P);
            Expr *e = ex_new(P, EX_TRY, sp);
            e->as.wrap.value = parse_expr(P);
            e->span = span_join(sp, e->as.wrap.value->span);
            return e;
        }
        case T_COMPTIME: {
            advance(P);
            Expr *e = ex_new(P, EX_COMPTIME, sp);
            e->as.wrap.value = parse_expr(P);
            e->span = span_join(sp, e->as.wrap.value->span);
            return e;
        }
        case T_LBRACE: {   /* block expression */
            Block *b = parse_block(P);
            Expr *e = ex_new(P, EX_BLOCK, sp);
            e->as.block.block = b;
            return e;
        }
        default: {
            if (!P->panic && tok_is_keyword(t->kind)) {
                Diag *d = diag_new(P->diags, DIAG_ERROR, "E0013",
                                   "`%s` is a keyword, so it cannot be used as a name", tok_name(t->kind));
                diag_label(P->diags, d, sp, true, "reserved by the language");
                diag_fix(P->diags, d, "pick another name, such as `%s_value`", tok_name(t->kind));
                P->panic = 1;
                return ex_new(P, EX_NIL, sp);
            }
            if (!P->panic) {
                Diag *d = diag_new(P->diags, DIAG_ERROR, "E0012", "Expected a value");
                diag_label(P->diags, d, sp, true, "`%s` cannot start an expression",
                           t->kind == T_NEWLINE ? "line break" : tok_name(t->kind));
                diag_note(P->diags, d, "an expression is a number, text, name, call, or `(` ... `)`");
                P->panic = 1;
            }
            advance(P);
            return ex_new(P, EX_NIL, sp);
        }
    }
}

static Expr *parse_postfix(Parser *P, Expr *e) {
    for (;;) {
        Token *t = cur(P);
        if (at(P, T_DOT) || at(P, T_QDOT)) {
            bool opt = at(P, T_QDOT);
            advance(P);
            Span nsp = cur(P)->span;
            const char *name = ident_of(P, "a field or method name");
            if (at(P, T_LPAREN)) {
                Expr *m = ex_new(P, EX_METHOD, span_join(e->span, nsp));
                m->as.method.recv = e;
                m->as.method.name = name;
                m->as.method.name_span = nsp;
                m->as.method.optional = opt;
                vec_init(&m->as.method.args);
                parse_call_args(P, &m->as.method.args, NULL);
                m->span = span_join(e->span, P->toks[P->pos - 1].span);
                e = m;
            } else if (at(P, T_LBRACE) && !P->no_struct && isupper((unsigned char)name[0]) &&
                       e->kind == EX_IDENT) {
                /* qualified struct literal:  shapes.Circle { radius: 2.0 } */
                Expr *lit = ex_new(P, EX_STRUCT, span_join(e->span, nsp));
                TypeExpr *ty = te_new(P, TE_NAME, lit->span);
                vec_push(&ty->path, e->as.ident.name);
                vec_push(&ty->path, name);
                ty->name = name;
                lit->as.strct.type = ty;
                vec_init(&lit->as.strct.fields);
                parse_struct_fields(P, lit);
                e = lit;
            } else {
                Expr *f = ex_new(P, EX_FIELD, span_join(e->span, nsp));
                f->as.field.obj = e;
                f->as.field.name = name;
                f->as.field.name_span = nsp;
                f->as.field.optional = opt;
                f->as.field.field_index = -1;
                e = f;
            }
            continue;
        }
        if (at(P, T_LPAREN)) {
            Expr *c = ex_new(P, EX_CALL, e->span);
            c->as.call.callee = e;
            vec_init(&c->as.call.args);
            vec_init(&c->as.call.names);
            parse_call_args(P, &c->as.call.args, &c->as.call.names);
            c->span = span_join(e->span, P->toks[P->pos - 1].span);
            e = c;
            continue;
        }
        if (at(P, T_LBRACKET)) {
            advance(P);
            int save = P->no_struct; P->no_struct = 0;
            Expr *idx = parse_expr(P);
            P->no_struct = save;
            expect(P, T_RBRACKET, "an index expression");
            Expr *ix = ex_new(P, EX_INDEX, span_join(e->span, P->toks[P->pos - 1].span));
            ix->as.index.obj = e;
            ix->as.index.index = idx;
            e = ix;
            continue;
        }
        if (at(P, T_QUESTION)) {
            advance(P);
            Expr *w = ex_new(P, EX_TRY, span_join(e->span, t->span));
            w->as.wrap.value = e;
            e = w;
            continue;
        }
        if (at(P, T_BANG)) {
            advance(P);
            Expr *w = ex_new(P, EX_FORCE, span_join(e->span, t->span));
            w->as.wrap.value = e;
            e = w;
            continue;
        }
        break;
    }
    return e;
}

static Expr *parse_unary(Parser *P) {
    Token *t = cur(P);
    if (at(P, T_MINUS) || at(P, T_NOT) || at(P, T_PLUS)) {
        advance(P);
        Expr *operand = parse_unary(P);
        if (t->kind == T_PLUS) return operand;
        Expr *e = ex_new(P, EX_UNARY, span_join(t->span, operand->span));
        e->as.unary.op = t->kind;
        e->as.unary.operand = operand;
        return e;
    }
    Expr *prim = parse_primary(P);
    return parse_postfix(P, prim);
}

static Expr *parse_pow(Parser *P) {
    Expr *lhs = parse_unary(P);
    if (at(P, T_POW)) {
        Token *t = advance(P);
        Expr *rhs = parse_pow(P);  /* right associative */
        Expr *e = ex_new(P, EX_BINARY, span_join(lhs->span, rhs->span));
        e->as.binary.op = t->kind;
        e->as.binary.lhs = lhs; e->as.binary.rhs = rhs;
        return e;
    }
    return lhs;
}

typedef struct { TokKind ops[6]; int n; } OpSet;

static Expr *parse_binary_level(Parser *P, int level);

static Expr *mk_binary(Parser *P, TokKind op, Expr *l, Expr *r) {
    Expr *e = ex_new(P, EX_BINARY, span_join(l->span, r->span));
    e->as.binary.op = op; e->as.binary.lhs = l; e->as.binary.rhs = r;
    return e;
}

/* precedence ladder, tightest first */
static const OpSet levels[] = {
    { { T_STAR, T_SLASH, T_PERCENT }, 3 },
    { { T_PLUS, T_MINUS }, 2 },
    { { T_SHL, T_SHR }, 2 },
    { { T_AMP }, 1 },
    { { T_CARET }, 1 },
    { { T_BAR }, 1 },
    { { T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE }, 6 },
};
#define NLEVELS ((int)(sizeof(levels) / sizeof(levels[0])))

static Expr *parse_cast_level(Parser *P) {
    Expr *e = parse_pow(P);
    for (;;) {
        if (at(P, T_AS)) {
            Span sp = cur(P)->span; advance(P);
            Expr *c = ex_new(P, EX_CAST, span_join(e->span, sp));
            c->as.cast.value = e;
            c->as.cast.type = parse_type(P);
            e = c;
        } else if (at(P, T_IS)) {
            Span sp = cur(P)->span; advance(P);
            Expr *c = ex_new(P, EX_IS, span_join(e->span, sp));
            c->as.is.value = e;
            c->as.is.type = parse_type(P);
            e = c;
        } else break;
    }
    return e;
}

/* x |> f(a)  ==>  f(x, a)      x |> f  ==>  x.f()
   `|>` binds tighter than the arithmetic and comparison operators, so
   `numbers |> sum == 6` reads as `(numbers |> sum) == 6`. */
static Expr *parse_pipeline_level(Parser *P) {
    Expr *l = parse_cast_level(P);
    while (at(P, T_PIPE)) {
        advance(P);
        skip_newlines(P);
        Expr *rhs = parse_cast_level(P);
        if (rhs->kind == EX_CALL) {
            ExprVec newargs; vec_init(&newargs);
            NameVec newnames; vec_init(&newnames);
            vec_push(&newargs, l); vec_push(&newnames, (const char *)NULL);
            vec_foreach(i, &rhs->as.call.args) {
                vec_push(&newargs, rhs->as.call.args.items[i]);
                vec_push(&newnames, i < rhs->as.call.names.len ? rhs->as.call.names.items[i] : NULL);
            }
            vec_free(&rhs->as.call.args);
            vec_free(&rhs->as.call.names);
            rhs->as.call.args = newargs;
            rhs->as.call.names = newnames;
            rhs->span = span_join(l->span, rhs->span);
            l = rhs;
        } else if (rhs->kind == EX_METHOD) {
            /* x |> obj.m(a) keeps receiver, appends x first */
            ExprVec newargs; vec_init(&newargs);
            vec_push(&newargs, l);
            vec_foreach(i, &rhs->as.method.args) vec_push(&newargs, rhs->as.method.args.items[i]);
            vec_free(&rhs->as.method.args);
            rhs->as.method.args = newargs;
            rhs->span = span_join(l->span, rhs->span);
            l = rhs;
        } else if (rhs->kind == EX_IDENT) {
            /* x |> sum  is the same as  x.sum()  — universal call syntax */
            Expr *m = ex_new(P, EX_METHOD, span_join(l->span, rhs->span));
            m->as.method.recv = l;
            m->as.method.name = rhs->as.ident.name;
            m->as.method.name_span = rhs->span;
            m->as.method.builtin = -1;
            vec_init(&m->as.method.args);
            l = m;
        } else {
            Expr *c = ex_new(P, EX_CALL, span_join(l->span, rhs->span));
            c->as.call.callee = rhs;
            vec_init(&c->as.call.args); vec_init(&c->as.call.names);
            vec_push(&c->as.call.args, l);
            vec_push(&c->as.call.names, (const char *)NULL);
            l = c;
        }
    }
    return l;
}

static Expr *parse_binary_level(Parser *P, int level) {
    if (level < 0) return parse_pipeline_level(P);
    Expr *lhs = parse_binary_level(P, level - 1);
    for (;;) {
        bool matched = false;
        for (int i = 0; i < levels[level].n; i++) {
            if (at(P, levels[level].ops[i])) {
                TokKind op = cur(P)->kind;
                advance(P);
                skip_newlines(P);
                Expr *rhs = parse_binary_level(P, level - 1);
                lhs = mk_binary(P, op, lhs, rhs);
                matched = true;
                break;
            }
        }
        if (!matched) break;
    }
    return lhs;
}

static Expr *parse_range(Parser *P) {
    Expr *lo = parse_binary_level(P, NLEVELS - 2); /* up to shifts/bitwise, below comparison */
    if (at(P, T_RANGE) || at(P, T_RANGE_IN)) {
        bool incl = at(P, T_RANGE_IN);
        advance(P);
        Expr *hi = parse_binary_level(P, NLEVELS - 2);
        Expr *e = ex_new(P, EX_RANGE, span_join(lo->span, hi->span));
        e->as.range.lo = lo; e->as.range.hi = hi; e->as.range.inclusive = incl;
        return e;
    }
    return lo;
}

static Expr *parse_comparison(Parser *P) {
    Expr *lhs = parse_range(P);
    for (;;) {
        TokKind k = cur(P)->kind;
        if (k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE) {
            advance(P);
            skip_newlines(P);
            Expr *rhs = parse_range(P);
            lhs = mk_binary(P, k, lhs, rhs);
        } else break;
    }
    return lhs;
}

static Expr *parse_not(Parser *P) {
    if (at(P, T_NOT)) {
        Span sp = cur(P)->span;
        advance(P);
        Expr *o = parse_not(P);
        Expr *e = ex_new(P, EX_UNARY, span_join(sp, o->span));
        e->as.unary.op = T_NOT; e->as.unary.operand = o;
        return e;
    }
    return parse_comparison(P);
}

static Expr *parse_and(Parser *P) {
    Expr *l = parse_not(P);
    while (at(P, T_AND)) {
        advance(P); skip_newlines(P);
        Expr *r = parse_not(P);
        Expr *e = ex_new(P, EX_LOGICAL, span_join(l->span, r->span));
        e->as.binary.op = T_AND; e->as.binary.lhs = l; e->as.binary.rhs = r;
        l = e;
    }
    return l;
}

static Expr *parse_or(Parser *P) {
    Expr *l = parse_and(P);
    while (at(P, T_OR)) {
        advance(P); skip_newlines(P);
        Expr *r = parse_and(P);
        Expr *e = ex_new(P, EX_LOGICAL, span_join(l->span, r->span));
        e->as.binary.op = T_OR; e->as.binary.lhs = l; e->as.binary.rhs = r;
        l = e;
    }
    return l;
}

static Expr *parse_coalesce(Parser *P) {
    Expr *l = parse_or(P);
    while (at(P, T_QQ)) {
        advance(P); skip_newlines(P);
        Expr *r = parse_or(P);
        Expr *e = ex_new(P, EX_COALESCE, span_join(l->span, r->span));
        e->as.coalesce.value = l; e->as.coalesce.fallback = r;
        l = e;
    }
    return l;
}

static Expr *parse_expr(Parser *P) { return parse_coalesce(P); }

/* --------------------------------------------------------------- patterns */
static Pattern *parse_pattern(Parser *P) {
    Pattern *p = NEW(P->arena, Pattern);
    p->span = cur(P)->span;
    vec_init(&p->subs); vec_init(&p->path); vec_init(&p->field_names);

    if (accept(P, T_UNDERSCORE)) { p->kind = PAT_WILDCARD; return p; }
    if (at(P, T_IS)) {
        advance(P);
        p->kind = PAT_TYPE;
        p->type = parse_type(P);
        return p;
    }
    if (at(P, T_INT) || at(P, T_NUM) || at(P, T_TEXT) || at(P, T_TRUE) ||
        at(P, T_FALSE) || at(P, T_NIL) || at(P, T_BYTE) || at(P, T_MINUS)) {
        Expr *lit = parse_unary(P);
        if (at(P, T_RANGE) || at(P, T_RANGE_IN)) {
            bool incl = at(P, T_RANGE_IN);
            advance(P);
            p->kind = PAT_RANGE;
            p->lo = lit; p->hi = parse_unary(P);
            p->name = incl ? "inclusive" : NULL;
            return p;
        }
        p->kind = PAT_LITERAL;
        p->lit = lit;
        return p;
    }
    if (at(P, T_LBRACKET)) {
        advance(P);
        p->kind = PAT_LIST;
        skip_newlines(P);
        while (!at(P, T_RBRACKET) && !at(P, T_EOF)) {
            if (at(P, T_RANGE)) { advance(P); p->rest = ident_of(P, "a rest pattern"); }
            else vec_push(&p->subs, parse_pattern(P));
            if (!accept(P, T_COMMA)) break;
            skip_newlines(P);
        }
        expect(P, T_RBRACKET, "a list pattern");
        return p;
    }

    const char *first = ident_of(P, "a pattern");
    vec_push(&p->path, first);
    while (at(P, T_DOT)) { advance(P); vec_push(&p->path, ident_of(P, "a pattern")); }
    p->name = p->path.items[p->path.len - 1];

    if (at(P, T_LPAREN)) {
        advance(P);
        p->kind = PAT_VARIANT;
        skip_newlines(P);
        while (!at(P, T_RPAREN) && !at(P, T_EOF)) {
            vec_push(&p->subs, parse_pattern(P));
            if (!accept(P, T_COMMA)) break;
            skip_newlines(P);
        }
        expect(P, T_RPAREN, "a variant pattern");
        return p;
    }
    if (at(P, T_LBRACE) && isupper((unsigned char)first[0])) {
        advance(P);
        p->kind = PAT_DATA;
        skip_newlines(P);
        while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
            const char *fname = ident_of(P, "a field pattern");
            vec_push(&p->field_names, fname);
            if (accept(P, T_COLON)) vec_push(&p->subs, parse_pattern(P));
            else {
                Pattern *bind = NEW(P->arena, Pattern);
                bind->kind = PAT_BIND; bind->name = fname; bind->span = p->span;
                vec_init(&bind->subs); vec_init(&bind->path); vec_init(&bind->field_names);
                vec_push(&p->subs, bind);
            }
            if (!accept(P, T_COMMA)) break;
            skip_newlines(P);
        }
        expect(P, T_RBRACE, "a data pattern");
        return p;
    }
    /* Uppercase bare name = unit variant, lowercase = binding */
    p->kind = (p->path.len > 1 || isupper((unsigned char)first[0])) ? PAT_VARIANT : PAT_BIND;
    return p;
}

/* ------------------------------------------------------------- statements */
static Block *parse_block(Parser *P) {
    Block *b = NEW(P->arena, Block);
    vec_init(&b->stmts);
    b->span = cur(P)->span;
    int save = P->no_struct;
    P->no_struct = 0;
    expect(P, T_LBRACE, "a block");
    skip_newlines(P);
    while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
        /* a `~~` note inside a body is a comment, nothing to compile */
        if (at(P, T_DOC)) { advance(P); skip_newlines(P); continue; }
        Stmt *s = parse_stmt(P);
        if (s) vec_push(&b->stmts, s);
        if (P->panic) sync_statement(P);
        skip_newlines(P);
    }
    expect(P, T_RBRACE, "a block");
    b->span = span_join(b->span, P->toks[P->pos - 1].span);
    P->no_struct = save;
    return b;
}

static Decl *parse_decl(Parser *P);

static Stmt *st_new(Parser *P, StmtKind k, Span sp) {
    Stmt *s = NEW(P->arena, Stmt);
    s->kind = k; s->span = sp;
    return s;
}

static Stmt *parse_stmt(Parser *P) {
    Token *t = cur(P);
    Span sp = t->span;

    switch (t->kind) {
        case T_LET: case T_VAR: case T_CONST: {
            advance(P);
            Stmt *s = st_new(P, ST_LET, sp);
            s->as.let.mutable_ = (t->kind == T_VAR);
            s->as.let.is_const = (t->kind == T_CONST);
            if (at(P, T_LBRACKET) || (at(P, T_IDENT) && peek(P, 1)->kind == T_LBRACE)) {
                s->as.let.pat = parse_pattern(P);
            } else {
                s->as.let.name = ident_of(P, "a variable name");
            }
            if (accept(P, T_COLON)) s->as.let.type = parse_type(P);
            if (accept(P, T_ASSIGN)) {
                skip_newlines(P);
                s->as.let.init = parse_expr(P);
            } else if (!s->as.let.type) {
                Diag *d = diag_new(P->diags, DIAG_ERROR, "E0013",
                                   "`%s` needs a value or a type", t->kind == T_VAR ? "var" : "let");
                diag_label(P->diags, d, sp, true, "this binding has neither");
                diag_fix(P->diags, d, "give it a value:  let x = 10");
                diag_fix(P->diags, d, "or a type:        let x: Int");
            }
            s->span = span_join(sp, P->toks[P->pos - 1].span);
            return s;
        }
        case T_GIVE: {
            advance(P);
            Stmt *s = st_new(P, ST_GIVE, sp);
            if (!at(P, T_NEWLINE) && !at(P, T_RBRACE) && !at(P, T_SEMI) && !at(P, T_EOF))
                s->as.give.value = parse_expr(P);
            s->span = span_join(sp, P->toks[P->pos - 1].span);
            return s;
        }
        case T_IF: {
            Expr *e = parse_if_expr(P);
            Stmt *s = st_new(P, ST_EXPR, e->span);
            s->as.expr = e;
            return s;
        }
        case T_WHILE: {
            advance(P);
            Stmt *s = st_new(P, ST_WHILE, sp);
            P->no_struct++;
            s->as.whil.cond = parse_expr(P);
            P->no_struct--;
            P->loop_depth++;
            s->as.whil.body = parse_block(P);
            P->loop_depth--;
            s->span = span_join(sp, P->toks[P->pos - 1].span);
            return s;
        }
        case T_LOOP: {
            advance(P);
            Stmt *s = st_new(P, ST_LOOP, sp);
            if (at(P, T_IDENT)) s->as.loop.label = ident_of(P, "a loop label");
            P->loop_depth++;
            s->as.loop.body = parse_block(P);
            P->loop_depth--;
            s->span = span_join(sp, P->toks[P->pos - 1].span);
            return s;
        }
        case T_FOR: {
            advance(P);
            Stmt *s = st_new(P, ST_FOR, sp);
            s->as.forr.var = ident_of(P, "a loop variable");
            if (accept(P, T_COMMA)) {
                Pattern *p = NEW(P->arena, Pattern);
                p->kind = PAT_BIND; p->name = ident_of(P, "a second loop variable"); p->span = sp;
                vec_init(&p->subs); vec_init(&p->path); vec_init(&p->field_names);
                s->as.forr.pat = p;
            }
            expect(P, T_IN, "a for loop");
            P->no_struct++;
            s->as.forr.iter = parse_expr(P);
            P->no_struct--;
            P->loop_depth++;
            s->as.forr.body = parse_block(P);
            P->loop_depth--;
            s->span = span_join(sp, P->toks[P->pos - 1].span);
            return s;
        }
        case T_MATCH: {
            Expr *e = parse_match(P);
            Stmt *s = st_new(P, ST_EXPR, e->span);
            s->as.expr = e;
            return s;
        }
        case T_BREAK: case T_SKIP: {
            advance(P);
            Stmt *s = st_new(P, t->kind == T_BREAK ? ST_BREAK : ST_SKIP, sp);
            if (at(P, T_IDENT) && !cur(P)->nl_before) s->as.brk.label = ident_of(P, "a loop label");
            if (P->loop_depth == 0) {
                Diag *d = diag_new(P->diags, DIAG_ERROR, "E0014", "`%s` outside of a loop",
                                   t->kind == T_BREAK ? "break" : "skip");
                diag_label(P->diags, d, sp, true, "there is no loop around this");
                diag_fix(P->diags, d, "use `give` to return from a function");
            }
            return s;
        }
        case T_DEFER: {
            advance(P);
            Stmt *s = st_new(P, ST_DEFER, sp);
            if (at(P, T_LBRACE)) s->as.block.block = parse_block(P);
            else {
                Block *b = NEW(P->arena, Block);
                vec_init(&b->stmts);
                Stmt *inner = parse_stmt(P);
                if (inner) vec_push(&b->stmts, inner);
                s->as.block.block = b;
            }
            return s;
        }
        case T_FAIL: {
            advance(P);
            Stmt *s = st_new(P, ST_FAIL, sp);
            s->as.fail.value = parse_expr(P);
            s->span = span_join(sp, P->toks[P->pos - 1].span);
            return s;
        }
        case T_UNSAFE: {
            advance(P);
            Stmt *s = st_new(P, ST_UNSAFE, sp);
            s->as.block.block = parse_block(P);
            return s;
        }
        case T_TRY: {
            if (peek(P, 1)->kind == T_LBRACE) {
                advance(P);
                Stmt *s = st_new(P, ST_TRY, sp);
                s->as.tryc.body = parse_block(P);
                skip_newlines(P);
                if (accept(P, T_CATCH)) {
                    if (at(P, T_IDENT)) s->as.tryc.err_name = ident_of(P, "a catch binding");
                    s->as.tryc.handler = parse_block(P);
                } else {
                    Diag *d = diag_new(P->diags, DIAG_ERROR, "E0015", "`try` block without `catch`");
                    diag_label(P->diags, d, sp, true, "this try has no handler");
                    diag_fix(P->diags, d, "add:  catch err { ... }");
                }
                s->span = span_join(sp, P->toks[P->pos - 1].span);
                return s;
            }
            break;
        }
        case T_LBRACE: {
            Stmt *s = st_new(P, ST_BLOCK, sp);
            s->as.block.block = parse_block(P);
            return s;
        }
        case T_FN: case T_OBJECT: case T_DATA: case T_TRAIT: case T_ENUM:
        case T_TYPE: case T_IMPL: case T_USE: case T_MACRO: {
            Decl *d = parse_decl(P);
            if (!d) return NULL;
            Stmt *s = st_new(P, ST_DECL, sp);
            s->as.decl = d;
            return s;
        }
        default: break;
    }

    /* expression or assignment statement */
    Expr *e = parse_expr(P);
    TokKind k = cur(P)->kind;
    if (k == T_ASSIGN || k == T_PLUSEQ || k == T_MINUSEQ || k == T_STAREQ ||
        k == T_SLASHEQ || k == T_PCTEQ) {
        advance(P);
        skip_newlines(P);
        Expr *val = parse_expr(P);
        Expr *as = ex_new(P, EX_ASSIGN, span_join(e->span, val->span));
        as->as.assign.op = k;
        as->as.assign.target = e;
        as->as.assign.value = val;
        Stmt *s = st_new(P, ST_EXPR, as->span);
        s->as.expr = as;
        return s;
    }
    Stmt *s = st_new(P, ST_EXPR, e->span);
    s->as.expr = e;
    return s;
}

/* ----------------------------------------------------------- declarations */
static void parse_generics(Parser *P, GenericVec *out) {
    if (!at(P, T_LT)) return;
    advance(P);
    while (!at(P, T_GT) && !at(P, T_EOF)) {
        GenericParam g = { 0 };
        vec_init(&g.bounds);
        g.span = cur(P)->span;
        g.name = ident_of(P, "a generic parameter");
        if (accept(P, T_COLON)) {
            do { vec_push(&g.bounds, ident_of(P, "a trait bound")); } while (accept(P, T_PLUS));
        }
        vec_push(out, g);
        if (!accept(P, T_COMMA)) break;
    }
    expect(P, T_GT, "generic parameters");
}

static FnDecl *parse_fn_rest(Parser *P, bool is_task, bool is_pub, Span start) {
    FnDecl *fn = NEW(P->arena, FnDecl);
    vec_init(&fn->generics); vec_init(&fn->params); vec_init(&fn->attrs);
    fn->is_task = is_task; fn->is_pub = is_pub;
    fn->span = start;
    fn->ir_index = -1;

    if (at(P, T_IDENT) || (tok_is_keyword(cur(P)->kind) && !at(P, T_LPAREN))) {
        if (!at(P, T_LPAREN)) {
            fn->name_span = cur(P)->span;
            fn->name = ident_of(P, "a function name");
        }
    }
    parse_generics(P, &fn->generics);
    expect(P, T_LPAREN, "a parameter list");
    skip_newlines(P);
    while (!at(P, T_RPAREN) && !at(P, T_EOF)) {
        Param p = { 0 };
        p.span = cur(P)->span;
        if (at(P, T_SELF)) {
            advance(P);
            p.is_self = true;
            p.name = intern(P->interner, str_cstr("self"));
        } else {
            if (accept(P, T_MUT)) p.mut = true;
            p.name = ident_of(P, "a parameter name");
            if (accept(P, T_COLON)) p.type = parse_type(P);
            if (accept(P, T_ASSIGN)) p.deflt = parse_expr(P);
        }
        vec_push(&fn->params, p);
        skip_newlines(P);
        if (!accept(P, T_COMMA)) break;
        skip_newlines(P);
    }
    expect(P, T_RPAREN, "a parameter list");
    if (accept(P, T_ARROW)) fn->ret = parse_type(P);

    if (accept(P, T_FATARROW)) {
        skip_newlines(P);
        fn->expr_body = parse_expr(P);
    } else if (at(P, T_LBRACE)) {
        fn->body = parse_block(P);
    } else if (!fn->is_extern) {
        /* trait method without body: declaration only */
    }
    fn->span = span_join(start, P->toks[P->pos - 1].span);
    return fn;
}

static UiNode *parse_ui_node(Parser *P);

static void parse_ui_body(Parser *P, UiNode *n) {
    expect(P, T_LBRACE, "a UI block");
    skip_newlines(P);
    while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
        if (at(P, T_DOC)) { advance(P); skip_newlines(P); continue; }
        if (at(P, T_ON)) {
            advance(P);
            const char *ev = ident_of(P, "an event name");
            /* an optional name for the value the event carries:
                   on change(text) { draft = text } */
            const char *arg = NULL;
            if (accept(P, T_LPAREN)) {
                arg = ident_of(P, "a name for the event value");
                expect(P, T_RPAREN, "an event value name");
            }
            Block *b = parse_block(P);
            vec_push(&n->handler_names, ev);
            vec_push(&n->handler_params, arg);
            vec_push(&n->handler_bodies, b);
            vec_push(&n->handler_fns, (FnDecl *)NULL);
        } else if (at(P, T_LET) || at(P, T_VAR)) {
            /* state declaration inside app */
            UiNode *st = NEW(P->arena, UiNode);
            st->kind = intern(P->interner, str_cstr("state"));
            vec_init(&st->prop_names); vec_init(&st->prop_values); vec_init(&st->children);
            vec_init(&st->handler_names); vec_init(&st->handler_params); vec_init(&st->handler_bodies); vec_init(&st->handler_fns);
            st->span = cur(P)->span;
            advance(P);
            const char *nm = ident_of(P, "a state variable");
            if (accept(P, T_COLON)) parse_type(P);
            expect(P, T_ASSIGN, "a state variable");
            Expr *v = parse_expr(P);
            vec_push(&st->prop_names, nm);
            vec_push(&st->prop_values, v);
            st->label = nm;
            vec_push(&n->children, st);
        } else if ((at(P, T_IDENT) || tok_is_keyword(cur(P)->kind)) && peek(P, 1)->kind == T_COLON) {
            const char *pname = ident_of(P, "a UI property");
            expect(P, T_COLON, "a UI property");
            Expr *v = parse_expr(P);
            vec_push(&n->prop_names, pname);
            vec_push(&n->prop_values, v);
        } else if (at(P, T_IDENT)) {
            vec_push(&n->children, parse_ui_node(P));
        } else {
            perr(P, cur(P)->span, "E0016", "Unexpected item inside a UI block",
                 "expected a property, a child element, or `on <event>`");
            sync_statement(P);
        }
        skip_newlines(P);
        if (P->panic) { sync_statement(P); skip_newlines(P); }
    }
    expect(P, T_RBRACE, "a UI block");
}

static UiNode *parse_ui_node(Parser *P) {
    UiNode *n = NEW(P->arena, UiNode);
    vec_init(&n->prop_names); vec_init(&n->prop_values); vec_init(&n->children);
    vec_init(&n->handler_names); vec_init(&n->handler_params); vec_init(&n->handler_bodies); vec_init(&n->handler_fns);
    n->span = cur(P)->span;
    n->kind = ident_of(P, "a UI element");
    if (at(P, T_TEXT)) {
        Token *lt = cur(P);
        n->label = lt->sval;
        if (lt->has_interp) {
            /* a label with {holes} becomes a bound `text` property so the
               runtime re-evaluates it whenever state changes */
            Expr *ie = parse_interp(P, lt);
            vec_push(&n->prop_names, intern(P->interner, str_cstr("text")));
            vec_push(&n->prop_values, ie);
        }
        advance(P);
    }
    if (at(P, T_LBRACE)) parse_ui_body(P, n);
    return n;
}

static Decl *decl_new(Parser *P, DeclKind k, Span sp) {
    Decl *d = NEW(P->arena, Decl);
    d->kind = k; d->span = sp;
    vec_init(&d->attrs);
    return d;
}

static Decl *parse_decl(Parser *P) {
    /* doc comments + attributes */
    StrBuf doc; sb_init(&doc);
    while (at(P, T_DOC)) {
        if (doc.len) sb_putc(&doc, '\n');
        sb_puts(&doc, cur(P)->sval);
        advance(P);
        skip_newlines(P);
    }
    AttrVec attrs; vec_init(&attrs);
    while (at(P, T_AT)) {
        advance(P);
        Attr a = { 0 };
        a.span = cur(P)->span;
        a.name = ident_of(P, "an attribute");
        if (accept(P, T_LPAREN)) {
            if (at(P, T_TEXT)) { a.value = cur(P)->sval; advance(P); }
            else if (at(P, T_IDENT)) a.value = ident_of(P, "an attribute value");
            else if (at(P, T_INT)) { a.value = arena_vsprintf(P->arena, "%lld", (long long)cur(P)->ival); advance(P); }
            expect(P, T_RPAREN, "an attribute");
        }
        vec_push(&attrs, a);
        skip_newlines(P);
    }

    bool is_pub = false;
    if (at(P, T_PUB) || at(P, T_EXPORT)) { is_pub = true; advance(P); }

    Token *t = cur(P);
    Span sp = t->span;
    Decl *d = NULL;

    switch (t->kind) {
        case T_MODULE: {
            advance(P);
            d = decl_new(P, D_MODULE, sp);
            vec_init(&d->as.module.path);
            vec_push(&d->as.module.path, ident_of(P, "a module name"));
            while (accept(P, T_DOT)) vec_push(&d->as.module.path, ident_of(P, "a module name"));
            break;
        }
        case T_USE: {
            advance(P);
            d = decl_new(P, D_USE, sp);
            vec_init(&d->as.use.path);
            vec_init(&d->as.use.items);
            vec_push(&d->as.use.path, ident_of(P, "a module path"));
            while (at(P, T_DOT)) {
                advance(P);
                if (at(P, T_LBRACE)) {
                    advance(P);
                    while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
                        vec_push(&d->as.use.items, ident_of(P, "an imported name"));
                        if (!accept(P, T_COMMA)) break;
                    }
                    expect(P, T_RBRACE, "an import list");
                    break;
                }
                vec_push(&d->as.use.path, ident_of(P, "a module path"));
            }
            if (accept(P, T_AS)) d->as.use.alias = ident_of(P, "an import alias");
            break;
        }
        case T_TASK: case T_FN: {
            bool is_task = (t->kind == T_TASK);
            advance(P);
            if (is_task && at(P, T_FN)) advance(P);
            FnDecl *fn = parse_fn_rest(P, is_task, is_pub, sp);
            d = decl_new(P, D_FN, sp);
            d->as.fn = fn;
            break;
        }
        case T_TEST: case T_BENCH: {
            bool bench = (t->kind == T_BENCH);
            advance(P);
            FnDecl *fn = NEW(P->arena, FnDecl);
            vec_init(&fn->generics); vec_init(&fn->params); vec_init(&fn->attrs);
            fn->span = sp; fn->ir_index = -1;
            fn->is_test = !bench; fn->is_bench = bench;
            if (at(P, T_TEXT)) { fn->name = cur(P)->sval; advance(P); }
            else fn->name = ident_of(P, "a test name");
            fn->name_span = sp;
            fn->body = parse_block(P);
            fn->span = span_join(sp, P->toks[P->pos - 1].span);
            d = decl_new(P, D_FN, sp);
            d->as.fn = fn;
            break;
        }
        case T_DATA: case T_OBJECT: case T_TRAIT: case T_ENUM: {
            advance(P);
            TypeDecl *td = NEW(P->arena, TypeDecl);
            vec_init(&td->generics); vec_init(&td->fields); vec_init(&td->variants);
            vec_init(&td->methods); vec_init(&td->traits); vec_init(&td->attrs);
            td->kind = (t->kind == T_DATA) ? TD_DATA : (t->kind == T_OBJECT) ? TD_OBJECT :
                       (t->kind == T_TRAIT) ? TD_TRAIT : TD_ENUM;
            td->is_pub = is_pub;
            td->span = sp;
            td->name_span = cur(P)->span;
            td->name = ident_of(P, "a type name");
            parse_generics(P, &td->generics);
            if (accept(P, T_EXTENDS)) td->base = ident_of(P, "a base type");
            if (accept(P, T_COLON)) {
                do { vec_push(&td->traits, ident_of(P, "a trait name")); } while (accept(P, T_COMMA));
            }
            expect(P, T_LBRACE, "a type body");
            skip_newlines(P);
            while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
                /* member doc + attributes */
                StrBuf mdoc; sb_init(&mdoc);
                while (at(P, T_DOC)) {
                    if (mdoc.len) sb_putc(&mdoc, '\n');
                    sb_puts(&mdoc, cur(P)->sval);
                    advance(P); skip_newlines(P);
                }
                AttrVec mattrs; vec_init(&mattrs);
                while (at(P, T_AT)) {
                    advance(P);
                    Attr a = { 0 };
                    a.span = cur(P)->span;
                    a.name = ident_of(P, "an attribute");
                    if (accept(P, T_LPAREN)) {
                        if (at(P, T_TEXT)) { a.value = cur(P)->sval; advance(P); }
                        else if (!at(P, T_RPAREN)) a.value = ident_of(P, "an attribute value");
                        expect(P, T_RPAREN, "an attribute");
                    }
                    vec_push(&mattrs, a);
                    skip_newlines(P);
                }
                bool mpub = false, mstatic = false;
                if (at(P, T_PUB)) { mpub = true; advance(P); }
                if (at(P, T_STATIC)) { mstatic = true; advance(P); }

                if (td->kind == TD_ENUM && !at(P, T_FN) && !at(P, T_TASK) && !at(P, T_INIT) && !at(P, T_DROP)) {
                    VariantDecl v = { 0 };
                    vec_init(&v.payload);
                    v.span = cur(P)->span;
                    v.name = ident_of(P, "a variant name");
                    v.doc = mdoc.len ? arena_strdup(P->arena, mdoc.data) : NULL;
                    v.tag = td->variants.len;
                    if (accept(P, T_LPAREN)) {
                        while (!at(P, T_RPAREN) && !at(P, T_EOF)) {
                            vec_push(&v.payload, parse_type(P));
                            if (!accept(P, T_COMMA)) break;
                        }
                        expect(P, T_RPAREN, "a variant payload");
                    }
                    if (accept(P, T_ASSIGN)) v.value = parse_expr(P);
                    vec_push(&td->variants, v);
                } else if (at(P, T_FN) || at(P, T_TASK) || at(P, T_INIT) || at(P, T_DROP)) {
                    bool is_task = at(P, T_TASK);
                    bool is_init = at(P, T_INIT);
                    bool is_drop = at(P, T_DROP);
                    advance(P);
                    if (is_task && at(P, T_FN)) advance(P);
                    Span fsp = cur(P)->span;
                    FnDecl *fn;
                    if (is_init || is_drop) {
                        fn = parse_fn_rest(P, false, mpub, fsp);
                        fn->name = intern(P->interner, str_cstr(is_init ? "init" : "drop"));
                        fn->is_init = is_init; fn->is_drop = is_drop;
                    } else {
                        fn = parse_fn_rest(P, is_task, mpub, fsp);
                    }
                    fn->is_static = mstatic;
                    fn->is_method = true;
                    fn->owner = td;
                    fn->doc = mdoc.len ? arena_strdup(P->arena, mdoc.data) : NULL;
                    fn->attrs = mattrs;
                    vec_push(&td->methods, fn);
                } else if (at(P, T_LET) || at(P, T_VAR) || at(P, T_CONST) || at(P, T_HAS) ||
                           at(P, T_IDENT)) {
                    bool ismut = at(P, T_VAR);
                    bool ishas = at(P, T_HAS);
                    if (at(P, T_LET) || at(P, T_VAR) || at(P, T_CONST) || at(P, T_HAS)) advance(P);
                    FieldDecl f = { 0 };
                    f.span = cur(P)->span;
                    f.name = ident_of(P, "a field name");
                    f.is_mut = ismut || td->kind == TD_OBJECT;
                    f.is_has = ishas;
                    f.is_pub = mpub;
                    f.is_static = mstatic;
                    f.doc = mdoc.len ? arena_strdup(P->arena, mdoc.data) : NULL;
                    f.index = td->fields.len;
                    if (accept(P, T_COLON)) f.type = parse_type(P);
                    if (accept(P, T_ASSIGN)) f.deflt = parse_expr(P);
                    if (!f.type && !f.deflt) {
                        Diag *dg = diag_new(P->diags, DIAG_ERROR, "E0017", "Field `%s` needs a type", f.name);
                        diag_label(P->diags, dg, f.span, true, "add `: Type` here");
                        diag_fix(P->diags, dg, "for example:  %s: Int", f.name);
                    }
                    vec_push(&td->fields, f);
                } else {
                    perr(P, cur(P)->span, "E0018", "Unexpected item in a type body",
                         "expected a field, method, or variant");
                    sync_statement(P);
                }
                sb_free(&mdoc);
                skip_newlines(P);
                if (P->panic) { sync_statement(P); skip_newlines(P); }
            }
            expect(P, T_RBRACE, "a type body");
            td->span = span_join(sp, P->toks[P->pos - 1].span);
            d = decl_new(P, D_TYPE, sp);
            d->as.type = td;
            break;
        }
        case T_IMPL: {
            advance(P);
            ImplDecl *im = NEW(P->arena, ImplDecl);
            vec_init(&im->methods);
            im->span = sp;
            im->trait_name = ident_of(P, "a trait name");
            if (accept(P, T_FOR)) im->target = parse_type(P);
            else {
                TypeExpr *te = te_new(P, TE_NAME, sp);
                vec_push(&te->path, im->trait_name);
                te->name = im->trait_name;
                im->target = te;
                im->trait_name = NULL;
            }
            expect(P, T_LBRACE, "an impl block");
            skip_newlines(P);
            while (!at(P, T_RBRACE) && !at(P, T_EOF)) {
                StrBuf mdoc; sb_init(&mdoc);
                while (at(P, T_DOC)) { if (mdoc.len) sb_putc(&mdoc, '\n'); sb_puts(&mdoc, cur(P)->sval); advance(P); skip_newlines(P); }
                bool mpub = accept(P, T_PUB);
                bool mstatic = accept(P, T_STATIC);
                bool is_task = at(P, T_TASK);
                if (at(P, T_FN) || at(P, T_TASK)) {
                    advance(P);
                    if (is_task && at(P, T_FN)) advance(P);
                    FnDecl *fn = parse_fn_rest(P, is_task, mpub, cur(P)->span);
                    fn->is_method = true;
                    fn->is_static = mstatic;
                    fn->doc = mdoc.len ? arena_strdup(P->arena, mdoc.data) : NULL;
                    vec_push(&im->methods, fn);
                } else {
                    perr(P, cur(P)->span, "E0019", "Only functions are allowed in an impl block", "expected `fn`");
                    sync_statement(P);
                }
                sb_free(&mdoc);
                skip_newlines(P);
            }
            expect(P, T_RBRACE, "an impl block");
            d = decl_new(P, D_IMPL, sp);
            d->as.impl = im;
            break;
        }
        case T_TYPE: case T_ALIAS: {
            advance(P);
            TypeDecl *td = NEW(P->arena, TypeDecl);
            vec_init(&td->generics); vec_init(&td->fields); vec_init(&td->variants);
            vec_init(&td->methods); vec_init(&td->traits); vec_init(&td->attrs);
            td->kind = TD_ALIAS;
            td->is_pub = is_pub;
            td->span = sp;
            td->name_span = cur(P)->span;
            td->name = ident_of(P, "a type name");
            parse_generics(P, &td->generics);
            expect(P, T_ASSIGN, "a type alias");
            td->aliased = parse_type(P);
            d = decl_new(P, D_TYPE, sp);
            d->as.type = td;
            break;
        }
        case T_CONST: case T_LET: case T_VAR: {
            bool mut = at(P, T_VAR);
            advance(P);
            d = decl_new(P, t->kind == T_CONST ? D_CONST : D_LET, sp);
            d->as.konst.mutable_ = mut;
            d->as.konst.name = ident_of(P, "a constant name");
            if (accept(P, T_COLON)) d->as.konst.type = parse_type(P);
            expect(P, T_ASSIGN, "a constant");
            d->as.konst.value = parse_expr(P);
            break;
        }
        case T_MACRO: {
            advance(P);
            MacroDecl *m = NEW(P->arena, MacroDecl);
            vec_init(&m->params);
            m->span = sp;
            m->name = ident_of(P, "a macro name");
            expect(P, T_LPAREN, "macro parameters");
            while (!at(P, T_RPAREN) && !at(P, T_EOF)) {
                vec_push(&m->params, ident_of(P, "a macro parameter"));
                if (!accept(P, T_COMMA)) break;
            }
            expect(P, T_RPAREN, "macro parameters");
            if (accept(P, T_FATARROW)) m->expr = parse_expr(P);
            else m->body = parse_block(P);
            d = decl_new(P, D_MACRO, sp);
            d->as.macro = m;
            vec_push(&P->macros, m);
            break;
        }
        case T_APP: {
            advance(P);
            d = decl_new(P, D_APP, sp);
            if (at(P, T_TEXT)) { d->as.app.name = cur(P)->sval; advance(P); }
            else d->as.app.name = "SPRFST App";
            UiNode *root = NEW(P->arena, UiNode);
            vec_init(&root->prop_names); vec_init(&root->prop_values); vec_init(&root->children);
            vec_init(&root->handler_names); vec_init(&root->handler_params); vec_init(&root->handler_bodies); vec_init(&root->handler_fns);
            root->kind = intern(P->interner, str_cstr("app"));
            root->label = d->as.app.name;
            root->span = sp;
            parse_ui_body(P, root);
            d->as.app.root = root;
            break;
        }
        case T_EXTERN: {
            advance(P);
            const char *lib = NULL;
            if (at(P, T_TEXT)) { lib = cur(P)->sval; advance(P); }
            if (at(P, T_FN)) {
                advance(P);
                FnDecl *fn = parse_fn_rest(P, false, is_pub, sp);
                fn->is_extern = true;
                fn->extern_name = lib;
                d = decl_new(P, D_FN, sp);
                d->as.fn = fn;
            }
            break;
        }
        default: {
            perr(P, sp, "E0020", "Expected a declaration",
                 "top level items are module, use, fn, data, object, trait, enum, impl, const, test, app");
            sync_statement(P);
            sb_free(&doc);
            vec_free(&attrs);
            return NULL;
        }
    }

    if (d) {
        d->is_pub = is_pub;
        d->doc = doc.len ? arena_strdup(P->arena, doc.data) : NULL;
        d->attrs = attrs;
        if (d->kind == D_FN) {
            d->as.fn->doc = d->as.fn->doc ? d->as.fn->doc : d->doc;
            if (!d->as.fn->attrs.len) d->as.fn->attrs = attrs;
            vec_foreach(i, &attrs) {
                if (strcmp(attrs.items[i].name, "test") == 0) d->as.fn->is_test = true;
                if (strcmp(attrs.items[i].name, "bench") == 0) d->as.fn->is_bench = true;
            }
            d->as.fn->is_pub = d->as.fn->is_pub || is_pub;
        }
        if (d->kind == D_TYPE) {
            d->as.type->doc = d->doc;
            d->as.type->attrs = attrs;
        }
        d->span = span_join(sp, P->toks[P->pos > 0 ? P->pos - 1 : 0].span);
    }
    sb_free(&doc);
    return d;
}

/* ------------------------------------------------------------------ entry */
Module *parse_module(Arena *a, Interner *in, DiagBag *db, SourceFile *f) {
    Parser P = { 0 };
    P.arena = a; P.interner = in; P.diags = db; P.file = f;
    vec_init(&P.macros);
    P.toks = lex_file(a, in, db, f, &P.ntok);

    Module *m = NEW(a, Module);
    vec_init(&m->decls);
    vec_init(&m->imports);
    m->path = f->path;
    m->file_id = f->id;
    m->name = NULL;

    /* leading file doc comment */
    StrBuf fdoc; sb_init(&fdoc);
    while (at(&P, T_DOC) && peek(&P, 1)->kind == T_NEWLINE) {
        if (fdoc.len) sb_putc(&fdoc, '\n');
        sb_puts(&fdoc, cur(&P)->sval);
        advance(&P);
        skip_newlines(&P);
        if (at(&P, T_MODULE) || at(&P, T_USE)) break;
    }
    m->doc = fdoc.len ? arena_strdup(a, fdoc.data) : NULL;
    sb_free(&fdoc);

    skip_newlines(&P);
    while (!at(&P, T_EOF)) {
        skip_newlines(&P);
        if (at(&P, T_EOF)) break;
        int before = P.pos;
        Decl *d = parse_decl(&P);
        if (d) {
            if (d->kind == D_MODULE) {
                StrBuf nb; sb_init(&nb);
                vec_foreach(i, &d->as.module.path) {
                    if (i) sb_putc(&nb, '.');
                    sb_puts(&nb, d->as.module.path.items[i]);
                }
                m->name = arena_strdup(a, nb.data ? nb.data : "main");
                sb_free(&nb);
            }
            vec_push(&m->decls, d);
        }
        if (P.panic) sync_statement(&P);
        if (P.pos == before) advance(&P);
        skip_newlines(&P);
    }
    if (!m->name) {
        char buf[256];
        const char *base = path_basename(f->path);
        size_t n = strlen(base);
        if (n > 4 && strcmp(base + n - 4, ".spf") == 0) n -= 4;
        snprintf(buf, sizeof buf, "%.*s", (int)n, base);
        m->name = arena_strdup(a, buf);
    }
    m->content_hash = hash_bytes(f->src, f->len);
    return m;
}

Expr *parse_expression_source(Arena *a, Interner *in, DiagBag *db, SourceFile *f) {
    Parser P = { 0 };
    P.arena = a; P.interner = in; P.diags = db; P.file = f;
    vec_init(&P.macros);
    P.toks = lex_file(a, in, db, f, &P.ntok);
    skip_newlines(&P);
    return parse_expr(&P);
}
