#pragma once

void flush_buffer(void);
char shell_getchar(void);
void *shell_server(void *arg);
void send_cap_to_procmgr(int cap);

#ifdef CHCORE_PLAT_RASPI3
void *handle_uart_irq(void *arg);
#else /* CHCORE_PLAT_RASPI3 */
void *other_plat_get_char(void *arg);
#endif /* CHCORE_PLAT_RASPI3 */
