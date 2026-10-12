#ifndef DSCO_EVENT_WAKEUP_H
#define DSCO_EVENT_WAKEUP_H
/* Private reactor notification. Producers must stop before close/free. */
typedef struct { int queue_fd, read_fd, write_fd; } ev_wakeup_t;
int ev_wakeup_init(ev_wakeup_t *wake, int queue_fd);
int ev_wakeup_signal(const ev_wakeup_t *wake);
void ev_wakeup_drain(const ev_wakeup_t *wake);
void ev_wakeup_close(ev_wakeup_t *wake);
#endif
