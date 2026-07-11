#ifndef MYKERNEL_DRIVERS_PS2_H
#define MYKERNEL_DRIVERS_PS2_H

#include <kernel/types.h>

#define PS2_DATA 0x60
#define PS2_STAT 0x64

#define PS2_STAT_OBF  0x01  /* output buffer full */
#define PS2_STAT_IBF  0x02  /* input buffer full */
#define PS2_STAT_AUX  0x20  /* mouse data */

void    ps2_init(void);
void    ps2_poll(void);                 /* drain ports → kbd / mouse handlers */
int     ps2_wait_write(void);
int     ps2_wait_read(void);
void    ps2_write_cmd(uint8_t cmd);
void    ps2_write_data(uint8_t data);
uint8_t ps2_read_data(void);
int     ps2_write_mouse(uint8_t data);  /* 0xD4 + data, returns ACK or -1 */

#endif
