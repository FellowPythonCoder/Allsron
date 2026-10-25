/* ========================================================================
   Ember DB — the embedded database behind the `db` module.

   A real, self-contained relational store: typed columns, a SQL subset
   (CREATE/DROP/INSERT/SELECT/UPDATE/DELETE), WHERE expressions with
   AND/OR/comparisons/LIKE, ORDER BY, LIMIT, transactions by snapshot, and
   a durable line-oriented file format.  No external engine is linked.
   ======================================================================== */

typedef enum { DBV_NIL, DBV_INT, DBV_NUM, DBV_TEXT } DbKind;
typedef struct { DbKind kind; int64_t i; double n; char *t; } DbVal;

typedef struct {
    char   *name;
    DbKind  type;
} DbCol;

typedef struct {
    char   *name;
    DbCol  *cols;
    int     ncols;
    DbVal **rows;         /* rows[r][c] */
    int     nrows, cap;
} DbTable;

typedef struct {
    bool      open;
    char     *path;
    DbTable **tables;
    int       ntables, tcap;
    bool      in_tx;
    char     *tx_snapshot;   /* serialized copy taken at BEGIN */
} DbHandle;

#define DB_MAX 32
static DbHandle g_dbs[DB_MAX];

/* --------------------------------------------------------------- values */
static void dbv_clear(DbVal *v) { if (v->kind == DBV_TEXT) free(v->t); v->kind = DBV_NIL; v->t = NULL; }

static DbVal dbv_text(const char *s, int n) {
    DbVal v = { DBV_TEXT, 0, 0, NULL };
    v.t = malloc((size_t)n + 1);
    memcpy(v.t, s, (size_t)n);
    v.t[n] = 0;
    return v;
}
static DbVal dbv_copy(DbVal s) {
    DbVal v = s;
    if (s.kind == DBV_TEXT && s.t) { v.t = malloc(strlen(s.t) + 1); strcpy(v.t, s.t); }
    return v;
}
static int dbv_cmp(DbVal a, DbVal b) {
    if (a.kind == DBV_TEXT || b.kind == DBV_TEXT) {
        const char *x = a.kind == DBV_TEXT ? a.t : "";
        const char *y = b.kind == DBV_TEXT ? b.t : "";
        int r = strcmp(x, y);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if (a.kind == DBV_NIL && b.kind == DBV_NIL) return 0;
    if (a.kind == DBV_NIL) return -1;
    if (b.kind == DBV_NIL) return 1;
    double x = a.kind == DBV_INT ? (double)a.i : a.n;
    double y = b.kind == DBV_INT ? (double)b.i : b.n;
    return x < y ? -1 : x > y ? 1 : 0;
}
static Value dbv_to_value(VM *vm, DbVal v) {
    switch (v.kind) {
        case DBV_INT: return v_int(v.i);
        case DBV_NUM: return v_num(v.n);
        case DBV_TEXT: return v_obj((Obj *)vm_text_cstr(vm, v.t ? v.t : ""));
        default: return v_nil();
    }
}

/* ------------------------------------------------------------- tokenizer */
typedef struct {
    const char *p;
    char tok[512];
    int  kind;            /* 0 end, 1 word, 2 number, 3 string, 4 punct */
    bool quoted;
} SqlLex;

static void sql_next(SqlLex *L) {
    while (isspace((unsigned char)*L->p)) L->p++;
    L->quoted = false;
    if (!*L->p) { L->kind = 0; L->tok[0] = 0; return; }
    char c = *L->p;
    if (isalpha((unsigned char)c) || c == '_') {
        int n = 0;
        while ((isalnum((unsigned char)*L->p) || *L->p == '_') && n < 511) L->tok[n++] = *L->p++;
        L->tok[n] = 0;
        L->kind = 1;
        return;
    }
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)L->p[1]))) {
        int n = 0;
        if (*L->p == '-') L->tok[n++] = *L->p++;
        while ((isdigit((unsigned char)*L->p) || *L->p == '.') && n < 511) L->tok[n++] = *L->p++;
        L->tok[n] = 0;
        L->kind = 2;
        return;
    }
    if (c == '\'' || c == '"') {
        char q = c;
        L->p++;
        int n = 0;
        while (*L->p && n < 511) {
            if (*L->p == q) { if (L->p[1] == q) { L->tok[n++] = q; L->p += 2; continue; } break; }
            L->tok[n++] = *L->p++;
        }
        if (*L->p == q) L->p++;
        L->tok[n] = 0;
        L->kind = 3;
        L->quoted = true;
        return;
    }
    /* punctuation, possibly two-char */
    int n = 0;
    L->tok[n++] = *L->p++;
    if ((L->tok[0] == '<' && (*L->p == '=' || *L->p == '>')) ||
        (L->tok[0] == '>' && *L->p == '=') ||
        (L->tok[0] == '!' && *L->p == '=')) L->tok[n++] = *L->p++;
    L->tok[n] = 0;
    L->kind = 4;
}
static bool sql_is(SqlLex *L, const char *w) { return L->kind == 1 && strcasecmp(L->tok, w) == 0; }
static bool sql_eat(SqlLex *L, const char *w) { if (sql_is(L, w)) { sql_next(L); return true; } return false; }
static bool sql_punct(SqlLex *L, const char *w) { if (L->kind == 4 && strcmp(L->tok, w) == 0) { sql_next(L); return true; } return false; }

/* --------------------------------------------------------------- tables */
static DbTable *db_find(DbHandle *h, const char *name) {
    for (int i = 0; i < h->ntables; i++) if (strcasecmp(h->tables[i]->name, name) == 0) return h->tables[i];
    return NULL;
}
static int db_col_index(DbTable *t, const char *name) {
    for (int i = 0; i < t->ncols; i++) if (strcasecmp(t->cols[i].name, name) == 0) return i;
    return -1;
}
static void db_add_table(DbHandle *h, DbTable *t) {
    if (h->ntables == h->tcap) {
        h->tcap = h->tcap ? h->tcap * 2 : 4;
        h->tables = realloc(h->tables, sizeof(DbTable *) * (size_t)h->tcap);
    }
    h->tables[h->ntables++] = t;
}
static void db_free_table(DbTable *t) {
    for (int r = 0; r < t->nrows; r++) {
        for (int c = 0; c < t->ncols; c++) dbv_clear(&t->rows[r][c]);
        free(t->rows[r]);
    }
    free(t->rows);
    for (int c = 0; c < t->ncols; c++) free(t->cols[c].name);
    free(t->cols);
    free(t->name);
    free(t);
}
static void db_table_push(DbTable *t, DbVal *row) {
    if (t->nrows == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->rows = realloc(t->rows, sizeof(DbVal *) * (size_t)t->cap);
    }
    t->rows[t->nrows++] = row;
}

/* ------------------------------------------------------- persistence */
static void db_escape(StrBuf *b, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '\t' || *p == '\n') { sb_putc(b, '\\'); sb_putc(b, *p == '\t' ? 't' : *p == '\n' ? 'n' : '\\'); }
        else sb_putc(b, *p);
    }
}
static char *db_serialize(DbHandle *h) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, "emberdb 1\n");
    for (int i = 0; i < h->ntables; i++) {
        DbTable *t = h->tables[i];
        sb_printf(&b, "table\t%s\t%d\n", t->name, t->ncols);
        for (int c = 0; c < t->ncols; c++)
            sb_printf(&b, "col\t%s\t%d\n", t->cols[c].name, (int)t->cols[c].type);
        for (int r = 0; r < t->nrows; r++) {
            sb_puts(&b, "row");
            for (int c = 0; c < t->ncols; c++) {
                sb_putc(&b, '\t');
                DbVal v = t->rows[r][c];
                switch (v.kind) {
                    case DBV_NIL: sb_puts(&b, "~"); break;
                    case DBV_INT: sb_printf(&b, "i%lld", (long long)v.i); break;
                    case DBV_NUM: sb_printf(&b, "n%.17g", v.n); break;
                    case DBV_TEXT: sb_putc(&b, 's'); db_escape(&b, v.t ? v.t : ""); break;
                }
            }
            sb_putc(&b, '\n');
        }
    }
    return b.data ? b.data : strdup("emberdb 1\n");
}
static void db_wipe(DbHandle *h) {
    for (int i = 0; i < h->ntables; i++) db_free_table(h->tables[i]);
    h->ntables = 0;
}
static char *db_unescape(const char *s) {
    char *out = malloc(strlen(s) + 1);
    int n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) { p++; out[n++] = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p; }
        else out[n++] = *p;
    }
    out[n] = 0;
    return out;
}
static void db_load_text(DbHandle *h, const char *text) {
    db_wipe(h);
    DbTable *cur = NULL;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = malloc(len + 1);
        memcpy(line, p, len);
        line[len] = 0;
        char *tab = strchr(line, '\t');
        if (tab && strncmp(line, "table", 5) == 0) {
            char *name = tab + 1;
            char *t2 = strchr(name, '\t');
            if (t2) *t2 = 0;
            cur = calloc(1, sizeof(DbTable));
            cur->name = strdup(name);
            db_add_table(h, cur);
        } else if (tab && strncmp(line, "col", 3) == 0 && cur) {
            char *name = tab + 1;
            char *t2 = strchr(name, '\t');
            int ty = DBV_TEXT;
            if (t2) { *t2 = 0; ty = atoi(t2 + 1); }
            cur->cols = realloc(cur->cols, sizeof(DbCol) * (size_t)(cur->ncols + 1));
            cur->cols[cur->ncols].name = strdup(name);
            cur->cols[cur->ncols].type = (DbKind)ty;
            cur->ncols++;
        } else if (tab && strncmp(line, "row", 3) == 0 && cur && cur->ncols) {
            DbVal *row = calloc((size_t)cur->ncols, sizeof(DbVal));
            char *field = tab + 1;
            for (int c = 0; c < cur->ncols; c++) {
                char *t2 = strchr(field, '\t');
                if (t2) *t2 = 0;
                if (field[0] == 'i') { row[c].kind = DBV_INT; row[c].i = strtoll(field + 1, NULL, 10); }
                else if (field[0] == 'n') { row[c].kind = DBV_NUM; row[c].n = strtod(field + 1, NULL); }
                else if (field[0] == 's') { char *u = db_unescape(field + 1); row[c] = dbv_text(u, (int)strlen(u)); free(u); }
                else row[c].kind = DBV_NIL;
                if (!t2) { for (int k = c + 1; k < cur->ncols; k++) row[k].kind = DBV_NIL; break; }
                field = t2 + 1;
            }
            db_table_push(cur, row);
        }
        free(line);
        if (!eol) break;
        p = eol + 1;
    }
}
static void db_flush(DbHandle *h) {
    if (!h->path || !*h->path || strcmp(h->path, ":memory:") == 0) return;
    char *s = db_serialize(h);
    write_file_bytes(h->path, s, strlen(s));
    free(s);
}

/* ------------------------------------------------------------- open/close */
static int db_open(const char *path) {
    for (int i = 0; i < DB_MAX; i++) {
        if (g_dbs[i].open) continue;
        DbHandle *h = &g_dbs[i];
        memset(h, 0, sizeof *h);
        h->open = true;
        h->path = strdup(path && *path ? path : ":memory:");
        if (strcmp(h->path, ":memory:") != 0 && file_exists(h->path)) {
            size_t n = 0;
            char *text = read_file(h->path, &n);
            if (text) { db_load_text(h, text); free(text); }
        }
        return i;
    }
    return -1;
}
static void db_close(int id) {
    if (id < 0 || id >= DB_MAX || !g_dbs[id].open) return;
    DbHandle *h = &g_dbs[id];
    db_flush(h);
    db_wipe(h);
    free(h->tables);
    free(h->path);
    free(h->tx_snapshot);
    memset(h, 0, sizeof *h);
}
static void db_shutdown(void) { for (int i = 0; i < DB_MAX; i++) if (g_dbs[i].open) db_close(i); }
static void db_begin(int id) {
    if (id < 0 || id >= DB_MAX || !g_dbs[id].open) return;
    DbHandle *h = &g_dbs[id];
    free(h->tx_snapshot);
    h->tx_snapshot = db_serialize(h);
    h->in_tx = true;
}
static void db_commit(int id) {
    if (id < 0 || id >= DB_MAX || !g_dbs[id].open) return;
    DbHandle *h = &g_dbs[id];
    free(h->tx_snapshot);
    h->tx_snapshot = NULL;
    h->in_tx = false;
    db_flush(h);
}
static void db_rollback(int id) {
    if (id < 0 || id >= DB_MAX || !g_dbs[id].open) return;
    DbHandle *h = &g_dbs[id];
    if (h->tx_snapshot) { db_load_text(h, h->tx_snapshot); free(h->tx_snapshot); h->tx_snapshot = NULL; }
    h->in_tx = false;
    db_flush(h);
}

/* ------------------------------------------------------- WHERE evaluation */
typedef struct {
    int   col;            /* -1 when the operand is a literal */
    DbVal lit;
} DbOperand;

typedef struct {
    DbOperand lhs, rhs;
    char op[4];
    int  join;            /* 0 none, 1 AND, 2 OR */
} DbCond;

#define DB_MAX_COND 16

static bool db_like(const char *s, const char *pat) {
    /* '%' = any run, '_' = any single character */
    if (!*pat) return !*s;
    if (*pat == '%') {
        for (const char *p = s; ; p++) { if (db_like(p, pat + 1)) return true; if (!*p) return false; }
    }
    if (!*s) return false;
    if (*pat == '_' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) return db_like(s + 1, pat + 1);
    return false;
}

static DbOperand db_operand(SqlLex *L, DbTable *t) {
    DbOperand o = { -1, { DBV_NIL, 0, 0, NULL } };
    if (L->kind == 1 && !L->quoted) {
        int ci = db_col_index(t, L->tok);
        if (ci >= 0) { o.col = ci; sql_next(L); return o; }
        if (strcasecmp(L->tok, "null") == 0) { sql_next(L); return o; }
        if (strcasecmp(L->tok, "true") == 0) { o.lit.kind = DBV_INT; o.lit.i = 1; sql_next(L); return o; }
        if (strcasecmp(L->tok, "false") == 0) { o.lit.kind = DBV_INT; o.lit.i = 0; sql_next(L); return o; }
        o.lit = dbv_text(L->tok, (int)strlen(L->tok));
        sql_next(L);
        return o;
    }
    if (L->kind == 2) {
        if (strchr(L->tok, '.')) { o.lit.kind = DBV_NUM; o.lit.n = strtod(L->tok, NULL); }
        else { o.lit.kind = DBV_INT; o.lit.i = strtoll(L->tok, NULL, 10); }
        sql_next(L);
        return o;
    }
    if (L->kind == 3) { o.lit = dbv_text(L->tok, (int)strlen(L->tok)); sql_next(L); return o; }
    sql_next(L);
    return o;
}

static int db_parse_where(SqlLex *L, DbTable *t, DbCond *conds) {
    int n = 0;
    for (;;) {
        if (n >= DB_MAX_COND) break;
        DbCond c;
        memset(&c, 0, sizeof c);
        c.lhs = db_operand(L, t);
        if (L->kind == 4) { snprintf(c.op, sizeof c.op, "%.3s", L->tok); sql_next(L); }
        else if (sql_is(L, "like")) { snprintf(c.op, sizeof c.op, "~"); sql_next(L); }
        else if (sql_is(L, "is")) { snprintf(c.op, sizeof c.op, "="); sql_next(L); sql_eat(L, "not"); }
        else snprintf(c.op, sizeof c.op, "=");
        c.rhs = db_operand(L, t);
        if (sql_is(L, "and")) { c.join = 1; sql_next(L); }
        else if (sql_is(L, "or")) { c.join = 2; sql_next(L); }
        conds[n++] = c;
        if (!c.join) break;
    }
    return n;
}

static bool db_cond_one(DbCond *c, DbVal *row) {
    DbVal a = c->lhs.col >= 0 ? row[c->lhs.col] : c->lhs.lit;
    DbVal b = c->rhs.col >= 0 ? row[c->rhs.col] : c->rhs.lit;
    if (c->op[0] == '~') return db_like(a.kind == DBV_TEXT && a.t ? a.t : "", b.kind == DBV_TEXT && b.t ? b.t : "");
    int r = dbv_cmp(a, b);
    if (strcmp(c->op, "=") == 0 || strcmp(c->op, "==") == 0) return r == 0;
    if (strcmp(c->op, "!=") == 0 || strcmp(c->op, "<>") == 0) return r != 0;
    if (strcmp(c->op, "<") == 0) return r < 0;
    if (strcmp(c->op, ">") == 0) return r > 0;
    if (strcmp(c->op, "<=") == 0) return r <= 0;
    if (strcmp(c->op, ">=") == 0) return r >= 0;
    return false;
}
static bool db_match(DbCond *conds, int n, DbVal *row) {
    if (!n) return true;
    bool acc = db_cond_one(&conds[0], row);
    for (int i = 1; i < n; i++) {
        bool v = db_cond_one(&conds[i], row);
        if (conds[i-1].join == 2) acc = acc || v;
        else acc = acc && v;
    }
    return acc;
}
static void db_free_conds(DbCond *c, int n) {
    for (int i = 0; i < n; i++) { if (c[i].lhs.col < 0) dbv_clear(&c[i].lhs.lit); if (c[i].rhs.col < 0) dbv_clear(&c[i].rhs.lit); }
}

/* --------------------------------------------------------------- execute */
static DbVal db_coerce(DbVal v, DbKind want) {
    if (v.kind == want || want == DBV_NIL) return v;
    DbVal out = { want, 0, 0, NULL };
    if (want == DBV_TEXT) {
        char buf[64];
        if (v.kind == DBV_INT) snprintf(buf, sizeof buf, "%lld", (long long)v.i);
        else if (v.kind == DBV_NUM) snprintf(buf, sizeof buf, "%g", v.n);
        else buf[0] = 0;
        out = dbv_text(buf, (int)strlen(buf));
    } else if (want == DBV_INT) {
        out.i = v.kind == DBV_NUM ? (int64_t)v.n : v.kind == DBV_TEXT && v.t ? strtoll(v.t, NULL, 10) : 0;
    } else if (want == DBV_NUM) {
        out.n = v.kind == DBV_INT ? (double)v.i : v.kind == DBV_TEXT && v.t ? strtod(v.t, NULL) : 0;
    }
    dbv_clear(&v);
    return out;
}

static int db_exec(VM *vm, int id, const char *sql, Value *rows_out) {
    if (id < 0 || id >= DB_MAX || !g_dbs[id].open) {
        if (vm) vm_runtime_error(vm, "db handle %d is not open — call db.open first", id);
        return -1;
    }
    DbHandle *h = &g_dbs[id];
    SqlLex L = { sql, { 0 }, 0, false };
    sql_next(&L);

    /* ---------------------------------------------------- CREATE TABLE */
    if (sql_eat(&L, "create")) {
        bool ifnot = false;
        sql_eat(&L, "table");
        if (sql_eat(&L, "if")) { sql_eat(&L, "not"); sql_eat(&L, "exists"); ifnot = true; }
        if (L.kind != 1) return -1;
        char tname[256];
        snprintf(tname, sizeof tname, "%s", L.tok);
        sql_next(&L);
        if (db_find(h, tname)) { if (ifnot) return 0; if (vm) vm_runtime_error(vm, "table '%s' already exists", tname); return -1; }
        DbTable *t = calloc(1, sizeof(DbTable));
        t->name = strdup(tname);
        if (sql_punct(&L, "(")) {
            while (L.kind && !(L.kind == 4 && strcmp(L.tok, ")") == 0)) {
                if (L.kind != 1) { sql_next(&L); continue; }
                char cname[256];
                snprintf(cname, sizeof cname, "%s", L.tok);
                sql_next(&L);
                DbKind ty = DBV_TEXT;
                if (L.kind == 1) {
                    if (strcasecmp(L.tok, "int") == 0 || strcasecmp(L.tok, "integer") == 0) ty = DBV_INT;
                    else if (strcasecmp(L.tok, "num") == 0 || strcasecmp(L.tok, "real") == 0 ||
                             strcasecmp(L.tok, "float") == 0 || strcasecmp(L.tok, "double") == 0) ty = DBV_NUM;
                    else ty = DBV_TEXT;
                    sql_next(&L);
                }
                while (L.kind == 1) sql_next(&L);          /* skip PRIMARY KEY / NOT NULL ... */
                t->cols = realloc(t->cols, sizeof(DbCol) * (size_t)(t->ncols + 1));
                t->cols[t->ncols].name = strdup(cname);
                t->cols[t->ncols].type = ty;
                t->ncols++;
                if (!sql_punct(&L, ",")) break;
            }
            sql_punct(&L, ")");
        }
        db_add_table(h, t);
        if (!h->in_tx) db_flush(h);
        return 0;
    }

    /* ------------------------------------------------------- DROP TABLE */
    if (sql_eat(&L, "drop")) {
        sql_eat(&L, "table");
        sql_eat(&L, "if");
        sql_eat(&L, "exists");
        if (L.kind != 1) return -1;
        for (int i = 0; i < h->ntables; i++)
            if (strcasecmp(h->tables[i]->name, L.tok) == 0) {
                db_free_table(h->tables[i]);
                for (int k = i; k < h->ntables - 1; k++) h->tables[k] = h->tables[k + 1];
                h->ntables--;
                if (!h->in_tx) db_flush(h);
                return 1;
            }
        return 0;
    }

    /* ------------------------------------------------------------ INSERT */
    if (sql_eat(&L, "insert")) {
        sql_eat(&L, "into");
        if (L.kind != 1) return -1;
        DbTable *t = db_find(h, L.tok);
        if (!t) { if (vm) vm_runtime_error(vm, "no such table: %s", L.tok); return -1; }
        sql_next(&L);
        int order[64];
        int ncols = 0;
        if (sql_punct(&L, "(")) {
            while (L.kind && !(L.kind == 4 && strcmp(L.tok, ")") == 0)) {
                if (L.kind == 1 && ncols < 64) order[ncols++] = db_col_index(t, L.tok);
                sql_next(&L);
                sql_punct(&L, ",");
            }
            sql_punct(&L, ")");
        } else {
            for (int i = 0; i < t->ncols && i < 64; i++) order[ncols++] = i;
        }
        sql_eat(&L, "values");
        int inserted = 0;
        while (sql_punct(&L, "(")) {
            DbVal *row = calloc((size_t)t->ncols, sizeof(DbVal));
            int k = 0;
            while (L.kind && !(L.kind == 4 && strcmp(L.tok, ")") == 0)) {
                DbOperand o = db_operand(&L, t);
                DbVal v = o.col >= 0 ? dbv_text(t->cols[o.col].name, (int)strlen(t->cols[o.col].name)) : o.lit;
                if (k < ncols && order[k] >= 0 && order[k] < t->ncols)
                    row[order[k]] = db_coerce(v, t->cols[order[k]].type);
                else dbv_clear(&v);
                k++;
                sql_punct(&L, ",");
            }
            sql_punct(&L, ")");
            db_table_push(t, row);
            inserted++;
            if (!sql_punct(&L, ",")) break;
        }
        if (!h->in_tx) db_flush(h);
        return inserted;
    }

    /* ------------------------------------------------------------ DELETE */
    if (sql_eat(&L, "delete")) {
        sql_eat(&L, "from");
        if (L.kind != 1) return -1;
        DbTable *t = db_find(h, L.tok);
        if (!t) { if (vm) vm_runtime_error(vm, "no such table: %s", L.tok); return -1; }
        sql_next(&L);
        DbCond conds[DB_MAX_COND];
        int nc = 0;
        if (sql_eat(&L, "where")) nc = db_parse_where(&L, t, conds);
        int removed = 0;
        for (int r = 0; r < t->nrows; ) {
            if (db_match(conds, nc, t->rows[r])) {
                for (int c = 0; c < t->ncols; c++) dbv_clear(&t->rows[r][c]);
                free(t->rows[r]);
                for (int k = r; k < t->nrows - 1; k++) t->rows[k] = t->rows[k + 1];
                t->nrows--;
                removed++;
            } else r++;
        }
        db_free_conds(conds, nc);
        if (!h->in_tx) db_flush(h);
        return removed;
    }

    /* ------------------------------------------------------------ UPDATE */
    if (sql_eat(&L, "update")) {
        if (L.kind != 1) return -1;
        DbTable *t = db_find(h, L.tok);
        if (!t) { if (vm) vm_runtime_error(vm, "no such table: %s", L.tok); return -1; }
        sql_next(&L);
        sql_eat(&L, "set");
        int cols[32];
        DbVal vals[32];
        int nset = 0;
        while (L.kind == 1 && nset < 32) {
            int ci = db_col_index(t, L.tok);
            sql_next(&L);
            sql_punct(&L, "=");
            DbOperand o = db_operand(&L, t);
            if (ci >= 0) { cols[nset] = ci; vals[nset] = db_coerce(o.col >= 0 ? (DbVal){ DBV_NIL, 0, 0, NULL } : o.lit, t->cols[ci].type); nset++; }
            else if (o.col < 0) dbv_clear(&o.lit);
            if (!sql_punct(&L, ",")) break;
        }
        DbCond conds[DB_MAX_COND];
        int nc = 0;
        if (sql_eat(&L, "where")) nc = db_parse_where(&L, t, conds);
        int changed = 0;
        for (int r = 0; r < t->nrows; r++) {
            if (!db_match(conds, nc, t->rows[r])) continue;
            for (int i = 0; i < nset; i++) {
                dbv_clear(&t->rows[r][cols[i]]);
                t->rows[r][cols[i]] = dbv_copy(vals[i]);
            }
            changed++;
        }
        for (int i = 0; i < nset; i++) dbv_clear(&vals[i]);
        db_free_conds(conds, nc);
        if (!h->in_tx) db_flush(h);
        return changed;
    }

    /* ------------------------------------------------------------ SELECT */
    if (sql_eat(&L, "select")) {
        char want[32][128];
        int nwant = 0;
        bool star = false;
        bool count_star = false;
        if (sql_punct(&L, "*")) star = true;
        else if (sql_is(&L, "count")) {
            sql_next(&L);
            if (sql_punct(&L, "(")) { sql_punct(&L, "*"); sql_punct(&L, ")"); }
            count_star = true;
        } else {
            while (L.kind == 1 && nwant < 32) {
                snprintf(want[nwant++], 128, "%s", L.tok);
                sql_next(&L);
                if (!sql_punct(&L, ",")) break;
            }
        }
        if (!sql_eat(&L, "from")) { if (vm) vm_runtime_error(vm, "SELECT needs a FROM clause"); return -1; }
        if (L.kind != 1) return -1;
        DbTable *t = db_find(h, L.tok);
        if (!t) { if (vm) vm_runtime_error(vm, "no such table: %s", L.tok); return -1; }
        sql_next(&L);
        DbCond conds[DB_MAX_COND];
        int nc = 0;
        if (sql_eat(&L, "where")) nc = db_parse_where(&L, t, conds);
        int order_col = -1;
        bool desc = false;
        if (sql_eat(&L, "order")) {
            sql_eat(&L, "by");
            if (L.kind == 1) { order_col = db_col_index(t, L.tok); sql_next(&L); }
            if (sql_eat(&L, "desc")) desc = true;
            else sql_eat(&L, "asc");
        }
        int limit = -1;
        if (sql_eat(&L, "limit")) { if (L.kind == 2) { limit = atoi(L.tok); sql_next(&L); } }

        /* collect matching row indexes */
        int *sel = malloc(sizeof(int) * (size_t)(t->nrows > 0 ? t->nrows : 1));
        int nsel = 0;
        for (int r = 0; r < t->nrows; r++) if (db_match(conds, nc, t->rows[r])) sel[nsel++] = r;
        db_free_conds(conds, nc);

        if (order_col >= 0)
            for (int i = 1; i < nsel; i++) {
                int key = sel[i], k = i - 1;
                while (k >= 0) {
                    int cmp = dbv_cmp(t->rows[sel[k]][order_col], t->rows[key][order_col]);
                    if (desc ? cmp >= 0 : cmp <= 0) break;
                    sel[k + 1] = sel[k];
                    k--;
                }
                sel[k + 1] = key;
            }
        if (limit >= 0 && nsel > limit) nsel = limit;

        if (rows_out && vm) {
            ObjList *out = vm_list(vm);
            if (count_star) {
                ObjMap *m = vm_map(vm);
                vm_map_set(vm, m, v_obj((Obj *)vm_text_cstr(vm, "count")), v_int(nsel));
                vec_push(&out->items, v_obj((Obj *)m));
            } else {
                for (int i = 0; i < nsel; i++) {
                    ObjMap *m = vm_map(vm);
                    if (star) {
                        for (int c = 0; c < t->ncols; c++)
                            vm_map_set(vm, m, v_obj((Obj *)vm_text_cstr(vm, t->cols[c].name)), dbv_to_value(vm, t->rows[sel[i]][c]));
                    } else {
                        for (int w = 0; w < nwant; w++) {
                            int c = db_col_index(t, want[w]);
                            vm_map_set(vm, m, v_obj((Obj *)vm_text_cstr(vm, want[w])),
                                       c >= 0 ? dbv_to_value(vm, t->rows[sel[i]][c]) : v_nil());
                        }
                    }
                    vec_push(&out->items, v_obj((Obj *)m));
                }
            }
            *rows_out = v_obj((Obj *)out);
        }
        free(sel);
        return count_star ? 1 : nsel;
    }

    if (vm) vm_runtime_error(vm, "Ember DB does not understand this statement: %.40s", sql);
    return -1;
}

static Value db_tables(VM *vm, int id) {
    ObjList *l = vm_list(vm);
    if (id >= 0 && id < DB_MAX && g_dbs[id].open)
        for (int i = 0; i < g_dbs[id].ntables; i++)
            vec_push(&l->items, v_obj((Obj *)vm_text_cstr(vm, g_dbs[id].tables[i]->name)));
    return v_obj((Obj *)l);
}
