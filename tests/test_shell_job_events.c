#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "shell_job_events.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double milliseconds(void) {
    struct timespec ts; assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void delay(int ms) {
    struct timespec ts = {ms/1000, ms%1000*1000000L};
    while (nanosleep(&ts, &ts) < 0) assert(errno == EINTR);
}
static void *publish(void *arg) {
    char *directory = arg; char temporary[1024], final[1024];
    snprintf(temporary, sizeof(temporary), "%s/temporary", directory);
    snprintf(final, sizeof(final), "%s/status.json", directory);
    delay(250); int fd = open(temporary, O_WRONLY|O_CREAT|O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "complete", 8) == 8 && close(fd) == 0);
    assert(rename(temporary, final) == 0);
    return NULL;
}
int main(void) {
    alarm(10);
    char directory[] = "/tmp/dsco-job-events-XXXXXX"; assert(mkdtemp(directory));
    shell_job_events_t *events = shell_job_events_open(directory, 0, false); assert(events);
    pthread_t thread; assert(pthread_create(&thread, NULL, publish, directory) == 0);
    double start = milliseconds(); assert(shell_job_events_wait(events, -1) > 0);
    assert(milliseconds() - start >= 200);
    assert(shell_job_events_wait_count(events) == 1 && shell_job_events_wake_count(events) == 1);
    assert(pthread_join(thread, NULL) == 0);
    printf("backend=%s directory_wait_ms=%.3f kernel_waits=1\n", shell_job_events_backend(), milliseconds()-start);
    shell_job_events_close(events);
    /* Registration before release removes the fork/registration race. */
    int gate[2]; assert(pipe(gate) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (!child) { close(gate[1]); char byte; if (read(gate[0], &byte, 1)!=1) _exit(99); delay(150); _exit(7); }
    close(gate[0]); events = shell_job_events_open(directory, child, false); assert(events);
    assert(write(gate[1], "x", 1) == 1); close(gate[1]);
    start = milliseconds(); assert(shell_job_events_wait(events, -1) > 0);
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    printf("process_exit_ms=%.3f owner_reaped=true\n", milliseconds()-start);
    shell_job_events_close(events);
    /* Exit-before-registration must reconcile promptly, never wait forever. */
    events = shell_job_events_open(directory, child, false); assert(events);
    assert(shell_job_events_wait(events, -1) > 0); shell_job_events_close(events);
    char file[1024]; snprintf(file, sizeof(file), "%s/status.json", directory); unlink(file);
    events = shell_job_events_open(directory, 0, false); assert(events);
    start = milliseconds(); assert(shell_job_events_wait(events, 50) == 0);
    assert(milliseconds()-start >= 30); shell_job_events_close(events); assert(rmdir(directory)==0);
    puts("PASS: directory publication, process exit, registration race and explicit observation budget");
}
