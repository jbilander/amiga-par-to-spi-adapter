#include "util.h"
#include <string.h>
#include <stdint.h>

void utf8_to_latin1(const char *utf8_str, char *latin1_buf, size_t buf_len) {
    if (buf_len == 0) return;
    size_t utf8_idx = 0;
    size_t latin1_idx = 0;
    while (utf8_str[utf8_idx] != '\0' && latin1_idx < buf_len - 1) {
        unsigned char c = (unsigned char)utf8_str[utf8_idx];
        if (c < 0x80) { // ASCII
            latin1_buf[latin1_idx++] = (char)c;
            utf8_idx++;
        } else if ((c & 0xE0) == 0xC0) { // 2-byte sequence
            unsigned char c2 = (unsigned char)utf8_str[utf8_idx + 1];
            if ((c2 & 0xC0) == 0x80) {
                uint16_t unicode_char = ((c & 0x1F) << 6) | (c2 & 0x3F);
                if (unicode_char >= 0x00A0 && unicode_char <= 0x00FF) {
                    latin1_buf[latin1_idx++] = (char)unicode_char;
                } else {
                    latin1_buf[latin1_idx++] = '?';
                }
                utf8_idx += 2;
            } else {
                latin1_buf[latin1_idx++] = '?';
                utf8_idx++;
            }
        } else { // 3+ byte sequence or invalid UTF-8
            latin1_buf[latin1_idx++] = '?';
            while ((utf8_str[utf8_idx] & 0xC0) == 0x80 && utf8_str[utf8_idx] != '\0') {
                utf8_idx++;
            }
            utf8_idx++;
        }
    }
    latin1_buf[latin1_idx] = '\0';
}