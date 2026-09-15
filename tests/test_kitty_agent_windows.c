/* Standalone, no Kitty windows or model requests:
 * cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude \
 *    tests/test_kitty_agent_windows.c -o /tmp/test_kitty_agent_windows && /tmp/test_kitty_agent_windows
 */
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <locale.h>
#include <wchar.h>

static char *mock_getenv(const char *name);
#define getenv mock_getenv
#define access mock_access
#define posix_spawn mock_posix_spawn
#include "../src/kitty_agent_windows.c"
#undef access
#undef posix_spawn
#undef getenv

static char *test_home_dir;
static char *mock_getenv(const char *name) {
    return !strcmp(name, "HOME") ? test_home_dir : getenv(name);
}

static char launch_args[40][PATH_MAX];
static int launches;
int mock_access(const char *path, int mode) {
    (void)path;
    (void)mode;
    return 0;
}
int mock_posix_spawn(pid_t *pid, const char *path,
                     const posix_spawn_file_actions_t *actions,
                     const posix_spawnattr_t *attr,
                     char *const argv[], char *const envp[]) {
    (void)pid; (void)path; (void)actions; (void)attr; (void)envp;
    launches++;
    memset(launch_args, 0, sizeof(launch_args));
    for (size_t i = 0; argv[i] && i < 40; i++)
        snprintf(launch_args[i], sizeof(launch_args[i]), "%s", argv[i]);
    return ENOENT; /* Inspect launch without creating a process or a real window. */
}
static const char *argument(const char *name) {
    for (size_t i = 0; i + 1 < 40; i++)
        if (!strcmp(launch_args[i], name))
            return launch_args[i + 1];
    return NULL;
}
static char *read_log(int id) {
    FILE *f = fopen(s_windows[id].log_path, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long size = ftell(f);
    assert(size >= 0);
    rewind(f);
    char *text = calloc((size_t)size + 1, 1);
    assert(text);
    assert(fread(text, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    return text;
}
int main(void) {
    unsetenv("DSCO_KITTY_AGENT_WINDOWS");
    unsetenv("KITTY_WINDOW_ID");
    unsetenv("KITTY_LISTEN_ON");
    unsetenv("DSCO_KITTY_AGENT_WINDOW_TYPE");
    unsetenv("DSCO_KITTY_AGENT_WINDOW_LOCATION");
    unsetenv("DSCO_KITTY_AGENT_WINDOWS_KEEP_OPEN");
    assert(!windows_enabled());
    setenv("KITTY_WINDOW_ID", "42", 1);
    assert(!windows_enabled());
    setenv("KITTY_LISTEN_ON", "unix:/tmp/nonexistent-test-kitty", 1);
    assert(!windows_enabled()); /* Remote control alone must not create OS panes. */
    setenv("DSCO_KITTY_AGENT_WINDOWS", "1", 1);
    assert(windows_enabled());
    setenv("DSCO_KITTY_AGENT_WINDOWS", "0", 1);
    assert(!windows_enabled());
    kitty_agent_window_spawn(0, 123, "disabled", "model");
    assert(!launches);
    setenv("DSCO_KITTY_AGENT_WINDOWS", "1", 1);

    char test_dir[] = "/tmp/dsco-kitty-test-XXXXXX";
    assert(mkdtemp(test_dir));
    test_home_dir = test_dir;
    setenv("DSCO_KITTY_SIGNATURE", "ᴀʀᴛʜᴜʀᴄᴏʟʟᴇ", 1);
    assert(setlocale(LC_CTYPE, "en_US.UTF-8") || setlocale(LC_CTYPE, "C.UTF-8"));
    char clipped[256];
    for (size_t cap = 1; cap <= sizeof(clipped); cap++) {
        safe_title(clipped, cap, "⧖𝚊𝚛𝚝𝚑𝚞𝚛", 4, "⌬ᵃʳᵗʰᵘʳᶜᵒˡˡᵉ🝮𝖆𝖗𝖙𝖍𝖚𝖗");
        assert(strlen(clipped) < cap);
        mbstate_t state = {0};
        const char *p = clipped;
        assert(mbsrtowcs(NULL, &p, 0, &state) != (size_t)-1);
    }
    char task[8192];
    memset(task, 'x', sizeof(task) - 1);
    task[sizeof(task) - 1] = 0;
    kitty_agent_window_spawn(0, 123, task, "model\033]52;c;bad\a safe");
    assert(launches == 1);
    assert(!strcmp(argument("--type"), "window"));
    assert(!strcmp(argument("--location"), "split"));
    assert(!strcmp(argument("--match"), "window_id:42"));
    assert(!strcmp(argument("--next-to"), "id:42"));
    assert(!strcmp(argument("--source-window"), "id:42"));
    assert(argument("--dont-take-focus"));
    assert(!argument("--allow-remote-control"));
    assert(strstr(argument("--title"), "ᴀʀᴛʜᴜʀᴄᴏʟʟᴇ"));

    const char *chunks[] = {"\nOUTPUT ", "\033[3", "1mred\033[0m ",
        "\033]52;c;secret", "\033", "\\ok ", "\033Pbad\033\\", "⧉\n"};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++)
        kitty_agent_window_append(0, chunks[i], strlen(chunks[i]));
    char *text = read_log(0);
    assert(strstr(text, "\033[38;2;104;211;225m"));
    assert(strstr(text, "ᴀʀᴛʜᴜʀᴄᴏʟʟᴇ"));
    assert(strstr(text, task)); /* Long metadata is neither over-read nor silently clipped. */
    assert(strstr(text, "OUTPUT red ok ⧉\n"));
    assert(!strstr(text, "secret"));
    assert(!strstr(text, "bad"));
    free(text);
    setenv("DSCO_KITTY_AGENT_WINDOWS_KEEP_OPEN", "1", 1);
    kitty_agent_window_complete(0, task, 0);
    text = read_log(0);
    assert(strstr(text, "EXIT 0"));
    free(text);
    assert(s_windows[0].used);
    unlink(s_windows[0].log_path);
    close_window(&s_windows[0]);

    setenv("DSCO_KITTY_AGENT_WINDOW_TYPE", "os-window", 1);
    setenv("DSCO_KITTY_AGENT_WINDOW_LOCATION", "hsplit", 1);
    kitty_agent_window_spawn(1, 124, "task", "model");
    assert(!strcmp(argument("--type"), "os-window"));
    assert(!strcmp(argument("--location"), "hsplit"));
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s", s_windows[1].log_path);
    unsetenv("DSCO_KITTY_AGENT_WINDOWS_KEEP_OPEN");
    kitty_agent_window_complete(1, "done", 0);
    assert(!s_windows[1].used);
    unlink(log_path);
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.dsco/sessions/swarm/%d", test_dir, (int)getpid());
    assert(!rmdir(dir));
    snprintf(dir, sizeof(dir), "%s/.dsco/sessions/swarm", test_dir); assert(!rmdir(dir));
    snprintf(dir, sizeof(dir), "%s/.dsco/sessions", test_dir); assert(!rmdir(dir));
    snprintf(dir, sizeof(dir), "%s/.dsco", test_dir); assert(!rmdir(dir));
    assert(!rmdir(test_dir));
    puts("Kitty companion launch, metadata bounds, lifecycle, and escape filtering passed");
}
