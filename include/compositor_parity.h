#ifndef DSCO_COMPOSITOR_PARITY_H
#define DSCO_COMPOSITOR_PARITY_H

#include <stdio.h>

/* Emit a deterministic established-TUI/native-compositor comparison corpus.
 * Returns 0 on success and writes a one-line summary when summary is non-NULL. */
int compositor_parity_write(const char *directory, FILE *summary);

#endif /* DSCO_COMPOSITOR_PARITY_H */
