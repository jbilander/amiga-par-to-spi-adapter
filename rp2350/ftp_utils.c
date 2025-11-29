/* ftp_utils.c - FTP utility function implementations */

#include "ftp_utils.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>

// ============================================================================
// String Utilities
// ============================================================================

int ftp_split_string(char *str, char delim, char **tokens, int max_tokens) {
    int count = 0;
    char *p = str;

    while (*p && count < max_tokens) {
        while (*p == delim) p++;
        if (!*p) break;

        tokens[count++] = p;

        while (*p && *p != delim) p++;

        if (*p) {
            *p = '\0';
            p++;
        }
    }

    return count;
}

void ftp_trim(char *str) {
    if (!str || !*str) return;

    char *start = str;
    while (isspace((unsigned char)*start)) start++;

    char *end = str + strlen(str) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;

    size_t len = end - start + 1;
    if (start != str) {
        memmove(str, start, len);
    }
    str[len] = '\0';
}

int ftp_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int ftp_strncasecmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;

    while (n-- > 0 && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }

    if (n == (size_t)-1) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

ftp_command_t ftp_parse_command(const char *cmd_str) {
    if (!cmd_str) return FTP_CMD_NONE;

    struct {
        const char *name;
        ftp_command_t cmd;
    } cmd_table[] = {
        {"USER", FTP_CMD_USER}, {"PASS", FTP_CMD_PASS}, {"QUIT", FTP_CMD_QUIT},
        {"SYST", FTP_CMD_SYST}, {"NOOP", FTP_CMD_NOOP}, {"FEAT", FTP_CMD_FEAT},
        {"PWD",  FTP_CMD_PWD},  {"CWD",  FTP_CMD_CWD},  {"CDUP", FTP_CMD_CDUP},
        {"TYPE", FTP_CMD_TYPE}, {"PASV", FTP_CMD_PASV}, {"PORT", FTP_CMD_PORT},
        {"LIST", FTP_CMD_LIST}, {"NLST", FTP_CMD_NLST}, {"MLSD", FTP_CMD_MLSD},
        {"RETR", FTP_CMD_RETR}, {"STOR", FTP_CMD_STOR}, {"DELE", FTP_CMD_DELE},
        {"MKD",  FTP_CMD_MKD},  {"RMD",  FTP_CMD_RMD},  {"RNFR", FTP_CMD_RNFR},
        {"RNTO", FTP_CMD_RNTO}, {"ABOR", FTP_CMD_ABOR}, {"OPTS", FTP_CMD_OPTS},
        {"MFMT", FTP_CMD_MFMT}, {"MFCT", FTP_CMD_MFCT},
        {"XMKD", FTP_CMD_XMKD}, {"XRMD", FTP_CMD_XRMD},
        {NULL, FTP_CMD_NONE}
    };

    for (int i = 0; cmd_table[i].name != NULL; i++) {
        if (ftp_strcasecmp(cmd_str, cmd_table[i].name) == 0) {
            return cmd_table[i].cmd;
        }
    }

    return FTP_CMD_NONE;
}

const char *ftp_month_str(uint8_t month) {
    static const char *months[] = {
        "ERR", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    return (month >= 1 && month <= 12) ? months[month] : months[0];
}

// ============================================================================
// Path Utilities
// ============================================================================

void ftp_path_init(ftp_path_t *path) {
    strcpy(path->path, "/");
}

void ftp_path_change(ftp_path_t *path, const char *new_path) {
    if (!new_path || !*new_path) return;

    char temp[FTP_PATH_MAX_LEN];
    size_t N = sizeof(temp);

    if (new_path[0] == '/') {
        // Absolute path — clamp to buffer
        snprintf(temp, N, "%.*s", (int)(N - 1), new_path);
    } else {
        // Relative path
        if (strcmp(path->path, "/") == 0) {
            // "/new"
            snprintf(temp, N, "/%.*s", (int)(N - 2), new_path);
        } else {
            size_t len_a = strnlen(path->path, N - 1);

            if (len_a >= N - 1) {
                // A is too long, safely truncate
                snprintf(temp, N, "%.*s", (int)(N - 1), path->path);
            } else {
                // Space left for "/" + part of new_path
                size_t avail_for_b = (N - 1) - len_a - 1; // -1 for '/', -1 for '\0'
                int cap_b = (avail_for_b > 0) ? (int)avail_for_b : 0;

                snprintf(temp, N, "%.*s/%.*s",
                         (int)len_a, path->path,
                         cap_b, new_path);
            }
        }
    }

    temp[N - 1] = '\0';
    ftp_path_normalize(temp);

    snprintf(path->path, sizeof(path->path), "%s", temp);
}

void ftp_path_up(ftp_path_t *path) {
    if (strcmp(path->path, "/") == 0) return;

    char *last_slash = strrchr(path->path, '/');
    if (last_slash == path->path) {
        strcpy(path->path, "/");
    } else if (last_slash) {
        *last_slash = '\0';
    }
}

void ftp_path_get_full(const ftp_path_t *path, const char *filename,
                       char *output, size_t output_len) {
    if (!filename || !*filename) {
        snprintf(output, output_len, "%s", path->path);
        output[output_len - 1] = '\0';
        return;
    }

    if (filename[0] == '/') {
        snprintf(output, output_len, "%.*s", (int)(output_len - 1), filename);
    } else {
        if (strcmp(path->path, "/") == 0) {
            snprintf(output, output_len, "/%s", filename);
        } else {
            snprintf(output, output_len, "%s/%s", path->path, filename);
        }
    }

    output[output_len - 1] = '\0';
    ftp_path_normalize(output);
}

void ftp_path_normalize(char *path) {
    if (!path || !*path) return;

    char *src = path;
    char *dst = path;
    bool last_was_slash = false;

    while (*src) {
        if (*src == '/') {
            if (!last_was_slash) {
                *dst++ = *src;
                last_was_slash = true;
            }
        } else {
            *dst++ = *src;
            last_was_slash = false;
        }
        src++;
    }
    *dst = '\0';

    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }

    if (path[0] != '/') {
        memmove(path + 1, path, strlen(path) + 1);
        path[0] = '/';
    }

    if (path[0] == '\0') {
        strcpy(path, "/");
    }
}

