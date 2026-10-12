/* Offline daemon integration fixture: only the test build executes shell tasks. */
#include "swarm_daemon.h"
int main(int argc, char **argv) { return swarm_daemon_cli(argc, argv); }
