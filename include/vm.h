#ifndef DSCO_VM_H
#define DSCO_VM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Opcodes ───────────────────────────────────────────────────────── */

typedef enum {
    OP_NOP = 0,
    OP_HALT,
    OP_CALL_TOOL,
    OP_CALL_NATIVE,
    OP_PUSH_STR,
    OP_PUSH_INT,
    OP_POP,
    OP_DUP,
    OP_LOAD,
    OP_STORE,
    OP_JUMP,
    OP_JUMP_IF,
    OP_DISPATCH,
    OP_YIELD,
    OP_RETURN,
    OP_EMIT,
    OP_COUNT
} vm_opcode_t;

/* ── Instruction ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t  opcode;
    int32_t  operand;
} vm_instr_t;

/* ── Value (tagged union on the stack) ─────────────────────────────── */

typedef enum { VM_VAL_NONE, VM_VAL_INT, VM_VAL_STR, VM_VAL_PTR } vm_val_type_t;

typedef struct {
    vm_val_type_t type;
    union {
        int64_t     i;
        const char *s;
        void       *p;
    };
} vm_val_t;

/* ── Dispatch table entry ──────────────────────────────────────────── */

typedef bool (*vm_tool_fn)(const char *input_json, char *result, size_t result_len);

typedef struct {
    const char *name;
    uint32_t    hash;
    vm_tool_fn  func;
    int         tool_index;
} vm_dispatch_entry_t;

/* ── VM instance ───────────────────────────────────────────────────── */

#define VM_STACK_MAX    256
#define VM_REGS_MAX     16
#define VM_STR_POOL_MAX 1024
#define VM_CODE_MAX     4096
#define VM_DISPATCH_MAX 512

typedef struct {
    vm_instr_t  code[VM_CODE_MAX];
    int         code_len;

    vm_val_t    stack[VM_STACK_MAX];
    int         sp;

    vm_val_t    regs[VM_REGS_MAX];

    const char *str_pool[VM_STR_POOL_MAX];
    int         str_pool_len;

    vm_dispatch_entry_t dispatch[VM_DISPATCH_MAX];
    int                 dispatch_len;
    int         hash_buckets[512];

    int         pc;
    bool        halted;
    bool        yielded;
    const char *error;

    char       *result_buf;
    size_t      result_buf_len;

    uint64_t    instructions_executed;
    uint64_t    dispatches;
    uint64_t    cache_hits;
} vm_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void vm_init(vm_t *vm);
void vm_reset(vm_t *vm);

/* ── Code generation ───────────────────────────────────────────────── */

int  vm_emit(vm_t *vm, vm_opcode_t op, int32_t operand);
int  vm_add_string(vm_t *vm, const char *s);

/* ── Dispatch table ────────────────────────────────────────────────── */

void vm_register_tool(vm_t *vm, const char *name, vm_tool_fn func, int tool_index);
void vm_build_dispatch_index(vm_t *vm);
bool vm_dispatch_tool(vm_t *vm, const char *tool_name,
                      const char *input_json, char *result, size_t result_len);

/* ── Execution ─────────────────────────────────────────────────────── */

int  vm_run(vm_t *vm);
int  vm_resume(vm_t *vm);

/* ── Stack operations ──────────────────────────────────────────────── */

void     vm_push_int(vm_t *vm, int64_t val);
void     vm_push_str(vm_t *vm, const char *s);
vm_val_t vm_pop(vm_t *vm);
vm_val_t vm_peek(vm_t *vm);

/* ── Stats ─────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t instructions_executed;
    uint64_t dispatches;
    uint64_t cache_hits;
    int      code_len;
    int      dispatch_entries;
    int      str_pool_size;
} vm_stats_t;

vm_stats_t vm_get_stats(vm_t *vm);

#endif /* DSCO_VM_H */
