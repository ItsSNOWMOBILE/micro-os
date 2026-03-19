/*
 * serial.h — COM1 serial port driver for debug output.
 */

#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *s);

/* Register COM1 with the HAL. */
void serial_register_hal(void);

#endif /* SERIAL_H */
