#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>
static bool deny_directory_sync;
static int owned_fsync(int fd) {
    struct stat st;
    if (deny_directory_sync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EIO; return -1;
    }
    return fsync(fd);
}
#define fsync owned_fsync
#include "../src/shell_jobs.c"
#undef fsync
int main(void) {
    char directory[] = "/tmp/dsco-job-publication-XXXXXX"; assert(mkdtemp(directory));
    deny_directory_sync = true;
    assert(!atomic_text(directory, "status.json", "{\"status\":\"completed\"}"));
    deny_directory_sync = false;
    assert(atomic_text(directory, "status.json", "{\"status\":\"completed\"}"));
    char *value = read_text(directory, "status.json"); assert(value && strstr(value, "completed")); free(value);
    char file[1024]; snprintf(file, sizeof(file), "%s/status.json", directory); unlink(file); assert(rmdir(directory)==0);
    puts("PASS: directory fsync failure cannot report durable publication; recovery preserves receipt");
}
