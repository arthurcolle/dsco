#ifndef DSCO_IDE_CLI_H
#define DSCO_IDE_CLI_H

/* Full main argv: dsco ide [PROJECT], dsco ide --check PROJECT, --help.
 * Returns 0 on success/help, 1 on operational failure, 2 on usage/non-TTY.
 * Interactive mode uses only the caller's existing terminal. Link with
 * native_buffer_editor.c. No tools, project configuration, or UI auto-launch.
 * Single-buffer, bounded UTF-8 editor; see --help for keys and limitations. */
int ide_cli(int argc, char **argv);

#endif
