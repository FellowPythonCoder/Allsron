/* ========================================================================
   SPRFST UI runtime — the engine behind `app` blocks and the `ui` module.

   The language half of the GUI is backend independent: a program builds a
   retained tree of widgets, attaches handlers, and calls ui.run().  Three
   backends consume that tree:

     term    a real interactive terminal renderer (default when attached
             to a tty) — widgets are drawn, keys dispatch events
     json    line protocol on stdout/stdin, used by SPRFST Studio and by
             the AppKit host: the tree is published as JSON, events arrive
             as "event <id> <name> [payload]" lines
     none    headless; ui.run() renders once and returns (used by tests)

   Select with SPRFST_UI=term|json|none.
   ======================================================================== */

#define UI_MAX_NODES  512
#define UI_MAX_PROPS  24
#define UI_MAX_EVENTS 8

typedef struct {
    bool  used;
    int   id, parent;
    char  kind[24];
    char *label;
    char *prop_keys[UI_MAX_PROPS];
    Value prop_vals[UI_MAX_PROPS];
    int   nprops;
    char *ev_names[UI_MAX_EVENTS];
    Value ev_fns[UI_MAX_EVENTS];
    int   nevents;
    int   children[UI_MAX_NODES / 4];
    int   nchildren;
} UiWidget;

static UiWidget g_ui[UI_MAX_NODES];
static int    g_ui_count = 0;
static bool   g_ui_running = false;
static bool   g_ui_dirty = true;
static char   g_ui_title[160] = "SPRFST";
static int    g_ui_w = 720, g_ui_h = 480;
static int    g_ui_backend = -1;        /* 0 term, 1 json, 2 none */

static const char *ui_backend_name(void) {
    if (g_ui_backend < 0) {
        const char *e = getenv("SPRFST_UI");
        if (e && strcmp(e, "json") == 0) g_ui_backend = 1;
        else if (e && strcmp(e, "none") == 0) g_ui_backend = 2;
        else if (e && strcmp(e, "term") == 0) g_ui_backend = 0;
        else g_ui_backend = isatty(0) && isatty(1) ? 0 : 2;
    }
    return g_ui_backend == 0 ? "term" : g_ui_backend == 1 ? "json" : "none";
}

static int ui_alloc(int parent, const char *kind, const char *label) {
    if (g_ui_count >= UI_MAX_NODES) return -1;
    int id = g_ui_count++;
    UiWidget *n = &g_ui[id];
    memset(n, 0, sizeof *n);
    n->used = true;
    n->id = id;
    n->parent = parent;
    snprintf(n->kind, sizeof n->kind, "%s", kind ? kind : "box");
    n->label = strdup(label ? label : "");
    if (parent >= 0 && parent < id && g_ui[parent].used &&
        g_ui[parent].nchildren < (int)(sizeof g_ui[parent].children / sizeof(int)))
        g_ui[parent].children[g_ui[parent].nchildren++] = id;
    g_ui_dirty = true;
    return id;
}

static int ui_window(VM *vm, const char *title, int w, int h) {
    (void)vm;
    for (int i = 0; i < g_ui_count; i++) {
        free(g_ui[i].label);
        for (int k = 0; k < g_ui[i].nprops; k++) free(g_ui[i].prop_keys[k]);
        for (int k = 0; k < g_ui[i].nevents; k++) free(g_ui[i].ev_names[k]);
    }
    g_ui_count = 0;
    snprintf(g_ui_title, sizeof g_ui_title, "%s", title && *title ? title : "SPRFST");
    if (w > 0) g_ui_w = w;
    if (h > 0) g_ui_h = h;
    return ui_alloc(-1, "window", g_ui_title);
}

static int ui_node(VM *vm, int parent, const char *kind, const char *label) {
    (void)vm;
    if (g_ui_count == 0) ui_alloc(-1, "window", g_ui_title);
    if (parent < 0 || parent >= g_ui_count) parent = 0;
    return ui_alloc(parent, kind, label);
}

static void ui_set(VM *vm, int id, const char *key, Value v) {
    if (id < 0 || id >= g_ui_count || !key) return;
    UiWidget *n = &g_ui[id];
    vm_root_add(v);
    bool stored = false;
    for (int i = 0; i < n->nprops && !stored; i++)
        if (strcmp(n->prop_keys[i], key) == 0) { n->prop_vals[i] = v; stored = true; }
    if (!stored && n->nprops < UI_MAX_PROPS) {
        n->prop_keys[n->nprops] = strdup(key);
        n->prop_vals[n->nprops] = v;
        n->nprops++;
        stored = true;
    }
    if (stored && (strcmp(key, "text") == 0 || strcmp(key, "label") == 0)) {
        char *s = vm_value_text(vm, v, false);
        free(n->label);
        n->label = s;
    }
    g_ui_dirty = true;
}

static Value ui_get(VM *vm, int id, const char *key) {
    (void)vm;
    if (id < 0 || id >= g_ui_count || !key) return v_nil();
    UiWidget *n = &g_ui[id];
    for (int i = 0; i < n->nprops; i++) if (strcmp(n->prop_keys[i], key) == 0) return n->prop_vals[i];
    if (strcmp(key, "label") == 0 || strcmp(key, "text") == 0) return v_obj((Obj *)vm_text_cstr(vm, n->label ? n->label : ""));
    return v_nil();
}

static void ui_on(VM *vm, int id, const char *event, Value fn) {
    (void)vm;
    if (id < 0 || id >= g_ui_count || !event) return;
    UiWidget *n = &g_ui[id];
    vm_root_add(fn);
    for (int i = 0; i < n->nevents; i++)
        if (strcmp(n->ev_names[i], event) == 0) { n->ev_fns[i] = fn; return; }
    if (n->nevents >= UI_MAX_EVENTS) return;
    n->ev_names[n->nevents] = strdup(event);
    n->ev_fns[n->nevents] = fn;
    n->nevents++;
}

static Value ui_handler(int id, const char *event) {
    if (id < 0 || id >= g_ui_count) return v_nil();
    UiWidget *n = &g_ui[id];
    for (int i = 0; i < n->nevents; i++) if (strcmp(n->ev_names[i], event) == 0) return n->ev_fns[i];
    return v_nil();
}

/* The compiler registers a synthesized closure as "refresh" on the window:
   it re-evaluates every bound property after state changes. */
static void ui_refresh(VM *vm) {
    Value fn = ui_handler(0, "refresh");
    if (fn.tag == V_OBJ) vm_call_value(vm, fn, NULL, 0);
}

/* Fire an event. A handler written as `on change(text) { ... }` takes one
   argument: the control's current value. `on click { ... }` takes none. */
static void ui_emit(VM *vm, int id, const char *event) {
    Value fn = ui_handler(id, event);
    if (fn.tag != V_OBJ || !fn.as.o || fn.as.o->kind != O_CLOSURE) return;
    ObjClosure *c = (ObjClosure *)fn.as.o;
    IRFunc *f = (c->fn >= 0 && c->fn < vm->prog->funcs.len) ? vm->prog->funcs.items[c->fn] : NULL;
    if (f && f->nparams >= 1) {
        Value arg = ui_get(vm, id, "value");
        vm_call_value(vm, fn, &arg, 1);
    } else {
        vm_call_value(vm, fn, NULL, 0);
    }
    ui_refresh(vm);
    g_ui_dirty = true;
}

/* ----------------------------------------------------------- describe */
static void ui_json_escape(StrBuf *b, const char *s) {
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"': sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n"); break;
            case '\r': sb_puts(b, "\\r"); break;
            case '\t': sb_puts(b, "\\t"); break;
            default: if (*p < 0x20) sb_printf(b, "\\u%04x", *p); else sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

static char *ui_describe(VM *vm) {
    StrBuf b; sb_init(&b);
    sb_printf(&b, "{\"title\":");
    ui_json_escape(&b, g_ui_title);
    sb_printf(&b, ",\"width\":%d,\"height\":%d,\"nodes\":[", g_ui_w, g_ui_h);
    for (int i = 0; i < g_ui_count; i++) {
        UiWidget *n = &g_ui[i];
        if (i) sb_putc(&b, ',');
        sb_printf(&b, "{\"id\":%d,\"parent\":%d,\"kind\":", n->id, n->parent);
        ui_json_escape(&b, n->kind);
        sb_puts(&b, ",\"label\":");
        ui_json_escape(&b, n->label);
        sb_puts(&b, ",\"props\":{");
        for (int k = 0; k < n->nprops; k++) {
            if (k) sb_putc(&b, ',');
            ui_json_escape(&b, n->prop_keys[k]);
            sb_putc(&b, ':');
            json_write_value(vm, n->prop_vals[k], &b, 0, 0);
        }
        sb_puts(&b, "},\"events\":[");
        for (int k = 0; k < n->nevents; k++) {
            if (k) sb_putc(&b, ',');
            ui_json_escape(&b, n->ev_names[k]);
        }
        sb_puts(&b, "],\"children\":[");
        for (int k = 0; k < n->nchildren; k++) sb_printf(&b, "%s%d", k ? "," : "", n->children[k]);
        sb_puts(&b, "]}");
    }
    sb_puts(&b, "]}");
    return b.data ? b.data : strdup("{}");
}

/* --------------------------------------------------- terminal rendering */
typedef struct { int id; char key; } UiHit;

static void ui_term_draw_node(VM *vm, int id, int depth, UiHit *hits, int *nhits) {
    if (id < 0 || id >= g_ui_count) return;
    UiWidget *n = &g_ui[id];
    char pad[64];
    int p = depth * 2;
    if (p > 60) p = 60;
    memset(pad, ' ', (size_t)p);
    pad[p] = 0;

    const char *k = n->kind;
    if (strcmp(k, "window") == 0) {
        /* the window itself draws no chrome in the terminal */
    } else if (strcmp(k, "button") == 0) {
        char key = (char)('1' + (*nhits < 9 ? *nhits : 9));
        if (*nhits < 9) { hits[*nhits].id = id; hits[*nhits].key = key; (*nhits)++; }
        printf("%s%s[%c]%s %s\n", pad, C_AMBER, key, C_RESET, n->label);
    } else if (strcmp(k, "input") == 0 || strcmp(k, "field") == 0) {
        char key = (char)('1' + (*nhits < 9 ? *nhits : 9));
        if (*nhits < 9) { hits[*nhits].id = id; hits[*nhits].key = key; (*nhits)++; }
        char *s = vm_value_text(vm, ui_get(vm, id, "value"), false);
        printf("%s%s[%c]%s %s %s<%s>%s\n", pad, C_AMBER, key, C_RESET, n->label, C_DIM, s, C_RESET);
        free(s);
    } else if (strcmp(k, "check") == 0 || strcmp(k, "toggle") == 0) {
        char key = (char)('1' + (*nhits < 9 ? *nhits : 9));
        if (*nhits < 9) { hits[*nhits].id = id; hits[*nhits].key = key; (*nhits)++; }
        bool on = v_truthy(ui_get(vm, id, "value"));
        printf("%s%s[%c]%s %s %s\n", pad, C_AMBER, key, C_RESET, on ? "[x]" : "[ ]", n->label);
    } else if (strcmp(k, "title") == 0 || strcmp(k, "heading") == 0) {
        printf("%s%s%s%s\n", pad, C_BOLD, n->label, C_RESET);
    } else if (strcmp(k, "divider") == 0) {
        printf("%s%s----------------------------%s\n", pad, C_GRAY, C_RESET);
    } else if (strcmp(k, "list") == 0) {
        if (n->label && *n->label) printf("%s%s\n", pad, n->label);
        Value items = ui_get(vm, id, "items");
        if (IS_OBJ(items, O_LIST)) {
            ObjList *l = AS_LIST(items);
            vec_foreach(i, &l->items) {
                char *s = vm_value_text(vm, l->items.items[i], false);
                printf("%s  %s-%s %s\n", pad, C_GRAY, C_RESET, s);
                free(s);
            }
        }
    } else {
        if (n->label && *n->label) printf("%s%s\n", pad, n->label);
    }
    for (int i = 0; i < n->nchildren; i++)
        ui_term_draw_node(vm, n->children[i], strcmp(k, "window") == 0 ? depth : depth + 1, hits, nhits);
}

static void ui_term_frame(VM *vm, UiHit *hits, int *nhits) {
    *nhits = 0;
    printf("\n  %s%s%s%s\n", C_BOLD, C_AMBER, g_ui_title, C_RESET);
    printf("  %s------------------------------------------%s\n", C_GRAY, C_RESET);
    if (g_ui_count) ui_term_draw_node(vm, 0, 1, hits, nhits);
    printf("  %s------------------------------------------%s\n", C_GRAY, C_RESET);
    printf("  %spress a number to act, q to quit%s\n  %s>%s ", C_DIM, C_RESET, C_AMBER, C_RESET);
    fflush(stdout);
}

static void ui_run_term(VM *vm) {
    UiHit hits[16];
    int nhits = 0;
    char line[512];
    while (g_ui_running) {
        ui_term_frame(vm, hits, &nhits);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0) continue;
        if (line[0] == 'q' || line[0] == 'Q') break;
        int which = -1;
        for (int i = 0; i < nhits; i++) if (hits[i].key == line[0]) which = hits[i].id;
        if (which < 0) continue;
        UiWidget *n = &g_ui[which];
        if (strcmp(n->kind, "input") == 0 || strcmp(n->kind, "field") == 0) {
            printf("  %s: ", n->label);
            fflush(stdout);
            char buf[512];
            if (fgets(buf, sizeof buf, stdin)) {
                size_t b = strlen(buf);
                while (b && (buf[b-1] == '\n' || buf[b-1] == '\r')) buf[--b] = 0;
                ui_set(vm, which, "value", v_obj((Obj *)vm_text_cstr(vm, buf)));
                ui_emit(vm, which, "change");
            }
        } else if (strcmp(n->kind, "check") == 0 || strcmp(n->kind, "toggle") == 0) {
            bool on = v_truthy(ui_get(vm, which, "value"));
            ui_set(vm, which, "value", v_bool(!on));
            ui_emit(vm, which, "change");
        } else {
            ui_emit(vm, which, "click");
            if (ui_handler(which, "click").tag == V_NIL) ui_emit(vm, which, "press");
        }
        if (vm->had_error) break;
    }
}

/* ----------------------------------------------------- Studio protocol */
static void ui_publish(VM *vm) {
    char *json = ui_describe(vm);
    printf("\x1e""ui ");
    fputs(json, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(json);
    g_ui_dirty = false;
}

static void ui_run_json(VM *vm) {
    char line[8192];
    ui_publish(vm);
    while (g_ui_running) {
        if (!fgets(line, sizeof line, stdin)) break;
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (strncmp(line, "event ", 6) == 0) {
            int id = 0;
            char ev[64] = { 0 };
            char payload[4096] = { 0 };
            int consumed = 0;
            if (sscanf(line + 6, "%d %63s%n", &id, ev, &consumed) >= 2) {
                const char *rest = line + 6 + consumed;
                while (*rest == ' ') rest++;
                snprintf(payload, sizeof payload, "%s", rest);
                if (*payload) ui_set(vm, id, "value", v_obj((Obj *)vm_text_cstr(vm, payload)));
                ui_emit(vm, id, ev);
            }
            ui_publish(vm);
        } else if (strcmp(line, "quit") == 0) {
            break;
        } else if (strcmp(line, "sync") == 0) {
            ui_publish(vm);
        }
        if (vm->had_error) break;
    }
}

static void ui_run(VM *vm) {
    const char *backend = ui_backend_name();
    if (g_ui_count == 0) return;
    g_ui_running = true;
    ui_refresh(vm);
    if (strcmp(backend, "json") == 0) ui_run_json(vm);
    else if (strcmp(backend, "term") == 0) ui_run_term(vm);
    else { char *s = ui_describe(vm); puts(s); free(s); }
    g_ui_running = false;
}

static void ui_quit(void) { g_ui_running = false; }
