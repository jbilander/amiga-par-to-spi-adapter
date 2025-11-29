/* ftp_server.c - FTP server with FatFS (runs on Core 1 in WiFi mode) */

#include "ftp_server.h"
#include "ftp_utils.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"

// lwIP includes
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

// Global filesystem pointer
static FATFS *g_fs = NULL;

// ============================================================================
// Helper Functions
// ============================================================================

static void init_session(ftp_session_t *session) {
    memset(session, 0, sizeof(ftp_session_t));
    session->ctrl_sock = -1;
    session->data_sock = -1;
    session->data_listen_sock = -1;
    session->state = FTP_STATE_IDLE;
    session->transfer_type = FTP_TYPE_BINARY;
    session->data_mode = FTP_DATA_MODE_NONE;
    session->transfer_state = FTP_TRANSFER_NONE;
    ftp_path_init(&session->cwd);
    session->rename_from_set = false;
}

static bool authenticate_user(const ftp_server_t *server, const char *username, const char *password) {
    for (int i = 0; i < server->user_count; i++) {
        if (strcmp(server->users[i].username, username) == 0 &&
            strcmp(server->users[i].password, password) == 0) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Public API Implementation
// ============================================================================

bool ftp_server_init(ftp_server_t *server, FATFS *fs) {
    if (!server || !fs) {
        return false;
    }

    memset(server, 0, sizeof(ftp_server_t));
    server->listen_sock = -1;
    server->user_count = 0;
    server->next_pasv_port = FTP_DATA_PORT_MIN;
    init_session(&server->session);

    g_fs = fs;
    return true;
}

bool ftp_server_add_user(ftp_server_t *server, const char *username, const char *password) {
    if (!server || !username || !password) {
        return false;
    }

    if (server->user_count >= FTP_MAX_USERS) {
        return false;
    }

    strncpy(server->users[server->user_count].username, username, FTP_USERNAME_MAX - 1);
    server->users[server->user_count].username[FTP_USERNAME_MAX - 1] = '\0';

    strncpy(server->users[server->user_count].password, password, FTP_PASSWORD_MAX - 1);
    server->users[server->user_count].password[FTP_PASSWORD_MAX - 1] = '\0';

    server->user_count++;
    return true;
}

bool ftp_server_begin(ftp_server_t *server) {
    if (!server) {
        return false;
    }

    // Create listening socket
    server->listen_sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_sock < 0) {
        printf("FTP: Failed to create socket\n");
        return false;
    }

    int opt = 1;
    lwip_setsockopt(server->listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = lwip_fcntl(server->listen_sock, F_GETFL, 0);
    lwip_fcntl(server->listen_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FTP_CTRL_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (lwip_bind(server->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("FTP: Failed to bind to port %d\n", FTP_CTRL_PORT);
        lwip_close(server->listen_sock);
        server->listen_sock = -1;
        return false;
    }

    if (lwip_listen(server->listen_sock, 1) < 0) {
        printf("FTP: Failed to listen\n");
        lwip_close(server->listen_sock);
        server->listen_sock = -1;
        return false;
    }

    printf("FTP: Server listening on port %d\n", FTP_CTRL_PORT);
    return true;
}

void ftp_send_response(ftp_session_t *session, int code, const char *message) {
    if (session->ctrl_sock < 0) {
        return;
    }

    char response[FTP_RESPONSE_MAX];

    // Limit %s safely
    int max_msg = sizeof(response) - 8; // space for "XYZ " + CRLF
    snprintf(response, sizeof(response),
             "%d %.*s\r\n",
             code,
             max_msg,
             message);

    lwip_send(session->ctrl_sock, response, strlen(response), 0);

    printf("FTP: >> %d %s\n", code, message);
}

void ftp_close_session(ftp_session_t *session) {
    if (session->ctrl_sock >= 0) {
        lwip_close(session->ctrl_sock);
        printf("FTP: Client disconnected\n");
    }
    if (session->data_sock >= 0) {
        lwip_close(session->data_sock);
    }
    if (session->data_listen_sock >= 0) {
        lwip_close(session->data_listen_sock);
    }
    init_session(session);
}

void ftp_process_command(ftp_server_t *server, ftp_session_t *session) {
    char *tokens[16];
    char cmd_copy[FTP_CMD_BUFFER_SIZE];

    strncpy(cmd_copy, session->cmd_buffer, FTP_CMD_BUFFER_SIZE - 1);
    cmd_copy[FTP_CMD_BUFFER_SIZE - 1] = '\0';

    int token_count = ftp_split_string(cmd_copy, ' ', tokens, 16);
    if (token_count == 0) {
        return;
    }

    ftp_command_t cmd = ftp_parse_command(tokens[0]);

    printf("FTP: << %s\n", session->cmd_buffer);

    switch (session->state) {
        case FTP_STATE_IDLE:
            if (cmd == FTP_CMD_USER) {
                if (token_count >= 2) {
                    strncpy(session->username, tokens[1], FTP_USERNAME_MAX - 1);
                    session->username[FTP_USERNAME_MAX - 1] = '\0';
                    session->state = FTP_STATE_USER;
                    ftp_send_response(session, 331, "OK. Password required.");
                } else {
                    ftp_send_response(session, 501, "Syntax error in parameters.");
                }
            } else {
                ftp_send_response(session, 530, "Please login with USER and PASS.");
            }
            break;

        case FTP_STATE_USER:
            if (cmd == FTP_CMD_PASS) {
                const char *password = (token_count >= 2) ? tokens[1] : "";
                if (authenticate_user(server, session->username, password)) {
                    session->state = FTP_STATE_AUTHENTICATED;
                    ftp_send_response(session, 230, "OK.");
                    printf("FTP: User '%s' logged in\n", session->username);
                } else {
                    ftp_send_response(session, 430, "Invalid username or password.");
                    session->state = FTP_STATE_IDLE;
                    memset(session->username, 0, sizeof(session->username));
                }
            } else {
                ftp_send_response(session, 503, "Login with USER first.");
            }
            break;

        case FTP_STATE_AUTHENTICATED:
            switch (cmd) {
                case FTP_CMD_SYST:
                    ftp_send_response(session, 215, "UNIX Type: L8");
                    break;

                case FTP_CMD_NOOP:
                    ftp_send_response(session, 200, "NOOP command successful.");
                    break;

                case FTP_CMD_FEAT:
                    lwip_send(session->ctrl_sock, "211-Features:\r\n", 15, 0);
                    lwip_send(session->ctrl_sock, " MLSD\r\n", 7, 0);
                    lwip_send(session->ctrl_sock, " MFMT\r\n", 7, 0);
                    lwip_send(session->ctrl_sock, " MFCT\r\n", 7, 0);
                    lwip_send(session->ctrl_sock, " PASV\r\n", 7, 0);
                    ftp_send_response(session, 211, "End");
                    break;

                case FTP_CMD_QUIT:
                    ftp_send_response(session, 221, "Goodbye.");
                    ftp_close_session(session);
                    break;

                case FTP_CMD_PWD: {
                        char msg[FTP_RESPONSE_MAX];
                        int space = sizeof(msg) - 25; // room for quotes + suffix
                        snprintf(msg, sizeof(msg),
                                 "\"%.*s\" is current directory.",
                                 space,
                                 session->cwd.path);
                        ftp_send_response(session, 257, msg);
                    }
                    break;

                case FTP_CMD_TYPE:
                    if (token_count >= 2) {
                        if (strcmp(tokens[1], "A") == 0) {
                            session->transfer_type = FTP_TYPE_ASCII;
                            ftp_send_response(session, 200, "TYPE is now ASCII");
                        } else if (strcmp(tokens[1], "I") == 0) {
                            session->transfer_type = FTP_TYPE_BINARY;
                            ftp_send_response(session, 200, "TYPE is now 8-bit binary");
                        } else {
                            ftp_send_response(session, 504, "Unknown TYPE");
                        }
                    } else {
                        ftp_send_response(session, 501, "Syntax error in parameters.");
                    }
                    break;

                default:
                    ftp_send_response(session, 502, "Command not implemented (yet).");
                    break;
            }
            break;
    }
}

void ftp_server_handle(ftp_server_t *server) {
    if (!server || server->listen_sock < 0) {
        return;
    }

    ftp_session_t *session = &server->session;

    if (session->ctrl_sock < 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int new_sock = lwip_accept(server->listen_sock,
                                   (struct sockaddr *)&client_addr,
                                   &addr_len);
        if (new_sock >= 0) {
            session->ctrl_sock = new_sock;
            session->client_addr.addr = client_addr.sin_addr.s_addr;
            session->client_port = ntohs(client_addr.sin_port);
            session->last_activity_ms = to_ms_since_boot(get_absolute_time());

            char ip_str[16];
            inet_ntoa_r(client_addr.sin_addr, ip_str, sizeof(ip_str));
            printf("FTP: New connection from %s:%d\n", ip_str, session->client_port);

            ftp_send_response(session, 220, "--- Welcome to Pico 2 W FTP Server ---");
        }
        return;
    }

    char buffer[256];
    int bytes = lwip_recv(session->ctrl_sock, buffer,
                          sizeof(buffer) - 1, MSG_DONTWAIT);

    if (bytes > 0) {
        session->last_activity_ms = to_ms_since_boot(get_absolute_time());

        for (int i = 0; i < bytes; i++) {
            char c = buffer[i];

            if (c == '\n') {
                session->cmd_buffer[session->cmd_len] = '\0';
                ftp_trim(session->cmd_buffer);

                if (session->cmd_len > 0) {
                    ftp_process_command(server, session);
                }
                session->cmd_len = 0;
            } else if (c >= 32 && c < 127) {
                if (session->cmd_len < FTP_CMD_BUFFER_SIZE - 1) {
                    session->cmd_buffer[session->cmd_len++] = c;
                }
            }
        }
    } else if (bytes == 0) {
        ftp_close_session(session);
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (session->ctrl_sock >= 0 &&
        (now - session->last_activity_ms) > FTP_TIMEOUT_MS) {
        printf("FTP: Client timeout\n");
        ftp_send_response(session, 421, "Timeout.");
        ftp_close_session(session);
    }
}

void ftp_server_stop(ftp_server_t *server) {
    if (!server) {
        return;
    }

    ftp_close_session(&server->session);

    if (server->listen_sock >= 0) {
        lwip_close(server->listen_sock);
        server->listen_sock = -1;
    }

    printf("FTP: Server stopped\n");
}

bool ftp_server_has_client(const ftp_server_t *server) {
    return server && server->session.ctrl_sock >= 0;
}

