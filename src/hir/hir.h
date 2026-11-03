#ifndef PAXSY_HIR_H
#define PAXSY_HIR_H

/*
 * hir.h - the Paxsy High-level Intermediate Representation.
 *
 * HIR sits between the semantic pass and MLIR/LLVM emission. It keeps
 * the control-flow structure of the source intact (blocks, if/while,
 * for ranges, match, defer) while making every value explicitly typed
 * and every operation explicit. Language features that do not map
 * directly onto LLVM keep dedicated HIR nodes so that lowering rules
 * stay in one place:
 *   - T! error values      -> error codes with {value, error} pairs
 *   - defer                  -> per-scope exit action lists
 *   - chan / select / close  -> runtime calls with a dedicated node kind
 *     (also the GPU offload boundary: loops containing them never
 *     offload)
 *   - routine / async        -> spawn nodes for the M:N scheduler
 *   - comptime parameters    -> monomorphization happens before HIR
 *
 * Ownership: one HModule owns every node it produced; freeing the
 * module frees the nodes. Node names are borrowed from the AST, which
 * outlives the HIR inside one pipeline run. Types follow the
 * frontend-wide arena policy and live for the rest of the process.
 */

#include "common.h"
#include "token.h"

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    HTY_VOID,        /* Void                                    */
    HTY_BOOL,        /* Bool                                    */
    HTY_CHAR,        /* Char (UTF-8 code unit)                  */
    HTY_INT,         /* signed integer, width below             */
    HTY_UINT,        /* unsigned integer, width below           */
    HTY_REAL,        /* binary floating point, width below      */
    HTY_PTR,         /* pointer to base                         */
    HTY_ARRAY,       /* fixed-size array: elem * count          */
    HTY_NAMED,       /* struct/union/enum reference by name     */
    HTY_ERROR,       /* T! : value type + error code            */
    HTY_FUNC,        /* function type: params -> ret            */
    HTY_CHAN,        /* channel of elem (runtime handle)        */
    HTY_COMPTIME,    /* comptime parameter slot (pre-mono)      */
} HTypeKind;

typedef struct HType HType;
struct HType {
    HTypeKind kind;
    int width;            /* bits for INT/UINT/REAL, else 0 */
    union {
        struct { const HType *elem; } ptr;
        struct { const HType *elem; long count; } array;
        struct { const char *name; } named;
        struct { const HType *value; } error;  /* HTY_ERROR: value type */
        struct { const HType **params; int nparams;
                const HType *ret; } func;
        struct { const HType *elem; } chan;
        struct { const char *name; } comptime;
    } u;
};

/* Byte-slice payload of string/char constants and inline asm. */
struct hslice { const char *start; int len; };

extern const HType hty_void, hty_bool, hty_char;
const HType *hty_int(int width);
const HType *hty_uint(int width);
const HType *hty_real(int width);
const HType *hty_ptr(const HType *elem);
const HType *hty_array(const HType *elem, long count);
const HType *hty_error(const HType *value);
const HType *hty_named(const char *name);
const HType *hty_func(const HType **params, int nparams, const HType *ret);
const HType *hty_chan(const HType *elem);

/* ------------------------------------------------------------------ */
/* Nodes                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    /* module level */
    HK_MODULE,
    /* functions */
    HK_FUNC,         /* name, params, ret, body, flags          */
    HK_EXTERN_FUNC,  /* declared prototype without a body       */
    HK_STRUCT,       /* named aggregate: fields                 */
    HK_UNION,
    HK_ENUM,         /* variants with discriminants             */
    HK_GLOBAL,       /* module-level let with optional init     */
    /* statements */
    HK_BLOCK,        /* children are statements, exit set       */
    HK_LET,          /* local binding with optional init        */
    HK_STORE,        /* assignment to an l-value                */
    HK_IF,           /* cond, then, else?                       */
    HK_WHILE,        /* cond, body                              */
    HK_DOWHILE,      /* body, cond                              */
    HK_FOR,          /* range loop: bounds, var, body           */
    HK_MATCH,        /* subject, cases, else?                   */
    HK_RETURN,       /* optional value                          */
    HK_BREAK,        /* optional label                          */
    HK_CONTINUE,     /* optional label                          */
    HK_DEFER,        /* action executed at scope exit           */
    HK_EXPR_STMT,    /* expression evaluated for effect         */
    HK_LABEL,        /* named loop target                       */
    /* channel operations (GPU offload boundary) */
    HK_SEND,         /* channel <- value                        */
    HK_RECV,         /* <- channel (value expression)           */
    HK_SELECT,       /* multiplexed arms + optional else        */
    HK_CLOSE,        /* close channel                           */
    HK_GO_ROUTINE,   /* routine { } (M:N scheduler task)        */
    HK_ASYNC,        /* async { } (native thread task)          */
    HK_AWAIT,        /* wait for an async channel result        */
    HK_INLINE_ASM,   /* asm "..." passthrough                   */
    /* expressions */
    HK_CONST_INT,
    HK_CONST_FLOAT,
    HK_CONST_STRING,
    HK_CONST_CHAR,
    HK_PARAM,        /* function parameter by index             */
    HK_LOCAL,        /* let-bound local by slot                 */
    HK_GLOBAL_REF,   /* module-level name                       */
    HK_BINOP,
    HK_UNOP,
    HK_CALL,
    HK_CAST,
    HK_INDEX,        /* array element                           */
    HK_SLICE,        /* sub-range of an array                   */
    HK_FIELD,        /* struct/union field by index             */
    HK_STRUCT_LIT,
    HK_ARRAY_LIT,
    HK_ADDR,         /* & l-value                               */
    HK_DEREF,        /* * pointer                               */
    HK_ALLOC,        /* heap allocation (counts for pairing)    */
    HK_FREE,         /* heap release                            */
    HK_NEW_CHAN,     /* chan of T [cap]                         */
    HK_TRY,          /* try expr: unwrap or propagate           */
    HK_CATCH,        /* catch |e| expr: error dispatch          */
    HK_FMT,          /* string interpolation parts              */
    HK_PANIC,        /* unrecoverable abort                     */
    HK_ERR_VAL,      /* error <expr>: construct an error value  */
} HNodeKind;

/* Effect flags used by the offload analysis: a function or loop that
 * contains any of these never maps onto the GPU. */
#define HF_SEND_RECV    0x0001 /* HK_SEND/HK_RECV/HK_SELECT/HK_CLOSE */
#define HF_ALLOC        0x0002 /* HK_ALLOC/HK_FREE                   */
#define HF_DEFER        0x0004 /* HK_DEFER                           */
#define HF_SPAWN        0x0008 /* HK_GO_ROUTINE/HK_ASYNC/HK_AWAIT    */
#define HF_UNKNOWN_CALL 0x0010 /* call to an unresolved/external fn   */
#define HF_EXPR         0x0020 /* HK_IF/HK_MATCH: value position, the
                                  branch blocks yield a trailing expr  */
#define HF_EXCLUSIVE    0x0100 /* HK_FOR: '..<h' upper bound           */

typedef enum {
    HOP_ADD, HOP_SUB, HOP_MUL, HOP_DIV, HOP_MOD,
    HOP_AND, HOP_OR, HOP_XOR, HOP_SHL, HOP_SHR, HOP_USHR,
    HOP_EQ, HOP_NE, HOP_LT, HOP_LE, HOP_GT, HOP_GE,
    HOP_LAND, HOP_LOR,
} HBinOp;

typedef enum {
    HUN_NEG, HUN_NOT, HUN_LNOT, HUN_BITNOT, HUN_DEREF, HOP_ADDR_OF,
} HUnOp;

typedef struct HNode HNode;
struct HNode {
    HNodeKind kind;
    const HType *type;    /* type of the value the node produces      */
    Token tok;            /* origin token for diagnostics             */
    int flags;            /* HF_* effect flags, computed upwards      */
    const char *name;     /* function/field/label/global name         */
    int aux;              /* emitter string-global id (CONST_STRING/
                             CONST_CHAR/FMT); -1 uncollected, -2 skip */
    union {
        long long ival;         /* HK_CONST_INT                    */
        double fval;            /* HK_CONST_FLOAT                  */
        struct hslice slice;    /* HK_CONST_STRING/CHAR, HK_INLINE_ASM */
        int index;              /* HK_PARAM/HK_LOCAL/HK_FIELD slot */
        HBinOp binop;           /* HK_BINOP                        */
        HUnOp unop;             /* HK_UNOP                         */
    } u;
    /* Generic child slots: most nodes use <= 4; blocks, match cases
     * and format parts keep an item array. */
    HNode *child[4];
    HNode **items;
    int nitems;
};

typedef struct HModule HModule;

HModule *hir_module_new(void);
HNode *hir_root(HModule *m);
void hir_module_free(HModule *m);
HNode *hir_node(HModule *m, HNodeKind kind, Token tok);
void hir_add(HNode *parent, HNode *item);
void hir_set_child(HNode *n, int slot, HNode *child);

/* Text form of one HIR module; used by the --hir dump flag. */
void hir_dump(const HModule *m, FILE *out);

#endif /* PAXSY_HIR_H */
