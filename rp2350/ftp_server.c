/**
 * ftp_server.c - Enhanced FTP Server using raw lwIP API
 * 
 * Adds PASV (passive mode) and LIST (directory listing) commands.
 * Integrates with FatFS for SD card access.
 * 
 * Uses raw lwIP API (not BSD sockets) with cyw43_arch_lwip_begin/end protection.
 */

#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pico/cyw43_arch.h>
#include <lwip/tcp.h>
#include <lwip/ip_addr.h>
#include <FreeRTOS.h>
#include <task.h>
#include "ff.h"  // FatFS

// FTP Server Configuration
#define FTP_PORT        21
#define FTP_DATA_PORT_MIN  50000
#define FTP_DATA_PORT_MAX  50010
#define FTP_USER        "pico"
#define FTP_PASSWORD    "pico"
#define FTP_MAX_CLIENTS 2

// FTP Response Codes
#define FTP_RESP_150_OPENING_DATA   "150 Opening data connection\r\n"
#define FTP_RESP_200_TYPE_OK        "200 Type set to I\r\n"
#define FTP_RESP_214_HELP           "214 Help: USER PASS QUIT SYST PWD TYPE PASV LIST NLST CWD CDUP\r\n"
#define FTP_RESP_215_SYSTEM         "215 UNIX Type: L8\r\n"
#define FTP_RESP_220_WELCOME        "220 Pico FTP Server ready\r\n"
#define FTP_RESP_221_GOODBYE        "221 Goodbye\r\n"
#define FTP_RESP_226_TRANSFER_OK    "226 Transfer complete\r\n"
#define FTP_RESP_230_LOGIN_OK       "230 User logged in\r\n"
#define FTP_RESP_250_FILE_OK        "250 File action okay\r\n"
#define FTP_RESP_257_PWD            "257 \"%s\" is current directory\r\n"
#define FTP_RESP_331_USER_OK        "331 User name okay, need password\r\n"
#define FTP_RESP_500_UNKNOWN        "500 Unknown command\r\n"
#define FTP_RESP_502_NOT_IMPL       "502 Command not implemented\r\n"
#define FTP_RESP_530_LOGIN_FAILED   "530 Login incorrect\r\n"
#define FTP_RESP_550_FILE_ERROR     "550 File/directory error\r\n"

// Client Connection State
typedef enum {
    FTP_STATE_IDLE = 0,
    FTP_STATE_USER_OK,
    FTP_STATE_LOGGED_IN
} ftp_state_t;

// Data Connection State
typedef struct ftp_data_conn {
    struct tcp_pcb *listen_pcb;    // Listening for PASV connection
    struct tcp_pcb *pcb;           // Active data connection
    uint16_t port;                 // Port for PASV
    volatile bool waiting_for_connection;   // True when waiting for client to connect
    volatile bool connected;                // True when data connection established
    volatile bool transfer_complete;        // True when data transfer is done and connection should close
} ftp_data_conn_t;

// Client Control Connection
typedef struct ftp_client {
    struct tcp_pcb *pcb;           // Control connection PCB
    ftp_state_t state;             // Authentication state
    char cmd_buffer[256];          // Command buffer
    uint16_t cmd_len;              // Current command length
    char username[32];             // Username provided by client
    char cwd[256];                 // Current working directory
    ftp_data_conn_t data_conn;     // Data connection info
    bool active;                   // Connection active flag
    bool pending_list;             // True if LIST is pending data connection
} ftp_client_t;

// Global FTP Server State
static struct tcp_pcb *ftp_server_pcb = NULL;
static ftp_client_t ftp_clients[FTP_MAX_CLIENTS];
static uint16_t next_data_port = FTP_DATA_PORT_MIN;
static FATFS *g_fs = NULL;  // FatFS filesystem

// Forward declarations
static err_t ftp_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t ftp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void ftp_error(void *arg, err_t err);
static void ftp_close_client(ftp_client_t *client);
static void ftp_close_data_connection(ftp_client_t *client);
static void ftp_send_list(ftp_client_t *client);  // Forward declaration for LIST handler

// Data connection forward declarations
static err_t ftp_data_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t ftp_data_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void ftp_data_error(void *arg, err_t err);

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
 * Send formatted response
 */
static void ftp_send_response_fmt(ftp_client_t *client, const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    ftp_send_response(client, buffer);
}

/**
 * Get next available PASV port
 */
static uint16_t ftp_get_next_data_port(void) {
    uint16_t port = next_data_port++;
    if (next_data_port > FTP_DATA_PORT_MAX) {
        next_data_port = FTP_DATA_PORT_MIN;
    }
    return port;
}

/**
 * Close data connection
 */
static void ftp_close_data_connection(ftp_client_t *client) {
    if (!client) return;
    
    cyw43_arch_lwip_begin();
    
    if (client->data_conn.pcb) {
        tcp_arg(client->data_conn.pcb, NULL);
        tcp_sent(client->data_conn.pcb, NULL);
        tcp_err(client->data_conn.pcb, NULL);
        tcp_close(client->data_conn.pcb);
        client->data_conn.pcb = NULL;
    }
    
    if (client->data_conn.listen_pcb) {
        tcp_arg(client->data_conn.listen_pcb, NULL);
        tcp_accept(client->data_conn.listen_pcb, NULL);
        tcp_close(client->data_conn.listen_pcb);
        client->data_conn.listen_pcb = NULL;
    }
    
    cyw43_arch_lwip_end();
    
    client->data_conn.waiting_for_connection = false;
    client->data_conn.connected = false;
    client->data_conn.port = 0;
}

/**
 * Data connection sent callback - called when data is ACKed
 */
static err_t ftp_data_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    ftp_client_t *client = (ftp_client_t *)arg;
    
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);
    
    if (!client) {
        return ERR_OK;
    }
    
    // Check if all data has been sent (send buffer empty)
    if (client->data_conn.pcb && tcp_sndbuf(client->data_conn.pcb) == TCP_SND_BUF) {
        printf("FTP Data: All data transmitted, closing connection\n");
        
        // All data sent and ACKed - close data connection
        ftp_close_data_connection(client);
        
        // Send completion message on control connection
        ftp_send_response(client, FTP_RESP_226_TRANSFER_OK);
    }
    
    return ERR_OK;
}

/**
 * Data connection error callback
 */
static void ftp_data_error(void *arg, err_t err) {
    ftp_client_t *client = (ftp_client_t *)arg;
    printf("FTP Data: Connection error %d\n", err);
    
    if (client) {
        client->data_conn.pcb = NULL;  // PCB already freed
        client->data_conn.connected = false;
    }
}

/**
 * Data connection accept callback
 */
static err_t ftp_data_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    ftp_client_t *client = (ftp_client_t *)arg;
    
    if (err != ERR_OK || !newpcb || !client) {
        printf("FTP Data: Accept error (err=%d, newpcb=%p, client=%p)\n", err, newpcb, client);
        return ERR_VAL;
    }
    
    printf("FTP Data: Client connected from %s\n", ipaddr_ntoa(&newpcb->remote_ip));
    
    // Set up data connection
    client->data_conn.pcb = newpcb;
    client->data_conn.connected = true;
    client->data_conn.waiting_for_connection = false;
    
    tcp_arg(newpcb, client);
    tcp_sent(newpcb, ftp_data_sent);
    tcp_err(newpcb, ftp_data_error);
    
    // Close the listening PCB - we only accept one data connection
    if (client->data_conn.listen_pcb) {
        printf("FTP Data: Closing listen socket\n");
        cyw43_arch_lwip_begin();
        tcp_close(client->data_conn.listen_pcb);
        cyw43_arch_lwip_end();
        client->data_conn.listen_pcb = NULL;
    }
    
    // Handle pending LIST operation
    if (client->pending_list) {
        printf("FTP Data: Pending LIST detected, sending directory listing\n");
        fflush(stdout);
        client->pending_list = false;
        ftp_send_list(client);
    }
    
    return ERR_OK;
}

/**
 * Handle PASV command - enter passive mode
 */
static void ftp_cmd_pasv(ftp_client_t *client) {
    printf("FTP: PASV command received\n");
    
    // Close any existing data connection
    ftp_close_data_connection(client);
    
    // Get next available port
    uint16_t port = ftp_get_next_data_port();
    printf("FTP: PASV - attempting to bind port %d\n", port);
    
    // Create new listening PCB for data connection
    cyw43_arch_lwip_begin();
    struct tcp_pcb *listen_pcb = tcp_new();
    cyw43_arch_lwip_end();
    
    if (!listen_pcb) {
        printf("FTP: PASV - failed to create data PCB\n");
        ftp_send_response(client, "425 Can't open data connection\r\n");
        return;
    }
    
    // Bind to data port
    err_t err;
    cyw43_arch_lwip_begin();
    err = tcp_bind(listen_pcb, IP_ADDR_ANY, port);
    cyw43_arch_lwip_end();
    
    if (err != ERR_OK) {
        printf("FTP: PASV - failed to bind data port %d, err=%d\n", port, err);
        cyw43_arch_lwip_begin();
        tcp_close(listen_pcb);
        cyw43_arch_lwip_end();
        ftp_send_response(client, "425 Can't open data connection\r\n");
        return;
    }
    
    printf("FTP: PASV - bound to port %d successfully\n", port);
    
    // Start listening with explicit backlog
    cyw43_arch_lwip_begin();
    struct tcp_pcb *new_listen_pcb = tcp_listen_with_backlog(listen_pcb, 1);
    cyw43_arch_lwip_end();
    
    if (!new_listen_pcb) {
        printf("FTP: PASV - failed to listen on data port\n");
        cyw43_arch_lwip_begin();
        tcp_close(listen_pcb);
        cyw43_arch_lwip_end();
        ftp_send_response(client, "425 Can't open data connection\r\n");
        return;
    }
    
    listen_pcb = new_listen_pcb;
    printf("FTP: PASV - listening on port %d with backlog=1\n", port);
    
    // Set accept callback
    cyw43_arch_lwip_begin();
    tcp_accept(listen_pcb, ftp_data_accept);
    tcp_arg(listen_pcb, client);
    cyw43_arch_lwip_end();
    
    client->data_conn.listen_pcb = listen_pcb;
    client->data_conn.port = port;
    client->data_conn.waiting_for_connection = true;
    
    // Get our IP address
    ip_addr_t our_ip;
    cyw43_arch_lwip_begin();
    our_ip = client->pcb->local_ip;
    cyw43_arch_lwip_end();
    
    // Format PASV response: 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)
    uint8_t *ip = (uint8_t *)&our_ip.addr;
    uint8_t p1 = port >> 8;
    uint8_t p2 = port & 0xFF;
    
    char response[128];
    snprintf(response, sizeof(response),
             "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\r\n",
             ip[0], ip[1], ip[2], ip[3], p1, p2);
    
    printf("FTP: PASV - sending response: %s", response);
    ftp_send_response(client, response);
    printf("FTP: PASV - waiting for client to connect on port %d\n", port);
}

/**
 * Send directory listing over data connection
 */
static void ftp_send_list(ftp_client_t *client) {
    if (!client->data_conn.connected) {
        ftp_send_response(client, "425 Data connection not established\r\n");
        return;
    }
    
    // Notify client we're starting transfer
    ftp_send_response(client, FTP_RESP_150_OPENING_DATA);
    
    // Open directory
    DIR dir;
    FRESULT res = f_opendir(&dir, client->cwd);
    
    if (res != FR_OK) {
        printf("FTP: Failed to open directory '%s', err=%d\n", client->cwd, res);
        ftp_send_response(client, FTP_RESP_550_FILE_ERROR);
        ftp_close_data_connection(client);
        return;
    }
    
    // Send directory entries
    FILINFO fno;
    char line[512];
    int total_sent = 0;
    
    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break;  // End of directory or error
        }
        
        // Format as Unix-style listing
        // Format: drwxr-xr-x 1 owner group size month day time filename
        char perms[11] = "-rw-r--r--";
        if (fno.fattrib & AM_DIR) {
            perms[0] = 'd';
        }
        
        // Format date/time
        int month = (fno.fdate >> 5) & 0x0F;
        int day = fno.fdate & 0x1F;
        int hour = (fno.ftime >> 11) & 0x1F;
        int minute = (fno.ftime >> 5) & 0x3F;
        
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const char *month_str = (month >= 1 && month <= 12) ? months[month - 1] : "???";
        
        // Format line
        int len = snprintf(line, sizeof(line),
                          "%s   1 owner group %8lu %s %2d %02d:%02d %s\r\n",
                          perms, (unsigned long)fno.fsize,
                          month_str, day, hour, minute, fno.fname);
        
        if (len > 0 && len < (int)sizeof(line)) {
            // Send line over data connection
            err_t err;
            cyw43_arch_lwip_begin();
            err = tcp_write(client->data_conn.pcb, line, len, TCP_WRITE_FLAG_COPY);
            cyw43_arch_lwip_end();
            
            if (err == ERR_OK) {
                total_sent += len;
            } else if (err == ERR_MEM) {
                // Buffer full - flush and stop for now
                // lwIP will send what's queued
                printf("FTP: Send buffer full at %d bytes, flushing\n", total_sent);
                break;
            } else {
                printf("FTP: Data write error %d\n", err);
                break;
            }
        }
    }
    
    f_closedir(&dir);
    
    printf("FTP: LIST queued %d bytes for sending\n", total_sent);
    
    // Flush all queued data
    cyw43_arch_lwip_begin();
    tcp_output(client->data_conn.pcb);
    cyw43_arch_lwip_end();
    
    // Don't close immediately - let lwIP transmit the data
    // The tcp_sent callback will close the connection and send 226 when all data is ACKed
    printf("FTP: Data queued, waiting for transmission to complete\n");
}

/**
 * Handle LIST command - NON-BLOCKING version
 */
static void ftp_cmd_list(ftp_client_t *client, const char *arg) {
    LWIP_UNUSED_ARG(arg);  // For now, ignore argument (always list CWD)
    
    printf("FTP: LIST command received\n");
    printf("FTP: LIST - waiting_for_connection=%d, connected=%d\n", 
           client->data_conn.waiting_for_connection, 
           client->data_conn.connected);
    fflush(stdout);
    
    if (!client->data_conn.waiting_for_connection && !client->data_conn.connected) {
        printf("FTP: LIST - ERROR: No PASV mode active\n");
        fflush(stdout);
        ftp_send_response(client, "425 Use PASV first\r\n");
        return;
    }
    
    // If already connected, send immediately
    if (client->data_conn.connected) {
        printf("FTP: LIST - Data connection already established, sending listing\n");
        fflush(stdout);
        ftp_send_list(client);
        return;
    }
    
    // If waiting for connection, mark LIST as pending and return
    // The listing will be sent when data connection is accepted
    printf("FTP: LIST - Data connection pending, marking LIST for later\n");
    fflush(stdout);
    
    client->pending_list = true;
    
    printf("FTP: LIST - Returning from handler, will send listing when connection arrives\n");
    fflush(stdout);
    
    // DON'T WAIT - just return! The accept callback will send the listing
    // Note: 150 response will be sent by ftp_send_list when connection is ready
}

/**
 * Handle CWD command - change working directory
 */
static void ftp_cmd_cwd(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error\r\n");
        return;
    }
    
    char new_path[256];
    
    // Handle absolute vs relative paths
    if (arg[0] == '/') {
        // Absolute path
        strncpy(new_path, arg, sizeof(new_path) - 1);
        new_path[sizeof(new_path) - 1] = '\0';
    } else {
        // Relative path - be careful about buffer size
        size_t cwd_len = strlen(client->cwd);
        size_t arg_len = strlen(arg);
        
        // Check if concatenation would overflow
        if (cwd_len + arg_len + 2 > sizeof(new_path)) {
            ftp_send_response(client, "550 Path too long\r\n");
            return;
        }
        
        snprintf(new_path, sizeof(new_path), "%s/%s", client->cwd, arg);
    }
    
    // Verify directory exists
    DIR dir;
    FRESULT res = f_opendir(&dir, new_path);
    
    if (res == FR_OK) {
        f_closedir(&dir);
        strncpy(client->cwd, new_path, sizeof(client->cwd) - 1);
        client->cwd[sizeof(client->cwd) - 1] = '\0';
        ftp_send_response(client, FTP_RESP_250_FILE_OK);
        printf("FTP: CWD changed to '%s'\n", client->cwd);
    } else {
        printf("FTP: CWD failed for '%s', err=%d\n", new_path, res);
        ftp_send_response(client, FTP_RESP_550_FILE_ERROR);
    }
}

/**
 * Handle CDUP command - change to parent directory
 */
static void ftp_cmd_cdup(ftp_client_t *client) {
    // Find last '/' in path
    char *last_slash = strrchr(client->cwd, '/');
    
    if (last_slash && last_slash != client->cwd) {
        *last_slash = '\0';  // Truncate at last slash
        ftp_send_response(client, FTP_RESP_250_FILE_OK);
        printf("FTP: CDUP changed to '%s'\n", client->cwd);
    } else {
        // Already at root
        strcpy(client->cwd, "/");
        ftp_send_response(client, FTP_RESP_250_FILE_OK);
    }
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
                strcpy(client->cwd, "/");  // Start at root
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
    else if (client->state != FTP_STATE_LOGGED_IN) {
        ftp_send_response(client, "530 Please login first\r\n");
        return;
    }
    // Commands below require authentication
    else if (strcmp(cmd, "QUIT") == 0) {
        ftp_send_response(client, FTP_RESP_221_GOODBYE);
        printf("FTP: Client disconnecting\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        ftp_close_client(client);
    }
    else if (strcmp(cmd, "SYST") == 0) {
        ftp_send_response(client, FTP_RESP_215_SYSTEM);
    }
    else if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0) {
        ftp_send_response_fmt(client, FTP_RESP_257_PWD, client->cwd);
    }
    else if (strcmp(cmd, "TYPE") == 0) {
        ftp_send_response(client, FTP_RESP_200_TYPE_OK);
    }
    else if (strcmp(cmd, "PASV") == 0) {
        ftp_cmd_pasv(client);
    }
    else if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0) {
        ftp_cmd_list(client, arg);
    }
    else if (strcmp(cmd, "CWD") == 0) {
        ftp_cmd_cwd(client, arg);
    }
    else if (strcmp(cmd, "CDUP") == 0) {
        ftp_cmd_cdup(client);
    }
    else if (strcmp(cmd, "HELP") == 0) {
        ftp_send_response(client, FTP_RESP_214_HELP);
    }
    else {
        printf("FTP: Unknown/unimplemented command: %s\n", cmd);
        ftp_send_response(client, FTP_RESP_502_NOT_IMPL);
    }
}

/**
 * TCP receive callback
 */
static err_t ftp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    ftp_client_t *client = (ftp_client_t *)arg;
    
    if (!client) {
        if (p) pbuf_free(p);
        tcp_close(tpcb);
        return ERR_ARG;
    }
    
    if (p == NULL) {
        printf("FTP: Connection closed by client\n");
        ftp_close_client(client);
        return ERR_OK;
    }
    
    if (p->tot_len > 0) {
        uint16_t available = sizeof(client->cmd_buffer) - client->cmd_len - 1;
        uint16_t to_copy = (p->tot_len < available) ? p->tot_len : available;
        
        pbuf_copy_partial(p, client->cmd_buffer + client->cmd_len, to_copy, 0);
        client->cmd_len += to_copy;
        client->cmd_buffer[client->cmd_len] = '\0';
        
        cyw43_arch_lwip_begin();
        tcp_recved(tpcb, p->tot_len);
        cyw43_arch_lwip_end();
        
        char *line_start = client->cmd_buffer;
        char *line_end;
        
        while ((line_end = strchr(line_start, '\n')) != NULL) {
            *line_end = '\0';
            
            if (strlen(line_start) > 0) {
                ftp_process_command(client, line_start);
            }
            
            line_start = line_end + 1;
        }
        
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
        client->pcb = NULL;
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
    
    // Close data connection first
    ftp_close_data_connection(client);
    
    if (client->pcb) {
        cyw43_arch_lwip_begin();
        tcp_arg(client->pcb, NULL);
        tcp_recv(client->pcb, NULL);
        tcp_err(client->pcb, NULL);
        tcp_close(client->pcb);
        cyw43_arch_lwip_end();
        client->pcb = NULL;
    }
    
    client->state = FTP_STATE_IDLE;
    client->cmd_len = 0;
    client->cmd_buffer[0] = '\0';
    client->username[0] = '\0';
    client->cwd[0] = '\0';
    client->active = false;
}

/**
 * TCP accept callback
 */
static err_t ftp_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    LWIP_UNUSED_ARG(arg);
    
    if (err != ERR_OK || newpcb == NULL) {
        printf("FTP: Accept error\n");
        return ERR_VAL;
    }
    
    ftp_client_t *client = NULL;
    for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
        if (!ftp_clients[i].active) {
            client = &ftp_clients[i];
            break;
        }
    }
    
    if (!client) {
        printf("FTP: Max clients reached\n");
        tcp_close(newpcb);
        return ERR_MEM;
    }
    
    memset(client, 0, sizeof(ftp_client_t));
    client->pcb = newpcb;
    client->state = FTP_STATE_IDLE;
    client->active = true;
    strcpy(client->cwd, "/");
    
    printf("FTP: Client connected from %s\n", ipaddr_ntoa(&newpcb->remote_ip));
    
    tcp_arg(newpcb, client);
    tcp_recv(newpcb, ftp_recv);
    tcp_err(newpcb, ftp_error);
    
    ftp_send_response(client, FTP_RESP_220_WELCOME);
    
    return ERR_OK;
}

/**
 * Initialize FTP server with FatFS
 */
bool ftp_server_init(void *fs) {
    printf("FTP: Initializing server on port %d\n", FTP_PORT);
    
    if (!fs) {
        printf("FTP: FatFS filesystem required\n");
        return false;
    }
    
    g_fs = (FATFS *)fs;  // Cast from void* to FATFS*
    memset(ftp_clients, 0, sizeof(ftp_clients));
    
    cyw43_arch_lwip_begin();
    ftp_server_pcb = tcp_new();
    cyw43_arch_lwip_end();
    
    if (!ftp_server_pcb) {
        printf("FTP: Failed to create PCB\n");
        return false;
    }
    
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
    
    cyw43_arch_lwip_begin();
    ftp_server_pcb = tcp_listen(ftp_server_pcb);
    cyw43_arch_lwip_end();
    
    if (!ftp_server_pcb) {
        printf("FTP: Listen failed\n");
        return false;
    }
    
    cyw43_arch_lwip_begin();
    tcp_accept(ftp_server_pcb, ftp_accept);
    cyw43_arch_lwip_end();
    
    printf("FTP: Server started successfully\n");
    printf("FTP: Username: %s\n", FTP_USER);
    printf("FTP: Password: %s\n", FTP_PASSWORD);
    
    return true;
}

/**
 * Process FTP server (placeholder)
 */
void ftp_server_process(void) {
    // CRITICAL: Poll lwIP to process incoming connections and data
    // This allows data connection callbacks to be processed
    cyw43_arch_poll();
    
    vTaskDelay(pdMS_TO_TICKS(10));  // Reduced from 100ms for better responsiveness
}

/**
 * Shutdown FTP server
 */
void ftp_server_shutdown(void) {
    printf("FTP: Shutting down server\n");
    
    for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
        if (ftp_clients[i].active) {
            ftp_close_client(&ftp_clients[i]);
        }
    }
    
    if (ftp_server_pcb) {
        cyw43_arch_lwip_begin();
        tcp_close(ftp_server_pcb);
        cyw43_arch_lwip_end();
        ftp_server_pcb = NULL;
    }
    
    printf("FTP: Server stopped\n");
}