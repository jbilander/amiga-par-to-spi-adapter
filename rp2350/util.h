#ifndef UTIL_H
#define UTIL_H
#include <stddef.h>
#include <stdint.h>
void utf8_to_latin1(const char *utf8_str, char *latin1_buf, size_t buf_len);
uint32_t get_fattime(void);
#endif