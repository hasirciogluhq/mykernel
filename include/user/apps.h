#ifndef MYKERNEL_USER_APPS_H
#define MYKERNEL_USER_APPS_H

#include <user/gx.h>

void app_terminal_open(int screen_w, int screen_h);
void app_terminal_close(void);
int  app_terminal_win(void);
void app_terminal_tick(const ugx_input_state *in, uint8_t pressed);
void app_terminal_paint(int focused);

void app_notepad_open(int screen_w, int screen_h);
void app_notepad_close(void);
int  app_notepad_win(void);
void app_notepad_tick(const ugx_input_state *in, uint8_t pressed);
void app_notepad_paint(int focused);

#endif
