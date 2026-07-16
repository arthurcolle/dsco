#ifndef DSCO_KITTY_AGENT_WINDOWS_H
#define DSCO_KITTY_AGENT_WINDOWS_H

#include <stddef.h>
#include <sys/types.h>

/* Live Kitty companions for real swarm workers.  These hooks are intentionally
 * lifecycle-shaped: swarm owns worker creation/output/completion, while this
 * module only mirrors that already-governed stream into a terminal window. */
void kitty_agent_window_spawn(int child_id, pid_t child_pid,
                              const char *task, const char *model);
void kitty_agent_window_append(int child_id, const char *data, size_t len);
void kitty_agent_window_complete(int child_id, const char *status, int exit_code);
void kitty_agent_windows_shutdown(void);

#endif /* DSCO_KITTY_AGENT_WINDOWS_H */
