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

/**
 * Get current time for FatFS
 * 
 * FatFS requires this function to provide timestamps for file operations.
 * Since the Pico doesn't have an RTC, we return a fixed timestamp.
 * 
 * Returns time packed into a 32-bit value:
 * bit 31:25 - Year from 1980 (0-127)
 * bit 24:21 - Month (1-12)
 * bit 20:16 - Day (1-31)
 * bit 15:11 - Hour (0-23)
 * bit 10:5  - Minute (0-59)
 * bit 4:0   - Second / 2 (0-29)
 */
uint32_t get_fattime(void) {
    // Return a fixed timestamp: 2024-01-01 00:00:00
    // Year: 2024 - 1980 = 44
    // Month: 1 (January)
    // Day: 1
    // Hour: 0
    // Minute: 0
    // Second: 0
    
    return ((uint32_t)(44) << 25)   // Year 2024
         | ((uint32_t)(1) << 21)    // January
         | ((uint32_t)(1) << 16)    // 1st day
         | ((uint32_t)(0) << 11)    // 0 hours
         | ((uint32_t)(0) << 5)     // 0 minutes
         | ((uint32_t)(0) >> 1);    // 0 seconds
}