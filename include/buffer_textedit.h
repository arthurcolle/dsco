#ifndef DSCO_BUFFER_TEXTEDIT_H
#define DSCO_BUFFER_TEXTEDIT_H
#include <stdbool.h>
#include <stddef.h>
/* Explicit macOS editor route; buffer access and launch retain the caller's gate. */
bool buffer_textedit_open(const char *input, char *result, size_t cap);
#endif
