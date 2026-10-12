#ifndef DSCO_VOICE_SESSION_H
#define DSCO_VOICE_SESSION_H

#include "llm.h"
#include "tui.h"

/* Owns audio and voice turns until Esc, Ctrl+O, or 'stop voice'. The existing
 * conversation, tool registry, trust tier, and compositor remain in place. */
int voice_session_run(conversation_t *conv, session_state_t *session,
                      tui_status_bar_t *sb, bool full_duplex);
/* Owned, bounded recent history; excludes private reasoning and binary media. */
char *voice_session_context(const conversation_t *conv);

#endif
