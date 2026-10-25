#include "sprfst/lexer.h"
#include <ctype.h>

static const char *tok_names[] = {
#define X(e, t, k) t,
    SPRFST_TOKENS(X)
#undef X
};
static const char tok_kw[] = {
#define X(e, t, k) k,
    SPRFST_TOKENS(X)
#undef X
};

const char *tok_name(TokKind k) { return (k >= 0 && k < T_COUNT) ? tok_names[k] : "?"; }
bool tok_is_keyword(TokKind k) { return (k >= 0 && k < T_COUNT) && tok_kw[k]; }

TokKind keyword_lookup(const char *s, size_t n) {
    for (int i = 0; i < T_COUNT; i++) {
        if (!tok_kw[i]) continue;
        const char *t = tok_names[i];
        if (strlen(t) == n && memcmp(t, s, n) == 0) return (TokKind)i;
    }
    return T_IDENT;
}

static bool ident_start(int c) { return isalpha(c) || c == '_' || (unsigned char)c >= 0x80; }
static bool ident_cont(int c) { return isalnum(c) || c == '_' || (unsigned char)c >= 0x80; }

static void push_tok(Lexer *L, Token t) { vec_push(&L->tokens, t); }

static Span here(Lexer *L, const char *s) {
    return span_make(L->file->id, (int)(s - L->start), (int)(L->p - L->start));
}

/* decode escapes; returns arena string */
char *lex_decode_text(Arena *a, Str raw, bool *has_interp) {
    StrBuf b; sb_init(&b);
    bool interp = false;
    for (size_t i = 0; i < raw.n; i++) {
        char c = raw.p[i];
        if (c == '\\' && i + 1 < raw.n) {
            char e = raw.p[++i];
            switch (e) {
                case 'n': sb_putc(&b, '\n'); break;
                case 't': sb_putc(&b, '\t'); break;
                case 'r': sb_putc(&b, '\r'); break;
                case '0': sb_putc(&b, '\0'); break;
                case 'e': sb_putc(&b, '\x1b'); break;
                case '\\': sb_putc(&b, '\\'); break;
                case '"': sb_putc(&b, '"'); break;
                case '\'': sb_putc(&b, '\''); break;
                case '{': sb_putc(&b, '{'); break;
                case '}': sb_putc(&b, '}'); break;
                case 'u': {
                    if (i + 1 < raw.n && raw.p[i + 1] == '{') {
                        i += 2;
                        unsigned cp = 0;
                        while (i < raw.n && raw.p[i] != '}') {
                            char h = raw.p[i++];
                            cp = cp * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower(h) - 'a' + 10));
                        }
                        /* utf8 encode */
                        if (cp < 0x80) sb_putc(&b, (char)cp);
                        else if (cp < 0x800) { sb_putc(&b, (char)(0xC0 | (cp >> 6))); sb_putc(&b, (char)(0x80 | (cp & 0x3F))); }
                        else if (cp < 0x10000) { sb_putc(&b, (char)(0xE0 | (cp >> 12))); sb_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F))); sb_putc(&b, (char)(0x80 | (cp & 0x3F))); }
                        else { sb_putc(&b, (char)(0xF0 | (cp >> 18))); sb_putc(&b, (char)(0x80 | ((cp >> 12) & 0x3F))); sb_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F))); sb_putc(&b, (char)(0x80 | (cp & 0x3F))); }
                    }
                    break;
                }
                default: sb_putc(&b, e);
            }
        } else {
            /* `{{` and `}}` are literal braces, not interpolation */
            if (c == '{' && i + 1 < raw.n && raw.p[i + 1] == '{') { sb_puts(&b, "{{"); i++; continue; }
            if (c == '}' && i + 1 < raw.n && raw.p[i + 1] == '}') { sb_puts(&b, "}}"); i++; continue; }
            if (c == '{') interp = true;
            sb_putc(&b, c);
        }
    }
    if (!interp && b.data) {
        /* no holes: collapse the doubled braces here, the parser never sees it */
        size_t w = 0;
        for (size_t i = 0; i < b.len; i++) {
            if ((b.data[i] == '{' || b.data[i] == '}') && i + 1 < b.len && b.data[i + 1] == b.data[i]) i++;
            b.data[w++] = b.data[i];
        }
        b.len = w;
        b.data[w] = 0;
    }
    if (has_interp) *has_interp = interp;
    char *out = arena_strndup(a, b.data ? b.data : "", b.len);
    sb_free(&b);
    return out;
}

Token *lex_file(Arena *a, Interner *in, DiagBag *db, SourceFile *f, int *out_count) {
    Lexer L = { 0 };
    L.file = f; L.interner = in; L.arena = a; L.diags = db;
    L.start = f->src; L.p = f->src; L.end = f->src + f->len;
    vec_init(&L.tokens);
    bool nl_pending = false;

    while (L.p < L.end) {
        const char *s = L.p;
        char c = *L.p;

        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r') { L.p++; continue; }
        if (c == '\n') {
            L.p++;
            if (L.depth == 0 && L.tokens.len > 0 && !L.prev_joins) {
                Token t = { 0 };
                t.kind = T_NEWLINE; t.span = here(&L, s); t.text = str_make(s, 1);
                push_tok(&L, t);
                L.prev_joins = true; /* collapse runs of newlines */
            }
            nl_pending = true;
            continue;
        }
        /* comments: ~ line, ~~ doc, ~[ block ]~ */
        if (c == '~') {
            if (L.p + 1 < L.end && L.p[1] == '[') {
                L.p += 2;
                int nest = 1;
                while (L.p < L.end && nest > 0) {
                    if (L.p + 1 < L.end && L.p[0] == '~' && L.p[1] == '[') { nest++; L.p += 2; }
                    else if (L.p + 1 < L.end && L.p[0] == ']' && L.p[1] == '~') { nest--; L.p += 2; }
                    else L.p++;
                }
                continue;
            }
            /* `~~` only documents when it starts its own line; after code
               on the same line it is an ordinary trailing comment */
            bool own_line = true;
            for (const char *q = L.p; q > L.start; q--) {
                char pc = q[-1];
                if (pc == '\n') break;
                if (pc != ' ' && pc != '\t' && pc != '\r') { own_line = false; break; }
            }
            bool doc = own_line && (L.p + 1 < L.end && L.p[1] == '~');
            const char *cs = L.p + (doc ? 2 : 1);
            while (L.p < L.end && *L.p != '\n') L.p++;
            if (doc) {
                Token t = { 0 };
                while (cs < L.p && (*cs == ' ' || *cs == '\t')) cs++;
                t.kind = T_DOC; t.span = here(&L, s);
                t.text = str_make(cs, (size_t)(L.p - cs));
                t.sval = arena_strndup(a, cs, (size_t)(L.p - cs));
                t.nl_before = nl_pending; nl_pending = false;
                push_tok(&L, t);
                L.prev_joins = true;
            }
            continue;
        }

        Token t = { 0 };
        t.nl_before = nl_pending;
        nl_pending = false;
        bool joins = false;

        /* numbers */
        if (isdigit((unsigned char)c)) {
            if (c == '0' && L.p + 1 < L.end && (L.p[1] == 'x' || L.p[1] == 'X' ||
                                                L.p[1] == 'b' || L.p[1] == 'B' ||
                                                L.p[1] == 'o' || L.p[1] == 'O')) {
                int base = (L.p[1] == 'x' || L.p[1] == 'X') ? 16 : (L.p[1] == 'b' || L.p[1] == 'B') ? 2 : 8;
                L.p += 2;
                int64_t v = 0;
                while (L.p < L.end && (isalnum((unsigned char)*L.p) || *L.p == '_')) {
                    if (*L.p == '_') { L.p++; continue; }
                    int d = isdigit((unsigned char)*L.p) ? *L.p - '0' : (tolower(*L.p) - 'a' + 10);
                    if (d >= base) break;
                    v = v * base + d;
                    L.p++;
                }
                t.kind = T_INT; t.ival = v;
            } else {
                bool isnum = false;
                while (L.p < L.end && (isdigit((unsigned char)*L.p) || *L.p == '_')) L.p++;
                if (L.p < L.end && *L.p == '.' && L.p + 1 < L.end && isdigit((unsigned char)L.p[1])) {
                    isnum = true; L.p++;
                    while (L.p < L.end && (isdigit((unsigned char)*L.p) || *L.p == '_')) L.p++;
                }
                if (L.p < L.end && (*L.p == 'e' || *L.p == 'E')) {
                    const char *save = L.p;
                    L.p++;
                    if (L.p < L.end && (*L.p == '+' || *L.p == '-')) L.p++;
                    if (L.p < L.end && isdigit((unsigned char)*L.p)) {
                        isnum = true;
                        while (L.p < L.end && isdigit((unsigned char)*L.p)) L.p++;
                    } else L.p = save;
                }
                char buf[64]; int bn = 0;
                for (const char *q = s; q < L.p && bn < 63; q++) if (*q != '_') buf[bn++] = *q;
                buf[bn] = 0;
                if (isnum) { t.kind = T_NUM; t.nval = strtod(buf, NULL); }
                else { t.kind = T_INT; t.ival = strtoll(buf, NULL, 10); }
            }
            t.span = here(&L, s);
            t.text = str_make(s, (size_t)(L.p - s));
            push_tok(&L, t); L.prev_joins = false;
            continue;
        }

        /* raw text literal: r"""..."""  — no escapes, no {holes}, braces and
           backslashes mean themselves. Made for pages, styles and scripts. */
        if (c == 'r' && L.p + 3 < L.end && L.p[1] == '"' && L.p[2] == '"' && L.p[3] == '"') {
            L.p += 4;
            const char *body = L.p;
            while (L.p + 2 < L.end && !(L.p[0] == '"' && L.p[1] == '"' && L.p[2] == '"')) L.p++;
            size_t n = (size_t)(L.p - body);
            t.sval = arena_strndup(a, body, n);
            t.has_interp = false;
            t.text = str_make(body, n);
            if (L.p + 2 < L.end) L.p += 3; else L.p = L.end;
            t.kind = T_TEXT;
            t.span = here(&L, s);
            push_tok(&L, t); L.prev_joins = false;
            continue;
        }

        /* identifiers / keywords */
        if (ident_start((unsigned char)c)) {
            while (L.p < L.end && ident_cont((unsigned char)*L.p)) L.p++;
            size_t n = (size_t)(L.p - s);
            TokKind k = keyword_lookup(s, n);
            t.kind = (n == 1 && *s == '_') ? T_UNDERSCORE : k;
            t.name = intern(in, str_make(s, n));
            t.span = here(&L, s);
            t.text = str_make(s, n);
            push_tok(&L, t);
            /* keywords that expect an operand continue the line */
            L.prev_joins = (t.kind == T_AND || t.kind == T_OR || t.kind == T_NOT ||
                            t.kind == T_IN || t.kind == T_AS || t.kind == T_IS ||
                            t.kind == T_ELSE);
            continue;
        }

        /* text literal */
        if (c == '"') {
            bool raw3 = (L.p + 2 < L.end && L.p[1] == '"' && L.p[2] == '"');
            const char *body;
            if (raw3) {
                L.p += 3;
                body = L.p;
                while (L.p + 2 < L.end && !(L.p[0] == '"' && L.p[1] == '"' && L.p[2] == '"')) L.p++;
                t.sval = lex_decode_text(a, str_make(body, (size_t)(L.p - body)), &t.has_interp);
                t.text = str_make(body, (size_t)(L.p - body));
                if (L.p + 2 < L.end) L.p += 3; else L.p = L.end;
            } else {
                L.p++;
                body = L.p;
                int hole = 0;            /* quotes may nest inside {holes} */
                while (L.p < L.end) {
                    if (*L.p == '\\' && L.p + 1 < L.end) { L.p += 2; continue; }
                    if (*L.p == '\n') break;
                    if (*L.p == '{' && L.p + 1 < L.end && L.p[1] == '{') { L.p += 2; continue; }
                    if (*L.p == '}' && L.p + 1 < L.end && L.p[1] == '}') { L.p += 2; continue; }
                    if (*L.p == '{') hole++;
                    else if (*L.p == '}' && hole > 0) hole--;
                    else if (*L.p == '"' && hole == 0) break;
                    L.p++;
                }
                if (L.p >= L.end || *L.p != '"') {
                    Diag *d = diag_new(db, DIAG_ERROR, "E0003", "Unterminated text literal");
                    diag_label(db, d, here(&L, s), true, "this text never closes");
                    diag_fix(db, d, "add a closing \" at the end of the text");
                    diag_fix(db, d, "use \"\"\" ... \"\"\" for text that spans several lines");
                } 
                t.sval = lex_decode_text(a, str_make(body, (size_t)(L.p - body)), &t.has_interp);
                t.text = str_make(body, (size_t)(L.p - body));
                if (L.p < L.end) L.p++;
            }
            t.kind = T_TEXT;
            t.span = here(&L, s);
            push_tok(&L, t); L.prev_joins = false;
            continue;
        }

        /* byte literal */
        if (c == '\'') {
            L.p++;
            const char *body = L.p;
            while (L.p < L.end && *L.p != '\'') { if (*L.p == '\\') L.p++; L.p++; }
            char *dec = lex_decode_text(a, str_make(body, (size_t)(L.p - body)), NULL);
            if (L.p < L.end) L.p++;
            t.kind = T_BYTE;
            t.ival = (int64_t)(unsigned char)dec[0];
            t.span = here(&L, s);
            t.text = str_make(body, (size_t)(L.p - body));
            push_tok(&L, t); L.prev_joins = false;
            continue;
        }

        /* operators */
        #define OP2(a_, b_, k_) if (c == a_ && L.p + 1 < L.end && L.p[1] == b_) { L.p += 2; t.kind = k_; goto done; }
        #define OP3(a_, b_, c_, k_) if (c == a_ && L.p + 2 < L.end && L.p[1] == b_ && L.p[2] == c_) { L.p += 3; t.kind = k_; goto done; }
        #define OP1(a_, k_) if (c == a_) { L.p += 1; t.kind = k_; goto done; }

        OP3('.', '.', '.', T_RANGE_IN)
        OP2('.', '.', T_RANGE)
        OP2('-', '>', T_ARROW)
        OP2('=', '>', T_FATARROW)
        OP2('|', '>', T_PIPE)
        OP2('?', '?', T_QQ)
        OP2('?', '.', T_QDOT)
        OP2('=', '=', T_EQ)
        OP2('!', '=', T_NE)
        OP2('<', '=', T_LE)
        OP2('>', '=', T_GE)
        OP2('<', '<', T_SHL)
        OP2('>', '>', T_SHR)
        OP2('*', '*', T_POW)
        OP2('+', '=', T_PLUSEQ)
        OP2('-', '=', T_MINUSEQ)
        OP2('*', '=', T_STAREQ)
        OP2('/', '=', T_SLASHEQ)
        OP2('%', '=', T_PCTEQ)
        OP1('(', T_LPAREN) OP1(')', T_RPAREN)
        OP1('{', T_LBRACE) OP1('}', T_RBRACE)
        OP1('[', T_LBRACKET) OP1(']', T_RBRACKET)
        OP1(',', T_COMMA) OP1(';', T_SEMI) OP1(':', T_COLON) OP1('.', T_DOT)
        OP1('@', T_AT) OP1('?', T_QUESTION) OP1('!', T_BANG) OP1('=', T_ASSIGN)
        OP1('+', T_PLUS) OP1('-', T_MINUS) OP1('*', T_STAR) OP1('/', T_SLASH)
        OP1('%', T_PERCENT) OP1('<', T_LT) OP1('>', T_GT)
        OP1('&', T_AMP) OP1('|', T_BAR) OP1('^', T_CARET)
        #undef OP1
        #undef OP2
        #undef OP3
        {
            Diag *d = diag_new(db, DIAG_ERROR, "E0001", "Unexpected character '%c' in source", c);
            diag_label(db, d, span_make(f->id, (int)(s - L.start), (int)(s - L.start) + 1), true,
                       "SPRFST does not use this character here");
            diag_note(db, d, "SPRFST comments start with ~ (line) or ~[ ... ]~ (block)");
            L.p++;
            continue;
        }
    done:
        t.span = here(&L, s);
        t.text = str_make(s, (size_t)(L.p - s));
        switch (t.kind) {
            case T_LPAREN: case T_LBRACKET: L.depth++; joins = true; break;
            case T_RPAREN: case T_RBRACKET: if (L.depth > 0) L.depth--; joins = false; break;
            case T_RBRACE: joins = false; break;
            case T_COMMA: case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH:
            case T_PERCENT: case T_POW: case T_ASSIGN: case T_PLUSEQ: case T_MINUSEQ:
            case T_STAREQ: case T_SLASHEQ: case T_PCTEQ: case T_EQ: case T_NE:
            case T_LT: case T_LE: case T_GT: case T_GE: case T_AMP: case T_BAR:
            case T_CARET: case T_SHL: case T_SHR: case T_PIPE: case T_ARROW:
            case T_FATARROW: case T_QQ: case T_QDOT: case T_COLON: case T_DOT:
            case T_AT: case T_LBRACE:
                joins = true; break;
            default: joins = false;
        }
        push_tok(&L, t);
        L.prev_joins = joins;
    }

    Token eof = { 0 };
    eof.kind = T_NEWLINE;
    eof.span = span_make(f->id, (int)f->len, (int)f->len);
    if (L.tokens.len && vec_last(&L.tokens).kind != T_NEWLINE) push_tok(&L, eof);
    eof.kind = T_EOF;
    eof.text = str_make("", 0);
    push_tok(&L, eof);

    *out_count = L.tokens.len;
    /* move the token array into the arena so it lives as long as the AST */
    Token *arr = NEWN(a, Token, L.tokens.len);
    memcpy(arr, L.tokens.items, sizeof(Token) * (size_t)L.tokens.len);
    vec_free(&L.tokens);
    return arr;
}
