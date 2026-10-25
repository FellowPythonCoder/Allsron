/* ========================================================================
   SPRFST — SPIR (intermediate representation), values and the runtime
   ======================================================================== */
#ifndef SPRFST_VM_H
#define SPRFST_VM_H

#include "sprfst/sema.h"

/* ---------------------------------------------------------------- SPIR */
#define SPRFST_OPS(X)                                                        \
    X(OP_NOP)        X(OP_CONST)     X(OP_MOVE)      X(OP_NIL)               \
    X(OP_ADD)        X(OP_SUB)       X(OP_MUL)       X(OP_DIV)               \
    X(OP_MOD)        X(OP_POW)       X(OP_NEG)       X(OP_CONCAT)            \
    X(OP_EQ)         X(OP_NE)        X(OP_LT)        X(OP_LE)                \
    X(OP_GT)         X(OP_GE)        X(OP_NOT)                               \
    X(OP_BAND)       X(OP_BOR)       X(OP_BXOR)      X(OP_SHL)   X(OP_SHR)   \
    X(OP_JUMP)       X(OP_BRTRUE)    X(OP_BRFALSE)                           \
    X(OP_CALL)       X(OP_CALLV)     X(OP_NATIVE)    X(OP_METHOD)            \
    X(OP_RET)        X(OP_RETNIL)                                            \
    X(OP_NEWLIST)    X(OP_NEWMAP)    X(OP_NEWSET)    X(OP_NEWOBJ)            \
    X(OP_GETFIELD)   X(OP_SETFIELD)  X(OP_GETINDEX)  X(OP_SETINDEX)          \
    X(OP_GETGLOBAL)  X(OP_SETGLOBAL) X(OP_CLOSURE)   X(OP_GETCAP)            \
    X(OP_ITERNEW)    X(OP_ITERNEXT)  X(OP_ITERKEY)                           \
    X(OP_TOTEXT)     X(OP_TYPEOF)    X(OP_ISTYPE)    X(OP_CAST)              \
    X(OP_VARIANT)    X(OP_GETTAG)    X(OP_PAYLOAD)                           \
    X(OP_OK)         X(OP_ERR)       X(OP_ISOK)      X(OP_UNWRAP)            \
    X(OP_ISNIL)      X(OP_FAIL)      X(OP_TRYPUSH)   X(OP_TRYPOP)            \
    X(OP_SPAWN)      X(OP_AWAIT)     X(OP_DROP)      X(OP_RANGE)             \
    X(OP_LEN)        X(OP_ASSERT)    X(OP_HALT)      X(OP_LINE)

typedef enum {
#define X(o) o,
    SPRFST_OPS(X)
#undef X
    OP_COUNT
} OpCode;

extern const char *const OP_NAMES[OP_COUNT];

typedef struct {
    uint16_t op;
    int32_t  a, b, c;
    int32_t  line;
} Instr;

typedef struct { Instr *items; int len, cap; } InstrVec;

/* --------------------------------------------------------------- values */
typedef struct Obj Obj;
typedef struct VM VM;

typedef enum { V_NIL, V_INT, V_NUM, V_BOOL, V_OBJ } ValueTag;

typedef struct {
    uint8_t tag;
    union {
        int64_t i;
        double  n;
        bool    b;
        Obj    *o;
    } as;
} Value;

typedef struct { Value *items; int len, cap; } ValueVec;

typedef enum {
    O_TEXT, O_LIST, O_MAP, O_SET, O_CLOSURE, O_INSTANCE, O_VARIANT,
    O_RESULT, O_ITER, O_CHAN, O_FUTURE, O_RANGE, O_NATIVE, O_TENSOR, O_IMAGE
} ObjKind;

struct Obj {
    ObjKind kind;
    bool    marked;
    Obj    *next;
};

typedef struct { Obj obj; int len; char *chars; uint64_t hash; } ObjText;
typedef struct { Obj obj; ValueVec items; } ObjList;
typedef struct { Obj obj; ValueVec keys, vals; } ObjMap;       /* small-map, linear probe free */
typedef struct { Obj obj; ValueVec items; } ObjSet;
typedef struct { Obj obj; int fn; ValueVec caps; } ObjClosure;
typedef struct { Obj obj; int type_id; ValueVec fields; const char *tname; } ObjInstance;
typedef struct { Obj obj; int tag; ValueVec payload; const char *ename, *vname; } ObjVariant;
typedef struct { Obj obj; bool ok; Value value; } ObjResult;
typedef struct { Obj obj; Value src; int index; int kind; Value key; } ObjIter;
typedef struct { Obj obj; ValueVec buffer; int cap; bool closed; } ObjChan;
typedef struct { Obj obj; Value result; bool done; int fn; ValueVec args; int thread_id; } ObjFuture;
typedef struct { Obj obj; int64_t lo, hi; bool inclusive; } ObjRange;
typedef struct { Obj obj; int ntype; void *ptr; int64_t handle; } ObjNative;
typedef struct { Obj obj; int ndim; int shape[4]; int count; double *data; } ObjTensor;
typedef struct { Obj obj; int w, h; uint32_t *px; } ObjImage;

#define AS_TEXT(v)     ((ObjText *)(v).as.o)
#define AS_LIST(v)     ((ObjList *)(v).as.o)
#define AS_MAP(v)      ((ObjMap *)(v).as.o)
#define AS_SET(v)      ((ObjSet *)(v).as.o)
#define AS_CLOSURE(v)  ((ObjClosure *)(v).as.o)
#define AS_INSTANCE(v) ((ObjInstance *)(v).as.o)
#define AS_VARIANT(v)  ((ObjVariant *)(v).as.o)
#define AS_RESULT(v)   ((ObjResult *)(v).as.o)
#define AS_CHAN(v)     ((ObjChan *)(v).as.o)
#define AS_FUTURE(v)   ((ObjFuture *)(v).as.o)
#define AS_RANGE(v)    ((ObjRange *)(v).as.o)
#define AS_NATIVE(v)   ((ObjNative *)(v).as.o)
#define AS_TENSOR(v)   ((ObjTensor *)(v).as.o)
#define AS_IMAGE(v)    ((ObjImage *)(v).as.o)

#define IS_OBJ(v, k)   ((v).tag == V_OBJ && (v).as.o && (v).as.o->kind == (k))

static inline Value v_nil(void)          { Value v; v.tag = V_NIL; v.as.i = 0; return v; }
static inline Value v_int(int64_t i)     { Value v; v.tag = V_INT; v.as.i = i; return v; }
static inline Value v_num(double n)      { Value v; v.tag = V_NUM; v.as.n = n; return v; }
static inline Value v_bool(bool b)       { Value v; v.tag = V_BOOL; v.as.b = b; return v; }
static inline Value v_obj(Obj *o)        { Value v; v.tag = V_OBJ; v.as.o = o; return v; }
static inline double v_tonum(Value v)    { return v.tag == V_INT ? (double)v.as.i : v.tag == V_NUM ? v.as.n : 0.0; }
static inline int64_t v_toint(Value v)   { return v.tag == V_INT ? v.as.i : v.tag == V_NUM ? (int64_t)v.as.n : 0; }
static inline bool v_truthy(Value v)     { return v.tag == V_BOOL ? v.as.b : v.tag == V_NIL ? false : true; }

/* ------------------------------------------------------------ functions */
typedef struct {
    const char *name;
    const char *qualname;
    int         nparams, nregs, ncaps;
    InstrVec    code;
    ValueVec    consts;
    int        *cap_src;        /* outer frame slot for every capture */
    int        *cap_slot;       /* local slot the capture is copied into */
    bool        is_task, is_method, is_test, is_bench;
    FnDecl     *decl;
    int         file_id;
    int         line;
    const char *doc;
    NameVec     local_names;    /* slot -> source name (debug symbols) */
} IRFunc;

typedef struct { IRFunc **items; int len, cap; } IRFuncVec;

typedef struct {
    const char *name;
    int         nfields;
    const char **field_names;
    int         init_fn;        /* index of init method, or -1 */
    int         drop_fn;        /* index of drop method, or -1 */
    TypeDecl   *decl;
    int         base;           /* index of base type, or -1 */
    int        *method_ids;     /* parallel to method_names */
    const char **method_names;
    int         nmethods;
} IRType;

typedef struct { IRType **items; int len, cap; } IRTypeVec;

typedef struct {
    IRFuncVec  funcs;
    IRTypeVec  types;
    ValueVec   globals;
    NameVec    global_names;
    int        entry;           /* index of main, -1 if none */
    Arena     *arena;
    SourceMap *sm;
    const char *name;
} IRProgram;

/* -------------------------------------------------------------- lowering */
IRProgram *ir_lower(Arena *a, Sema *sema, SourceMap *sm, DiagBag *db);
void       ir_dump(IRProgram *p, StrBuf *out);

/* ------------------------------------------------------------- optimizer */
typedef struct {
    int folded, moves_removed, dead_removed, jumps_threaded, blocks_removed;
} OptStats;
OptStats ir_optimize(IRProgram *p, int level);

/* --------------------------------------------------------------- runtime */
typedef struct Frame {
    IRFunc  *fn;
    Value   *regs;
    int      ip;
    int      ret_reg;
    ObjClosure *closure;
    int      try_sp;
} Frame;

#define VM_MAX_FRAMES 512

struct VM {
    IRProgram *prog;
    Value     *stack;        /* register stack */
    int        stack_size;
    Frame      frames[VM_MAX_FRAMES];
    int        nframes;
    Obj       *objects;      /* all heap objects (GC list) */
    size_t     bytes_allocated, next_gc, peak_bytes;
    int        obj_count;
    bool       had_error;
    char       error_msg[512];
    Span       error_span;
    int        exit_code;
    SourceMap *sm;
    Arena     *arena;
    /* debug support */
    bool       debug_mode;
    bool       trace;
    int        step_mode;     /* 0 run, 1 step over, 2 step into, 3 step out */
    int        step_depth;
    void     (*on_breakpoint)(VM *vm, Frame *f, int line, const char *file);
    void      *host;          /* Studio / debugger host context */
    int        instr_count;
    double     start_time;
    ValueVec   task_queue;
    int        gc_runs;
    /* anything a native allocates stays reachable until the native returns */
    ValueVec   temp_roots;
    int        in_native;
};

VM   *vm_new(IRProgram *p, SourceMap *sm, Arena *a);
int   vm_run(VM *vm, int argc, char **argv);
Value vm_call_function(VM *vm, int fn_index, Value *args, int nargs);
Value vm_call_value(VM *vm, Value callable, Value *args, int nargs);
void  vm_free(VM *vm);
void  vm_runtime_error(VM *vm, const char *fmt, ...);

/* object helpers (shared with the native library) */
ObjText     *vm_text(VM *vm, const char *s, int len);
ObjInstance *vm_instance_copy(VM *vm, ObjInstance *src);
ObjText     *vm_text_cstr(VM *vm, const char *s);
ObjList     *vm_list(VM *vm);
ObjMap      *vm_map(VM *vm);
ObjSet      *vm_set(VM *vm);
ObjNative   *vm_native_obj(VM *vm, int ntype, void *ptr, int64_t handle);
ObjTensor   *vm_tensor(VM *vm, int ndim, int *shape);
ObjImage    *vm_image(VM *vm, int w, int h);
ObjFuture   *vm_future(VM *vm);
ObjChan     *vm_chan(VM *vm, int cap);
ObjResult   *vm_result(VM *vm, bool ok, Value v);
void         vm_gc(VM *vm);
void         vm_root_add(Value v);   /* pin a value against collection */
int          vm_native_enter(VM *vm);                      /* open a scope of temporaries */
void         vm_native_leave(VM *vm, int mark, Value result); /* close it, keeping the result */
char        *vm_value_text(VM *vm, Value v, bool quote_text);
bool         vm_values_equal(Value a, Value b);
uint64_t     vm_value_hash(Value v);
Value        vm_map_get(ObjMap *m, Value key, bool *found);
void         vm_map_set(VM *vm, ObjMap *m, Value key, Value val);
bool         vm_map_remove(ObjMap *m, Value key, Value *out);

/* native dispatch (implemented in nativelib.c) */
Value vm_native_call(VM *vm, int native_id, Value *args, int nargs, bool *ok);
void  vm_native_shutdown(void);
void  vm_set_args(int argc, char **argv);

#endif
