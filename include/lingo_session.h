#ifndef DSCO_LINGO_SESSION_H
#define DSCO_LINGO_SESSION_H
#include <stdbool.h>
#include <stddef.h>

/* Owned Lua states stay behind the native host. Every public command enters
 * through the registered lingo_session tool and its capability gate. */
typedef struct lingo_vm lingo_vm;
bool lingo_vm_open(const char *request, const char *restore_view, lingo_vm **vm, char *result,
                   size_t capacity);
bool lingo_vm_command(lingo_vm *vm, const char *command, char *result, size_t capacity);
unsigned lingo_vm_calls(const lingo_vm *vm);
void lingo_vm_record_call(lingo_vm *vm);
void lingo_vm_close(lingo_vm *vm);
void lingo_vm_view_hash(char out[65]);

extern const char lingo_session_schema[];
bool lingo_session_execute(const char *input, char *result, size_t capacity);
#endif
