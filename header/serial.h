#ifndef SERIAL_H
#define SERIAL_H
#include <stdint.h>

void serial_init(void);
int  serial_tx_ready(void);
void serial_putc(char c);
void serial_puts(const char *s);

#endif
