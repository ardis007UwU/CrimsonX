#ifndef SHELL_H
#define SHELL_H

#include "keyboard.h"

void shell_init(void);
void shell_handle_input(struct KeyboardEvent event);
void shell_update(void);

#endif