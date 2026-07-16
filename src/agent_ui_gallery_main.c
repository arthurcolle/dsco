#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#define _POSIX_C_SOURCE 200809L

#include "agent_ui_gallery.h"
#include "agent_ui_theme.h"

#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/signal.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t s_resized = 0;

static void on_resize(int signal_number) {
    (void)signal_number;
    s_resized = 1;
}

static void usage(const char *name) {
    fprintf(stderr,
        "usage: %s [--theme ID] [--page 1-7|NAME] [--ppm PATH]\n"
        "          [--width PHYSICAL] [--height PHYSICAL] [--dpr 1-4]\n"
        "          [--no-hold] [--list-themes] [--list-pages]\n",
        name ? name : "dsco-agent-ui-gallery");
}

static void list_themes(void) {
    for (size_t i = 0; i < agent_ui_theme_count(); i++) {
        const agent_ui_theme_t *theme = agent_ui_theme_at(i);
        printf("%-18s  %-20s  %s\n", theme->id, theme->name, theme->description);
    }
}

static void list_pages(void) {
    for (int i = 0; i < AGENT_UI_GALLERY_PAGE_COUNT; i++)
        printf("%d  %s\n", i + 1,
               agent_ui_gallery_page_name((agent_ui_gallery_page_t)i));
}

static bool parse_page(const char *value, agent_ui_gallery_page_t *page) {
    if (!value || !*value || !page) return false;
    char *end = NULL;
    long number = strtol(value, &end, 10);
    if (end != value && *end == '\0' && number >= 1 &&
        number <= AGENT_UI_GALLERY_PAGE_COUNT) {
        *page = (agent_ui_gallery_page_t)(number - 1);
        return true;
    }
    for (int i = 0; i < AGENT_UI_GALLERY_PAGE_COUNT; i++)
        if (!strcasecmp(value,
                        agent_ui_gallery_page_name((agent_ui_gallery_page_t)i))) {
            *page = (agent_ui_gallery_page_t)i;
            return true;
        }
    return false;
}

static bool set_raw(struct termios *saved) {
    if (!saved || !isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, saved) != 0)
        return false;
    struct termios raw = *saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

static bool present(agent_ui_gallery_session_t *session,
                    agent_ui_gallery_page_t page,
                    const agent_ui_theme_t *theme) {
    return agent_ui_gallery_session_present(session, stdout, page, theme);
}

int main(int argc, char **argv) {
    const char *ppm = NULL;
    const agent_ui_theme_t *theme = agent_ui_theme_default();
    agent_ui_gallery_page_t page = AGENT_UI_GALLERY_WORKBENCH;
    int width = 0, height = 0, dpr = 0;
    bool hold = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--list-themes")) {
            list_themes();
            return 0;
        } else if (!strcmp(argv[i], "--list-pages")) {
            list_pages();
            return 0;
        } else if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
            theme = agent_ui_theme_find(argv[++i]);
            if (!theme) {
                fprintf(stderr, "unknown theme: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            if (!parse_page(argv[++i], &page)) {
                fprintf(stderr, "unknown page: %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) {
            ppm = argv[++i];
        } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dpr") && i + 1 < argc) {
            dpr = atoi(argv[++i]);
            if (dpr < 1 || dpr > 4) {
                fprintf(stderr, "--dpr must be between 1 and 4\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--no-hold")) {
            hold = false;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (ppm) {
        int scale = dpr > 0 ? dpr : 1;
        if (width <= 0) width = 1800 * scale;
        if (height <= 0) height = 1000 * scale;
        if (!agent_ui_gallery_write_ppm(ppm, width, height, scale, page, theme)) {
            fprintf(stderr, "failed to write gallery frame: %s\n", ppm);
            return 1;
        }
        return 0;
    }

    agent_ui_gallery_session_t session;
    if (!agent_ui_gallery_session_begin(&session, stdout, width, height, dpr)) {
        fprintf(stderr,
                "dsco-agent-ui-gallery: Kitty graphics unavailable; use --ppm PATH\n");
        return 1;
    }
    if (!present(&session, page, theme)) {
        agent_ui_gallery_session_end(&session, stdout);
        fprintf(stderr, "dsco-agent-ui-gallery: failed to render component page\n");
        return 1;
    }

    struct termios saved;
    bool raw = hold && set_raw(&saved);
    if (hold && isatty(STDIN_FILENO)) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_resize;
        sigemptyset(&action.sa_mask);
        (void)sigaction(SIGWINCH, &action, NULL);
        bool running = true;
        while (running) {
            if (s_resized) {
                s_resized = 0;
                if (agent_ui_gallery_session_geometry_changed(&session, stdout) &&
                    !present(&session, page, theme))
                    break;
            }
            struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
            int ready = poll(&input, 1, 180);
            if (ready < 0) continue;
            if (ready == 0 || !(input.revents & POLLIN)) continue;
            unsigned char bytes[16] = {0};
            ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count <= 0) continue;
            bool changed = false;
            unsigned char key = bytes[0];
            if (key == 'q' || key == 'Q') {
                running = false;
            } else if (key >= '1' && key < '1' + AGENT_UI_GALLERY_PAGE_COUNT) {
                page = (agent_ui_gallery_page_t)(key - '1');
                changed = true;
            } else if (key == '[') {
                theme = agent_ui_theme_next(theme, -1);
                changed = true;
            } else if (key == ']' || key == '\t' || key == 't' || key == 'T') {
                theme = agent_ui_theme_next(theme, 1);
                changed = true;
            } else if (key == 'h' || key == 'H' ||
                       (key == 27 && count >= 3 && bytes[1] == '[' && bytes[2] == 'D')) {
                page = (agent_ui_gallery_page_t)((page + AGENT_UI_GALLERY_PAGE_COUNT - 1) %
                                                  AGENT_UI_GALLERY_PAGE_COUNT);
                changed = true;
            } else if (key == 'l' || key == 'L' ||
                       (key == 27 && count >= 3 && bytes[1] == '[' && bytes[2] == 'C')) {
                page = (agent_ui_gallery_page_t)((page + 1) % AGENT_UI_GALLERY_PAGE_COUNT);
                changed = true;
            } else if (key == 27) {
                running = false;
            }
            if (changed && !present(&session, page, theme)) break;
        }
    }
    if (raw) (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    agent_ui_gallery_session_end(&session, stdout);
    return 0;
}
