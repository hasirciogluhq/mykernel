#ifndef MYKERNEL_DRIVERS_KEYBOARD_H
#define MYKERNEL_DRIVERS_KEYBOARD_H

#include <kernel/types.h>

void keyboard_init(void);
void keyboard_poll(void);          /* drain PS/2 → console rx queue */
int  keyboard_has_input(void);

#endif
