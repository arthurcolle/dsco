#ifndef DSCO_NATIVE_WORKBENCH_STATE_H
#define DSCO_NATIVE_WORKBENCH_STATE_H
#include <stdbool.h>
#include <stddef.h>
bool native_workbench_state_start(char *error,size_t cap);
bool native_workbench_state_checkpoint(char *error,size_t cap);
bool native_workbench_state_command(const char *json,char *out,size_t cap);
void native_workbench_state_restore(void);
void native_workbench_state_close(void);
#endif
