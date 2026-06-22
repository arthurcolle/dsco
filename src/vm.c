#include "vm.h"
#include "crypto.h"  /* fnv1a_32 — consolidated */
#include <string.h>
#include <stdio.h>

/* ── Computed goto detection ───────────────────────────────────────── */

#if defined(__GNUC__) || defined(__clang__)
#define VM_USE_COMPUTED_GOTO 1
#endif

/* FNV-1a — consolidated into fnv1a_32() in crypto.h */
#define fnv1a(s) fnv1a_32(s)

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void vm_init(vm_t *vm) {
    memset(vm, 0, sizeof(*vm));
    for (int i = 0; i < 512; i++)
        vm->hash_buckets[i] = -1;
}

void vm_reset(vm_t *vm) {
    vm->sp = 0;
    vm->pc = 0;
    vm->halted = false;
    vm->yielded = false;
    vm->error = NULL;
    memset(vm->regs, 0, sizeof(vm->regs));
}

/* ── Code generation ───────────────────────────────────────────────── */

int vm_emit(vm_t *vm, vm_opcode_t op, int32_t operand) {
    if (vm->code_len >= VM_CODE_MAX) {
        vm->error = "code overflow";
        return -1;
    }
    int pc = vm->code_len;
    vm->code[pc].opcode  = (uint8_t)op;
    vm->code[pc].operand = operand;
    vm->code_len++;
    return pc;
}

int vm_add_string(vm_t *vm, const char *s) {
    if (vm->str_pool_len >= VM_STR_POOL_MAX) return -1;
    int idx = vm->str_pool_len++;
    vm->str_pool[idx] = s;
    return idx;
}

/* ── Dispatch table ────────────────────────────────────────────────── */

void vm_register_tool(vm_t *vm, const char *name, vm_tool_fn func, int tool_index) {
    if (vm->dispatch_len >= VM_DISPATCH_MAX) return;
    int idx = vm->dispatch_len++;
    vm->dispatch[idx].name       = name;
    vm->dispatch[idx].hash       = fnv1a(name);
    vm->dispatch[idx].func       = func;
    vm->dispatch[idx].tool_index = tool_index;
}

void vm_build_dispatch_index(vm_t *vm) {
    for (int i = 0; i < 512; i++)
        vm->hash_buckets[i] = -1;

    /* Open addressing with linear probing */
    for (int i = 0; i < vm->dispatch_len; i++) {
        uint32_t bucket = vm->dispatch[i].hash & 511;
        while (vm->hash_buckets[bucket] != -1) {
            bucket = (bucket + 1) & 511;
        }
        vm->hash_buckets[bucket] = i;
    }
}

bool vm_dispatch_tool(vm_t *vm, const char *tool_name,
                      const char *input_json, char *result, size_t result_len) {
    if (!vm || !tool_name) return false;

    uint32_t h = fnv1a(tool_name);
    uint32_t bucket = h & 511;
    vm->dispatches++;

    for (int probe = 0; probe < 512; probe++) {
        int idx = vm->hash_buckets[bucket];
        if (idx < 0) return false; /* empty slot — not found */

        vm_dispatch_entry_t *e = &vm->dispatch[idx];
        if (e->hash == h && strcmp(e->name, tool_name) == 0) {
            vm->cache_hits++;
            return e->func(input_json, result, result_len);
        }
        bucket = (bucket + 1) & 511;
    }
    return false;
}

/* ── Stack operations ──────────────────────────────────────────────── */

void vm_push_int(vm_t *vm, int64_t val) {
    if (vm->sp >= VM_STACK_MAX) {
        vm->error = "stack overflow";
        vm->halted = true;
        return;
    }
    vm->stack[vm->sp].type = VM_VAL_INT;
    vm->stack[vm->sp].i = val;
    vm->sp++;
}

void vm_push_str(vm_t *vm, const char *s) {
    if (vm->sp >= VM_STACK_MAX) {
        vm->error = "stack overflow";
        vm->halted = true;
        return;
    }
    vm->stack[vm->sp].type = VM_VAL_STR;
    vm->stack[vm->sp].s = s;
    vm->sp++;
}

vm_val_t vm_pop(vm_t *vm) {
    if (vm->sp <= 0) {
        vm->error = "stack underflow";
        vm->halted = true;
        vm_val_t none = { .type = VM_VAL_NONE };
        return none;
    }
    return vm->stack[--vm->sp];
}

vm_val_t vm_peek(vm_t *vm) {
    if (vm->sp <= 0) {
        vm_val_t none = { .type = VM_VAL_NONE };
        return none;
    }
    return vm->stack[vm->sp - 1];
}

/* ── Execution engine ──────────────────────────────────────────────── */

int vm_run(vm_t *vm) {
    if (!vm || vm->halted) return -1;
    vm->yielded = false;

#ifdef VM_USE_COMPUTED_GOTO
    static void *dispatch_table[OP_COUNT] = {
        [OP_NOP]         = &&op_nop,
        [OP_HALT]        = &&op_halt,
        [OP_CALL_TOOL]   = &&op_call_tool,
        [OP_CALL_NATIVE] = &&op_call_native,
        [OP_PUSH_STR]    = &&op_push_str,
        [OP_PUSH_INT]    = &&op_push_int,
        [OP_POP]         = &&op_pop,
        [OP_DUP]         = &&op_dup,
        [OP_LOAD]        = &&op_load,
        [OP_STORE]       = &&op_store,
        [OP_JUMP]        = &&op_jump,
        [OP_JUMP_IF]     = &&op_jump_if,
        [OP_DISPATCH]    = &&op_dispatch,
        [OP_YIELD]       = &&op_yield,
        [OP_RETURN]      = &&op_return,
        [OP_EMIT]        = &&op_emit,
    };

    #define DISPATCH() do { \
        if (vm->pc >= vm->code_len || vm->halted) goto done; \
        goto *dispatch_table[vm->code[vm->pc].opcode]; \
    } while(0)
    #define NEXT() do { vm->pc++; vm->instructions_executed++; DISPATCH(); } while(0)

    DISPATCH();

op_nop:
    NEXT();

op_halt:
    vm->halted = true;
    goto done;

op_call_tool: {
    int32_t idx = vm->code[vm->pc].operand;
    if (idx >= 0 && idx < vm->dispatch_len) {
        vm_dispatch_entry_t *e = &vm->dispatch[idx];
        if (vm->result_buf) {
            e->func(NULL, vm->result_buf, vm->result_buf_len);
        }
    }
    NEXT();
}

op_call_native:
    /* Reserved for future native C function calls */
    NEXT();

op_push_str: {
    int32_t idx = vm->code[vm->pc].operand;
    if (idx >= 0 && idx < vm->str_pool_len) {
        vm_push_str(vm, vm->str_pool[idx]);
    }
    NEXT();
}

op_push_int:
    vm_push_int(vm, vm->code[vm->pc].operand);
    NEXT();

op_pop:
    vm_pop(vm);
    NEXT();

op_dup: {
    vm_val_t v = vm_peek(vm);
    if (vm->sp < VM_STACK_MAX) {
        vm->stack[vm->sp++] = v;
    }
    NEXT();
}

op_load: {
    int32_t r = vm->code[vm->pc].operand;
    if (r >= 0 && r < VM_REGS_MAX) {
        if (vm->sp < VM_STACK_MAX) {
            vm->stack[vm->sp++] = vm->regs[r];
        }
    }
    NEXT();
}

op_store: {
    int32_t r = vm->code[vm->pc].operand;
    if (r >= 0 && r < VM_REGS_MAX) {
        vm->regs[r] = vm_pop(vm);
    }
    NEXT();
}

op_jump:
    vm->pc = vm->code[vm->pc].operand;
    vm->instructions_executed++;
    DISPATCH();

op_jump_if: {
    vm_val_t cond = vm_pop(vm);
    if (cond.type == VM_VAL_INT && cond.i != 0) {
        vm->pc = vm->code[vm->pc].operand;
        vm->instructions_executed++;
        DISPATCH();
    }
    NEXT();
}

op_dispatch: {
    vm_val_t name_val = vm_pop(vm);
    if (name_val.type == VM_VAL_STR && name_val.s) {
        vm->dispatches++;
        uint32_t h = fnv1a(name_val.s);
        uint32_t bucket = h & 511;
        for (int probe = 0; probe < 512; probe++) {
            int idx = vm->hash_buckets[bucket];
            if (idx < 0) break;
            if (vm->dispatch[idx].hash == h &&
                strcmp(vm->dispatch[idx].name, name_val.s) == 0) {
                vm->cache_hits++;
                if (vm->result_buf) {
                    bool ok = vm->dispatch[idx].func(NULL, vm->result_buf, vm->result_buf_len);
                    vm_push_int(vm, ok ? 1 : 0);
                } else {
                    vm_push_int(vm, 0);
                }
                goto dispatch_found;
            }
            bucket = (bucket + 1) & 511;
        }
        vm_push_int(vm, 0); /* not found */
    }
dispatch_found:
    NEXT();
}

op_yield:
    vm->yielded = true;
    vm->pc++;
    vm->instructions_executed++;
    return 1;

op_return:
    vm->halted = true;
    goto done;

op_emit: {
    vm_val_t v = vm_pop(vm);
    if (v.type == VM_VAL_STR && v.s && vm->result_buf) {
        size_t len = strlen(v.s);
        if (len >= vm->result_buf_len) len = vm->result_buf_len - 1;
        memcpy(vm->result_buf, v.s, len);
        vm->result_buf[len] = '\0';
    }
    NEXT();
}

done:
    return vm->halted ? 0 : -1;

#else
    /* Fallback: switch-based dispatch */
    while (vm->pc < vm->code_len && !vm->halted) {
        vm_instr_t instr = vm->code[vm->pc];
        vm->instructions_executed++;

        switch (instr.opcode) {
        case OP_NOP:
            vm->pc++;
            break;
        case OP_HALT:
            vm->halted = true;
            return 0;
        case OP_PUSH_INT:
            vm_push_int(vm, instr.operand);
            vm->pc++;
            break;
        case OP_PUSH_STR:
            if (instr.operand >= 0 && instr.operand < vm->str_pool_len)
                vm_push_str(vm, vm->str_pool[instr.operand]);
            vm->pc++;
            break;
        case OP_POP:
            vm_pop(vm);
            vm->pc++;
            break;
        case OP_DUP: {
            vm_val_t v = vm_peek(vm);
            if (vm->sp < VM_STACK_MAX) vm->stack[vm->sp++] = v;
            vm->pc++;
            break;
        }
        case OP_LOAD:
            if (instr.operand >= 0 && instr.operand < VM_REGS_MAX) {
                if (vm->sp < VM_STACK_MAX)
                    vm->stack[vm->sp++] = vm->regs[instr.operand];
            }
            vm->pc++;
            break;
        case OP_STORE:
            if (instr.operand >= 0 && instr.operand < VM_REGS_MAX)
                vm->regs[instr.operand] = vm_pop(vm);
            vm->pc++;
            break;
        case OP_JUMP:
            vm->pc = instr.operand;
            break;
        case OP_JUMP_IF: {
            vm_val_t cond = vm_pop(vm);
            if (cond.type == VM_VAL_INT && cond.i != 0)
                vm->pc = instr.operand;
            else
                vm->pc++;
            break;
        }
        case OP_CALL_TOOL:
            if (instr.operand >= 0 && instr.operand < vm->dispatch_len) {
                vm_dispatch_entry_t *e = &vm->dispatch[instr.operand];
                if (vm->result_buf)
                    e->func(NULL, vm->result_buf, vm->result_buf_len);
            }
            vm->pc++;
            break;
        case OP_CALL_NATIVE:
            vm->pc++;
            break;
        case OP_DISPATCH: {
            vm_val_t name_val = vm_pop(vm);
            if (name_val.type == VM_VAL_STR && name_val.s) {
                bool found = false;
                vm->dispatches++;
                uint32_t h = fnv1a(name_val.s);
                uint32_t bucket = h & 511;
                for (int probe = 0; probe < 512; probe++) {
                    int idx = vm->hash_buckets[bucket];
                    if (idx < 0) break;
                    if (vm->dispatch[idx].hash == h &&
                        strcmp(vm->dispatch[idx].name, name_val.s) == 0) {
                        vm->cache_hits++;
                        if (vm->result_buf)
                            vm->dispatch[idx].func(NULL, vm->result_buf, vm->result_buf_len);
                        found = true;
                        break;
                    }
                    bucket = (bucket + 1) & 511;
                }
                vm_push_int(vm, found ? 1 : 0);
            }
            vm->pc++;
            break;
        }
        case OP_YIELD:
            vm->yielded = true;
            vm->pc++;
            return 1;
        case OP_RETURN:
            vm->halted = true;
            return 0;
        case OP_EMIT: {
            vm_val_t v = vm_pop(vm);
            if (v.type == VM_VAL_STR && v.s && vm->result_buf) {
                size_t len = strlen(v.s);
                if (len >= vm->result_buf_len) len = vm->result_buf_len - 1;
                memcpy(vm->result_buf, v.s, len);
                vm->result_buf[len] = '\0';
            }
            vm->pc++;
            break;
        }
        default:
            vm->error = "unknown opcode";
            vm->halted = true;
            return -1;
        }
    }
    return vm->halted ? 0 : -1;
#endif
}

int vm_resume(vm_t *vm) {
    if (!vm || vm->halted) return -1;
    vm->yielded = false;
    return vm_run(vm);
}

/* ── Stats ─────────────────────────────────────────────────────────── */

vm_stats_t vm_get_stats(vm_t *vm) {
    vm_stats_t st;
    memset(&st, 0, sizeof(st));
    if (!vm) return st;
    st.instructions_executed = vm->instructions_executed;
    st.dispatches = vm->dispatches;
    st.cache_hits = vm->cache_hits;
    st.code_len = vm->code_len;
    st.dispatch_entries = vm->dispatch_len;
    st.str_pool_size = vm->str_pool_len;
    return st;
}
