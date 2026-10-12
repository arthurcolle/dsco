/* Exercises scoped handler restoration with the actual interop entry point. */
#include "agent_interop.h"
#include "process_capture.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t observed;
static void previous_handler(int number) { observed = number; }
static char executable[PATH_MAX];
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "fixture")) {
        /* Signal only the owning interop parent, not its private group. */
        if (argc > 2 && strcmp(argv[2], "0")) {
            assert(kill(getppid(), atoi(argv[2])) == 0);
            if (argc > 3 && !strcmp(argv[3], "exit")) return 0;
            usleep(10000);
            assert(kill(getppid(), atoi(argv[2])) == 0);
            for (;;) pause();
        }
        return 0;
    }
    assert(realpath(argv[0], executable));
    int cases = 0;
    for (int number = 0; number <= 2; number++) {
        int sig = number == 1 ? SIGINT : number == 2 ? SIGTERM : 0;
        struct sigaction custom = {0}, old_int, old_term;
        custom.sa_handler = previous_handler; sigemptyset(&custom.sa_mask);
        assert(!sigaction(SIGINT, &custom, &old_int));
        assert(!sigaction(SIGTERM, &custom, &old_term));
        sigset_t before, after; assert(!sigprocmask(SIG_SETMASK, NULL, &before));
        observed = 0;
        char signal_text[16]; snprintf(signal_text, sizeof(signal_text), "%d", sig);
        char *args[] = {"dsco", "interop", "run", "generic", "--prompt", "fixture",
            "--json", "--timeout-ms", "1500", "--", executable, "fixture", signal_text, NULL};
        int rc = agent_interop_cli(13, args, executable);
        assert(rc == (sig ? 128 + sig : 0));
        assert(observed == sig);
        struct sigaction current;
        assert(!sigaction(SIGINT, NULL, &current)); assert(current.sa_handler == previous_handler);
        assert(!sigaction(SIGTERM, NULL, &current)); assert(current.sa_handler == previous_handler);
        assert(!sigprocmask(SIG_SETMASK, NULL, &after));
        assert(sigismember(&before, SIGINT) == sigismember(&after, SIGINT));
        assert(sigismember(&before, SIGTERM) == sigismember(&after, SIGTERM));
        assert(!sigaction(SIGINT, &old_int, NULL)); assert(!sigaction(SIGTERM, &old_term, NULL));
        cases++;
    }
    for (int blocked = 0; blocked < 2; blocked++) {
        struct sigaction action = {0}, old;
        action.sa_handler = blocked ? previous_handler : SIG_IGN;
        sigemptyset(&action.sa_mask); assert(!sigaction(SIGINT, &action, &old));
        sigset_t mask, previous; sigemptyset(&mask); sigaddset(&mask, SIGINT);
        assert(!sigprocmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &mask, &previous));
        observed = 0;
        char sigtext[16]; snprintf(sigtext, sizeof(sigtext), "%d", SIGINT);
        char *args[] = {"dsco", "interop", "run", "generic", "--prompt", "fixture",
            "--json", "--timeout-ms", "1500", "--", executable, "fixture", sigtext, "exit", NULL};
        assert(agent_interop_cli(14, args, executable) == 0);
        assert(!observed);
        struct sigaction restored; assert(!sigaction(SIGINT, NULL, &restored));
        assert(restored.sa_handler == action.sa_handler);
        if (blocked) {
            sigset_t pending; assert(!sigpending(&pending));
            assert(sigismember(&pending, SIGINT));
            assert(!sigprocmask(SIG_UNBLOCK, &mask, NULL));
            assert(observed == SIGINT);
        }
        assert(!sigprocmask(SIG_SETMASK, &previous, NULL));
        assert(!sigaction(SIGINT, &old, NULL)); cases++;
    }
    /* Capture must not install signal state or launch on a preexisting latch. */
    volatile sig_atomic_t latch = SIGINT;
    process_capture_t capture; char *args[] = {"/definitely/missing", NULL};
    assert(!process_capture_cancellable(args[0], args, 1500, 128, &capture, &latch));
    assert(capture.cancelled && capture.cancel_signal == SIGINT && !capture.spawn_error);
    assert(capture.cleanup_complete && !capture.output && capture.unreaped_pid == 0);
    process_capture_free(&capture);
    printf("interop signal scopes: %d real-process cases + prespawn cancellation passed\n", cases);
}
