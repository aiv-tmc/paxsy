#include "ir.h"
#include "../errhandler/errhandler.h"
#include "../utils/str_utils.h"
#include "../utils/memory_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *ir_alloc(size_t size) {
    void *p = calloc(1, size);
    if (!p) errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "ir", "IR memory allocation failed");
    return p;
}
static void ir_free(void *p) { free(p); }

static bool grow_ptr_array(void ***array, uint32_t *count, uint32_t *capacity) {
    uint32_t new_cap = (*capacity == 0) ? 4 : (*capacity * 2);
    void **new_arr = realloc(*array, new_cap * sizeof(void *));
    if (!new_arr) {
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , 0
            , 0
            , "ir"
            , "Failed to grow IR pointer array"
        );
        return false;
    }
    *array = (void **)new_arr;
    *capacity = new_cap;
    return true;
}

static IrBasicBlock *create_block(IrFunction *func, const char *name) {
    IrBasicBlock *bb = ir_alloc(sizeof(IrBasicBlock));
    if (!bb) return NULL;
    snprintf(bb->label, sizeof(bb->label), "%s", name ? name : "");
    bb->id = func->next_block_id++;
    bb->function = func;
    if (func->block_count >= func->block_capacity) {
        if (!grow_ptr_array((void ***)&func->all_blocks, &func->block_count, &func->block_capacity)) {
            ir_free(bb);
            return NULL;
        }
    } else if (func->all_blocks == NULL) {
        func->block_capacity = 4;
        func->all_blocks = ir_alloc(4 * sizeof(IrBasicBlock *));
        if (!func->all_blocks) { ir_free(bb); return NULL; }
    }
    func->all_blocks[func->block_count++] = bb;
    return bb;
}

static bool add_predecessor(IrBasicBlock *bb, IrBasicBlock *pred) {
    if (bb->pred_count >= bb->pred_capacity)
        if (!grow_ptr_array((void ***)&bb->predecessors, &bb->pred_count, &bb->pred_capacity))
            return false;
    bb->predecessors[bb->pred_count++] = pred;
    return true;
}
static bool add_successor(IrBasicBlock *bb, IrBasicBlock *succ) {
    if (bb->succ_count >= bb->succ_capacity)
        if (!grow_ptr_array((void ***)&bb->successors, &bb->succ_count, &bb->succ_capacity))
            return false;
    bb->successors[bb->succ_count++] = succ;
    return true;
}
static void link_blocks(IrBasicBlock *from, IrBasicBlock *to) {
    add_successor(from, to);
    add_predecessor(to, from);
}

static void append_instruction(IrBasicBlock *bb, IrInstruction *inst) {
    inst->parent = bb;
    if (bb->last_inst) { bb->last_inst->next = inst; inst->prev = bb->last_inst; }
    else { bb->first_inst = inst; inst->prev = NULL; }
    bb->last_inst = inst;
    inst->next = NULL;
}

static IrValue *new_ir_value(IrValueKind kind, DataType type, Type *type_info) {
    IrValue *v = ir_alloc(sizeof(IrValue));
    if (!v) return NULL;
    v->kind = kind; v->type = type; v->type_info = type_info; v->name[0] = '\0';
    return v;
}

IrValue *ir__value_temp(IrFunction *func, DataType type, Type *type_info) {
    IrValue *v = new_ir_value(IR_VALUE_TEMP, type, type_info);
    if (!v) return NULL;
    v->id = func->next_temp_id++;
    snprintf(v->name, sizeof(v->name), "%%t%u", v->id);
    return v;
}

IrValue *ir__value_const_int(int64_t val) {
    IrValue *v = new_ir_value(IR_VALUE_CONST_INT, TYPE_INT, NULL);
    if (!v) return NULL;
    v->const_data.int_val = val;
    snprintf(v->name, sizeof(v->name), "%lld", (long long)val);
    return v;
}

IrValue *ir__value_const_real(double val) {
    IrValue *v = new_ir_value(IR_VALUE_CONST_REAL, TYPE_REAL, NULL);
    if (!v) return NULL;
    v->const_data.real_val = val;
    snprintf(v->name, sizeof(v->name), "%.6g", val);
    return v;
}

IrValue *ir__value_const_char(char val) {
    IrValue *v = new_ir_value(IR_VALUE_CONST_CHAR, TYPE_CHAR, NULL);
    if (!v) return NULL;
    v->const_data.char_val = val;
    snprintf(v->name, sizeof(v->name), "'%c'", val);
    return v;
}

IrValue *ir__value_global(const char *name, DataType type, Type *type_info) {
    IrValue *v = new_ir_value(IR_VALUE_GLOBAL_SYMBOL, type, type_info);
    if (!v) return NULL;
    strncpy(v->name, name, sizeof(v->name)-1);
    return v;
}

IrValue *ir__value_param(IrFunction *func, uint32_t index, DataType type, Type *type_info) {
    IrValue *v = new_ir_value(IR_VALUE_PARAM, type, type_info);
    if (!v) return NULL;
    v->id = index;
    snprintf(v->name, sizeof(v->name), "%%p%u", index);
    return v;
}

IrValue *ir__value_label(IrBasicBlock *block) {
    IrValue *v = new_ir_value(IR_VALUE_LABEL, TYPE_LABEL, NULL);
    if (!v) return NULL;
    strncpy(v->name, block->label, sizeof(v->name)-1);
    return v;
}

IrValue *ir__value_struct_field(uint32_t index) {
    IrValue *v = new_ir_value(IR_VALUE_STRUCT_FIELD, TYPE_INT, NULL);
    if (!v) return NULL;
    v->const_data.field_index = index;
    snprintf(v->name, sizeof(v->name), "field%u", index);
    return v;
}

void ir__value_free(IrValue *val) { ir_free(val); }

static IrInstruction *emit_instruction
    ( IrBuilder *b
    , IrOpcode op
    , IrValue *res
    , IrValue *op1
    , IrValue *op2
) {
    if (!b || !b->current_block) {
        errhandler__report_error(ERROR_CODE_IR_INVALID_INSTR, 0, 0, "ir", "No current block");
        return NULL;
    }
    IrInstruction *inst = ir_alloc(sizeof(IrInstruction));
    if (!inst) return NULL;
    inst->opcode = op; inst->result = res; inst->operand1 = op1; inst->operand2 = op2; inst->extra = NULL;
    append_instruction(b->current_block, inst);
    return inst;
}

IrInstruction *ir__emit_op2(IrBuilder *b, IrOpcode op, IrValue *result, IrValue *op1, IrValue *op2) {
    return emit_instruction(b, op, result, op1, op2);
}

IrInstruction *ir__emit_op1(IrBuilder *b, IrOpcode op, IrValue *result, IrValue *op1) {
    return ir__emit_op2(b, op, result, op1, NULL);
}

IrInstruction *ir__emit_store(IrBuilder *b, IrValue *ptr, IrValue *val) {
    return ir__emit_op2(b, IR_STORE, NULL, ptr, val);
}

IrInstruction *ir__emit_load(IrBuilder *b, IrValue *result, IrValue *ptr) {
    return ir__emit_op1(b, IR_LOAD, result, ptr);
}

IrInstruction *ir__emit_br(IrBuilder *b, IrBasicBlock *target) {
    IrValue *lbl = ir__value_label(target);
    IrInstruction *inst = ir__emit_op1(b, IR_BR, NULL, lbl);
    if (inst) link_blocks(b->current_block, target);
    return inst;
}

IrInstruction *ir__emit_brcond
    ( IrBuilder *b
    , IrValue *cond
    , IrBasicBlock *true_bb
    , IrBasicBlock *false_bb
) {
    IrValue *true_val = ir__value_label(true_bb);
    IrInstruction *inst = emit_instruction(b, IR_BRCOND, NULL, cond, true_val);
    if (!inst) return NULL;
    IrCondBranchExtra *extra = ir_alloc(sizeof(IrCondBranchExtra));
    if (!extra) { ir_free(inst); return NULL; }
    extra->true_target = true_bb; extra->false_target = false_bb;
    inst->extra = extra;
    link_blocks(b->current_block, true_bb);
    link_blocks(b->current_block, false_bb);
    return inst;
}

IrInstruction *ir__emit_ret(IrBuilder *b, IrValue *value) {
    return ir__emit_op1(b, IR_RET, NULL, value);
}

IrInstruction *ir__emit_call
    ( IrBuilder *b
    , IrValue *result
    , IrValue *callee
    , IrValue **args
    , uint32_t arg_count
) {
    IrInstruction *inst = emit_instruction(b, IR_CALL, result, callee, NULL);
    if (!inst) return NULL;
    IrCallExtra *extra = ir_alloc(sizeof(IrCallExtra));
    if (!extra) { ir_free(inst); return NULL; }
    extra->arg_count = arg_count;
    extra->args = ir_alloc(sizeof(IrValue *) * arg_count);
    if (!extra->args) { ir_free(extra); ir_free(inst); return NULL; }
    memcpy(extra->args, args, sizeof(IrValue *) * arg_count);
    inst->extra = extra;
    return inst;
}

IrInstruction *ir__emit_phi
    ( IrBuilder *b
    , IrValue *result
    , IrValue **values
    , IrBasicBlock **blocks
    , uint32_t count
) {
    IrInstruction *inst = emit_instruction(b, IR_PHI, result, NULL, NULL);
    if (!inst) return NULL;
    IrPhiExtra *extra = ir_alloc(sizeof(IrPhiExtra));
    if (!extra) { ir_free(inst); return NULL; }
    extra->values = ir_alloc(sizeof(IrValue *) * count);
    extra->blocks = ir_alloc(sizeof(IrBasicBlock *) * count);
    if (!extra->values || !extra->blocks) {
        ir_free(extra->values);
        ir_free(extra->blocks);
        ir_free(extra);
        ir_free(inst);
        return NULL;
    }
    memcpy(extra->values, values, sizeof(IrValue *) * count);
    memcpy(extra->blocks, blocks, sizeof(IrBasicBlock *) * count);
    extra->count = count;
    inst->extra = extra;
    return inst;
}
IrInstruction *ir__emit_alloca
    ( IrBuilder *b
    , IrValue *result
    , DataType pointee_type
    , Type *pointee_info
) {
    (void)pointee_type; (void)pointee_info;
    return ir__emit_op1(b, IR_ALLOCA, result, NULL);
}

IrInstruction *ir__emit_gep
    ( IrBuilder *b
    , IrValue *result
    , IrValue *base
    , IrValue **indices
    , uint32_t index_count
) {
    if (!indices || index_count == 0) return NULL;
    IrInstruction *inst = emit_instruction(b, IR_GEP, result, base, indices[0]);
    if (!inst) return NULL;
    if (index_count > 1) {
        IrGepExtra *extra = ir_alloc(sizeof(IrGepExtra));
        if (!extra) { ir_free(inst); return NULL; }
        extra->index_count = index_count - 1;
        extra->indices = ir_alloc(sizeof(IrValue *) * extra->index_count);
        if (!extra->indices) { ir_free(extra); ir_free(inst); return NULL; }
        memcpy(extra->indices, indices + 1, sizeof(IrValue *) * extra->index_count);
        inst->extra = extra;
    }
    return inst;
}

IrInstruction *ir__emit_cast
    ( IrBuilder *b
    , IrValue *result
    , IrValue *src
    , DataType target_type
    , Type *target_info
) {
    (void)target_type; (void)target_info;
    return ir__emit_op1(b, IR_CAST, result, src);
}

IrInstruction *ir__emit_nop(IrBuilder *b) { return emit_instruction(b, IR_NOP, NULL, NULL, NULL); }

IrBuilder *ir__builder_create(SemanticContext *sem_ctx) {
    IrBuilder *b = ir_alloc(sizeof(IrBuilder));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->sem_ctx = sem_ctx;
    return b;
}

void ir__builder_destroy(IrBuilder *b) {
    if (!b) return;
    ir_free(b->locals);
    ir_free(b->break_stack);
    ir_free(b->continue_stack);
    ir_free(b);
}

IrFunction *ir__builder_start_function
    ( IrBuilder *b
    , const char *name
    , DataType return_type
    , Type *return_type_info
    , IrValue **params
    , uint32_t param_count
) {
    if (!b || !name) return NULL;
    IrFunction *func = ir_alloc(sizeof(IrFunction));
    if (!func) return NULL;
    func->name = u__strdup_safe(name);
    func->return_type = return_type;
    func->return_type_info = return_type_info;
    func->next_temp_id = func->next_block_id = 0;
    func->entry_block = NULL;
    func->all_blocks = NULL; func->block_count = func->block_capacity = 0;
    func->parameters = NULL; func->param_count = func->param_capacity = 0;
    func->module = b->module;
    if (params && param_count > 0) {
        func->param_capacity = param_count;
        func->parameters = ir_alloc(param_count * sizeof(IrValue *));
        if (!func->parameters) { ir_free(func->name); ir_free(func); return NULL; }
        memcpy(func->parameters, params, param_count * sizeof(IrValue *));
        func->param_count = param_count;
    }
    IrBasicBlock *entry = create_block(func, "entry");
    if (!entry) { ir_free(func->name); ir_free(func->parameters); ir_free(func); return NULL; }
    func->entry_block = entry;
    b->current_function = func;
    b->current_block = entry;
    IrModule *mod = b->module;
    if (mod->func_count >= mod->func_capacity) {
        if (!grow_ptr_array((void ***)&mod->functions, &mod->func_count, &mod->func_capacity)) {
            ir_free(func->name);
            ir_free(func->parameters);
            ir_free(func->all_blocks);
            ir_free(func);
            return NULL;
        }
    } else if (mod->functions == NULL) {
        mod->func_capacity = 4;
        mod->functions = ir_alloc(4 * sizeof(IrFunction *));
        if (!mod->functions) {
            ir_free(func->name);
            ir_free(func->parameters);
            ir_free(func->all_blocks);
            ir_free(func);
            return NULL;
        }
    }
    mod->functions[mod->func_count++] = func;
    return func;
}
IrBasicBlock *ir__builder_add_block(IrBuilder *b, const char *label, bool set_current) {
    if (!b || !b->current_function) return NULL;
    IrBasicBlock *bb = create_block(b->current_function, label);
    if (bb && set_current) b->current_block = bb;
    return bb;
}

void ir__builder_set_block(IrBuilder *b, IrBasicBlock *block) { b->current_block = block; }

IrValue *ir__builder_get_local(IrBuilder *b, const char *name) {
    for (uint32_t i = 0; i < b->local_count; i++)
        if (strcmp(b->locals[i].name, name) == 0)
            return b->locals[i].ir_value;
    return NULL;
}

void ir__builder_set_local(IrBuilder *b, const char *name, IrValue *alloca) {
    for (uint32_t i = 0; i < b->local_count; i++)
        if (strcmp(b->locals[i].name, name) == 0) {
            b->locals[i].ir_value = alloca;
            return;
        }
    if (b->local_count >= b->local_capacity) {
        uint32_t new_cap = (b->local_capacity == 0) ? 8 : b->local_capacity * 2;
        void *new_arr = realloc(b->locals, new_cap * sizeof(b->locals[0]));
        if (!new_arr) {
            errhandler__report_error
                ( ERROR_CODE_MEMORY_ALLOCATION
                , 0
                , 0
                , "ir"
                , "Failed to grow local table"
            );
            return;
        }
        b->locals = new_arr; b->local_capacity = new_cap;
    }
    strncpy(b->locals[b->local_count].name, name, sizeof(b->locals[0].name)-1);
    b->locals[b->local_count].ir_value = alloca;
    b->local_count++;
}

char *ir__builder_temp_name(IrFunction *func, char *buf, size_t bufsize) {
    uint32_t id = func->next_temp_id++;
    snprintf(buf, bufsize, "%%t%u", id);
    return buf;
}

/* AST → IR translation – only IR, no assembly. */
static IrValue *ir_visit_expr(IrBuilder *b, ASTNode *node);
static void ir_visit_stmt(IrBuilder *b, ASTNode *node);

static IrValue *ir_get_variable(IrBuilder *b, const char *name, uint16_t line, uint16_t col) {
    IrValue *ptr = ir__builder_get_local(b, name);
    if (!ptr) errhandler__report_error(ERROR_CODE_IR_UNDEFINED_VAR, line, col, "ir", "Undefined variable '%s'", name);
    return ptr;
}

static IrValue *ir_literal_to_value(ASTNode *node) {
    if (!node) return NULL;
    TokenType tt = node->operation_type;
    if (tt == TOKEN_NUMBER) {
        const char *s = node->value;
        if (strchr(s, '.') || strchr(s, 'e') || strchr(s, 'E')) return ir__value_const_real(atof(s));
        else return ir__value_const_int(atoll(s));
    } else if (tt == TOKEN_CHAR)
        return node->value && node->value[0] ? ir__value_const_char(node->value[0])
                                             : ir__value_const_int(0);
    return ir__value_const_int(0);
}

static IrOpcode map_binary_op(TokenType tt) {
    switch (tt) {
        case TOKEN_PLUS: return IR_ADD;
        case TOKEN_MINUS: return IR_SUB;
        case TOKEN_STAR: return IR_MUL;
        case TOKEN_SLASH: return IR_DIV;
        case TOKEN_PERCENT: return IR_MOD;
        case TOKEN_AMPERSAND: return IR_AND;
        case TOKEN_PIPE: return IR_OR;
        case TOKEN_CARET: return IR_XOR;
        case TOKEN_SHL: return IR_SHL;
        case TOKEN_SHR: return IR_SHR;
        default: return IR_ADD;
    }
}

static IrValue *ir_load_variable(IrBuilder *b, IrValue *ptr, DataType type, Type *type_info) {
    IrValue *temp = ir__value_temp(b->current_function, type, type_info);
    ir__emit_load(b, temp, ptr);
    return temp;
}

static IrValue *ir_visit_expr(IrBuilder *b, ASTNode *node) {
    if (!node) return NULL;
    switch (node->type) {
        case AST_LITERAL_VALUE: return ir_literal_to_value(node);
        case AST_IDENTIFIER: {
            IrValue *ptr = ir_get_variable(b, node->value, node->line, node->column);
            return ptr ? ir_load_variable(b, ptr, ptr->type, ptr->type_info) : ir__value_const_int(0);
        }
        case AST_BINARY_OPERATION: {
            IrValue *left = ir_visit_expr(b, node->left), *right = ir_visit_expr(b, node->right);
            if (!left || !right) return NULL;
            DataType common = (left->type == TYPE_REAL || right->type == TYPE_REAL)
                            ? TYPE_REAL
                            : TYPE_INT;
            IrValue *res = ir__value_temp(b->current_function, common, NULL);
            ir__emit_op2(b, map_binary_op(node->operation_type), res, left, right);
            return res;
        }
        case AST_UNARY_OPERATION: {
            IrValue *opd = ir_visit_expr(b, node->left);
            if (!opd) return NULL;
            IrValue *res = ir__value_temp(b->current_function, opd->type, NULL);
            IrOpcode op = (node->operation_type == TOKEN_MINUS) ? IR_NEG : IR_NOT;
            ir__emit_op1(b, op, res, opd);
            return res;
        }
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT: {
            IrValue *rval = ir_visit_expr(b, node->right);
            if (!rval) return NULL;
            ASTNode *lhs = node->left;
            if (lhs->type == AST_IDENTIFIER) {
                IrValue *ptr = ir_get_variable(b, lhs->value, lhs->line, lhs->column);
                if (ptr) ir__emit_store(b, ptr, rval);
            } else if (lhs->type == AST_FIELD_ACCESS) {
                IrValue *base = ir_visit_expr(b, lhs->left);
                if (!base) return NULL;
                uint32_t idx = 0; /* simplified */
                IrValue *idx_val = ir__value_struct_field(idx);
                IrValue *field_ptr = ir__value_temp(b->current_function, TYPE_POINTER, NULL);
                ir__emit_gep(b, field_ptr, base, &idx_val, 1);
                ir__emit_store(b, field_ptr, rval);
            } else {
                errhandler__report_error
                    ( ERROR_CODE_IR_UNSUPPORTED_NODE
                    , node->line
                    , node->column
                    , "ir"
                    , "Complex lvalue"
                );
            }
            return rval;
        }
        case AST_FIELD_ACCESS: {
            IrValue *base = ir_visit_expr(b, node->left);
            if (!base) return NULL;
            uint32_t idx = 0;
            IrValue *idx_val = ir__value_struct_field(idx);
            IrValue *field_ptr = ir__value_temp(b->current_function, TYPE_POINTER, NULL);
            ir__emit_gep(b, field_ptr, base, &idx_val, 1);
            IrValue *res = ir__value_temp(b->current_function, TYPE_INT, NULL);
            ir__emit_load(b, res, field_ptr);
            return res;
        }
        case AST_TERNARY_OPERATION: {
            /* Generate IR for ternary: cond ? true : false using phi node. */
            IrValue *cond = ir_visit_expr(b, node->left);
            if (!cond) return NULL;
            IrBasicBlock *then_bb = ir__builder_add_block(b, "tern.then", false);
            IrBasicBlock *else_bb = ir__builder_add_block(b, "tern.else", false);
            IrBasicBlock *merge_bb = ir__builder_add_block(b, "tern.end", false);
            ir__emit_brcond(b, cond, then_bb, else_bb);
            ir__builder_set_block(b, then_bb);
            IrValue *then_val = ir_visit_expr(b, node->right);
            if (!then_val) return NULL;
            ir__emit_br(b, merge_bb);
            ir__builder_set_block(b, else_bb);
            IrValue *else_val = ir_visit_expr(b, (ASTNode *)node->extra);
            if (!else_val) return NULL;
            ir__emit_br(b, merge_bb);
            ir__builder_set_block(b, merge_bb);
            IrValue *res = ir__value_temp(b->current_function, then_val->type, then_val->type_info);
            IrValue *vals[2] = { then_val, else_val };
            IrBasicBlock *blks[2] = { then_bb, else_bb };
            ir__emit_phi(b, res, vals, blks, 2);
            return res;
        }
        case AST_FUNCTION_CALL: {
            ASTNode *callee_node = node->left;
            if (!callee_node) return NULL;
            IrValue *callee = ir__value_global(callee_node->value, TYPE_FUNCTION, NULL);
            AST *arg_list = (AST *)node->extra;
            uint32_t argc = arg_list ? arg_list->count : 0;
            IrValue **args = ir_alloc(sizeof(IrValue *) * argc);
            for (uint32_t i = 0; i < argc; i++) args[i] = ir_visit_expr(b, arg_list->nodes[i]);
            IrValue *res = ir__value_temp(b->current_function, TYPE_INT, NULL);
            ir__emit_call(b, res, callee, args, argc);
            ir_free(args);
            return res;
        }
        case AST_CAST: {
            IrValue *src = ir_visit_expr(b, node->left);
            if (!src) return NULL;
            IrValue *res = ir__value_temp(b->current_function, TYPE_INT, node->variable_type);
            ir__emit_cast(b, res, src, TYPE_INT, node->variable_type);
            return res;
        }
        case AST_MULTI_INITIALIZER: {
            AST *list = (AST *)node->extra;
            if (!list || list->count == 0) return ir__value_const_int(0);
            IrValue *first = ir_visit_expr(b, list->nodes[0]);
            DataType elem_type = first ? first->type : TYPE_INT;
            IrValue *temp_alloca = ir__value_temp(b->current_function, TYPE_POINTER, NULL);
            ir__emit_alloca(b, temp_alloca, elem_type, NULL);
            for (uint16_t i = 0; i < list->count; i++) {
                IrValue *elem = ir_visit_expr(b, list->nodes[i]);
                if (!elem) continue;
                IrValue *idx = ir__value_const_int(i);
                IrValue *elem_ptr = ir__value_temp(b->current_function, TYPE_POINTER, NULL);
                ir__emit_gep(b, elem_ptr, temp_alloca, &idx, 1);
                ir__emit_store(b, elem_ptr, elem);
            }
            return temp_alloca;
        }
        default:
            errhandler__report_error
                ( ERROR_CODE_IR_UNSUPPORTED_NODE
                , node->line
                , node->column
                , "ir"
                , "Unsupported expr %d"
                , node->type
            );
            return ir__value_const_int(0);
    }
}
static void ir_visit_stmt(IrBuilder *b, ASTNode *node) {
    if (!node) return;
    switch (node->type) {
        case AST_VARIABLE_DECLARATION: {
            if (!node->value) break;
            IrValue *alloca = ir__value_temp(b->current_function, TYPE_POINTER, NULL);
            ir__emit_alloca(b, alloca, TYPE_INT, NULL);
            ir__builder_set_local(b, node->value, alloca);
            if (node->default_value) {
                IrValue *init = ir_visit_expr(b, node->default_value);
                if (init) ir__emit_store(b, alloca, init);
            }
            break;
        }
        case AST_BLOCK: {
            AST *list = (AST *)node->extra;
            if (list) for (uint16_t i = 0; i < list->count; i++) ir_visit_stmt(b, list->nodes[i]);
            break;
        }
        case AST_IF_STATEMENT: {
            IrValue *cond = ir_visit_expr(b, node->left);
            if (!cond) break;
            IrBasicBlock *then_bb = ir__builder_add_block(b, "if.then", false);
            IrBasicBlock *else_bb = node->extra ? ir__builder_add_block(b, "if.else", false) : NULL;
            IrBasicBlock *merge_bb = ir__builder_add_block(b, "if.end", false);
            ir__emit_brcond(b, cond, then_bb, else_bb ? else_bb : merge_bb);
            ir__builder_set_block(b, then_bb);
            ir_visit_stmt(b, node->right);
            ir__emit_br(b, merge_bb);
            if (else_bb) {
                ir__builder_set_block(b, else_bb);
                ir_visit_stmt(b, (ASTNode *)node->extra);
                ir__emit_br(b, merge_bb);
            }
            ir__builder_set_block(b, merge_bb);
            break;
        }
        case AST_DO_LOOP: {
            IrBasicBlock *header = ir__builder_add_block(b, "do.header", false);
            IrBasicBlock *body = ir__builder_add_block(b, "do.body", false);
            IrBasicBlock *end = ir__builder_add_block(b, "do.end", false);
            ir__emit_br(b, header);
            ir__builder_set_block(b, header);
            IrValue *cond = ir_visit_expr(b, node->left);
            ir__emit_brcond(b, cond, body, end);
            ir__builder_set_block(b, body);
            ir_visit_stmt(b, node->right);
            ir__emit_br(b, header);
            ir__builder_set_block(b, end);
            break;
        }
        case AST_RETURN: {
            IrValue *val = node->left ? ir_visit_expr(b, node->left) : NULL;
            ir__emit_ret(b, val);
            break;
        }
        case AST_LABEL_DECLARATION:
            ir__builder_add_block(b, node->value, true);
            break;
        case AST_NOP:
            ir__emit_nop(b);
            break;
        default:
            ir_visit_expr(b, node);
            break;
    }
}

static void ir_convert_function(IrBuilder *b, ASTNode *func_decl) {
    const char *name = func_decl->value;
    ASTNode *params_node = func_decl->left;
    ASTNode *body = func_decl->right;
    DataType ret_type = func_decl->variable_type ? TYPE_INT : TYPE_VOID;
    IrValue **params = NULL;
    uint32_t param_count = 0;
    if (params_node && params_node->type == AST_BLOCK && params_node->extra) {
        AST *plist = (AST *)params_node->extra;
        param_count = plist->count;
        if (param_count > 0) {
            params = ir_alloc(sizeof(IrValue *) * param_count);
            for (uint32_t i = 0; i < param_count; i++) {
                ASTNode *p = plist->nodes[i];
                params[i] = ir__value_param(NULL, i, TYPE_INT, NULL);
                if (p->value) strncpy(params[i]->name, p->value, sizeof(params[i]->name)-1);
            }
        }
    }
    IrFunction *func = ir__builder_start_function
        ( b
        , name
        , ret_type
        , func_decl->variable_type
        , params
        , param_count
    );
    for (uint32_t i = 0; i < param_count; i++) {
        if (params[i]->name[0]) {
            IrValue *alloca = ir__value_temp(func, TYPE_POINTER, NULL);
            ir__emit_alloca(b, alloca, TYPE_INT, NULL);
            ir__emit_store(b, alloca, params[i]);
            ir__builder_set_local(b, params[i]->name, alloca);
        }
    }
    if (body) ir_visit_stmt(b, body);
    if (b->current_block && b->current_block->last_inst) {
        IrOpcode last_op = b->current_block->last_inst->opcode;
        if (last_op != IR_RET && last_op != IR_BR && last_op != IR_BRCOND)
            ir__emit_ret(b, NULL);
    }
}

IrModule *ir__build_from_ast(IrBuilder *b, AST *ast) {
    if (!b || !ast) return NULL;
    for (uint16_t i = 0; i < ast->count; i++) {
        ASTNode *node = ast->nodes[i];
        if (node->type == AST_FUNCTION_DECLARATION) {
            const char *mod = node->state_modifier;
            if (!mod || strcmp(mod, "def") == 0) ir_convert_function(b, node);
        }
    }
    return b->module;
}

IrModule *ir__generate_module(SemanticContext *sem_ctx, AST *ast) {
    IrBuilder *b = ir__builder_create(sem_ctx);
    if (!b) return NULL;
    IrModule *mod = ir__module_create(sem_ctx->global_scope);
    if (!mod) { ir__builder_destroy(b); return NULL; }
    b->module = mod;
    mod = ir__build_from_ast(b, ast);
    ir__builder_destroy(b);
    return mod;
}

IrModule *ir__module_create(SymbolTable *global_scope) {
    IrModule *mod = ir_alloc(sizeof(IrModule));
    if (mod) {
        mod->functions = NULL;
        mod->func_count = mod->func_capacity = 0;
        mod->symbols = global_scope;
    }
    return mod;
}

void ir__module_destroy(IrModule *mod) {
    if (!mod) return;
    for (uint32_t i = 0; i < mod->func_count; i++) {
        IrFunction *f = mod->functions[i];
        for (uint32_t j = 0; j < f->block_count; j++) {
            IrBasicBlock *bb = f->all_blocks[j];
            IrInstruction *inst = bb->first_inst;
            while (inst) {
                IrInstruction *next = inst->next;
                if (inst->extra) {
                    if (inst->opcode == IR_CALL) { IrCallExtra *call = inst->extra; ir_free(call->args); }
                    else if (inst->opcode == IR_GEP) { IrGepExtra *gep = inst->extra; if (gep) ir_free(gep->indices); }
                    else if (inst->opcode == IR_PHI) { IrPhiExtra *phi = inst->extra; ir_free(phi->values); ir_free(phi->blocks); }
                    ir_free(inst->extra);
                }
                ir_free(inst);
                inst = next;
            }
            ir_free(bb->predecessors); ir_free(bb->successors); ir_free(bb->phi_nodes); ir_free(bb);
        }
        ir_free(f->all_blocks); ir_free(f->parameters); ir_free(f->name); ir_free(f);
    }
    ir_free(mod->functions); ir_free(mod);
}

static void ir_print_value(FILE *f, const IrValue *v) {
    if (!v) { fprintf(f, "void"); return; }
    switch (v->kind) {
        case IR_VALUE_TEMP: case IR_VALUE_PARAM: case IR_VALUE_LABEL: case IR_VALUE_GLOBAL_SYMBOL:
            fprintf(f, "%s", v->name); break;
        case IR_VALUE_CONST_INT: fprintf(f, "%lld", (long long)v->const_data.int_val); break;
        case IR_VALUE_CONST_REAL: fprintf(f, "%.6g", v->const_data.real_val); break;
        case IR_VALUE_CONST_CHAR: fprintf(f, "'%c'", v->const_data.char_val); break;
        case IR_VALUE_STRUCT_FIELD: fprintf(f, "field%u", v->const_data.field_index); break;
        default: fprintf(f, "?");
    }
}

static void ir_print_opcode(FILE *f, IrOpcode op) {
    switch (op) {
        case IR_NOP: fprintf(f, "nop"); break; case IR_ADD: fprintf(f, "add"); break;
        case IR_SUB: fprintf(f, "sub"); break; case IR_MUL: fprintf(f, "mul"); break;
        case IR_DIV: fprintf(f, "div"); break; case IR_MOD: fprintf(f, "mod"); break;
        case IR_NEG: fprintf(f, "neg"); break; case IR_EQ: fprintf(f, "eq"); break;
        case IR_NEQ: fprintf(f, "ne"); break; case IR_LT: fprintf(f, "lt"); break;
        case IR_LE: fprintf(f, "le"); break; case IR_GT: fprintf(f, "gt"); break;
        case IR_GE: fprintf(f, "ge"); break; case IR_AND: fprintf(f, "and"); break;
        case IR_OR: fprintf(f, "or"); break; case IR_XOR: fprintf(f, "xor"); break;
        case IR_SHL: fprintf(f, "shl"); break; case IR_SHR: fprintf(f, "shr"); break;
        case IR_SAR: fprintf(f, "sar"); break; case IR_NOT: fprintf(f, "not"); break;
        case IR_LOAD: fprintf(f, "load"); break; case IR_STORE: fprintf(f, "store"); break;
        case IR_ALLOCA: fprintf(f, "alloca"); break; case IR_GEP: fprintf(f, "gep"); break;
        case IR_BR: fprintf(f, "br"); break; case IR_BRCOND: fprintf(f, "brcond"); break;
        case IR_CALL: fprintf(f, "call"); break; case IR_RET: fprintf(f, "ret"); break;
        case IR_PHI: fprintf(f, "phi"); break; case IR_CAST: fprintf(f, "cast"); break;
        default: fprintf(f, "??");
    }
}

void ir__print_module(FILE *f, const IrModule *mod) {
    if (!mod) return;
    for (uint32_t i = 0; i < mod->func_count; i++) {
        IrFunction *func = mod->functions[i];
        fprintf(f, "define %s %s(", semantic__type_to_string(func->return_type), func->name);
        for (uint32_t j = 0; j < func->param_count; j++) { if (j) fprintf(f, ", "); ir_print_value(f, func->parameters[j]); }
        fprintf(f, ") {\n");
        for (uint32_t j = 0; j < func->block_count; j++) {
            IrBasicBlock *bb = func->all_blocks[j];
            fprintf(f, "%s:\n", bb->label);
            for (IrInstruction *inst = bb->first_inst; inst; inst = inst->next) {
                fprintf(f, "  ");
                if (inst->result) { ir_print_value(f, inst->result); fprintf(f, " = "); }
                ir_print_opcode(f, inst->opcode);
                if (inst->operand1) { fprintf(f, " "); ir_print_value(f, inst->operand1); }
                if (inst->operand2) { fprintf(f, ", "); ir_print_value(f, inst->operand2); }
                if (inst->extra) {
                    if (inst->opcode == IR_CALL) {
                        IrCallExtra *call = inst->extra;
                        for (uint32_t k = 0; k < call->arg_count; k++) { fprintf(f, ", "); ir_print_value(f, call->args[k]); }
                    } else if (inst->opcode == IR_BRCOND) {
                        IrCondBranchExtra *br = inst->extra;
                        fprintf(f, " ? %s : %s", br->true_target->label, br->false_target->label);
                    } else if (inst->opcode == IR_PHI) {
                        IrPhiExtra *phi = inst->extra;
                        fprintf(f, " [");
                        for (uint32_t k = 0; k < phi->count; k++) { if (k) fprintf(f, ", "); ir_print_value(f, phi->values[k]); fprintf(f, ":%s", phi->blocks[k]->label); }
                        fprintf(f, "]");
                    } else if (inst->opcode == IR_GEP) {
                        IrGepExtra *gep = inst->extra;
                        if (gep) for (uint32_t k = 0; k < gep->index_count; k++) { fprintf(f, ", "); ir_print_value(f, gep->indices[k]); }
                    }
                }
                fprintf(f, "\n");
            }
        }
        fprintf(f, "}\n\n");
    }
}
