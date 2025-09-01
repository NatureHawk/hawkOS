#pragma once
#include <stdint.h>

void console_init(void);                // clear screen, reset cursor
void console_putc(char c);              // prints one char (handles \n,\r,\b)
void console_puts(const char* s);       // prints a string
void console_printu_at(int x,int y,unsigned long long n); // tiny decimal
