#ifndef DSCO_TRANSCRIPT_VIEW_H
#define DSCO_TRANSCRIPT_VIEW_H
#include "llm.h"
char *transcript_view_snapshot(const conversation_t *conv);
/* Caller owns idle session input; never call against a mutating conversation. */
void transcript_view_show(void *conv);
#endif
