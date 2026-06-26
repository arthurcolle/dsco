#include "mcp_names.h"

#include <stdio.h>
#include <string.h>

#define CLAUDEAI_SERVER_PREFIX "claude.ai "

static bool mcp_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

void dsco_mcp_normalize_name(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    if (!in || !*in)
        in = "server";

    bool collapse = strncmp(in, CLAUDEAI_SERVER_PREFIX, strlen(CLAUDEAI_SERVER_PREFIX)) == 0;
    size_t j = 0;
    bool last_underscore = false;

    for (size_t i = 0; in[i] && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        char next = mcp_name_char(c) ? (char)c : '_';
        if (collapse && next == '_' && last_underscore)
            continue;
        out[j++] = next;
        last_underscore = next == '_';
    }

    if (collapse) {
        while (j > 0 && out[0] == '_') {
            memmove(out, out + 1, j);
            j--;
        }
        while (j > 0 && out[j - 1] == '_')
            j--;
    }

    if (j == 0 && out_len > 1) {
        snprintf(out, out_len, "mcp");
        return;
    }
    out[j] = '\0';
}

void dsco_mcp_build_tool_name(const char *server_name, const char *tool_name, char *out,
                              size_t out_len) {
    if (!out || out_len == 0)
        return;
    char server[128];
    char tool[128];
    dsco_mcp_normalize_name(server_name, server, sizeof(server));
    dsco_mcp_normalize_name(tool_name, tool, sizeof(tool));
    snprintf(out, out_len, "mcp__%s__%s", server, tool);
}

bool dsco_mcp_is_canonical_tool_name(const char *name) {
    return name && strncmp(name, "mcp__", 5) == 0;
}

void dsco_mcp_legacy_alias_from_canonical(const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!dsco_mcp_is_canonical_tool_name(name))
        return;

    size_t pos = 0;
    int n = snprintf(out, out_len, "mcp_");
    if (n < 0)
        return;
    pos = (size_t)n;
    if (pos >= out_len) {
        out[out_len - 1] = '\0';
        return;
    }

    for (const char *p = name + 5; *p && pos + 1 < out_len;) {
        if (p[0] == '_' && p[1] == '_') {
            out[pos++] = '_';
            p += 2;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
}
