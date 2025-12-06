/**
 * ftp_server.c - Enhanced FTP Server using raw lwIP API
 * 
 * VERSION: 2025-12-06-v14 (CRITICAL FIX: tcp_output after each send)
 * 
 * Features:
 * - PASV (passive mode) support
 * - LIST/NLST (directory listing) commands
 * - MLSD (machine-readable directory listing - RFC 3659)
 * - RETR (file download) with RAM buffering
 * - MDTM (modification time query)
 * - SIZE (file size query)
 * - FEAT (feature negotiation - RFC 2389)
 * - CWD/CDUP (directory navigation)
 * - Non-blocking event-driven architecture
 * - RAM buffering for efficient transfers (up to 256KB)
 * - Proper lwIP callback handling with ACK detection
 * 
 * Integrates with FatFS for SD card access.
 * Uses raw lwIP API (not BSD sockets) with cyw43_arch_lwip_begin/end protection.
 * 
 * CRITICAL: Check len > 0 in tcp_sent for real ACKs, fall through to close on complete
 */

#include "ftp_server.h"
#include "ftp_types.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pico/cyw43_arch.h>
#include <lwip/tcp.h>
#include <lwip/ip_addr.h>
#include <FreeRTOS.h>
#include <task.h>

// FTP Server Configuration
#define FTP_USER        "pico"
#define FTP_PASSWORD    "pico"

// Global FTP Server State
static struct tcp_pcb *ftp_server_pcb = NULL;
static ftp_client_t ftp_clients[FTP_MAX_CLIENTS];
static uint16_t next_data_port = FTP_DATA_PORT_MIN;
static FATFS *g_fs = NULL;  // FatFS filesystem

// Forward declarations for internal static functions
static int ftp_get_current_year(void);
static err_t ftp_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t ftp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void ftp_error(void *arg, err_t err);
static void ftp_close_client(ftp_client_t *client);
static void ftp_close_data_connection(ftp_client_t *client);
static void ftp_send_list(ftp_client_t *client);
static void ftp_send_mlsd(ftp_client_t *client);
static void ftp_start_file_transfer(ftp_client_t *client, const char *filepath);
static void ftp_send_file_chunk(ftp_client_t *client);
static err_t ftp_data_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t ftp_data_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void ftp_data_error(void *arg, err_t err);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Get current year for LIST timestamp formatting
 * Uses compile date as reference (good enough for most cases)
 * 
 * If you need accurate current year, consider:
 * - Adding NTP client (lwIP SNTP)
 * - Adding external RTC module
 * - Manually updating hardcoded year
 */
static int ftp_get_current_year(void) {
    // Parse year from compile date __DATE__ which is "Dec 05 2025"
    const char *date = __DATE__;  // "MMM DD YYYY"
    int year = 2025;  // Default fallback
    
    // Find the year (last 4 characters)
    size_t len = strlen(date);
    if (len >= 4) {
        // Extract year from end of string
        const char *year_str = date + len - 4;
        year = atoi(year_str);
    }
    
    return year;
}

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
    
    // Close any open file handle
    if (client->retr_file_open) {
        f_close(&client->retr_file);
        client->retr_file_open = false;
        printf("FTP: Closed open file handle\n");
    }
    
    // Free RAM buffer if allocated
    if (client->file_buffer) {
        free(client->file_buffer);
        client->file_buffer = NULL;
        client->file_buffer_size = 0;
        client->file_buffer_pos = 0;
        printf("FTP: Freed file buffer\n");
    }
    
    client->data_conn.waiting_for_connection = false;
    client->data_conn.connected = false;
    client->data_conn.port = 0;
    client->data_conn.transfer_complete = false;
}

/**
 * Data connection sent callback - called when data is ACKed
 */
static err_t ftp_data_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    ftp_client_t *client = (ftp_client_t *)arg;
    
    LWIP_UNUSED_ARG(tpcb);
    // DON'T ignore len! It tells us if this is a real ACK or not
    
    if (!client) {
        return ERR_OK;
    }
    
    // CRITICAL: Only send more data when we get a REAL ACK from the network
    // len == 0 means internal callback (no transmission)
    // len > 0 means client ACKed data (actual transmission)
    if (len == 0) {
        return ERR_OK;
    }
    
    // If file transfer in progress, send next chunk
    if (client->file_buffer && client->data_conn.connected) {
        // Check if there's more to send
        if (client->file_buffer_pos < client->file_buffer_size) {
            ftp_send_file_chunk(client);
            
            // CRITICAL: Flush immediately after sending more data!
            // Without this, data waits in buffer for timeout (~1 second)
            // and transfer becomes VERY slow (1 chunk per second)
            if (client->data_conn.pcb) {
                cyw43_arch_lwip_begin();
                tcp_output(client->data_conn.pcb);
                cyw43_arch_lwip_end();
            }
            
            return ERR_OK;  // More data to send, return and wait for next ACK
        } else {
            // All data has been sent and ACKed!
            printf("FTP: File transfer complete, %lu bytes sent and ACKed\n", 
                   client->file_buffer_pos);
            fflush(stdout);
            client->data_conn.transfer_complete = true;
            
            // Don't return - fall through to close connection immediately
        }
    }
    
    // Check if transfer is complete and all data sent
    if (client->data_conn.transfer_complete && client->data_conn.pcb &&
        tcp_sndbuf(client->data_conn.pcb) == TCP_SND_BUF) {
        printf("FTP Data: All data transmitted, closing connection\n");
        
        client->data_conn.transfer_complete = false;
        
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
    
    // Handle pending MLSD operation
    if (client->pending_mlsd) {
        printf("FTP Data: Pending MLSD detected, sending machine-readable listing\n");
        fflush(stdout);
        client->pending_mlsd = false;
        ftp_send_mlsd(client);
    }
    
    // Handle pending RETR operation
    if (client->pending_retr) {
        printf("FTP Data: Pending RETR detected, starting file transfer\n");
        fflush(stdout);
        client->pending_retr = false;
        ftp_start_file_transfer(client, client->retr_filename);
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
        int year = 1980 + ((fno.fdate >> 9) & 0x7F);  // Year from FAT timestamp
        int month = (fno.fdate >> 5) & 0x0F;
        int day = fno.fdate & 0x1F;
        int hour = (fno.ftime >> 11) & 0x1F;
        int minute = (fno.ftime >> 5) & 0x3F;
        
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const char *month_str = (month >= 1 && month <= 12) ? months[month - 1] : "???";
        
        // Get current year (from compile date or NTP if available)
        int current_year = ftp_get_current_year();
        
        // Standard FTP LIST format:
        // - Files from current year: show time (HH:MM)
        // - Files from other years: show year (YYYY)
        char datetime[16];
        if (year == current_year) {
            // Show time for files from this year
            snprintf(datetime, sizeof(datetime), "%02d:%02d", hour, minute);
        } else {
            // Show year for files from other years
            snprintf(datetime, sizeof(datetime), " %4d", year);
        }
        
        // Format line
        int len = snprintf(line, sizeof(line),
                          "%s   1 owner group %8lu %s %2d %5s %s\r\n",
                          perms, (unsigned long)fno.fsize,
                          month_str, day, datetime, fno.fname);
        
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
    
    // Mark transfer as complete - tcp_sent will close when all data is ACKed
    client->data_conn.transfer_complete = true;
    
    // Don't close immediately - let lwIP transmit the data
    // The tcp_sent callback will close the connection and send 226 when all data is ACKed
    printf("FTP: Data queued, waiting for transmission to complete\n");
}

/**
 * Send next chunk of file data
 */
/**
 * Send next chunk of file data from RAM buffer
 */
static void ftp_send_file_chunk(ftp_client_t *client) {
    if (!client->file_buffer || !client->data_conn.connected || !client->data_conn.pcb) {
        return;
    }
    
    // CRITICAL: Prevent re-entry!
    if (client->sending_in_progress) {
        return;
    }
    
    client->sending_in_progress = true;
    
    // Check if all data has been sent
    if (client->file_buffer_pos >= client->file_buffer_size) {
        // All data sent!
        printf("FTP: File transfer complete, %lu bytes sent\n", client->file_buffer_pos);
        fflush(stdout);
        
        // Close file if streaming
        if (client->retr_file_open) {
            f_close(&client->retr_file);
            client->retr_file_open = false;
            printf("FTP: Closed file after streaming\n");
        }
        
        // Mark transfer as complete
        client->data_conn.transfer_complete = true;
        
        // Flush any buffered data
        cyw43_arch_lwip_begin();
        tcp_output(client->data_conn.pcb);
        cyw43_arch_lwip_end();
        
        client->sending_in_progress = false;
        
        // tcp_sent callback will close connection and send 226
        return;
    }
    
    // Get available TCP send buffer space
    uint16_t available = tcp_sndbuf(client->data_conn.pcb);
    
    if (available == 0) {
        client->sending_in_progress = false;
        return;
    }
    
    // Streaming mode: manage buffer and SD card reads
    if (client->retr_file_open) {
        // Check if buffer needs refilling
        if (client->buffer_send_pos >= client->buffer_data_len) {
            // Before reading from SD, verify connection is still alive
            if (!client->data_conn.connected || !client->data_conn.pcb) {
                // Client disconnected, stop reading
                printf("FTP: Client disconnected during streaming, aborting\n");
                fflush(stdout);
                if (client->retr_file_open) {
                    f_close(&client->retr_file);
                    client->retr_file_open = false;
                }
                client->sending_in_progress = false;
                return;
            }
            
            // Buffer is empty - read next chunk from SD card
            uint32_t remaining_file = client->file_buffer_size - client->file_buffer_pos;
            uint32_t to_read = (remaining_file > 32768) ? 32768 : remaining_file;
            
            // Read minimum 16KB chunks for efficiency (except at end of file)
            if (to_read > 16384 && to_read < 32768) {
                to_read = 16384;
            }
            
            printf("FTP: Buffer empty, reading %lu bytes from SD (file pos %lu/%lu)\n",
                   to_read, client->file_buffer_pos, client->file_buffer_size);
            fflush(stdout);
            
            UINT bytes_read = 0;
            FRESULT res = f_read(&client->retr_file, client->file_buffer, to_read, &bytes_read);
            
            if (res != FR_OK) {
                printf("FTP: File read error %d\n", res);
                fflush(stdout);
                client->sending_in_progress = false;
                if (client->retr_file_open) {
                    f_close(&client->retr_file);
                    client->retr_file_open = false;
                }
                ftp_close_data_connection(client);
                ftp_send_response(client, "426 Transfer aborted: read error\r\n");
                return;
            }
            
            client->buffer_data_len = bytes_read;
            client->buffer_send_pos = 0;
            printf("FTP: Loaded %u bytes into buffer\n", bytes_read);
            fflush(stdout);
            
            if (bytes_read == 0) {
                // Shouldn't happen but handle gracefully
                printf("FTP: No data read from file\n");
                client->sending_in_progress = false;
                return;
            }
        }
        
        // Send from buffer
        uint32_t buffer_available = client->buffer_data_len - client->buffer_send_pos;
        uint16_t tcp_available = tcp_sndbuf(client->data_conn.pcb);
        
        if (tcp_available == 0) {
            client->sending_in_progress = false;
            return;
        }
        
        // Determine chunk size to send
        uint16_t chunk_size = (buffer_available > tcp_available) ? tcp_available : buffer_available;
        if (chunk_size > 4096) {
            chunk_size = 4096;  // Smooth flow
        }
        
        // Send chunk from buffer
        err_t err;
        cyw43_arch_lwip_begin();
        err = tcp_write(client->data_conn.pcb,
                       client->file_buffer + client->buffer_send_pos,
                       chunk_size,
                       TCP_WRITE_FLAG_COPY);
        cyw43_arch_lwip_end();
        
        if (err == ERR_OK) {
            client->buffer_send_pos += chunk_size;
            client->file_buffer_pos += chunk_size;
            client->retr_bytes_sent += chunk_size;
        } else if (err != ERR_MEM) {
            // Only log actual errors, not buffer full
            printf("FTP: TCP write error %d\n", err);
            fflush(stdout);
            if (client->retr_file_open) {
                f_close(&client->retr_file);
                client->retr_file_open = false;
            }
            ftp_close_data_connection(client);
            ftp_send_response(client, "426 Transfer aborted: write error\r\n");
        }
        
        client->sending_in_progress = false;
        return;
    }
    
    // RAM buffer mode: send from existing buffer
    uint32_t remaining = client->file_buffer_size - client->file_buffer_pos;
    uint16_t chunk_size = (remaining > available) ? available : remaining;
    
    if (chunk_size == 0) {
        client->sending_in_progress = false;
        return;
    }
    
    // Limit chunk size to something reasonable (4KB max per call)
    if (chunk_size > 4096) {
        chunk_size = 4096;
    }
    
    // Send chunk from RAM buffer
    err_t err;
    cyw43_arch_lwip_begin();
    err = tcp_write(client->data_conn.pcb, 
                    client->file_buffer + client->file_buffer_pos, 
                    chunk_size, 
                    TCP_WRITE_FLAG_COPY);
    cyw43_arch_lwip_end();
    
    if (err == ERR_OK) {
        // Data queued successfully
        client->file_buffer_pos += chunk_size;
        client->retr_bytes_sent += chunk_size;
    } else if (err != ERR_MEM) {
        // Only log actual errors, not buffer full
        printf("FTP: TCP write error %d\n", err);
        fflush(stdout);
        ftp_close_data_connection(client);
        ftp_send_response(client, "426 Transfer aborted: write error\r\n");
    }
    
    // Clear the sending flag - we're done with this iteration
    client->sending_in_progress = false;
}

/**
 * Start file transfer (called when data connection is established)
 * Loads entire file into RAM for efficient transfer
 */
static void ftp_start_file_transfer(ftp_client_t *client, const char *filepath) {
    printf("FTP: Starting file transfer: %s\n", filepath);
    
    // Open file for reading
    FIL file;
    FRESULT res = f_open(&file, filepath, FA_READ);
    if (res != FR_OK) {
        printf("FTP: Failed to open file '%s', err=%d\n", filepath, res);
        ftp_send_response(client, "550 Failed to open file\r\n");
        ftp_close_data_connection(client);
        return;
    }
    
    // Get file size
    uint32_t file_size = f_size(&file);
    printf("FTP: File size: %lu bytes\n", file_size);
    
    // Strategy:
    // - Small files (<= 256KB): Load into RAM, close file (fast)
    // - Large files (> 256KB): Keep file open, stream chunks (saves RAM)
    bool use_streaming = (file_size > 262144);  // 256KB threshold
    
    if (use_streaming) {
        // Large file - use streaming mode
        printf("FTP: Large file (%lu bytes), using streaming mode\n", file_size);
        
        // Allocate 32KB read buffer for streaming
        client->file_buffer = (uint8_t *)malloc(32768);  // 32KB
        if (!client->file_buffer) {
            printf("FTP: Failed to allocate streaming buffer\n");
            f_close(&file);
            ftp_send_response(client, "451 Out of memory\r\n");
            ftp_close_data_connection(client);
            return;
        }
        
        // Keep file open for streaming
        memcpy(&client->retr_file, &file, sizeof(FIL));
        client->retr_file_open = true;
        client->file_buffer_size = file_size;  // Total file size
        client->file_buffer_pos = 0;           // Current file position (bytes sent)
        client->retr_bytes_sent = 0;
        client->buffer_data_len = 0;           // No data in buffer yet
        client->buffer_send_pos = 0;           // Buffer is empty
        
    } else {
        // Small file - load entirely into RAM
        printf("FTP: Small file (%lu bytes), loading into RAM\n", file_size);
        
        // Allocate RAM buffer for entire file
        client->file_buffer = (uint8_t *)malloc(file_size);
        if (!client->file_buffer) {
            printf("FTP: Failed to allocate %lu bytes for file buffer\n", file_size);
            f_close(&file);
            ftp_send_response(client, "451 Out of memory\r\n");
            ftp_close_data_connection(client);
            return;
        }
        
        // Read entire file into RAM
        UINT bytes_read;
        FRESULT res = f_read(&file, client->file_buffer, file_size, &bytes_read);
        f_close(&file);  // Close file immediately after reading
        
        if (res != FR_OK || bytes_read != file_size) {
            printf("FTP: File read error %d, got %u bytes\n", res, bytes_read);
            free(client->file_buffer);
            client->file_buffer = NULL;
            ftp_send_response(client, "451 Read error\r\n");
            ftp_close_data_connection(client);
            return;
        }
        
        printf("FTP: Loaded %u bytes into RAM buffer\n", bytes_read);
        
        // Initialize transfer state
        client->file_buffer_size = bytes_read;
        client->file_buffer_pos = 0;
        client->retr_bytes_sent = 0;
        client->retr_file_open = false;  // File is closed
        client->buffer_data_len = 0;     // Not used in RAM mode
        client->buffer_send_pos = 0;     // Not used in RAM mode
    }
    
    printf("FTP: Initialized: buffer_size=%lu, buffer_pos=%lu, retr_bytes_sent=%lu\n",
           client->file_buffer_size, client->file_buffer_pos, client->retr_bytes_sent);
    fflush(stdout);
    
    // Send 150 response
    ftp_send_response(client, "150 Opening data connection\r\n");
    
    printf("FTP: About to call ftp_send_file_chunk()\n");
    fflush(stdout);
    
    // Start sending from RAM buffer
    ftp_send_file_chunk(client);
    
    // CRITICAL: Flush the initial chunk to start transmission
    // Without this, data stays queued and never transmits
    if (client->data_conn.pcb) {
        cyw43_arch_lwip_begin();
        tcp_output(client->data_conn.pcb);
        cyw43_arch_lwip_end();
        printf("FTP: Initial chunk flushed, transmission started\n");
        fflush(stdout);
    }
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
 * Send MLSD (Machine-readable listing) response
 * RFC 3659 format: type=file;size=1234;modify=20190813220504; filename
 * This format is unambiguous and easy for clients to parse
 */
static void ftp_send_mlsd(ftp_client_t *client) {
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
    
    // Send directory entries in machine-readable format
    FILINFO fno;
    char line[512];
    int total_sent = 0;
    
    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break;  // End of directory or error
        }
        
        // Extract timestamp components
        int year = 1980 + ((fno.fdate >> 9) & 0x7F);
        int month = (fno.fdate >> 5) & 0x0F;
        int day = fno.fdate & 0x1F;
        int hour = (fno.ftime >> 11) & 0x1F;
        int minute = (fno.ftime >> 5) & 0x3F;
        int second = (fno.ftime & 0x1F) * 2;  // FAT stores seconds/2
        
        // Format: type=file;size=12345;modify=20190813220504; filename\r\n
        // modify format: YYYYMMDDhhmmss (always 14 digits, local time)
        const char *type = (fno.fattrib & AM_DIR) ? "dir" : "file";
        
        int len = snprintf(line, sizeof(line),
                          "type=%s;size=%lu;modify=%04d%02d%02d%02d%02d%02d; %s\r\n",
                          type, (unsigned long)fno.fsize,
                          year, month, day, hour, minute, second,
                          fno.fname);
        
        // Debug: Print what we're sending
        printf("MLSD: %s", line);
        
        if (len > 0 && len < (int)sizeof(line)) {
            // Send line over data connection
            err_t err;
            cyw43_arch_lwip_begin();
            err = tcp_write(client->data_conn.pcb, line, len, TCP_WRITE_FLAG_COPY);
            cyw43_arch_lwip_end();
            
            if (err == ERR_OK) {
                total_sent += len;
            } else if (err == ERR_MEM) {
                printf("FTP: Send buffer full at %d bytes, flushing\n", total_sent);
                break;
            } else {
                printf("FTP: Data write error %d\n", err);
                break;
            }
        }
    }
    
    f_closedir(&dir);
    
    printf("FTP: MLSD queued %d bytes for sending\n", total_sent);
    
    // Flush all queued data
    cyw43_arch_lwip_begin();
    tcp_output(client->data_conn.pcb);
    cyw43_arch_lwip_end();
    
    // Mark transfer as complete
    client->data_conn.transfer_complete = true;
    
    printf("FTP: MLSD data queued, waiting for transmission to complete\n");
}

/**
 * Handle MLSD command - machine-readable directory listing
 * RFC 3659: Provides unambiguous timestamp and size information
 */
static void ftp_cmd_mlsd(ftp_client_t *client, const char *arg) {
    LWIP_UNUSED_ARG(arg);  // For now, ignore argument (always list CWD)
    
    printf("FTP: MLSD command received\n");
    printf("FTP: MLSD - waiting_for_connection=%d, connected=%d\n", 
           client->data_conn.waiting_for_connection, 
           client->data_conn.connected);
    fflush(stdout);
    
    if (!client->data_conn.waiting_for_connection && !client->data_conn.connected) {
        printf("FTP: MLSD - ERROR: No PASV mode active\n");
        fflush(stdout);
        ftp_send_response(client, "425 Use PASV first\r\n");
        return;
    }
    
    // If already connected, send immediately
    if (client->data_conn.connected) {
        printf("FTP: MLSD - Data connection already established, sending listing\n");
        fflush(stdout);
        ftp_send_mlsd(client);
        return;
    }
    
    // If waiting for connection, mark MLSD as pending
    printf("FTP: MLSD - Data connection pending, marking MLSD for later\n");
    fflush(stdout);
    
    client->pending_mlsd = true;
    
    printf("FTP: MLSD - Returning from handler, will send listing when connection arrives\n");
    fflush(stdout);
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
 * Handle RETR command - download file
 */
static void ftp_cmd_retr(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error: filename required\r\n");
        return;
    }
    
    if (!client->data_conn.waiting_for_connection && !client->data_conn.connected) {
        ftp_send_response(client, "425 Use PASV first\r\n");
        return;
    }
    
    // Build full file path
    char filepath[512];
    if (arg[0] == '/') {
        // Absolute path
        strncpy(filepath, arg, sizeof(filepath) - 1);
        filepath[sizeof(filepath) - 1] = '\0';
    } else {
        // Relative path
        size_t cwd_len = strlen(client->cwd);
        size_t arg_len = strlen(arg);
        
        if (cwd_len + arg_len + 2 > sizeof(filepath)) {
            ftp_send_response(client, "550 Path too long\r\n");
            return;
        }
        
        snprintf(filepath, sizeof(filepath), "%s/%s", client->cwd, arg);
    }
    
    // Check if file exists and is not a directory
    FILINFO fno;
    FRESULT res = f_stat(filepath, &fno);
    
    if (res != FR_OK) {
        printf("FTP: RETR - file not found: %s (err=%d)\n", filepath, res);
        ftp_send_response(client, "550 File not found\r\n");
        return;
    }
    
    if (fno.fattrib & AM_DIR) {
        printf("FTP: RETR - is a directory: %s\n", filepath);
        ftp_send_response(client, "550 Is a directory\r\n");
        return;
    }
    
    printf("FTP: RETR requested: %s (%lu bytes)\n", filepath, (unsigned long)fno.fsize);
    
    // If already connected, start transfer immediately
    if (client->data_conn.connected) {
        ftp_start_file_transfer(client, filepath);
        return;
    }
    
    // Mark as pending - will start when data connection arrives
    client->pending_retr = true;
    strncpy(client->retr_filename, filepath, sizeof(client->retr_filename) - 1);
    client->retr_filename[sizeof(client->retr_filename) - 1] = '\0';
    
    printf("FTP: RETR pending, waiting for data connection\n");
}

/**
 * Handle FEAT command - Feature negotiation (RFC 2389)
 * Returns list of supported extensions so clients know what's available
 */
static void ftp_cmd_feat(ftp_client_t *client) {
    printf("FTP: FEAT command received\n");
    
    // Send multi-line response (format: 211-Features: ... 211 End)
    ftp_send_response(client, FTP_RESP_211_FEAT_START);
    
    // List supported features (one per line, starting with space)
    ftp_send_response(client, " MDTM\r\n");           // Modification time query
    ftp_send_response(client, " SIZE\r\n");           // File size query
    ftp_send_response(client, " MLST type*;size*;modify*;\r\n"); // MLST facts
    ftp_send_response(client, " MLSD\r\n");           // Machine-readable listing
    ftp_send_response(client, " PASV\r\n");           // Passive mode
    ftp_send_response(client, " REST STREAM\r\n");    // Resume transfer
    // TODO: Add when implemented:
    // ftp_send_response(client, " MFMT\r\n");        // Modify file modification time
    
    // End features list
    ftp_send_response(client, FTP_RESP_211_FEAT_END);
}

/**
 * Handle MDTM command - get file modification time
 */
static void ftp_cmd_mdtm(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error: filename required\r\n");
        return;
    }
    
    // Build full file path
    char filepath[512];
    if (arg[0] == '/') {
        // Absolute path
        strncpy(filepath, arg, sizeof(filepath) - 1);
        filepath[sizeof(filepath) - 1] = '\0';
    } else {
        // Relative path
        size_t cwd_len = strlen(client->cwd);
        size_t arg_len = strlen(arg);
        
        if (cwd_len + arg_len + 2 > sizeof(filepath)) {
            ftp_send_response(client, "550 Path too long\r\n");
            return;
        }
        
        snprintf(filepath, sizeof(filepath), "%s/%s", client->cwd, arg);
    }
    
    // Get file info
    FILINFO fno;
    FRESULT res = f_stat(filepath, &fno);
    
    if (res != FR_OK) {
        printf("FTP: MDTM - file not found: %s (err=%d)\n", filepath, res);
        ftp_send_response(client, "550 File not found\r\n");
        return;
    }
    
    if (fno.fattrib & AM_DIR) {
        printf("FTP: MDTM - is a directory: %s\n", filepath);
        ftp_send_response(client, "550 Is a directory\r\n");
        return;
    }
    
    // Extract date/time from FAT timestamp
    // fno.fdate bits: 15-9=year from 1980, 8-5=month, 4-0=day
    // fno.ftime bits: 15-11=hour, 10-5=minute, 4-0=second/2
    
    int year = 1980 + ((fno.fdate >> 9) & 0x7F);
    int month = (fno.fdate >> 5) & 0x0F;
    int day = fno.fdate & 0x1F;
    int hour = (fno.ftime >> 11) & 0x1F;
    int minute = (fno.ftime >> 5) & 0x3F;
    int second = ((fno.ftime & 0x1F) * 2);
    
    // Format: 213 YYYYMMDDHHmmss
    char response[64];
    snprintf(response, sizeof(response), "213 %04d%02d%02d%02d%02d%02d\r\n",
             year, month, day, hour, minute, second);
    
    printf("FTP: MDTM %s -> %04d-%02d-%02d %02d:%02d:%02d\n", 
           filepath, year, month, day, hour, minute, second);
    
    ftp_send_response(client, response);
}

/**
 * Handle SIZE command - get file size in bytes
 */
static void ftp_cmd_size(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error: filename required\r\n");
        return;
    }
    
    // Build full file path
    char filepath[512];
    if (arg[0] == '/') {
        strncpy(filepath, arg, sizeof(filepath) - 1);
    } else {
        snprintf(filepath, sizeof(filepath), "%s/%s", client->cwd, arg);
    }
    filepath[sizeof(filepath) - 1] = '\0';
    
    // Get file info using f_stat
    FILINFO fno;
    FRESULT res = f_stat(filepath, &fno);
    
    if (res != FR_OK) {
        ftp_send_response(client, "550 File not found\r\n");
        return;
    }
    
    if (fno.fattrib & AM_DIR) {
        ftp_send_response(client, "550 Is a directory\r\n");
        return;
    }
    
    // Send response: 213 <size>
    char response[64];
    snprintf(response, sizeof(response), "213 %lu\r\n", (unsigned long)fno.fsize);
    
    printf("FTP: SIZE %s = %lu bytes\n", filepath, (unsigned long)fno.fsize);
    
    ftp_send_response(client, response);
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
    else if (strcmp(cmd, "MLSD") == 0) {
        ftp_cmd_mlsd(client, arg);
    }
    else if (strcmp(cmd, "CWD") == 0) {
        ftp_cmd_cwd(client, arg);
    }
    else if (strcmp(cmd, "CDUP") == 0) {
        ftp_cmd_cdup(client);
    }
    else if (strcmp(cmd, "RETR") == 0) {
        ftp_cmd_retr(client, arg);
    }
    else if (strcmp(cmd, "MDTM") == 0) {
        ftp_cmd_mdtm(client, arg);
    }
    else if (strcmp(cmd, "SIZE") == 0) {
        ftp_cmd_size(client, arg);
    }
    else if (strcmp(cmd, "FEAT") == 0) {
        ftp_cmd_feat(client);
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
bool ftp_server_init(FATFS *fs) {
    printf("FTP: Initializing server on port %d\n", FTP_PORT);
    
    if (!fs) {
        printf("FTP: FatFS filesystem required\n");
        return false;
    }
    
    g_fs = fs;  // Store filesystem pointer
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