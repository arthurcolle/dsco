#ifndef DSCO_SCHEDULER_IO_H
#define DSCO_SCHEDULER_IO_H
/* Private, single-threaded scheduler/event-loop bridge. */
typedef struct scheduler_t scheduler_t;
typedef struct sched_io sched_io_t;
sched_io_t *sched_io_new(void);
void sched_io_free(sched_io_t *io);
/* Synchronize current waits; resume ready/error tasks. -1 = backend failure. */
int sched_io_poll(sched_io_t *io, scheduler_t *s, int timeout_ms);
#endif
