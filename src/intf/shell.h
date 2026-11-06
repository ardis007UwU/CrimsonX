#ifndef STRING_H
#define STRING_H

#include "keyboard.h"

void shell_init(void);

void shell_handle_input(struct KeyboardEvent event);

#endif