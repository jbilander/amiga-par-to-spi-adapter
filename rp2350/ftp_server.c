/**
 * ftp_server.c - Simple FTP Server using raw lwIP API
 * 
 * This is a minimal FTP server for testing connectivity with FileZilla.
 * Implements basic FTP commands to allow connection and authentication.
 * 
 * Uses raw lwIP API (not BSD sockets) with cyw43_arch_lwip_begin/end protection.
 */

#include "main.h"
#include <string.h>
#include <stdio.h>
#include <pico/cyw43_arch.h>
#include <lwip/tcp.h>
#include <FreeRTOS.h>
#include <task.h>

// FTP Server Configuration
#define FTP_PORT        21
#define FTP_USER        "pico"
#define FTP_PASSWORD    "pico"
#define FTP_MAX_CLIENTS 2

// FTP Response Codes
#define FTP_RESP_220_WELCOME        "220 Pico FTP Server ready\r\n"
#define FTP_RESP_331_USER_OK        "331 User name okay, need password\r\n"
#define FTP_RESP_230_LOGIN_OK       "230 User logged in\r\n"
#define FTP_RESP_530_LOGIN_FAILED   "530 Login incorrect\r\n"
#define FTP_RESP_221_GOODBYE        "221 Goodbye\r\n"
#define FTP_RESP_214_HELP           "214 Help: USER PASS QUIT SYST PWD TYPE\r\n"
#define FTP_RESP_215_SYSTEM         "215 UNIX Type: L8\r\n"
#define FTP_RESP_257_PWD            "257 \"/\" is current directory\r\n"
#define FTP_RESP_200_TYPE_OK        "200 Type set to I\r\n"
#define FTP_RESP_500_UNKNOWN        "500 Unknown command\r\n"
#define FTP_RESP_502_NOT_IMPL       "502 Command not implemented\r\n"

// Client Connection State
typedef enum {
    FTP_STATE_IDLE = 0,
    FTP_STATE_USER_OK,
    FTP_STATE_LOGGED_IN
} ftp_state_t;

// Client Control Connection
typedef struct ftp_client {
    struct tcp_pcb *pcb;           // Control connection PCB
    ftp_state_t state;             // Authentication state
    char cmd_buffer[256];          // Command buffer
    uint16_t cmd_len;              // Current command length
    char username[32];             // Username provided by client
    bool active;                   // Connection active flag
} ftp_client_t;

// Global FTP Server State
static struct tcp_pcb *ftp_server_pcb = NULL;
static ftp_client_t ftp_clients[FTP_MAX_CLIENTS];

// Forward declarations
static err_t ftp_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t ftp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void ftp_error(void *arg, err_t err);
static void ftp_close_client(ftp_client_t *client);

/**
 * Send FTP response to client
 */
static err_t ftp_send_response(ftp_client_t *client, const char *response) {
    if (!client || !client->pcb || !response) {
        return ERR_ARG;
    }
    
    size_t len = strlen(response);
    err_t err;
    
    cyw43_arch_lwip_begin();
    err = tcp_write(client->pcb, response, len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        err = tcp_output(client->pcb);
    }
    cyw43_arch_lwip_end();
    
    if (err != ERR_OK) {
        printf("FTP: Failed to send response, err=%d\n", err);
    }
    
    return err;
}

/**
 * Process FTP command
 */
static void ftp_process_command(ftp_client_t *client, char *cmd) {
    // Trim trailing \r\n
    char *end = cmd + strlen(cmd) - 1;
    while (end > cmd && (*end == '\r' || *end == '\n' || *end == ' ')) {
        *end = '\0';
        end--;
    }
    
    printf("FTP: Received command: '%s'\n", cmd);
    
    // Parse command (first word)
    char *arg = strchr(cmd, ' ');
    if (arg) {
        *arg = '\0';
        arg++;
        // Trim leading spaces
        while (*arg == ' ') arg++;
    }
    
    // Convert command to uppercase
    for (char *p = cmd; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = *p - 'a' + 'A';
        }
    }
    
    // Handle commands based on current state
    if (strcmp(cmd, "USER") == 0) {
        if (arg) {
            strncpy(client->username, arg, sizeof(client->username) - 1);
            client->username[sizeof(client->username) - 1] = '\0';
            client->state = FTP_STATE_USER_OK;
            ftp_send_response(client, FTP_RESP_331_USER_OK);
            printf("FTP: User '%s' requested login\n", client->username);
        } else {
            ftp_send_response(client, FTP_RESP_500_UNKNOWN);
        }
    }
    else if (strcmp(cmd, "PASS") == 0) {
        if (client->state == FTP_STATE_USER_OK) {
            // Simple authentication check
            if (strcmp(client->username, FTP_USER) == 0 && 
                arg && strcmp(arg, FTP_PASSWORD) == 0) {
                client->state = FTP_STATE_LOGGED_IN;
                ftp_send_response(client, FTP_RESP_230_LOGIN_OK);
                printf("FTP: User '%s' logged in successfully\n", client->username);
            } else {
                client->state = FTP_STATE_IDLE;
                ftp_send_response(client, FTP_RESP_530_LOGIN_FAILED);
                printf("FTP: Login failed for user '%s'\n", client->username);
            }
        } else {
            ftp_send_response(client, FTP_RESP_500_UNKNOWN);
        }
    }
    else if (strcmp(cmd, "QUIT") == 0) {
        ftp_send_response(client, FTP_RESP_221_GOODBYE);
        printf("FTP: Client disconnecting\n");
        // Client will be closed after response is sent
        vTaskDelay(pdMS_TO_TICKS(100)); // Give time for response to send
        ftp_close_client(client);
    }
    else if (strcmp(cmd, "SYST") == 0) {
        ftp_send_response(client, FTP_RESP_215_SYSTEM);
    }
    else if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0) {
        ftp_send_response(client, FTP_RESP_257_PWD);
    }
    else if (strcmp(cmd, "TYPE") == 0) {
        // Accept any TYPE command (usually TYPE I for binary)
        ftp_send_response(client, FTP_RESP_200_TYPE_OK);
    }
    else if (strcmp(cmd, "HELP") == 0) {
        ftp_send_response(client, FTP_RESP_214_HELP);
    }
    else {
        // Unknown or not yet implemented command
        printf("FTP: Unknown/unimplemented command: %s\n", cmd);
        ftp_send_response(client, FTP_RESP_502_NOT_IMPL);
    }
}

/**
 * TCP receive callback - called when data is received from client
 */
static err_t ftp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    ftp_client_t *client = (ftp_client_t *)arg;
    
    if (!client) {
        // No client context, close connection
        if (p) pbuf_free(p);
        tcp_close(tpcb);
        return ERR_ARG;
    }
    
    // Connection closed by client
    if (p == NULL) {
        printf("FTP: Connection closed by client\n");
        ftp_close_client(client);
        return ERR_OK;
    }
    
    // Process received data
    if (p->tot_len > 0) {
        // Copy data to command buffer
        uint16_t available = sizeof(client->cmd_buffer) - client->cmd_len - 1;
        uint16_t to_copy = (p->tot_len < available) ? p->tot_len : available;
        
        pbuf_copy_partial(p, client->cmd_buffer + client->cmd_len, to_copy, 0);
        client->cmd_len += to_copy;
        client->cmd_buffer[client->cmd_len] = '\0';
        
        // Tell TCP we processed the data
        cyw43_arch_lwip_begin();
        tcp_recved(tpcb, p->tot_len);
        cyw43_arch_lwip_end();
        
        // Process complete commands (ending with \n)
        char *line_start = client->cmd_buffer;
        char *line_end;
        
        while ((line_end = strchr(line_start, '\n')) != NULL) {
            // Found complete command
            *line_end = '\0';
            
            if (strlen(line_start) > 0) {
                ftp_process_command(client, line_start);
            }
            
            // Move to next command
            line_start = line_end + 1;
        }
        
        // Move remaining incomplete command to start of buffer
        if (line_start > client->cmd_buffer) {
            size_t remaining = strlen(line_start);
            memmove(client->cmd_buffer, line_start, remaining + 1);
            client->cmd_len = remaining;
        }
    }
    
    pbuf_free(p);
    return ERR_OK;
}

/**
 * TCP error callback
 */
static void ftp_error(void *arg, err_t err) {
    ftp_client_t *client = (ftp_client_t *)arg;
    printf("FTP: TCP error %d\n", err);
    
    if (client) {
        client->pcb = NULL; // PCB already freed by lwIP
        client->active = false;
    }
}

/**
 * Close client connection
 */
static void ftp_close_client(ftp_client_t *client) {
    if (!client || !client->active) {
        return;
    }
    
    printf("FTP: Closing client connection\n");
    
    if (client->pcb) {
        cyw43_arch_lwip_begin();
        tcp_arg(client->pcb, NULL);
        tcp_recv(client->pcb, NULL);
        tcp_err(client->pcb, NULL);
        tcp_close(client->pcb);
        cyw43_arch_lwip_end();
        client->pcb = NULL;
    }
    
    // Reset client state
    client->state = FTP_STATE_IDLE;
    client->cmd_len = 0;
    client->cmd_buffer[0] = '\0';
    client->username[0] = '\0';
    client->active = false;
}

/**
 * TCP accept callback - called when new client connects
 */
static err_t ftp_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    LWIP_UNUSED_ARG(arg);
    
    if (err != ERR_OK || newpcb == NULL) {
        printf("FTP: Accept error\n");
        return ERR_VAL;
    }
    
    // Find free client slot
    ftp_client_t *client = NULL;
    for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
        if (!ftp_clients[i].active) {
            client = &ftp_clients[i];
            break;
        }
    }
    
    if (!client) {
        printf("FTP: Max clients reached, rejecting connection\n");
        tcp_close(newpcb);
        return ERR_MEM;
    }
    
    // Initialize client
    memset(client, 0, sizeof(ftp_client_t));
    client->pcb = newpcb;
    client->state = FTP_STATE_IDLE;
    client->active = true;
    
    printf("FTP: Client connected from %s\n", ipaddr_ntoa(&newpcb->remote_ip));
    
    // Set up callbacks
    tcp_arg(newpcb, client);
    tcp_recv(newpcb, ftp_recv);
    tcp_err(newpcb, ftp_error);
    
    // Send welcome message
    ftp_send_response(client, FTP_RESP_220_WELCOME);
    
    return ERR_OK;
}

/**
 * Initialize and start FTP server
 */
bool ftp_server_init(void) {
    printf("FTP: Initializing server on port %d\n", FTP_PORT);
    
    // Initialize client array
    memset(ftp_clients, 0, sizeof(ftp_clients));
    
    // Create new TCP PCB
    cyw43_arch_lwip_begin();
    ftp_server_pcb = tcp_new();
    cyw43_arch_lwip_end();
    
    if (!ftp_server_pcb) {
        printf("FTP: Failed to create PCB\n");
        return false;
    }
    
    // Bind to FTP port
    err_t err;
    cyw43_arch_lwip_begin();
    err = tcp_bind(ftp_server_pcb, IP_ADDR_ANY, FTP_PORT);
    cyw43_arch_lwip_end();
    
    if (err != ERR_OK) {
        printf("FTP: Bind failed, err=%d\n", err);
        cyw43_arch_lwip_begin();
        tcp_close(ftp_server_pcb);
        cyw43_arch_lwip_end();
        ftp_server_pcb = NULL;
        return false;
    }
    
    // Start listening
    cyw43_arch_lwip_begin();
    ftp_server_pcb = tcp_listen(ftp_server_pcb);
    cyw43_arch_lwip_end();
    
    if (!ftp_server_pcb) {
        printf("FTP: Listen failed\n");
        return false;
    }
    
    // Set accept callback
    cyw43_arch_lwip_begin();
    tcp_accept(ftp_server_pcb, ftp_accept);
    cyw43_arch_lwip_end();
    
    printf("FTP: Server started successfully\n");
    printf("FTP: Username: %s\n", FTP_USER);
    printf("FTP: Password: %s\n", FTP_PASSWORD);
    
    return true;
}

/**
 * FTP Server main loop (called from ftp_server_application_task)
 */
void ftp_server_process(void) {
    // In raw lwIP, all processing is done via callbacks
    // This function is just a placeholder for future expansion
    // (e.g., periodic cleanup, statistics, etc.)
    
    // For now, just yield to allow callbacks to run
    vTaskDelay(pdMS_TO_TICKS(100));
}

/**
 * Shutdown FTP server
 */
void ftp_server_shutdown(void) {
    printf("FTP: Shutting down server\n");
    
    // Close all client connections
    for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
        if (ftp_clients[i].active) {
            ftp_close_client(&ftp_clients[i]);
        }
    }
    
    // Close server PCB
    if (ftp_server_pcb) {
        cyw43_arch_lwip_begin();
        tcp_close(ftp_server_pcb);
        cyw43_arch_lwip_end();
        ftp_server_pcb = NULL;
    }
    
    printf("FTP: Server stopped\n");
}