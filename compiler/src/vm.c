/* ========================================================================
   SPRFST — runtime: values, garbage collector and the SPIR interpreter
   ======================================================================== */
#define _GNU_SOURCE 1
#include "sprfst/vm.h"
#include "sprfst/natives.h"
#include <math.h>
#include <stdarg.h>
#include <pthread.h>

/* ---------------------------------------------------------- global state */
static pthread_mutex_t g_runtime_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_VMS 64
static VM *g_vms[MAX_VMS];
static int g_nvms = 0;
static VM *g_root = NULL;

void sprfst_lock(void)   { pthread_mutex_lock(&g_runtime_lock); }
void sprfst_unlock(void) { pthread_mutex_unlock(&g_runtime_lock); }

static void vm_register(VM *vm) {
    if (g_nvms < MAX_VMS) g_vms[g_nvms++] = vm;
    if (!g_root) g_root = vm;
}
static void vm_unregister(VM *vm) {
    for (int i = 0; i < g_nvms; i++)
        if (g_vms[i] == vm) { g_vms[i] = g_vms[--g_nvms]; break; }
}

/* ------------------------------------------------------------ allocation */
static bool g_gc_stress = false;   /* SPRFST_GC_STRESS=1: collect on every allocation */

static Obj *alloc_obj(VM *vm, size_t size, ObjKind kind) {
    VM *root = g_root ? g_root : vm;
    if (g_gc_stress || root->bytes_allocated > root->next_gc) vm_gc(vm);
    Obj *o = calloc(1, size);
    if (!o) { fprintf(stderr, "sprfst: out of memory\n"); exit(70); }
    o->kind = kind;
    o->marked = false;
    o->next = root->objects;
    root->objects = o;
    root->bytes_allocated += size;
    if (root->bytes_allocated > root->peak_bytes) root->peak_bytes = root->bytes_allocated;
    root->obj_count++;
    /* A native builds its answer out of several objects. Half built, they are
       not referenced from any register yet, so a collection in the middle of
       the native would sweep them away. Hold on to everything allocated
       while a native is running; the scope is dropped when it returns. */
    if (vm && vm->in_native > 0) vec_push(&vm->temp_roots, v_obj(o));
    return o;
}

int vm_native_enter(VM *vm) {
    vm->in_native++;
    return vm->temp_roots.len;
}

void vm_native_leave(VM *vm, int mark, Value result) {
    if (mark < vm->temp_roots.len) vm->temp_roots.len = mark;
    vm->in_native--;
    /* the answer itself is still only a C local in the caller: keep it */
    if (vm->in_native > 0 && result.tag == V_OBJ) vec_push(&vm->temp_roots, result);
}

ObjText *vm_text(VM *vm, const char *s, int len) {
    ObjText *t = (ObjText *)alloc_obj(vm, sizeof(ObjText), O_TEXT);
    t->chars = malloc((size_t)len + 1);
    if (len && s) memcpy(t->chars, s, (size_t)len);
    t->chars[len] = 0;
    t->len = len;
    t->hash = hash_bytes(t->chars, (size_t)len);
    return t;
}
ObjText *vm_text_cstr(VM *vm, const char *s) { return vm_text(vm, s, s ? (int)strlen(s) : 0); }
ObjList *vm_list(VM *vm) { ObjList *l = (ObjList *)alloc_obj(vm, sizeof(ObjList), O_LIST); vec_init(&l->items); return l; }
ObjMap  *vm_map(VM *vm)  { ObjMap *m = (ObjMap *)alloc_obj(vm, sizeof(ObjMap), O_MAP); vec_init(&m->keys); vec_init(&m->vals); return m; }
ObjSet  *vm_set(VM *vm)  { ObjSet *s = (ObjSet *)alloc_obj(vm, sizeof(ObjSet), O_SET); vec_init(&s->items); return s; }
ObjNative *vm_native_obj(VM *vm, int ntype, void *ptr, int64_t handle) {
    ObjNative *n = (ObjNative *)alloc_obj(vm, sizeof(ObjNative), O_NATIVE);
    n->ntype = ntype; n->ptr = ptr; n->handle = handle;
    return n;
}
ObjTensor *vm_tensor(VM *vm, int ndim, int *shape) {
    ObjTensor *t = (ObjTensor *)alloc_obj(vm, sizeof(ObjTensor), O_TENSOR);
    t->ndim = ndim < 4 ? ndim : 4;
    int count = 1;
    for (int i = 0; i < t->ndim; i++) { t->shape[i] = shape[i]; count *= shape[i] > 0 ? shape[i] : 1; }
    t->count = count;
    t->data = calloc((size_t)count, sizeof(double));
    return t;
}
ObjImage *vm_image(VM *vm, int w, int h) {
    ObjImage *im = (ObjImage *)alloc_obj(vm, sizeof(ObjImage), O_IMAGE);
    im->w = w; im->h = h;
    im->px = calloc((size_t)(w * h), sizeof(uint32_t));
    return im;
}
ObjFuture *vm_future(VM *vm) {
    ObjFuture *f = (ObjFuture *)alloc_obj(vm, sizeof(ObjFuture), O_FUTURE);
    vec_init(&f->args);
    f->result = v_nil();
    f->thread_id = -1;
    f->fn = -1;
    return f;
}
ObjChan *vm_chan(VM *vm, int cap) {
    ObjChan *c = (ObjChan *)alloc_obj(vm, sizeof(ObjChan), O_CHAN);
    vec_init(&c->buffer);
    c->cap = cap > 0 ? cap : 64;
    return c;
}
ObjResult *vm_result(VM *vm, bool ok, Value v) {
    ObjResult *r = (ObjResult *)alloc_obj(vm, sizeof(ObjResult), O_RESULT);
    r->ok = ok; r->value = v;
    return r;
}
static ObjInstance *vm_instance(VM *vm, int type_id, const char *tname, int nfields) {
    ObjInstance *o = (ObjInstance *)alloc_obj(vm, sizeof(ObjInstance), O_INSTANCE);
    o->type_id = type_id;
    o->tname = tname;
    vec_init(&o->fields);
    for (int i = 0; i < nfields; i++) vec_push(&o->fields, v_nil());
    return o;
}
/* A copy of an object, field for field: what `clone` gives you. */
ObjInstance *vm_instance_copy(VM *vm, ObjInstance *src) {
    ObjInstance *o = vm_instance(vm, src->type_id, src->tname, 0);
    vec_foreach(i, &src->fields) vec_push(&o->fields, src->fields.items[i]);
    return o;
}
static ObjVariant *vm_variant(VM *vm, int tag, const char *vname) {
    ObjVariant *v = (ObjVariant *)alloc_obj(vm, sizeof(ObjVariant), O_VARIANT);
    v->tag = tag; v->vname = vname;
    vec_init(&v->payload);
    return v;
}
static ObjClosure *vm_closure(VM *vm, int fn) {
    ObjClosure *c = (ObjClosure *)alloc_obj(vm, sizeof(ObjClosure), O_CLOSURE);
    c->fn = fn;
    vec_init(&c->caps);
    return c;
}

/* ------------------------------------------------------------------- GC */
static void mark_value(Value v);

static void mark_obj(Obj *o) {
    if (!o || o->marked) return;
    o->marked = true;
    switch (o->kind) {
        case O_LIST:  { ObjList *l = (ObjList *)o; vec_foreach(i, &l->items) mark_value(l->items.items[i]); break; }
        case O_SET:   { ObjSet *s = (ObjSet *)o; vec_foreach(i, &s->items) mark_value(s->items.items[i]); break; }
        case O_MAP:   { ObjMap *m = (ObjMap *)o;
                        vec_foreach(i, &m->keys) mark_value(m->keys.items[i]);
                        vec_foreach(i, &m->vals) mark_value(m->vals.items[i]);
                        break; }
        case O_CLOSURE: { ObjClosure *c = (ObjClosure *)o; vec_foreach(i, &c->caps) mark_value(c->caps.items[i]); break; }
        case O_INSTANCE: { ObjInstance *n = (ObjInstance *)o; vec_foreach(i, &n->fields) mark_value(n->fields.items[i]); break; }
        case O_VARIANT: { ObjVariant *n = (ObjVariant *)o; vec_foreach(i, &n->payload) mark_value(n->payload.items[i]); break; }
        case O_RESULT: mark_value(((ObjResult *)o)->value); break;
        case O_ITER: { ObjIter *it = (ObjIter *)o; mark_value(it->src); mark_value(it->key); break; }
        case O_CHAN: { ObjChan *c = (ObjChan *)o; vec_foreach(i, &c->buffer) mark_value(c->buffer.items[i]); break; }
        case O_FUTURE: { ObjFuture *f = (ObjFuture *)o; mark_value(f->result);
                         vec_foreach(i, &f->args) mark_value(f->args.items[i]);
                         break; }
        default: break;
    }
}
static void mark_value(Value v) { if (v.tag == V_OBJ) mark_obj(v.as.o); }

/* values pinned by host code (UI handlers, embedder state) */
static ValueVec g_pinned;
void vm_root_add(Value v) { if (v.tag == V_OBJ) vec_push(&g_pinned, v); }

static void free_obj(Obj *o) {
    switch (o->kind) {
        case O_TEXT: free(((ObjText *)o)->chars); break;
        case O_LIST: vec_free(&((ObjList *)o)->items); break;
        case O_SET: vec_free(&((ObjSet *)o)->items); break;
        case O_MAP: vec_free(&((ObjMap *)o)->keys); vec_free(&((ObjMap *)o)->vals); break;
        case O_CLOSURE: vec_free(&((ObjClosure *)o)->caps); break;
        case O_INSTANCE: vec_free(&((ObjInstance *)o)->fields); break;
        case O_VARIANT: vec_free(&((ObjVariant *)o)->payload); break;
        case O_CHAN: vec_free(&((ObjChan *)o)->buffer); break;
        case O_FUTURE: vec_free(&((ObjFuture *)o)->args); break;
        case O_TENSOR: free(((ObjTensor *)o)->data); break;
        case O_IMAGE: free(((ObjImage *)o)->px); break;
        default: break;
    }
    free(o);
}

void vm_gc(VM *vm) {
    VM *root = g_root ? g_root : vm;
    /* roots: every live VM's registers and frames, plus globals and constants */
    for (int i = 0; i < g_nvms; i++) {
        VM *v = g_vms[i];
        if (!v) continue;
        int top = 0;
        for (int fi = 0; fi < v->nframes; fi++) {
            Frame *f = &v->frames[fi];
            int base = (int)(f->regs - v->stack);
            int end = base + (f->fn ? f->fn->nregs : 0);
            if (end > top) top = end;
            if (f->closure) mark_obj((Obj *)f->closure);
        }
        for (int r = 0; r < top && r < v->stack_size; r++) mark_value(v->stack[r]);
        vec_foreach(k, &v->task_queue) mark_value(v->task_queue.items[k]);
        vec_foreach(k, &v->temp_roots) mark_value(v->temp_roots.items[k]);
    }
    vec_foreach(i, &g_pinned) mark_value(g_pinned.items[i]);
    vec_foreach(i, &root->prog->globals) mark_value(root->prog->globals.items[i]);
    vec_foreach(fi, &root->prog->funcs) {
        IRFunc *f = root->prog->funcs.items[fi];
        vec_foreach(ci, &f->consts) mark_value(f->consts.items[ci]);
    }

    Obj **slot = &root->objects;
    while (*slot) {
        Obj *o = *slot;
        if (!o->marked) {
            *slot = o->next;
            free_obj(o);
            root->obj_count--;
        } else {
            o->marked = false;
            slot = &o->next;
        }
    }
    root->bytes_allocated = (size_t)root->obj_count * 64;
    root->next_gc = root->bytes_allocated * 2 + (1u << 20);
    root->gc_runs++;
}

/* ------------------------------------------------------- value utilities */
bool vm_values_equal(Value a, Value b) {
    if (a.tag != b.tag) {
        if ((a.tag == V_INT && b.tag == V_NUM) || (a.tag == V_NUM && b.tag == V_INT))
            return v_tonum(a) == v_tonum(b);
        return false;
    }
    switch (a.tag) {
        case V_NIL: return true;
        case V_INT: return a.as.i == b.as.i;
        case V_NUM: return a.as.n == b.as.n;
        case V_BOOL: return a.as.b == b.as.b;
        case V_OBJ: {
            if (a.as.o == b.as.o) return true;
            if (!a.as.o || !b.as.o || a.as.o->kind != b.as.o->kind) return false;
            if (a.as.o->kind == O_TEXT) {
                ObjText *x = (ObjText *)a.as.o, *y = (ObjText *)b.as.o;
                return x->len == y->len && memcmp(x->chars, y->chars, (size_t)x->len) == 0;
            }
            if (a.as.o->kind == O_LIST) {
                ObjList *x = (ObjList *)a.as.o, *y = (ObjList *)b.as.o;
                if (x->items.len != y->items.len) return false;
                vec_foreach(i, &x->items) if (!vm_values_equal(x->items.items[i], y->items.items[i])) return false;
                return true;
            }
            if (a.as.o->kind == O_VARIANT) {
                ObjVariant *x = (ObjVariant *)a.as.o, *y = (ObjVariant *)b.as.o;
                if (x->tag != y->tag || x->payload.len != y->payload.len) return false;
                vec_foreach(i, &x->payload) if (!vm_values_equal(x->payload.items[i], y->payload.items[i])) return false;
                return true;
            }
            if (a.as.o->kind == O_INSTANCE) {
                ObjInstance *x = (ObjInstance *)a.as.o, *y = (ObjInstance *)b.as.o;
                if (x->type_id != y->type_id || x->fields.len != y->fields.len) return false;
                vec_foreach(i, &x->fields) if (!vm_values_equal(x->fields.items[i], y->fields.items[i])) return false;
                return true;
            }
            return false;
        }
    }
    return false;
}

uint64_t vm_value_hash(Value v) {
    switch (v.tag) {
        case V_NIL: return 0;
        case V_INT: return hash_bytes(&v.as.i, sizeof v.as.i);
        case V_NUM: {
            double d = v.as.n;
            if (d == (double)(int64_t)d) { int64_t i = (int64_t)d; return hash_bytes(&i, sizeof i); }
            return hash_bytes(&d, sizeof d);
        }
        case V_BOOL: return v.as.b ? 1231 : 1237;
        case V_OBJ:
            if (v.as.o && v.as.o->kind == O_TEXT) return ((ObjText *)v.as.o)->hash;
            if (v.as.o && v.as.o->kind == O_VARIANT) return (uint64_t)((ObjVariant *)v.as.o)->tag * 31;
            return (uint64_t)(uintptr_t)v.as.o;
    }
    return 0;
}

Value vm_map_get(ObjMap *m, Value key, bool *found) {
    vec_foreach(i, &m->keys) {
        if (vm_values_equal(m->keys.items[i], key)) { if (found) *found = true; return m->vals.items[i]; }
    }
    if (found) *found = false;
    return v_nil();
}
void vm_map_set(VM *vm, ObjMap *m, Value key, Value val) {
    vec_foreach(i, &m->keys)
        if (vm_values_equal(m->keys.items[i], key)) { m->vals.items[i] = val; return; }
    vec_push(&m->keys, key);
    vec_push(&m->vals, val);
}
bool vm_map_remove(ObjMap *m, Value key, Value *out) {
    vec_foreach(i, &m->keys) {
        if (vm_values_equal(m->keys.items[i], key)) {
            if (out) *out = m->vals.items[i];
            for (int k = i; k < m->keys.len - 1; k++) {
                m->keys.items[k] = m->keys.items[k + 1];
                m->vals.items[k] = m->vals.items[k + 1];
            }
            m->keys.len--; m->vals.len--;
            return true;
        }
    }
    return false;
}

static void text_of(VM *vm, Value v, StrBuf *b, bool quote, int depth) {
    if (depth > 8) { sb_puts(b, "..."); return; }
    switch (v.tag) {
        case V_NIL: sb_puts(b, "nil"); break;
        case V_BOOL: sb_puts(b, v.as.b ? "true" : "false"); break;
        case V_INT: sb_printf(b, "%lld", (long long)v.as.i); break;
        case V_NUM: {
            double d = v.as.n;
            if (d == (double)(int64_t)d && fabs(d) < 1e15) sb_printf(b, "%.1f", d);
            else sb_printf(b, "%.10g", d);
            break;
        }
        case V_OBJ: {
            Obj *o = v.as.o;
            if (!o) { sb_puts(b, "nil"); break; }
            switch (o->kind) {
                case O_TEXT: {
                    ObjText *t = (ObjText *)o;
                    if (quote) { sb_putc(b, '"'); sb_put(b, t->chars, (size_t)t->len); sb_putc(b, '"'); }
                    else sb_put(b, t->chars, (size_t)t->len);
                    break;
                }
                case O_LIST: {
                    ObjList *l = (ObjList *)o;
                    sb_putc(b, '[');
                    vec_foreach(i, &l->items) { if (i) sb_puts(b, ", "); text_of(vm, l->items.items[i], b, true, depth + 1); }
                    sb_putc(b, ']');
                    break;
                }
                case O_SET: {
                    ObjSet *s = (ObjSet *)o;
                    sb_puts(b, "Set[");
                    vec_foreach(i, &s->items) { if (i) sb_puts(b, ", "); text_of(vm, s->items.items[i], b, true, depth + 1); }
                    sb_putc(b, ']');
                    break;
                }
                case O_MAP: {
                    ObjMap *m = (ObjMap *)o;
                    sb_putc(b, '[');
                    if (!m->keys.len) sb_putc(b, ':');
                    vec_foreach(i, &m->keys) {
                        if (i) sb_puts(b, ", ");
                        text_of(vm, m->keys.items[i], b, true, depth + 1);
                        sb_puts(b, ": ");
                        text_of(vm, m->vals.items[i], b, true, depth + 1);
                    }
                    sb_putc(b, ']');
                    break;
                }
                case O_INSTANCE: {
                    ObjInstance *n = (ObjInstance *)o;
                    IRType *t = (vm && vm->prog && n->type_id >= 0 && n->type_id < vm->prog->types.len)
                                ? vm->prog->types.items[n->type_id] : NULL;
                    sb_printf(b, "%s {", t ? t->name : (n->tname ? n->tname : "object"));
                    vec_foreach(i, &n->fields) {
                        if (i) sb_putc(b, ',');
                        sb_putc(b, ' ');
                        if (t && i < t->nfields) sb_printf(b, "%s: ", t->field_names[i]);
                        text_of(vm, n->fields.items[i], b, true, depth + 1);
                    }
                    sb_puts(b, " }");
                    break;
                }
                case O_VARIANT: {
                    ObjVariant *n = (ObjVariant *)o;
                    sb_puts(b, n->vname ? n->vname : "variant");
                    if (n->payload.len) {
                        sb_putc(b, '(');
                        vec_foreach(i, &n->payload) { if (i) sb_puts(b, ", "); text_of(vm, n->payload.items[i], b, true, depth + 1); }
                        sb_putc(b, ')');
                    }
                    break;
                }
                case O_RESULT: {
                    ObjResult *r = (ObjResult *)o;
                    sb_puts(b, r->ok ? "Ok(" : "Err(");
                    text_of(vm, r->value, b, true, depth + 1);
                    sb_putc(b, ')');
                    break;
                }
                case O_RANGE: {
                    ObjRange *r = (ObjRange *)o;
                    sb_printf(b, "%lld%s%lld", (long long)r->lo, r->inclusive ? "..." : "..", (long long)r->hi);
                    break;
                }
                case O_CLOSURE: sb_printf(b, "fn@%d", ((ObjClosure *)o)->fn); break;
                case O_FUTURE: sb_puts(b, ((ObjFuture *)o)->done ? "Future(done)" : "Future(pending)"); break;
                case O_CHAN: sb_printf(b, "Chan(%d)", ((ObjChan *)o)->buffer.len); break;
                case O_TENSOR: {
                    ObjTensor *t = (ObjTensor *)o;
                    sb_puts(b, "Tensor[");
                    for (int i = 0; i < t->ndim; i++) { if (i) sb_puts(b, "x"); sb_printf(b, "%d", t->shape[i]); }
                    sb_puts(b, "]");
                    break;
                }
                case O_IMAGE: sb_printf(b, "Image(%dx%d)", ((ObjImage *)o)->w, ((ObjImage *)o)->h); break;
                case O_NATIVE: sb_printf(b, "Handle(%lld)", (long long)((ObjNative *)o)->handle); break;
                default: sb_puts(b, "object");
            }
            break;
        }
    }
}

char *vm_value_text(VM *vm, Value v, bool quote_text) {
    StrBuf b; sb_init(&b);
    text_of(vm, v, &b, quote_text, 0);
    return sb_take(&b);
}

/* ------------------------------------------------------------- errors */
void vm_runtime_error(VM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof vm->error_msg, fmt, ap);
    va_end(ap);
    vm->had_error = true;
}

typedef struct { int frame, target, err_slot; } TryEntry;
#define MAX_TRY 64

/* --------------------------------------------------------- the interpreter */
typedef struct {
    TryEntry entries[MAX_TRY];
    int      len;
} TryStack;

static Value iterator_new(VM *vm, Value src) {
    ObjIter *it = (ObjIter *)alloc_obj(vm, sizeof(ObjIter), O_ITER);
    it->src = src;
    it->index = 0;
    it->key = v_nil();
    if (src.tag == V_OBJ && src.as.o) {
        switch (src.as.o->kind) {
            case O_LIST: it->kind = 0; break;
            case O_MAP: it->kind = 1; break;
            case O_SET: it->kind = 2; break;
            case O_RANGE: it->kind = 3; break;
            case O_TEXT: it->kind = 4; break;
            case O_CHAN: it->kind = 5; break;
            default: it->kind = 6;
        }
    } else it->kind = 6;
    return v_obj((Obj *)it);
}

static bool iterator_next(VM *vm, ObjIter *it, Value *out) {
    switch (it->kind) {
        case 0: { ObjList *l = (ObjList *)it->src.as.o;
                  if (it->index >= l->items.len) return false;
                  *out = l->items.items[it->index++]; return true; }
        case 1: { ObjMap *m = (ObjMap *)it->src.as.o;
                  if (it->index >= m->keys.len) return false;
                  *out = m->keys.items[it->index];
                  it->key = m->vals.items[it->index];
                  it->index++; return true; }
        case 2: { ObjSet *s = (ObjSet *)it->src.as.o;
                  if (it->index >= s->items.len) return false;
                  *out = s->items.items[it->index++]; return true; }
        case 3: { ObjRange *r = (ObjRange *)it->src.as.o;
                  int64_t v = r->lo + it->index;
                  if (r->inclusive ? (v > r->hi) : (v >= r->hi)) return false;
                  it->index++; *out = v_int(v); return true; }
        case 4: { ObjText *t = (ObjText *)it->src.as.o;
                  if (it->index >= t->len) return false;
                  int start = it->index;
                  int n = 1;
                  unsigned char c = (unsigned char)t->chars[start];
                  if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
                  if (start + n > t->len) n = 1;
                  it->index += n;
                  *out = v_obj((Obj *)vm_text(vm, t->chars + start, n));
                  return true; }
        case 5: { ObjChan *c = (ObjChan *)it->src.as.o;
                  if (it->index >= c->buffer.len) return false;
                  *out = c->buffer.items[it->index++]; return true; }
        default: return false;
    }
}

static Value run_frame(VM *vm, TryStack *trys);

static bool push_frame(VM *vm, int fn_index, Value *args, int nargs, ObjClosure *cl, int ret_reg) {
    if (vm->nframes >= VM_MAX_FRAMES - 1) {
        vm_runtime_error(vm, "call depth limit reached (%d frames) — is a function calling itself without an exit?", VM_MAX_FRAMES);
        return false;
    }
    IRFunc *f = vm->prog->funcs.items[fn_index];
    int base = 0;
    if (vm->nframes > 0) {
        Frame *caller = &vm->frames[vm->nframes - 1];
        base = (int)(caller->regs - vm->stack) + caller->fn->nregs;
    }
    if (base + f->nregs + 8 >= vm->stack_size) {
        int newsize = (base + f->nregs + 64) * 2;
        Value *ns = realloc(vm->stack, sizeof(Value) * (size_t)newsize);
        if (!ns) { vm_runtime_error(vm, "out of stack memory"); return false; }
        /* re-point existing frames */
        for (int i = 0; i < vm->nframes; i++) {
            int off = (int)(vm->frames[i].regs - vm->stack);
            vm->frames[i].regs = ns + off;
        }
        vm->stack = ns;
        vm->stack_size = newsize;
    }
    Frame *fr = &vm->frames[vm->nframes++];
    fr->fn = f;
    fr->regs = vm->stack + base;
    fr->ip = 0;
    fr->ret_reg = ret_reg;
    fr->closure = cl;
    for (int i = 0; i < f->nregs; i++) fr->regs[i] = v_nil();
    for (int i = 0; i < nargs && i < f->nregs; i++) fr->regs[i] = args[i];
    return true;
}

Value vm_call_function(VM *vm, int fn_index, Value *args, int nargs) {
    if (fn_index < 0 || fn_index >= vm->prog->funcs.len) return v_nil();
    TryStack trys = { .len = 0 };
    int saved = vm->nframes;
    if (!push_frame(vm, fn_index, args, nargs, NULL, -1)) return v_nil();
    /* SPRFST code called back from a native roots itself through its frame,
       so step out of the native scope while it runs */
    int native = vm->in_native;
    vm->in_native = 0;
    Value r = run_frame(vm, &trys);
    vm->in_native = native;
    vm->nframes = saved;
    if (native > 0 && r.tag == V_OBJ) vec_push(&vm->temp_roots, r);
    return r;
}

Value vm_call_value(VM *vm, Value callable, Value *args, int nargs) {
    if (callable.tag == V_OBJ && callable.as.o && callable.as.o->kind == O_CLOSURE) {
        ObjClosure *c = (ObjClosure *)callable.as.o;
        TryStack trys = { .len = 0 };
        int saved = vm->nframes;
        if (!push_frame(vm, c->fn, args, nargs, c, -1)) return v_nil();
        int native = vm->in_native;
        vm->in_native = 0;
        Value r = run_frame(vm, &trys);
        vm->in_native = native;
        vm->nframes = saved;
        if (native > 0 && r.tag == V_OBJ) vec_push(&vm->temp_roots, r);
        return r;
    }
    return v_nil();
}

static Value resolve_future(VM *vm, ObjFuture *f) {
    if (f->done) return f->result;
    if (f->fn >= 0) {
        f->result = vm_call_function(vm, f->fn, f->args.items, f->args.len);
        f->done = true;
    } else f->done = true;
    return f->result;
}

/* numeric helper used by arithmetic opcodes */
static bool arith(VM *vm, OpCode op, Value a, Value b, Value *out) {
    if (a.tag == V_INT && b.tag == V_INT) {
        int64_t x = a.as.i, y = b.as.i;
        switch (op) {
            case OP_ADD: *out = v_int(x + y); return true;
            case OP_SUB: *out = v_int(x - y); return true;
            case OP_MUL: *out = v_int(x * y); return true;
            case OP_DIV:
                if (y == 0) { vm_runtime_error(vm, "cannot divide %lld by zero", (long long)x); return false; }
                *out = v_num((double)x / (double)y);
                return true;
            case OP_MOD:
                if (y == 0) { vm_runtime_error(vm, "cannot take %lld modulo zero", (long long)x); return false; }
                *out = v_int(x % y); return true;
            case OP_POW: *out = v_num(pow((double)x, (double)y)); return true;
            default: break;
        }
    }
    if ((a.tag == V_INT || a.tag == V_NUM) && (b.tag == V_INT || b.tag == V_NUM)) {
        double x = v_tonum(a), y = v_tonum(b);
        switch (op) {
            case OP_ADD: *out = v_num(x + y); return true;
            case OP_SUB: *out = v_num(x - y); return true;
            case OP_MUL: *out = v_num(x * y); return true;
            case OP_DIV:
                if (y == 0) { vm_runtime_error(vm, "cannot divide by zero"); return false; }
                *out = v_num(x / y); return true;
            case OP_MOD:
                if (y == 0) { vm_runtime_error(vm, "cannot take modulo zero"); return false; }
                *out = v_num(fmod(x, y)); return true;
            case OP_POW: *out = v_num(pow(x, y)); return true;
            default: break;
        }
    }
    /* list concatenation */
    if (op == OP_ADD && IS_OBJ(a, O_LIST) && IS_OBJ(b, O_LIST)) {
        ObjList *l = vm_list(vm);
        vec_foreach(i, &AS_LIST(a)->items) vec_push(&l->items, AS_LIST(a)->items.items[i]);
        vec_foreach(i, &AS_LIST(b)->items) vec_push(&l->items, AS_LIST(b)->items.items[i]);
        *out = v_obj((Obj *)l);
        return true;
    }
    {
        char *ta = vm_value_text(vm, a, false), *tb = vm_value_text(vm, b, false);
        vm_runtime_error(vm, "cannot use `%s` on %s and %s",
                         op == OP_ADD ? "+" : op == OP_SUB ? "-" : op == OP_MUL ? "*" :
                         op == OP_DIV ? "/" : op == OP_MOD ? "%" : "**", ta, tb);
        free(ta); free(tb);
    }
    return false;
}

static int compare_values(Value a, Value b) {
    if ((a.tag == V_INT || a.tag == V_NUM) && (b.tag == V_INT || b.tag == V_NUM)) {
        double x = v_tonum(a), y = v_tonum(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (IS_OBJ(a, O_TEXT) && IS_OBJ(b, O_TEXT)) {
        int c = strcmp(AS_TEXT(a)->chars, AS_TEXT(b)->chars);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    return 0;
}

/* dynamic method dispatch used for `Any` receivers */
static Value dynamic_method(VM *vm, Value recv, const char *name, Value *args, int nargs, bool *handled) {
    *handled = true;
    if (recv.tag == V_OBJ && recv.as.o && recv.as.o->kind == O_INSTANCE) {
        ObjInstance *inst = (ObjInstance *)recv.as.o;
        if (inst->type_id >= 0 && inst->type_id < vm->prog->types.len) {
            IRType *t = vm->prog->types.items[inst->type_id];
            int ti = inst->type_id;
            while (ti >= 0) {
                IRType *tt = vm->prog->types.items[ti];
                for (int i = 0; i < tt->nmethods; i++)
                    if (strcmp(tt->method_names[i], name) == 0) {
                        Value all[16];
                        all[0] = recv;
                        for (int k = 0; k < nargs && k < 15; k++) all[k + 1] = args[k];
                        return vm_call_function(vm, tt->method_ids[i], all, nargs + 1);
                    }
                ti = tt->base;
            }
            (void)t;
        }
    }
    /* builtin type methods */
    const char *tag = NULL;
    if (IS_OBJ(recv, O_TEXT)) tag = "@Text";
    else if (IS_OBJ(recv, O_LIST)) tag = "@List";
    else if (IS_OBJ(recv, O_MAP)) tag = "@Map";
    else if (IS_OBJ(recv, O_SET)) tag = "@Set";
    else if (IS_OBJ(recv, O_FUTURE)) tag = "@Future";
    else if (IS_OBJ(recv, O_CHAN)) tag = "@Chan";
    if (tag) {
        int nid = natives_lookup(tag, name);
        if (nid >= 0) {
            Value all[16];
            all[0] = recv;
            for (int k = 0; k < nargs && k < 15; k++) all[k + 1] = args[k];
            bool ok = true;
            return vm_native_call(vm, nid, all, nargs + 1, &ok);
        }
    }
    *handled = false;
    return v_nil();
}

static Value run_frame(VM *vm, TryStack *trys) {
    Frame *f = &vm->frames[vm->nframes - 1];
    int base_frame = vm->nframes - 1;
    Value ret = v_nil();
    int instr_budget = 0;

    for (;;) {
        if (vm->nframes <= base_frame) break;
        f = &vm->frames[vm->nframes - 1];
        if (f->ip >= f->fn->code.len) {
            /* fell off the end: return nil */
            vm->nframes--;
            if (vm->nframes <= base_frame) { return ret; }
            Frame *caller = &vm->frames[vm->nframes - 1];
            if (f->ret_reg >= 0) caller->regs[f->ret_reg] = v_nil();
            continue;
        }
        Instr *in = &f->fn->code.items[f->ip];
        Value *R = f->regs;
        f->ip++;
        vm->instr_count++;

        /* cooperative scheduling point for OS threads */
        if ((++instr_budget & 0x3ff) == 0 && g_nvms > 1) {
            sprfst_unlock();
            sched_yield();
            sprfst_lock();
        }

        if (vm->debug_mode && vm->on_breakpoint) {
            const char *file = NULL;
            if (f->fn->file_id >= 0 && vm->sm) {
                SourceFile *sf = sourcemap_get(vm->sm, f->fn->file_id);
                file = sf ? sf->path : NULL;
            }
            vm->on_breakpoint(vm, f, in->line, file);
        }

        switch ((OpCode)in->op) {
            case OP_NOP: case OP_LINE: break;
            case OP_CONST: R[in->a] = f->fn->consts.items[in->b]; break;
            case OP_NIL:   R[in->a] = v_nil(); break;
            case OP_MOVE:  R[in->a] = R[in->b]; break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: {
                Value out;
                if (!arith(vm, (OpCode)in->op, R[in->b], R[in->c], &out)) goto runtime_error;
                R[in->a] = out;
                break;
            }
            case OP_NEG: {
                Value v = R[in->b];
                if (v.tag == V_INT) R[in->a] = v_int(-v.as.i);
                else if (v.tag == V_NUM) R[in->a] = v_num(-v.as.n);
                else { vm_runtime_error(vm, "cannot negate this value"); goto runtime_error; }
                break;
            }
            case OP_CONCAT: {
                char *x = vm_value_text(vm, R[in->b], false);
                char *y = vm_value_text(vm, R[in->c], false);
                size_t lx = strlen(x), ly = strlen(y);
                char *buf = malloc(lx + ly + 1);
                memcpy(buf, x, lx); memcpy(buf + lx, y, ly); buf[lx + ly] = 0;
                R[in->a] = v_obj((Obj *)vm_text(vm, buf, (int)(lx + ly)));
                free(x); free(y); free(buf);
                break;
            }
            case OP_EQ: R[in->a] = v_bool(vm_values_equal(R[in->b], R[in->c])); break;
            case OP_NE: R[in->a] = v_bool(!vm_values_equal(R[in->b], R[in->c])); break;
            case OP_LT: R[in->a] = v_bool(compare_values(R[in->b], R[in->c]) < 0); break;
            case OP_LE: R[in->a] = v_bool(compare_values(R[in->b], R[in->c]) <= 0); break;
            case OP_GT: R[in->a] = v_bool(compare_values(R[in->b], R[in->c]) > 0); break;
            case OP_GE: R[in->a] = v_bool(compare_values(R[in->b], R[in->c]) >= 0); break;
            case OP_NOT: R[in->a] = v_bool(!v_truthy(R[in->b])); break;
            case OP_BAND: R[in->a] = v_int(v_toint(R[in->b]) & v_toint(R[in->c])); break;
            case OP_BOR:  R[in->a] = v_int(v_toint(R[in->b]) | v_toint(R[in->c])); break;
            case OP_BXOR: R[in->a] = v_int(v_toint(R[in->b]) ^ v_toint(R[in->c])); break;
            case OP_SHL:  R[in->a] = v_int(v_toint(R[in->b]) << v_toint(R[in->c])); break;
            case OP_SHR:  R[in->a] = v_int(v_toint(R[in->b]) >> v_toint(R[in->c])); break;
            case OP_JUMP: f->ip = in->a; break;
            case OP_BRTRUE:  if (v_truthy(R[in->b])) f->ip = in->a; break;
            case OP_BRFALSE: if (!v_truthy(R[in->b])) f->ip = in->a; break;
            case OP_CALL: {
                int argbase = in->c >> 8, nargs = in->c & 0xff;
                if (!push_frame(vm, in->b, R + argbase, nargs, NULL, in->a)) goto runtime_error;
                break;
            }
            case OP_CALLV: {
                int argbase = in->c >> 8, nargs = in->c & 0xff;
                Value callee = R[in->b];
                if (!IS_OBJ(callee, O_CLOSURE)) {
                    vm_runtime_error(vm, "this value is not a function");
                    goto runtime_error;
                }
                ObjClosure *c = AS_CLOSURE(callee);
                if (!push_frame(vm, c->fn, R + argbase, nargs, c, in->a)) goto runtime_error;
                break;
            }
            case OP_NATIVE: {
                int argbase = in->c >> 8, nargs = in->c & 0xff;
                bool ok = true;
                Value r = vm_native_call(vm, in->b, R + argbase, nargs, &ok);
                if (!ok) goto runtime_error;
                R[in->a] = r;
                break;
            }
            case OP_METHOD: {
                int argbase = in->c >> 8, nargs = in->c & 0xff;
                Value recv = R[argbase];
                const char *name = ((ObjText *)f->fn->consts.items[in->b].as.o)->chars;
                bool handled = false;
                Value r = dynamic_method(vm, recv, name, R + argbase + 1, nargs - 1, &handled);
                if (!handled) {
                    char *t = vm_value_text(vm, recv, false);
                    vm_runtime_error(vm, "no method `%s` on this value (%s)", name, t);
                    free(t);
                    goto runtime_error;
                }
                R[in->a] = r;
                break;
            }
            case OP_RET: case OP_RETNIL: {
                Value rv = (in->op == OP_RET) ? R[in->a] : v_nil();
                int ret_reg = f->ret_reg;
                vm->nframes--;
                while (trys->len > 0 && trys->entries[trys->len - 1].frame >= vm->nframes) trys->len--;
                if (vm->nframes <= base_frame) return rv;
                Frame *caller = &vm->frames[vm->nframes - 1];
                if (ret_reg >= 0) caller->regs[ret_reg] = rv;
                ret = rv;
                break;
            }
            case OP_NEWLIST: {
                ObjList *l = vm_list(vm);
                for (int i = 0; i < in->c; i++) vec_push(&l->items, R[in->b + i]);
                R[in->a] = v_obj((Obj *)l);
                break;
            }
            case OP_NEWMAP: {
                ObjMap *m = vm_map(vm);
                for (int i = 0; i < in->c; i++) vm_map_set(vm, m, R[in->b + i * 2], R[in->b + i * 2 + 1]);
                R[in->a] = v_obj((Obj *)m);
                break;
            }
            case OP_NEWSET: {
                ObjSet *s = vm_set(vm);
                for (int i = 0; i < in->c; i++) {
                    bool dup = false;
                    vec_foreach(k, &s->items) if (vm_values_equal(s->items.items[k], R[in->b + i])) dup = true;
                    if (!dup) vec_push(&s->items, R[in->b + i]);
                }
                R[in->a] = v_obj((Obj *)s);
                break;
            }
            case OP_NEWOBJ: {
                int argbase = in->c >> 8, nfields = in->c & 0xff;
                IRType *t = (in->b >= 0 && in->b < vm->prog->types.len) ? vm->prog->types.items[in->b] : NULL;
                ObjInstance *o = vm_instance(vm, in->b, t ? t->name : "object", t ? t->nfields : nfields);
                for (int i = 0; i < nfields && i < o->fields.len; i++) o->fields.items[i] = R[argbase + i];
                R[in->a] = v_obj((Obj *)o);
                break;
            }
            case OP_GETFIELD: {
                Value o = R[in->b];
                if (!IS_OBJ(o, O_INSTANCE)) {
                    if (o.tag == V_NIL) { vm_runtime_error(vm, "cannot read a field of nothing (nil)"); goto runtime_error; }
                    vm_runtime_error(vm, "this value has no fields");
                    goto runtime_error;
                }
                ObjInstance *inst = AS_INSTANCE(o);
                if (in->c < 0 || in->c >= inst->fields.len) { R[in->a] = v_nil(); break; }
                R[in->a] = inst->fields.items[in->c];
                break;
            }
            case OP_SETFIELD: {
                Value o = R[in->a];
                if (!IS_OBJ(o, O_INSTANCE)) { vm_runtime_error(vm, "cannot set a field on this value"); goto runtime_error; }
                ObjInstance *inst = AS_INSTANCE(o);
                if (in->b >= 0 && in->b < inst->fields.len) inst->fields.items[in->b] = R[in->c];
                break;
            }
            case OP_GETINDEX: {
                Value o = R[in->b], ix = R[in->c];
                if (IS_OBJ(o, O_LIST)) {
                    ObjList *l = AS_LIST(o);
                    int64_t i = v_toint(ix);
                    if (i < 0) i += l->items.len;
                    if (i < 0 || i >= l->items.len) {
                        vm_runtime_error(vm, "list index %lld is outside the list (length %d)",
                                         (long long)v_toint(ix), l->items.len);
                        goto runtime_error;
                    }
                    R[in->a] = l->items.items[i];
                } else if (IS_OBJ(o, O_MAP)) {
                    bool found = false;
                    R[in->a] = vm_map_get(AS_MAP(o), ix, &found);
                } else if (IS_OBJ(o, O_TEXT)) {
                    ObjText *t = AS_TEXT(o);
                    int64_t i = v_toint(ix);
                    if (i < 0) i += t->len;
                    if (i < 0 || i >= t->len) { vm_runtime_error(vm, "text index %lld is outside the text (length %d)", (long long)i, t->len); goto runtime_error; }
                    R[in->a] = v_obj((Obj *)vm_text(vm, t->chars + i, 1));
                } else if (IS_OBJ(o, O_TENSOR)) {
                    ObjTensor *t = AS_TENSOR(o);
                    int64_t i = v_toint(ix);
                    R[in->a] = (i >= 0 && i < t->count) ? v_num(t->data[i]) : v_nil();
                } else {
                    vm_runtime_error(vm, "this value cannot be indexed with [ ]");
                    goto runtime_error;
                }
                break;
            }
            case OP_SETINDEX: {
                Value o = R[in->a], ix = R[in->b], val = R[in->c];
                if (IS_OBJ(o, O_LIST)) {
                    ObjList *l = AS_LIST(o);
                    int64_t i = v_toint(ix);
                    if (i < 0) i += l->items.len;
                    if (i < 0 || i >= l->items.len) {
                        vm_runtime_error(vm, "list index %lld is outside the list (length %d)", (long long)i, l->items.len);
                        goto runtime_error;
                    }
                    l->items.items[i] = val;
                } else if (IS_OBJ(o, O_MAP)) {
                    vm_map_set(vm, AS_MAP(o), ix, val);
                } else if (IS_OBJ(o, O_TENSOR)) {
                    ObjTensor *t = AS_TENSOR(o);
                    int64_t i = v_toint(ix);
                    if (i >= 0 && i < t->count) t->data[i] = v_tonum(val);
                } else {
                    vm_runtime_error(vm, "cannot assign into this value");
                    goto runtime_error;
                }
                break;
            }
            case OP_GETGLOBAL:
                R[in->a] = (in->b >= 0 && in->b < vm->prog->globals.len) ? vm->prog->globals.items[in->b] : v_nil();
                break;
            case OP_SETGLOBAL:
                if (in->a >= 0 && in->a < vm->prog->globals.len) vm->prog->globals.items[in->a] = R[in->b];
                break;
            case OP_CLOSURE: {
                ObjClosure *c = vm_closure(vm, in->b);
                IRFunc *target = vm->prog->funcs.items[in->b];
                for (int i = 0; i < target->ncaps; i++) {
                    int src = target->cap_src ? target->cap_src[i] : i;
                    vec_push(&c->caps, R[src]);
                }
                R[in->a] = v_obj((Obj *)c);
                break;
            }
            case OP_GETCAP:
                R[in->a] = (f->closure && in->b < f->closure->caps.len) ? f->closure->caps.items[in->b] : v_nil();
                break;
            case OP_ITERNEW: R[in->a] = iterator_new(vm, R[in->b]); break;
            case OP_ITERNEXT: {
                Value itv = R[in->b];
                if (!IS_OBJ(itv, O_ITER)) { f->ip = in->c; break; }
                Value out;
                if (!iterator_next(vm, (ObjIter *)itv.as.o, &out)) { f->ip = in->c; break; }
                R[in->a] = out;
                break;
            }
            case OP_ITERKEY: {
                Value itv = R[in->b];
                R[in->a] = IS_OBJ(itv, O_ITER) ? ((ObjIter *)itv.as.o)->key : v_nil();
                break;
            }
            case OP_TOTEXT: {
                char *s = vm_value_text(vm, R[in->b], false);
                R[in->a] = v_obj((Obj *)vm_text_cstr(vm, s));
                free(s);
                break;
            }
            case OP_TYPEOF: {
                const char *n = "Nil";
                Value v = R[in->b];
                switch (v.tag) {
                    case V_INT: n = "Int"; break;
                    case V_NUM: n = "Num"; break;
                    case V_BOOL: n = "Bool"; break;
                    case V_NIL: n = "Nil"; break;
                    case V_OBJ:
                        switch (v.as.o->kind) {
                            case O_TEXT: n = "Text"; break;
                            case O_LIST: n = "List"; break;
                            case O_MAP: n = "Map"; break;
                            case O_SET: n = "Set"; break;
                            case O_CLOSURE: n = "Fn"; break;
                            case O_INSTANCE: n = ((ObjInstance *)v.as.o)->tname; break;
                            case O_VARIANT: n = ((ObjVariant *)v.as.o)->vname; break;
                            case O_RESULT: n = "Result"; break;
                            case O_FUTURE: n = "Future"; break;
                            case O_CHAN: n = "Chan"; break;
                            case O_TENSOR: n = "Tensor"; break;
                            case O_IMAGE: n = "Image"; break;
                            default: n = "Any";
                        }
                        break;
                }
                R[in->a] = v_obj((Obj *)vm_text_cstr(vm, n));
                break;
            }
            case OP_ISTYPE: {
                int kind = in->c >> 16;
                Value v = R[in->b];
                bool res = false;
                switch (kind) {
                    case TY_INT:  res = v.tag == V_INT; break;
                    case TY_NUM:  res = v.tag == V_NUM || v.tag == V_INT; break;
                    case TY_BOOL: res = v.tag == V_BOOL; break;
                    case TY_NIL:  res = v.tag == V_NIL; break;
                    case TY_TEXT: res = IS_OBJ(v, O_TEXT); break;
                    case TY_LIST: res = IS_OBJ(v, O_LIST); break;
                    case TY_MAP:  res = IS_OBJ(v, O_MAP); break;
                    case TY_SET:  res = IS_OBJ(v, O_SET); break;
                    case TY_OBJECT: case TY_DATA: {
                        int tid = (int)(int16_t)(in->c & 0xffff);
                        res = IS_OBJ(v, O_INSTANCE) && (tid < 0 || AS_INSTANCE(v)->type_id == tid);
                        break;
                    }
                    case TY_ANY: res = true; break;
                    default: res = false;
                }
                R[in->a] = v_bool(res);
                break;
            }
            case OP_CAST: {
                Value v = R[in->b];
                switch (in->c) {
                    case TY_INT: R[in->a] = v_int(IS_OBJ(v, O_TEXT) ? strtoll(AS_TEXT(v)->chars, NULL, 10) : v_toint(v)); break;
                    case TY_NUM: R[in->a] = v_num(IS_OBJ(v, O_TEXT) ? strtod(AS_TEXT(v)->chars, NULL) : v_tonum(v)); break;
                    case TY_BOOL: R[in->a] = v_bool(v_truthy(v)); break;
                    case TY_TEXT: { char *s = vm_value_text(vm, v, false); R[in->a] = v_obj((Obj *)vm_text_cstr(vm, s)); free(s); break; }
                    default: R[in->a] = v;
                }
                break;
            }
            case OP_VARIANT: {
                int tag = in->b >> 16;
                int nameidx = in->b & 0xffff;
                const char *nm = ((ObjText *)f->fn->consts.items[nameidx].as.o)->chars;
                ObjVariant *v = vm_variant(vm, tag, nm);
                int argbase = in->c >> 8, n = in->c & 0xff;
                for (int i = 0; i < n; i++) vec_push(&v->payload, R[argbase + i]);
                R[in->a] = v_obj((Obj *)v);
                break;
            }
            case OP_GETTAG:
                R[in->a] = IS_OBJ(R[in->b], O_VARIANT) ? v_int(AS_VARIANT(R[in->b])->tag) : v_int(-1);
                break;
            case OP_PAYLOAD: {
                Value v = R[in->b];
                R[in->a] = (IS_OBJ(v, O_VARIANT) && in->c < AS_VARIANT(v)->payload.len)
                           ? AS_VARIANT(v)->payload.items[in->c] : v_nil();
                break;
            }
            case OP_OK:  R[in->a] = v_obj((Obj *)vm_result(vm, true, R[in->b])); break;
            case OP_ERR: R[in->a] = v_obj((Obj *)vm_result(vm, false, R[in->b])); break;
            case OP_ISOK: R[in->a] = v_bool(IS_OBJ(R[in->b], O_RESULT) ? AS_RESULT(R[in->b])->ok : R[in->b].tag != V_NIL); break;
            case OP_ISNIL: R[in->a] = v_bool(R[in->b].tag == V_NIL); break;
            case OP_UNWRAP: {
                Value v = R[in->b];
                if (IS_OBJ(v, O_RESULT)) {
                    ObjResult *r = AS_RESULT(v);
                    if (!r->ok && in->c == 1) {
                        char *t = vm_value_text(vm, r->value, false);
                        vm_runtime_error(vm, "unwrapped a failed Result: %s", t);
                        free(t);
                        goto runtime_error;
                    }
                    R[in->a] = r->value;
                } else if (v.tag == V_NIL && in->c == 1) {
                    vm_runtime_error(vm, "unwrapped a value that was nothing (nil)");
                    goto runtime_error;
                } else R[in->a] = v;
                break;
            }
            case OP_FAIL: {
                char *t = vm_value_text(vm, R[in->a], false);
                vm_runtime_error(vm, "%s", t);
                free(t);
                goto runtime_error;
            }
            case OP_TRYPUSH:
                if (trys->len < MAX_TRY) {
                    trys->entries[trys->len].frame = vm->nframes - 1;
                    trys->entries[trys->len].target = in->a;
                    trys->entries[trys->len].err_slot = in->b - 1;
                    trys->len++;
                }
                break;
            case OP_TRYPOP: if (trys->len > 0) trys->len--; break;
            case OP_SPAWN: {
                int argbase = in->c >> 8, nargs = in->c & 0xff;
                Value callee = R[in->b];
                ObjFuture *fu = vm_future(vm);
                if (IS_OBJ(callee, O_CLOSURE)) fu->fn = AS_CLOSURE(callee)->fn;
                for (int i = 0; i < nargs; i++) vec_push(&fu->args, R[argbase + i]);
                R[in->a] = v_obj((Obj *)fu);
                vec_push(&vm->task_queue, R[in->a]);
                break;
            }
            case OP_AWAIT: {
                Value v = R[in->b];
                if (IS_OBJ(v, O_FUTURE)) R[in->a] = resolve_future(vm, AS_FUTURE(v));
                else if (IS_OBJ(v, O_CHAN)) {
                    ObjChan *c = AS_CHAN(v);
                    if (c->buffer.len) {
                        R[in->a] = c->buffer.items[0];
                        for (int i = 0; i < c->buffer.len - 1; i++) c->buffer.items[i] = c->buffer.items[i + 1];
                        c->buffer.len--;
                    } else R[in->a] = v_nil();
                } else R[in->a] = v;
                break;
            }
            case OP_RANGE: {
                ObjRange *r = (ObjRange *)alloc_obj(vm, sizeof(ObjRange), O_RANGE);
                r->lo = v_toint(R[in->b]);
                r->hi = v_toint(R[in->c >> 1]);
                r->inclusive = (in->c & 1) != 0;
                R[in->a] = v_obj((Obj *)r);
                break;
            }
            case OP_LEN: {
                Value v = R[in->b];
                int64_t n = 0;
                if (IS_OBJ(v, O_LIST)) n = AS_LIST(v)->items.len;
                else if (IS_OBJ(v, O_TEXT)) n = AS_TEXT(v)->len;
                else if (IS_OBJ(v, O_MAP)) n = AS_MAP(v)->keys.len;
                else if (IS_OBJ(v, O_SET)) n = AS_SET(v)->items.len;
                R[in->a] = v_int(n);
                break;
            }
            case OP_DROP: break;
            case OP_HALT: return ret;
            default: break;
        }
        continue;

    runtime_error:
        if (trys->len > 0) {
            TryEntry te = trys->entries[--trys->len];
            while (vm->nframes - 1 > te.frame) vm->nframes--;
            Frame *hf = &vm->frames[te.frame];
            if (te.err_slot >= 0 && te.err_slot < hf->fn->nregs)
                hf->regs[te.err_slot] = v_obj((Obj *)vm_text_cstr(vm, vm->error_msg));
            hf->ip = te.target;
            vm->had_error = false;
            vm->error_msg[0] = 0;
            continue;
        }
        return v_nil();
    }
    return ret;
}

/* ------------------------------------------------------------------ API */
VM *vm_new(IRProgram *p, SourceMap *sm, Arena *a) {
    VM *vm = calloc(1, sizeof(VM));
    vm->prog = p;
    vm->sm = sm;
    vm->arena = a;
    vm->stack_size = 8192;
    vm->stack = calloc((size_t)vm->stack_size, sizeof(Value));
    vm->next_gc = 4u << 20;
    vm->start_time = now_seconds();
    vec_init(&vm->task_queue);
    vec_init(&vm->temp_roots);
    { const char *st = getenv("SPRFST_GC_STRESS");
      if (st && *st && *st != '0') g_gc_stress = true; }
    vm_register(vm);
    return vm;
}

void vm_free(VM *vm) {
    if (!vm) return;
    vm_unregister(vm);
    if (vm == g_root) {
        Obj *o = vm->objects;
        while (o) { Obj *n = o->next; free_obj(o); o = n; }
        vm->objects = NULL;
        g_root = NULL;
    }
    vec_free(&vm->task_queue);
    vec_free(&vm->temp_roots);
    free(vm->stack);
    free(vm);
}

static void report_runtime_error(VM *vm) {
    fflush(stdout);
    fprintf(stderr, "\n%s%serror%s  %s%s%s\n", C_BOLD, C_RED, C_RESET, C_BOLD, vm->error_msg, C_RESET);
    /* call stack */
    for (int i = vm->nframes - 1; i >= 0 && i >= vm->nframes - 8; i--) {
        Frame *f = &vm->frames[i];
        int line = (f->ip > 0 && f->ip <= f->fn->code.len) ? f->fn->code.items[f->ip - 1].line : f->fn->line;
        const char *file = "?";
        if (f->fn->file_id >= 0 && vm->sm) {
            SourceFile *sf = sourcemap_get(vm->sm, f->fn->file_id);
            if (sf) file = sf->path;
        }
        fprintf(stderr, "  %sin%s %s%s%s  %s%s:%d%s\n", C_GRAY, C_RESET, C_AMBER,
                f->fn->qualname ? f->fn->qualname : f->fn->name, C_RESET, C_GRAY, file, line, C_RESET);
        if (vm->sm && f->fn->file_id >= 0) {
            Str src = sourcemap_line(vm->sm, f->fn->file_id, line);
            if (src.n) fprintf(stderr, "     %s" STRFMT "%s\n", C_DIM, STRARG(src), C_RESET);
        }
    }
    fprintf(stderr, "\n");
}

int vm_run(VM *vm, int argc, char **argv) {
    vm_set_args(argc, argv);
    sprfst_lock();
    Value r = vm_call_function(vm, vm->prog->entry, NULL, 0);
    /* drain any tasks that were spawned but never awaited */
    vec_foreach(i, &vm->task_queue) {
        Value t = vm->task_queue.items[i];
        if (IS_OBJ(t, O_FUTURE) && !AS_FUTURE(t)->done) resolve_future(vm, AS_FUTURE(t));
    }
    sprfst_unlock();
    if (vm->had_error) {
        report_runtime_error(vm);
        return vm->exit_code ? vm->exit_code : 1;
    }
    if (r.tag == V_INT) return (int)r.as.i;
    return vm->exit_code;
}
