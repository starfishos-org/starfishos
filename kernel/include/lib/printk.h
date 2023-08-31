#pragma once

typedef void (*graphic_putc_handler)(char c);
extern graphic_putc_handler graphic_putc;

void set_graphic_putc_handler(graphic_putc_handler f);
void printk(const char *fmt, ...);
