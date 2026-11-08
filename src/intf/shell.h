#ifndef SHELL_H
#define SHELL_H

#include "keyboard.h"

/** Initialize interactive shell state and draw the first prompt. */
void shell_init(void);
/** Consume a keyboard event and update the command line. */
void shell_handle_input(struct KeyboardEvent event);
/** Process pending shell work and redraw the current prompt. */
void shell_update(void);

#endif