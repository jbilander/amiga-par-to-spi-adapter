/* ftp_utils.h - FTP utility functions for string and path handling */

#ifndef FTP_UTILS_H
#define FTP_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include "ftp_types.h"

// ============================================================================
// String Utilities
// ============================================================================

/**
 * Split a string by delimiter into array of tokens
 * @param str String to split (will be modified with null terminators)
 * @param delim Delimiter character
 * @param tokens Array to store pointers to tokens
 * @param max_tokens Maximum number of tokens
 * @return Number of tokens found
 */
int ftp_split_string(char *str, char delim, char **tokens, int max_tokens);

/**
 * Trim whitespace from both ends of string (in place)
 * @param str String to trim (modified in place)
 */
void ftp_trim(char *str);

/**
 * Case-insensitive string comparison
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, non-zero otherwise
 */
int ftp_strcasecmp(const char *s1, const char *s2);

/**
 * Case-insensitive string comparison (n characters)
 * @param s1 First string
 * @param s2 Second string
 * @param n Maximum number of characters to compare
 * @return 0 if equal, non-zero otherwise
 */
int ftp_strncasecmp(const char *s1, const char *s2, size_t n);

/**
 * Parse FTP command string into command enum
 * @param cmd_str Command string (e.g., "USER", "PASS")
 * @return FTP command enum value
 */
ftp_command_t ftp_parse_command(const char *cmd_str);

/**
 * Get month abbreviation string (3 letters)
 * @param month Month number (1-12)
 * @return Month string (e.g., "JAN", "FEB") or "ERR"
 */
const char *ftp_month_str(uint8_t month);

// ============================================================================
// Path Utilities
// ============================================================================

/**
 * Initialize path to root
 * @param path Path structure to initialize
 */
void ftp_path_init(ftp_path_t *path);

/**
 * Change to new path (absolute or relative)
 * @param path Current path structure
 * @param new_path New path string (absolute starts with '/', relative otherwise)
 */
void ftp_path_change(ftp_path_t *path, const char *new_path);

/**
 * Go up one directory level
 * @param path Path structure to modify
 */
void ftp_path_up(ftp_path_t *path);

/**
 * Get full file path (combining current directory with filename)
 * @param path Current path structure
 * @param filename Filename (can be absolute or relative)
 * @param output Output buffer for full path
 * @param output_len Output buffer size
 */
void ftp_path_get_full(const ftp_path_t *path, const char *filename, 
                       char *output, size_t output_len);

/**
 * Normalize path (remove .., handle //, etc.)
 * @param path Path to normalize (modified in place)
 */
void ftp_path_normalize(char *path);

#endif // FTP_UTILS_H