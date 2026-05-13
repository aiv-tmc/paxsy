#ifndef IR_H
#define IR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "../semantic/semantic.h"
#include "../parser/parser.h"

typedef struct IrBasicBlock IrBasicBlock;
typedef struct IrFunction   IrFunction;
typedef struct IrModule     IrModule;
typedef struct IrInstruction IrInstruction;
typedef struct IrBuilder    IrBuilder;

/* IR opcodes – all are architecture‑independent. */
typedef enum {
    IR_NOP, IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD, IR_NEG,
    IR_EQ, IR_NEQ, IR_LT, IR_LE, IR_GT, IR_GE,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR, IR_SAR, IR_NOT,
    IR_LOAD, IR_STORE, IR_ALLOCA, IR_GEP,
    IR_BR, IR_BRCOND, IR_CALL, IR_RET, IR_PHI, IR_CAST
} IrOpcode;

/* Kinds of IR values. */
typedef enum {
    IR_VALUE_NONE, IR_VALUE_TEMP, IR_VALUE_CONST_INT, IR_VALUE_CONST_REAL,
    IR_VALUE_CONST_CHAR, IR_VALUE_GLOBAL_SYMBOL, IR_VALUE_PARAM,
    IR_VALUE_LABEL, IR_VALUE_STRUCT_FIELD, IR_VALUE_STRUCT_INIT
} IrValueKind;

typedef struct IrValue {
    IrValueKind kind;
    DataType    type;
    Type       *type_info;
    union {
        int64_t  int_val;
        double   real_val;
        char     char_val;
        uint32_t field_index;
    } const_data;
    char        name[64];
    uint32_t    id;
} IrValue;

/* Instruction structure – no machine‑specific fields. */
struct IrInstruction {
    IrOpcode      opcode;
    IrValue      *result;
    IrValue      *operand1;
    IrValue      *operand2;
    void         *extra;
    IrBasicBlock *parent;
    struct IrInstruction *prev;
    struct IrInstruction *next;
};

/* Extra data for GEP, calls, branches, phi. */
typedef struct IrGepExtra { IrValue **indices; uint32_t index_count; } IrGepExtra;
typedef struct IrCallExtra { IrValue **args; uint32_t arg_count; } IrCallExtra;
typedef struct IrCondBranchExtra {
    IrBasicBlock *true_target;
    IrBasicBlock *false_target;
} IrCondBranchExtra;
typedef struct IrPhiExtra { IrValue **values; IrBasicBlock **blocks; uint32_t count; } IrPhiExtra;

/* Basic block – holds a list of IR instructions. */
struct IrBasicBlock {
    char                 label[64];
    IrInstruction       *first_inst;
    IrInstruction       *last_inst;
    IrFunction          *function;
    uint32_t             id;
    IrBasicBlock       **predecessors;
    uint32_t             pred_count, pred_capacity;
    IrBasicBlock       **successors;
    uint32_t             succ_count, succ_capacity;
    IrInstruction       **phi_nodes;
    uint32_t             phi_count, phi_capacity;
};

/* Function – contains basic blocks and parameters. */
struct IrFunction {
    char             *name;
    DataType          return_type;
    Type             *return_type_info;
    IrBasicBlock     *entry_block;
    IrBasicBlock    **all_blocks;
    uint32_t          block_count, block_capacity;
    IrValue         **parameters;
    uint32_t          param_count, param_capacity;
    uint32_t          next_temp_id;
    uint32_t          next_block_id;
    IrModule         *module;
};

/* Module – container for functions. */
struct IrModule {
    IrFunction      **functions;
    uint32_t          func_count, func_capacity;
    SymbolTable      *symbols;
};

/* IR builder – state for constructing IR. */
struct IrBuilder {
    IrModule         *module;
    IrFunction       *current_function;
    IrBasicBlock     *current_block;
    SemanticContext  *sem_ctx;
    struct {
        char      name[128];
        IrValue  *ir_value;
    } *locals;
    uint32_t          local_count, local_capacity;
    IrBasicBlock    **break_stack;
    uint32_t          break_count, break_capacity;
    IrBasicBlock    **continue_stack;
    uint32_t          continue_count, continue_capacity;
};

/* Public API – IR construction only. */
IrModule    *ir__module_create(SymbolTable *global_scope);
void         ir__module_destroy(IrModule *mod);
IrModule    *ir__generate_module(SemanticContext *sem_ctx, AST *ast);
void         ir__print_module(FILE *f, const IrModule *mod);

IrValue     *ir__value_temp(IrFunction *func, DataType type, Type *type_info);
IrValue     *ir__value_const_int(int64_t val);
IrValue     *ir__value_const_real(double val);
IrValue     *ir__value_const_char(char val);
IrValue     *ir__value_global(const char *name, DataType type, Type *type_info);
IrValue     *ir__value_param(IrFunction *func, uint32_t index, DataType type, Type *type_info);
IrValue     *ir__value_label(IrBasicBlock *block);
IrValue     *ir__value_struct_field(uint32_t index);
void         ir__value_free(IrValue *val);

IrInstruction *ir__emit_op2(IrBuilder *b, IrOpcode op, IrValue *result,
                            IrValue *op1, IrValue *op2);
IrInstruction *ir__emit_op1(IrBuilder *b, IrOpcode op, IrValue *result, IrValue *op1);
IrInstruction *ir__emit_store(IrBuilder *b, IrValue *ptr, IrValue *val);
IrInstruction *ir__emit_load(IrBuilder *b, IrValue *result, IrValue *ptr);
IrInstruction *ir__emit_br(IrBuilder *b, IrBasicBlock *target);
IrInstruction *ir__emit_brcond(IrBuilder *b, IrValue *cond,
                               IrBasicBlock *true_bb, IrBasicBlock *false_bb);
IrInstruction *ir__emit_ret(IrBuilder *b, IrValue *value);
IrInstruction *ir__emit_call(IrBuilder *b, IrValue *result, IrValue *callee,
                             IrValue **args, uint32_t arg_count);
IrInstruction *ir__emit_phi(IrBuilder *b, IrValue *result,
                            IrValue **values, IrBasicBlock **blocks, uint32_t count);
IrInstruction *ir__emit_alloca(IrBuilder *b, IrValue *result,
                               DataType pointee_type, Type *pointee_info);
IrInstruction *ir__emit_gep(IrBuilder *b, IrValue *result, IrValue *base,
                            IrValue **indices, uint32_t index_count);
IrInstruction *ir__emit_cast(IrBuilder *b, IrValue *result, IrValue *src,
                             DataType target_type, Type *target_info);
IrInstruction *ir__emit_nop(IrBuilder *b);

IrBuilder    *ir__builder_create(SemanticContext *sem_ctx);
void          ir__builder_destroy(IrBuilder *b);
IrFunction   *ir__builder_start_function(IrBuilder *b, const char *name,
                                         DataType return_type, Type *return_type_info,
                                         IrValue **params, uint32_t param_count);
IrBasicBlock *ir__builder_add_block(IrBuilder *b, const char *label, bool set_current);
void          ir__builder_set_block(IrBuilder *b, IrBasicBlock *block);
IrValue      *ir__builder_get_local(IrBuilder *b, const char *name);
void          ir__builder_set_local(IrBuilder *b, const char *name, IrValue *alloca);
char         *ir__builder_temp_name(IrFunction *func, char *buf, size_t bufsize);
IrModule     *ir__build_from_ast(IrBuilder *b, AST *ast);

#endif
