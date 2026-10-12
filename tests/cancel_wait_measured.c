#include <poll.h>
extern int measured_poll(struct pollfd *, nfds_t, int);
#define poll measured_poll
#include "../src/cancel_wait.c"
