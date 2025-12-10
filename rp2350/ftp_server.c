/**
 * ftp_server.c - Enhanced FTP Server using raw lwIP API
 * 
 * VERSION: 2025-12-09-v18-complete-fix (Fixed empty directory listing timeout)
 * 
 * Features:
 * - PASV (passive mode) support
 * - LIST/NLST (directory listing) commands
 * - MLSD (machine-readable directory listing - RFC 3659)
 * - RETR (file download) with RAM buffering and streaming mode
 * - STOR (file upload) with RAM buffering and streaming mode
 * - DELE (file deletion)
 * - RNFR/RNTO (file/directory rename)
 * - MKD/XMKD (make directory)
 * - RMD/XRMD (remove directory)
 * - NOOP (keepalive)
 * - MFMT (set file modification time - timestamp preservation)
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

#ifndef FTP_DEBUG
#define FTP_DEBUG 0
#endif

#if FTP_DEBUG
#define FTP_LOG(...) printf(__VA_ARGS__)
#else
#define FTP_LOG(...)
#endif

// FTP Server Configuration
// These can be overridden at compile time via CMake (in wifi_credentials.cmake)
// Default credentials for easy out-of-the-box usage
#ifndef FTP_USER
#define FTP_USER        "pico"
#endif

#ifndef FTP_PASSWORD
#define FTP_PASSWORD    "pico"
#endif

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

// Forward declarations for STOR (upload) support
static err_t ftp_data_recv(void *arg, struct tcp_pcb *tpcb, 
                          struct pbuf *p, err_t err);
static void ftp_cmd_stor(ftp_client_t *client, const char *arg);
static void ftp_start_file_upload(ftp_client_t *client, const char *filename);
static void ftp_complete_buffered_upload(ftp_client_t *client);

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
        FTP_LOG("FTP: Failed to send response, err=%d\n", err);
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
    uint16_t attempts = 0;
    uint16_t port;
    uint16_t max_attempts = FTP_DATA_PORT_MAX - FTP_DATA_PORT_MIN + 1;
    
    while (attempts < max_attempts) {
        port = next_data_port++;
        if (next_data_port > FTP_DATA_PORT_MAX) {
            next_data_port = FTP_DATA_PORT_MIN;
        }
        
        // Check if this port is already in use by another client
        bool port_in_use = false;
        for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
            if (ftp_clients[i].active && 
                ftp_clients[i].data_conn.port == port &&
                ftp_clients[i].data_conn.listen_pcb != NULL) {
                port_in_use = true;
                break;
            }
        }
        
        if (!port_in_use) {
            return port;
        }
        
        attempts++;
    }
    
    // All ports in use - return next port anyway and hope for the best
    FTP_LOG("FTP: Warning - all data ports in use, may cause conflicts!\n");
    return next_data_port++;
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
        FTP_LOG("FTP: Closed open file handle\n");
    }
    
    // Close any open upload file
    if (client->stor_file_open) {
        f_close(&client->stor_file);
        client->stor_file_open = false;
        FTP_LOG("FTP[%p]: Closed open upload file handle\n", client);
    }
    
    // Free RAM buffer if allocated
    if (client->file_buffer) {
        free(client->file_buffer);
        client->file_buffer = NULL;
        client->file_buffer_size = 0;
        client->file_buffer_pos = 0;
        FTP_LOG("FTP: Freed file buffer\n");
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
    
    // MULTI-CLIENT FIX: Validate client pointer
    if (!client || !client->active) {
        FTP_LOG("FTP Data: Invalid client in sent callback\n");
        return ERR_ABRT;
    }
    
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
            FTP_LOG("FTP: File transfer complete, %lu bytes sent and ACKed\n", 
                   client->file_buffer_pos);
            client->data_conn.transfer_complete = true;
            
            // Don't return - fall through to close connection immediately
        }
    }
    
    // Check if transfer is complete and all data sent
    if (client->data_conn.transfer_complete && client->data_conn.pcb &&
        tcp_sndbuf(client->data_conn.pcb) == TCP_SND_BUF) {
        FTP_LOG("FTP Data: All data transmitted, closing connection\n");
        
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
    FTP_LOG("FTP Data: Connection error %d\n", err);
    
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
    
    // MULTI-CLIENT FIX: Validate client pointer
    if (!client || !client->active) {
        FTP_LOG("FTP Data: Invalid client in accept callback\n");
        if (newpcb) {
            cyw43_arch_lwip_begin();
            tcp_abort(newpcb);
            cyw43_arch_lwip_end();
        }
        return ERR_ABRT;
    }
    
    // Verify this client is expecting a data connection
    if (!client->data_conn.waiting_for_connection) {
        FTP_LOG("FTP Data[%p]: Unexpected data connection\n", client);
        if (newpcb) {
            cyw43_arch_lwip_begin();
            tcp_abort(newpcb);
            cyw43_arch_lwip_end();
        }
        return ERR_ABRT;
    }
    
    if (err != ERR_OK || !newpcb) {
        FTP_LOG("FTP Data[%p]: Accept error (err=%d, newpcb=%p)\n", client, err, newpcb);
        return ERR_VAL;
    }
    
    FTP_LOG("FTP Data[%p]: Client connected from %s\n", client, ipaddr_ntoa(&newpcb->remote_ip));
    
    // Set up data connection
    client->data_conn.pcb = newpcb;
    client->data_conn.connected = true;
    client->data_conn.waiting_for_connection = false;
    
    tcp_arg(newpcb, client);
    tcp_sent(newpcb, ftp_data_sent);
    tcp_err(newpcb, ftp_data_error);
    
    // Close the listening PCB - we only accept one data connection
    if (client->data_conn.listen_pcb) {
        FTP_LOG("FTP Data: Closing listen socket\n");
        cyw43_arch_lwip_begin();
        tcp_close(client->data_conn.listen_pcb);
        cyw43_arch_lwip_end();
        client->data_conn.listen_pcb = NULL;
    }
    
    // Handle pending LIST operation
    if (client->pending_list) {
        FTP_LOG("FTP Data: Pending LIST detected, sending directory listing\n");
        client->pending_list = false;
        ftp_send_list(client);
    }
    
    // Handle pending MLSD operation
    if (client->pending_mlsd) {
        FTP_LOG("FTP Data: Pending MLSD detected, sending machine-readable listing\n");
        client->pending_mlsd = false;
        ftp_send_mlsd(client);
    }
    
    // Handle pending RETR operation
    if (client->pending_retr) {
        FTP_LOG("FTP Data: Pending RETR detected, starting file transfer\n");
        client->pending_retr = false;
        ftp_start_file_transfer(client, client->retr_filename);
    }
    
    // Handle pending STOR operation
    if (client->pending_stor) {
        FTP_LOG("FTP Data[%p]: Pending STOR detected, starting file upload\n", client);
        client->pending_stor = false;
        ftp_start_file_upload(client, client->stor_filename);
    }
    
    return ERR_OK;
}

/**
 * Handle PASV command - enter passive mode
 */
static void ftp_cmd_pasv(ftp_client_t *client) {
    FTP_LOG("FTP: PASV command received\n");
    
    // Close any existing data connection
    ftp_close_data_connection(client);
    
    // Get next available port
    uint16_t port = ftp_get_next_data_port();
    FTP_LOG("FTP: PASV - attempting to bind port %d\n", port);
    
    // Create new listening PCB for data connection
    cyw43_arch_lwip_begin();
    struct tcp_pcb *listen_pcb = tcp_new();
    cyw43_arch_lwip_end();
    
    if (!listen_pcb) {
        FTP_LOG("FTP: PASV - failed to create data PCB\n");
        ftp_send_response(client, "425 Can't open data connection\r\n");
        return;
    }
    
    // Bind to data port
    err_t err;
    cyw43_arch_lwip_begin();
    err = tcp_bind(listen_pcb, IP_ADDR_ANY, port);
    cyw43_arch_lwip_end();
    
    if (err != ERR_OK) {
        FTP_LOG("FTP: PASV - failed to bind data port %d, err=%d\n", port, err);
        cyw43_arch_lwip_begin();
        tcp_close(listen_pcb);
        cyw43_arch_lwip_end();
        ftp_send_response(client, "425 Can't open data connection\r\n");
        return;
    }
    
    FTP_LOG("FTP: PASV - bound to port %d successfully\n", port);
    
    // Start listening with explicit backlog
    cyw43_arch_lwip_begin();
    struct tcp_pcb *new_listen_pcb = tcp_listen_with_backlog(listen_pcb, 1);
    cyw43_arch_lwip_end();
    
    if (!new_listen_pcb) {
        FTP_LOG("FTP: PASV - failed to listen on data port\n");
        cyw43_arch_lwip_begin();
        tcp_close(listen_pcb);
        cyw43_arch_lwip_end();
        ftp_send_response(client, "425 Can't open data connection\r\n");
        return;
    }
    
    listen_pcb = new_listen_pcb;
    FTP_LOG("FTP: PASV - listening on port %d with backlog=1\n", port);
    
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
    
    FTP_LOG("FTP: PASV - sending response: %s", response);
    ftp_send_response(client, response);
    FTP_LOG("FTP: PASV - waiting for client to connect on port %d\n", port);
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
        FTP_LOG("FTP: Failed to open directory '%s', err=%d\n", client->cwd, res);
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
                FTP_LOG("FTP: Send buffer full at %d bytes, flushing\n", total_sent);
                break;
            } else {
                FTP_LOG("FTP: Data write error %d\n", err);
                break;
            }
        }
    }
    
    f_closedir(&dir);
    
    FTP_LOG("FTP: LIST queued %d bytes for sending\n", total_sent);
    
    // Handle empty directory case
    if (total_sent == 0) {
        // No data to send - close connection immediately and send success
        FTP_LOG("FTP: LIST - empty directory, closing immediately\n");
        ftp_close_data_connection(client);
        ftp_send_response(client, FTP_RESP_226_TRANSFER_OK);
        return;
    }
    
    // Flush all queued data
    cyw43_arch_lwip_begin();
    tcp_output(client->data_conn.pcb);
    cyw43_arch_lwip_end();
    
    // Mark transfer as complete - tcp_sent will close when all data is ACKed
    client->data_conn.transfer_complete = true;
    
    // Don't close immediately - let lwIP transmit the data
    // The tcp_sent callback will close the connection and send 226 when all data is ACKed
    FTP_LOG("FTP: Data queued, waiting for transmission to complete\n");
}

/**
 * Send next chunk of file data from RAM buffer / streaming buffer
 */
static void ftp_send_file_chunk(ftp_client_t *client) {
    if (!client->file_buffer || !client->data_conn.connected || !client->data_conn.pcb) {
        FTP_LOG("FTP: Send chunk skipped: buffer=%p, connected=%d, pcb=%p\n",
               client->file_buffer, client->data_conn.connected, client->data_conn.pcb);
        return;
    }
    
    // Prevent re-entry
    if (client->sending_in_progress) {
        FTP_LOG("FTP: Send already in progress, skipping nested call\n");
        return;
    }
    
    client->sending_in_progress = true;
    
    // Check if all data has been sent
    if (client->file_buffer_pos >= client->file_buffer_size) {
        // All data sent
        FTP_LOG("FTP: File transfer complete, %lu bytes queued\n", client->file_buffer_pos);
        
        // In streaming mode, close file now since we're done
        if (client->retr_file_open) {
            f_close(&client->retr_file);
            client->retr_file_open = false;
            FTP_LOG("FTP: Closed file after streaming\n");
        }
        
        // Mark transfer as complete; tcp_sent() will close and send 226 after ACKs
        client->data_conn.transfer_complete = true;
        
        // Flush any buffered data
        cyw43_arch_lwip_begin();
        tcp_output(client->data_conn.pcb);
        cyw43_arch_lwip_end();
        
        client->sending_in_progress = false;
        return;
    }
    
    // Check TCP send buffer availability
    uint16_t available = tcp_sndbuf(client->data_conn.pcb);
    if (available == 0) {
        FTP_LOG("FTP: No TCP send buffer available, deferring send\n");
        client->sending_in_progress = false;
        return;
    }
    
    // There are two modes:
    // 1. RAM buffer mode - file was fully loaded into memory (small files)
    // 2. Streaming mode - file is being read in chunks (large files)
    
    if (client->retr_file_open) {
        // STREAMING MODE
        FTP_LOG("FTP: Streaming mode, buffer_pos=%lu, data_len=%lu, file_pos=%lu/%lu\n",
               client->buffer_send_pos, client->buffer_data_len,
               client->file_buffer_pos, client->file_buffer_size);
        
        // Refill buffer from file if needed
        if (client->buffer_send_pos >= client->buffer_data_len) {
            FTP_LOG("FTP: Buffer empty, reading next chunk from file\n");
            
            // Verify connection is still alive before reading from SD
            if (!client->data_conn.connected || !client->data_conn.pcb) {
                FTP_LOG("FTP: Client disconnected during streaming, aborting\n");
                if (client->retr_file_open) {
                    f_close(&client->retr_file);
                    client->retr_file_open = false;
                }
                client->sending_in_progress = false;
                return;
            }
            
            // Calculate remaining bytes in file
            uint32_t remaining_file = client->file_buffer_size - client->file_buffer_pos;
            if (remaining_file == 0) {
                FTP_LOG("FTP: No more file data to stream\n");
                client->sending_in_progress = false;
                return;
            }
            
            // Decide how much to read into our streaming buffer
            uint32_t to_read = (remaining_file > FTP_STREAM_BUFFER_SIZE)
                               ? FTP_STREAM_BUFFER_SIZE
                               : remaining_file;
            
            // Try to read at least half-buffer chunks when possible (except near EOF)
            if (to_read > (FTP_STREAM_BUFFER_SIZE / 2) && to_read < FTP_STREAM_BUFFER_SIZE) {
                to_read = FTP_STREAM_BUFFER_SIZE / 2;
            }
            
            FTP_LOG("FTP: Reading %lu bytes from SD at file pos %lu\n",
                   to_read, client->file_buffer_pos);
            
            UINT bytes_read = 0;
            FRESULT res = f_read(&client->retr_file, client->file_buffer, to_read, &bytes_read);
            
            if (res != FR_OK) {
                FTP_LOG("FTP: File read error %d\n", res);
                client->sending_in_progress = false;
                if (client->retr_file_open) {
                    f_close(&client->retr_file);
                    client->retr_file_open = false;
                }
                ftp_close_data_connection(client);
                ftp_send_response(client, "426 Transfer aborted: read error\r\n");
                return;
            }
            
            // Update streaming buffer tracking
            client->buffer_data_len = bytes_read;
            client->buffer_send_pos = 0;
            FTP_LOG("FTP: Loaded %u bytes into streaming buffer\n", bytes_read);
            
            if (bytes_read == 0) {
                // Shouldn't happen unless near EOF, but handle gracefully
                FTP_LOG("FTP: No data read from file, possible EOF\n");
                client->sending_in_progress = false;
                return;
            }
        }
        
        // Send from streaming buffer
        uint32_t buffer_available = client->buffer_data_len - client->buffer_send_pos;
        uint16_t tcp_available = tcp_sndbuf(client->data_conn.pcb);
        if (tcp_available == 0) {
            FTP_LOG("FTP: TCP buffer empty in streaming send, deferring\n");
            client->sending_in_progress = false;
            return;
        }
        
        // Limit chunk to half the available send buffer, capped at FTP_MAX_CHUNK_SIZE
        uint16_t max_chunk = tcp_available / 2;
        if (max_chunk > FTP_MAX_CHUNK_SIZE) {
            max_chunk = FTP_MAX_CHUNK_SIZE;
        }
        if (max_chunk == 0) {
            FTP_LOG("FTP: max_chunk == 0, cannot send\n");
            client->sending_in_progress = false;
            return;
        }
        
        uint16_t chunk_size = (buffer_available > max_chunk) ? max_chunk : buffer_available;
        
        FTP_LOG("FTP: Streaming chunk: size=%u, buffer_pos=%lu, file_pos=%lu\n",
               chunk_size, client->buffer_send_pos, client->file_buffer_pos);
        
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
            FTP_LOG("FTP: Streaming sent %u bytes, file_pos=%lu\n",
                   chunk_size, client->file_buffer_pos);
        } else if (err == ERR_MEM) {
            FTP_LOG("FTP: ERR_MEM during streaming write, will retry later\n");
        } else {
            FTP_LOG("FTP: TCP write error %d during streaming\n", err);
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
    
    // RAM BUFFER MODE
    // For small files fully loaded into memory
    
    uint32_t remaining = client->file_buffer_size - client->file_buffer_pos;
    
    // Limit chunk to half the available send buffer, capped at FTP_MAX_CHUNK_SIZE
    uint16_t max_chunk = available / 2;
    if (max_chunk > FTP_MAX_CHUNK_SIZE) {
        max_chunk = FTP_MAX_CHUNK_SIZE;
    }
    if (max_chunk == 0) {
        FTP_LOG("FTP: max_chunk == 0 in RAM mode, cannot send\n");
        client->sending_in_progress = false;
        return;
    }
    
    uint16_t chunk_size = (remaining > max_chunk) ? max_chunk : remaining;
    if (chunk_size == 0) {
        FTP_LOG("FTP: No data remaining to send in RAM mode\n");
        client->sending_in_progress = false;
        return;
    }
    
    FTP_LOG("FTP: RAM chunk: size=%u, file_pos=%lu/%lu, sndbuf=%u\n",
           chunk_size, client->file_buffer_pos, client->file_buffer_size, available);
    
    err_t err;
    cyw43_arch_lwip_begin();
    err = tcp_write(client->data_conn.pcb,
                    client->file_buffer + client->file_buffer_pos,
                    chunk_size,
                    TCP_WRITE_FLAG_COPY);
    cyw43_arch_lwip_end();
    
    if (err == ERR_OK) {
        client->file_buffer_pos += chunk_size;
        client->retr_bytes_sent += chunk_size;
        FTP_LOG("FTP: RAM mode sent %u bytes, new pos=%lu\n",
               chunk_size, client->file_buffer_pos);
    } else if (err == ERR_MEM) {
        FTP_LOG("FTP: ERR_MEM in RAM mode, will retry later\n");
    } else {
        FTP_LOG("FTP: TCP write error %d in RAM mode\n", err);
        ftp_close_data_connection(client);
        ftp_send_response(client, "426 Transfer aborted: write error\r\n");
    }
    
    client->sending_in_progress = false;
}

/**
 * Start file transfer (called when data connection is established)
 * Loads entire file into RAM for efficient transfer or streams with buffer
 */
static void ftp_start_file_transfer(ftp_client_t *client, const char *filepath) {
    FTP_LOG("FTP: Starting file transfer: %s\n", filepath);
    
    // Open file for reading
    FIL file;
    FRESULT res = f_open(&file, filepath, FA_READ);
    if (res != FR_OK) {
        FTP_LOG("FTP: Failed to open file '%s', err=%d\n", filepath, res);
        ftp_send_response(client, "550 Failed to open file\r\n");
        ftp_close_data_connection(client);
        return;
    }
    
    // Get file size
    uint32_t file_size = f_size(&file);
    FTP_LOG("FTP: File size: %lu bytes\n", file_size);
    
    // Strategy:
    // - Small files (<= 256KB): Load into RAM, close file (fast)
    // - Large files (> 256KB): Keep file open, stream chunks (saves RAM)
    bool use_streaming = (file_size > 262144);  // 256KB threshold
    
    if (use_streaming) {
        // Large file - use streaming mode
        FTP_LOG("FTP: Large file (%lu bytes), using streaming mode\n", file_size);
        
        // Allocate streaming buffer for large file transfers
        client->file_buffer = (uint8_t *)malloc(FTP_STREAM_BUFFER_SIZE);  // Streaming buffer
        if (!client->file_buffer) {
            FTP_LOG("FTP: Failed to allocate streaming buffer\n");
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
        FTP_LOG("FTP: Small file (%lu bytes), loading into RAM\n", file_size);
        
        // Allocate RAM buffer for entire file
        client->file_buffer = (uint8_t *)malloc(file_size);
        if (!client->file_buffer) {
            FTP_LOG("FTP: Failed to allocate %lu bytes for file buffer\n", file_size);
            f_close(&file);
            ftp_send_response(client, "451 Out of memory\r\n");
            ftp_close_data_connection(client);
            return;
        }
        
        // Read entire file into RAM
        UINT bytes_read;
        FRESULT res2 = f_read(&file, client->file_buffer, file_size, &bytes_read);
        f_close(&file);  // Close file immediately after reading
        
        if (res2 != FR_OK || bytes_read != file_size) {
            FTP_LOG("FTP: File read error %d, got %u bytes\n", res2, bytes_read);
            free(client->file_buffer);
            client->file_buffer = NULL;
            ftp_send_response(client, "451 Read error\r\n");
            ftp_close_data_connection(client);
            return;
        }
        
        FTP_LOG("FTP: Loaded %u bytes into RAM buffer\n", bytes_read);
        
        // Initialize transfer state
        client->file_buffer_size = bytes_read;
        client->file_buffer_pos = 0;
        client->retr_bytes_sent = 0;
        client->retr_file_open = false;  // File is closed
        client->buffer_data_len = 0;     // Not used in RAM mode
        client->buffer_send_pos = 0;     // Not used in RAM mode
    }
    
    FTP_LOG("FTP: Initialized: buffer_size=%lu, buffer_pos=%lu, retr_bytes_sent=%lu\n",
           client->file_buffer_size, client->file_buffer_pos, client->retr_bytes_sent);
    
    // Send 150 response
    ftp_send_response(client, "150 Opening data connection\r\n");
    
    FTP_LOG("FTP: About to call ftp_send_file_chunk()\n");
    
    // Start sending
    ftp_send_file_chunk(client);
    
    // Flush the initial chunk to start transmission
    if (client->data_conn.pcb) {
        cyw43_arch_lwip_begin();
        tcp_output(client->data_conn.pcb);
        cyw43_arch_lwip_end();
        FTP_LOG("FTP: Initial chunk flushed, transmission started\n");
    }
}

/**
 * Handle LIST command - NON-BLOCKING version
 */
static void ftp_cmd_list(ftp_client_t *client, const char *arg) {
    LWIP_UNUSED_ARG(arg);  // For now, ignore argument (always list CWD)
    
    FTP_LOG("FTP: LIST command received\n");
    FTP_LOG("FTP: LIST - waiting_for_connection=%d, connected=%d\n", 
           client->data_conn.waiting_for_connection, 
           client->data_conn.connected);
    
    if (!client->data_conn.waiting_for_connection && !client->data_conn.connected) {
        FTP_LOG("FTP: LIST - ERROR: No PASV mode active\n");
        ftp_send_response(client, "425 Use PASV first\r\n");
        return;
    }
    
    // If already connected, send immediately
    if (client->data_conn.connected) {
        FTP_LOG("FTP: LIST - Data connection already established, sending listing\n");
        ftp_send_list(client);
        return;
    }
    
    // If waiting for connection, mark LIST as pending and return
    FTP_LOG("FTP: LIST - Data connection pending, marking LIST for later\n");
    
    client->pending_list = true;
    
    FTP_LOG("FTP: LIST - Returning from handler, will send listing when connection arrives\n");
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
        FTP_LOG("FTP: Failed to open directory '%s', err=%d\n", client->cwd, res);
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
        
        FTP_LOG("MLSD: %s", line);
        
        if (len > 0 && len < (int)sizeof(line)) {
            // Send line over data connection
            err_t err;
            cyw43_arch_lwip_begin();
            err = tcp_write(client->data_conn.pcb, line, len, TCP_WRITE_FLAG_COPY);
            cyw43_arch_lwip_end();
            
            if (err == ERR_OK) {
                total_sent += len;
            } else if (err == ERR_MEM) {
                FTP_LOG("FTP: Send buffer full at %d bytes, flushing\n", total_sent);
                break;
            } else {
                FTP_LOG("FTP: Data write error %d\n", err);
                break;
            }
        }
    }
    
    f_closedir(&dir);
    
    FTP_LOG("FTP: MLSD queued %d bytes for sending\n", total_sent);
    
    // Handle empty directory case
    if (total_sent == 0) {
        // No data to send - close connection immediately and send success
        FTP_LOG("FTP: MLSD - empty directory, closing immediately\n");
        ftp_close_data_connection(client);
        ftp_send_response(client, FTP_RESP_226_TRANSFER_OK);
        return;
    }
    
    // Flush all queued data
    cyw43_arch_lwip_begin();
    tcp_output(client->data_conn.pcb);
    cyw43_arch_lwip_end();
    
    // Mark transfer as complete
    client->data_conn.transfer_complete = true;
    
    FTP_LOG("FTP: MLSD data queued, waiting for transmission to complete\n");
}

/**
 * Handle MLSD command - machine-readable directory listing
 * RFC 3659: Provides unambiguous timestamp and size information
 */
static void ftp_cmd_mlsd(ftp_client_t *client, const char *arg) {
    LWIP_UNUSED_ARG(arg);  // For now, ignore argument (always list CWD)
    
    FTP_LOG("FTP: MLSD command received\n");
    FTP_LOG("FTP: MLSD - waiting_for_connection=%d, connected=%d\n", 
           client->data_conn.waiting_for_connection, 
           client->data_conn.connected);
    
    if (!client->data_conn.waiting_for_connection && !client->data_conn.connected) {
        FTP_LOG("FTP: MLSD - ERROR: No PASV mode active\n");
        ftp_send_response(client, "425 Use PASV first\r\n");
        return;
    }
    
    // If already connected, send immediately
    if (client->data_conn.connected) {
        FTP_LOG("FTP: MLSD - Data connection already established, sending listing\n");
        ftp_send_mlsd(client);
        return;
    }
    
    // If waiting for connection, mark MLSD as pending
    FTP_LOG("FTP: MLSD - Data connection pending, marking MLSD for later\n");
    
    client->pending_mlsd = true;
    
    FTP_LOG("FTP: MLSD - Returning from handler, will send listing when connection arrives\n");
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
        FTP_LOG("FTP: CWD changed to '%s'\n", client->cwd);
    } else {
        FTP_LOG("FTP: CWD failed for '%s', err=%d\n", new_path, res);
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
        FTP_LOG("FTP: CDUP changed to '%s'\n", client->cwd);
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
        FTP_LOG("FTP: RETR - file not found: %s (err=%d)\n", filepath, res);
        ftp_send_response(client, "550 File not found\r\n");
        return;
    }
    
    if (fno.fattrib & AM_DIR) {
        FTP_LOG("FTP: RETR - is a directory: %s\n", filepath);
        ftp_send_response(client, "550 Is a directory\r\n");
        return;
    }
    
    FTP_LOG("FTP: RETR requested: %s (%lu bytes)\n", filepath, (unsigned long)fno.fsize);
    
    // If already connected, start transfer immediately
    if (client->data_conn.connected) {
        ftp_start_file_transfer(client, filepath);
        return;
    }
    
    // Mark as pending - will start when data connection arrives
    client->pending_retr = true;
    strncpy(client->retr_filename, filepath, sizeof(client->retr_filename) - 1);
    client->retr_filename[sizeof(client->retr_filename) - 1] = '\0';
    
    FTP_LOG("FTP: RETR pending, waiting for data connection\n");
}

/**
 * Handle STOR command - prepare to receive file upload
 */
static void ftp_cmd_stor(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 No filename specified\r\n");
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
    
    FTP_LOG("FTP[%p]: STOR requested: %s\n", client, filepath);
    
    // Check if we have a data connection ready
    if (!client->data_conn.waiting_for_connection && !client->data_conn.connected) {
        ftp_send_response(client, "425 Use PASV first\r\n");
        return;
    }
    
    // If data connection not yet established, mark STOR as pending
    if (client->data_conn.waiting_for_connection) {
        FTP_LOG("FTP[%p]: STOR pending, waiting for data connection\n", client);
        client->pending_stor = true;
        strncpy(client->stor_filename, filepath, sizeof(client->stor_filename) - 1);
        client->stor_filename[sizeof(client->stor_filename) - 1] = '\0';
        ftp_send_response(client, FTP_RESP_150_OPENING_DATA);
        return;
    }
    
    // Data connection ready, start upload immediately
    ftp_send_response(client, FTP_RESP_150_OPENING_DATA);
    ftp_start_file_upload(client, filepath);
}

/**
 * Start receiving file upload
 * Decides between RAM buffering (small files) or streaming (large files)
 */
static void ftp_start_file_upload(ftp_client_t *client, const char *filename) {
    FTP_LOG("FTP[%p]: Starting file upload: %s\n", client, filename);
    
    // Initialize upload state
    client->stor_bytes_received = 0;
    client->stor_file_open = false;
    client->stor_use_buffer = false;
    client->file_buffer = NULL;
    client->file_buffer_pos = 0;
    client->buffer_data_len = 0;
    
    // Save filename for later (when writing to SD)
    strncpy(client->stor_filename, filename, sizeof(client->stor_filename) - 1);
    client->stor_filename[sizeof(client->stor_filename) - 1] = '\0';
    
    // Decide buffering strategy based on expected file size
    // Note: We don't always know the size in advance, so we use streaming by default
    // If we had SIZE command before STOR, we'd have stor_expected_size set
    
    uint32_t expected_size = client->stor_expected_size;
    
    if (expected_size > 0 && expected_size <= FTP_FILE_BUFFER_MAX) {
        // Small file - use RAM buffering
        FTP_LOG("FTP[%p]: Small file upload (%lu bytes), using RAM buffering\n", 
               client, expected_size);
        
        client->file_buffer = (uint8_t *)malloc(expected_size);
        if (!client->file_buffer) {
            FTP_LOG("FTP[%p]: Failed to allocate %lu bytes for upload buffer\n", 
                   client, expected_size);
            ftp_send_response(client, "451 Memory allocation failed\r\n");
            ftp_close_data_connection(client);
            return;
        }
        
        client->file_buffer_size = expected_size;
        client->stor_use_buffer = true;
        FTP_LOG("FTP[%p]: Allocated %lu byte buffer, ready to receive\n", 
               client, expected_size);
    } else {
        // Large file or unknown size - use streaming with smaller buffer
        FTP_LOG("FTP[%p]: Large/unknown size file upload, using streaming mode\n", client);
        
        // Open file for writing immediately
        FRESULT res = f_open(&client->stor_file, filename, 
                            FA_CREATE_ALWAYS | FA_WRITE);
        
        if (res != FR_OK) {
            FTP_LOG("FTP[%p]: Failed to open file for writing: %d\n", client, res);
            ftp_send_response(client, FTP_RESP_550_FILE_ERROR);
            ftp_close_data_connection(client);
            return;
        }
        
        client->stor_file_open = true;
        
        // Allocate streaming buffer
        client->file_buffer = (uint8_t *)malloc(FTP_STREAM_BUFFER_SIZE);
        if (!client->file_buffer) {
            FTP_LOG("FTP[%p]: Failed to allocate streaming buffer\n", client);
            f_close(&client->stor_file);
            client->stor_file_open = false;
            ftp_send_response(client, "451 Memory allocation failed\r\n");
            ftp_close_data_connection(client);
            return;
        }
        
        client->file_buffer_size = FTP_STREAM_BUFFER_SIZE;
        client->stor_use_buffer = false;  // Streaming mode
        FTP_LOG("FTP[%p]: File opened and buffer allocated, ready to receive\n", client);
    }
    
    // Set up receive callback to handle incoming data
    if (client->data_conn.pcb) {
        cyw43_arch_lwip_begin();
        tcp_recv(client->data_conn.pcb, ftp_data_recv);
        cyw43_arch_lwip_end();
    }
}

/**
 * Complete a buffered upload - write entire RAM buffer to SD
 */
static void ftp_complete_buffered_upload(ftp_client_t *client) {
    FTP_LOG("FTP[%p]: Completing buffered upload - %lu bytes to write\n", 
           client, client->buffer_data_len);
    
    // Open file for writing
    FRESULT res = f_open(&client->stor_file, client->stor_filename, 
                        FA_CREATE_ALWAYS | FA_WRITE);
    
    if (res != FR_OK) {
        FTP_LOG("FTP[%p]: Failed to open file for writing: %d\n", client, res);
        ftp_send_response(client, FTP_RESP_550_FILE_ERROR);
        return;
    }
    
    // Write entire buffer to SD in one shot
    UINT bytes_written;
    res = f_write(&client->stor_file, client->file_buffer, 
                 client->buffer_data_len, &bytes_written);
    
    f_close(&client->stor_file);
    
    if (res != FR_OK || bytes_written != client->buffer_data_len) {
        FTP_LOG("FTP[%p]: Write error: %d (wrote %lu/%lu bytes)\n", 
               client, res, bytes_written, client->buffer_data_len);
        ftp_send_response(client, "426 Write error\r\n");
        return;
    }
    
    FTP_LOG("FTP[%p]: Upload complete - %lu bytes written to SD\n", 
           client, bytes_written);
    
    // Send success response
    char response[128];
    snprintf(response, sizeof(response), 
            "226 Transfer complete (%lu bytes received)\r\n",
            client->stor_bytes_received);
    ftp_send_response(client, response);
}

/**
 * Data connection receive callback
 * Called when file data arrives from client during STOR
 */
static err_t ftp_data_recv(void *arg, struct tcp_pcb *tpcb, 
                          struct pbuf *p, err_t err) {
    ftp_client_t *client = (ftp_client_t *)arg;
    
    LWIP_UNUSED_ARG(tpcb);
    
    // Validate client
    if (!client || !client->active) {
        FTP_LOG("FTP Data: Invalid client in recv callback\n");
        if (p) pbuf_free(p);
        return ERR_ABRT;
    }
    
    // Connection closed by client (end of upload)
    if (p == NULL) {
        FTP_LOG("FTP[%p]: Client closed data connection, upload complete\n", client);
        
        // Handle completion based on buffering mode
        if (client->stor_use_buffer) {
            // RAM buffered mode - write entire buffer to SD now
            ftp_complete_buffered_upload(client);
        } else {
            // Streaming mode - write any remaining data in buffer
            if (client->buffer_data_len > 0 && client->stor_file_open) {
                UINT bytes_written;
                FRESULT res = f_write(&client->stor_file, client->file_buffer, 
                                     client->buffer_data_len, &bytes_written);
                
                if (res != FR_OK || bytes_written != client->buffer_data_len) {
                    FTP_LOG("FTP[%p]: Final write error: %d\n", client, res);
                    ftp_send_response(client, "426 Write error\r\n");
                } else {
                    FTP_LOG("FTP[%p]: Upload complete - %lu bytes received\n", 
                           client, client->stor_bytes_received);
                    
                    char response[128];
                    snprintf(response, sizeof(response), 
                            "226 Transfer complete (%lu bytes received)\r\n",
                            client->stor_bytes_received);
                    ftp_send_response(client, response);
                }
            }
            
            // Close file
            if (client->stor_file_open) {
                f_close(&client->stor_file);
                client->stor_file_open = false;
            }
        }
        
        // Free buffer and close connection
        if (client->file_buffer) {
            free(client->file_buffer);
            client->file_buffer = NULL;
        }
        
        ftp_close_data_connection(client);
        return ERR_OK;
    }
    
    // Check for errors
    if (err != ERR_OK) {
        FTP_LOG("FTP[%p]: Receive error %d\n", client, err);
        pbuf_free(p);
        return err;
    }
    
    // Check if buffer allocated
    if (!client->file_buffer) {
        FTP_LOG("FTP[%p]: Received data but no buffer allocated!\n", client);
        pbuf_free(p);
        return ERR_ABRT;
    }
    
    uint16_t total_len = p->tot_len;
    
    // Process received data based on buffering mode
    if (client->stor_use_buffer) {
        // ═══════════════════════════════════════════════════════════════════
        // RAM BUFFERING MODE - Store everything in RAM
        // ═══════════════════════════════════════════════════════════════════
        
        // Check if data fits in buffer
        if (client->buffer_data_len + total_len > client->file_buffer_size) {
            FTP_LOG("FTP[%p]: Upload exceeds expected size, aborting\n", client);
            pbuf_free(p);
            ftp_send_response(client, "426 File too large\r\n");
            ftp_close_data_connection(client);
            return ERR_ABRT;
        }
        
        // Copy data from pbuf chain into RAM buffer
        uint16_t copied = 0;
        struct pbuf *q;
        for (q = p; q != NULL; q = q->next) {
            memcpy(client->file_buffer + client->buffer_data_len + copied, 
                   q->payload, q->len);
            copied += q->len;
        }
        
        client->buffer_data_len += copied;
        client->stor_bytes_received += copied;
        
    } else {
        // ═══════════════════════════════════════════════════════════════════
        // STREAMING MODE - Write to SD when buffer fills
        // ═══════════════════════════════════════════════════════════════════
        
        struct pbuf *q;
        for (q = p; q != NULL; q = q->next) {
            uint16_t data_left = q->len;
            uint8_t *data_ptr = (uint8_t *)q->payload;
            
            while (data_left > 0) {
                // Calculate how much we can copy to buffer
                uint32_t space_in_buffer = client->file_buffer_size - client->buffer_data_len;
                uint16_t to_copy = (data_left < space_in_buffer) ? data_left : space_in_buffer;
                
                // Copy to buffer
                memcpy(client->file_buffer + client->buffer_data_len, 
                      data_ptr, to_copy);
                client->buffer_data_len += to_copy;
                client->stor_bytes_received += to_copy;
                data_ptr += to_copy;
                data_left -= to_copy;
                
                // If buffer full, write to SD
                if (client->buffer_data_len >= client->file_buffer_size) {
                    UINT bytes_written;
                    FRESULT res = f_write(&client->stor_file, client->file_buffer, 
                                         client->buffer_data_len, &bytes_written);
                    
                    if (res != FR_OK || bytes_written != client->buffer_data_len) {
                        FTP_LOG("FTP[%p]: Write error: %d\n", client, res);
                        pbuf_free(p);
                        ftp_send_response(client, "426 Write error\r\n");
                        ftp_close_data_connection(client);
                        return ERR_ABRT;
                    }
                    
                    // Reset buffer for next chunk
                    client->buffer_data_len = 0;
                }
            }
        }
    }
    
    // Tell lwIP we processed the data (flow control)
    cyw43_arch_lwip_begin();
    tcp_recved(tpcb, total_len);
    cyw43_arch_lwip_end();
    
    // Free the pbuf
    pbuf_free(p);
    
    return ERR_OK;
}

/**
 * Handle FEAT command - Feature negotiation (RFC 2389)
 * Returns list of supported extensions so clients know what's available
 */
static void ftp_cmd_feat(ftp_client_t *client) {
    FTP_LOG("FTP: FEAT command received\n");
    
    // Send multi-line response (format: 211-Features: ... 211 End)
    ftp_send_response(client, FTP_RESP_211_FEAT_START);
    
    // List supported features (one per line, starting with space)
    ftp_send_response(client, " MDTM\r\n");           // Modification time query
    ftp_send_response(client, " SIZE\r\n");           // File size query
    ftp_send_response(client, " MLST type*;size*;modify*;\r\n"); // MLST facts
    ftp_send_response(client, " MLSD\r\n");           // Machine-readable listing
    ftp_send_response(client, " PASV\r\n");           // Passive mode
    ftp_send_response(client, " MFMT\r\n");           // Modify file time
    ftp_send_response(client, " REST STREAM\r\n");    // Resume transfer
    
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
        FTP_LOG("FTP: MDTM - file not found: %s (err=%d)\n", filepath, res);
        ftp_send_response(client, "550 File not found\r\n");
        return;
    }
    
    if (fno.fattrib & AM_DIR) {
        FTP_LOG("FTP: MDTM - is a directory: %s\n", filepath);
        ftp_send_response(client, "550 Is a directory\r\n");
        return;
    }
    
    // Extract date/time from FAT timestamp
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
    
    FTP_LOG("FTP: MDTM %s -> %04d-%02d-%02d %02d:%02d:%02d\n", 
           filepath, year, month, day, hour, minute, second);
    
    ftp_send_response(client, response);
}

/**
 * Handle MFMT command - set file modification time
 * Format: MFMT YYYYMMDDHHMMSS filename
 * Example: MFMT 20231215143022 /test.txt
 */
static void ftp_cmd_mfmt(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error\r\n");
        return;
    }
    
    // Parse timestamp and filename
    // Format: YYYYMMDDHHMMSS filename
    char timestamp[15];
    const char *filename = NULL;
    
    // Extract timestamp (first 14 characters)
    if (strlen(arg) < 15) {  // At least "YYYYMMDDHHMMSSspace"
        ftp_send_response(client, "501 Invalid timestamp format\r\n");
        return;
    }
    
    strncpy(timestamp, arg, 14);
    timestamp[14] = '\0';
    
    // Find filename (skip timestamp and space)
    filename = arg + 14;
    while (*filename == ' ') filename++;
    
    if (strlen(filename) == 0) {
        ftp_send_response(client, "501 No filename specified\r\n");
        return;
    }
    
    // Build full path
    char filepath[512];
    if (filename[0] == '/') {
        strncpy(filepath, filename, sizeof(filepath) - 1);
        filepath[sizeof(filepath) - 1] = '\0';
    } else {
        snprintf(filepath, sizeof(filepath), "%s/%s", client->cwd, filename);
    }
    
    // Parse timestamp: YYYYMMDDHHMMSS
    FILINFO fno;
    int year, month, day, hour, min, sec;
    
    if (sscanf(timestamp, "%4d%2d%2d%2d%2d%2d", 
               &year, &month, &day, &hour, &min, &sec) != 6) {
        ftp_send_response(client, "501 Invalid timestamp format\r\n");
        return;
    }
    
    // Validate ranges
    if (year < 1980 || year > 2107 ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour < 0 || hour > 23 ||
        min < 0 || min > 59 ||
        sec < 0 || sec > 59) {
        ftp_send_response(client, "501 Timestamp out of range\r\n");
        return;
    }
    
    // Convert to FAT timestamp format
    // FAT date: bits 15-9=year (from 1980), 8-5=month, 4-0=day
    // FAT time: bits 15-11=hour, 10-5=minute, 4-0=second/2
    fno.fdate = ((year - 1980) << 9) | (month << 5) | day;
    fno.ftime = (hour << 11) | (min << 5) | (sec / 2);
    
    // Set file timestamp
    FRESULT res = f_utime(filepath, &fno);
    
    if (res != FR_OK) {
        FTP_LOG("FTP: MFMT failed for %s: %d\n", filepath, res);
        ftp_send_response(client, "550 Could not set file time\r\n");
        return;
    }
    
    FTP_LOG("FTP: MFMT set %s to %s\n", filepath, timestamp);
    
    // Send success response (RFC 3659 format)
    char response[256];
    snprintf(response, sizeof(response), 
             "213 Modify=%s; %s\r\n", timestamp, filename);
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
    
    FTP_LOG("FTP: SIZE %s = %lu bytes\n", filepath, (unsigned long)fno.fsize);
    
    ftp_send_response(client, response);
}

/**
 * Handle DELE command - delete file
 */
static void ftp_cmd_dele(ftp_client_t *client, const char *arg) {
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
        snprintf(filepath, sizeof(filepath), "%s/%s", client->cwd, arg);
    }
    
    // Check if file exists
    FILINFO fno;
    FRESULT res = f_stat(filepath, &fno);
    
    if (res != FR_OK) {
        FTP_LOG("FTP: DELE - file not found: %s (err=%d)\n", filepath, res);
        ftp_send_response(client, "550 File not found\r\n");
        return;
    }
    
    // Check if it's a directory (use RMD for directories)
    if (fno.fattrib & AM_DIR) {
        FTP_LOG("FTP: DELE - is a directory: %s\n", filepath);
        ftp_send_response(client, "550 Is a directory (use RMD)\r\n");
        return;
    }
    
    // Delete the file
    res = f_unlink(filepath);
    
    if (res != FR_OK) {
        FTP_LOG("FTP: DELE - delete failed: %s (err=%d)\n", filepath, res);
        ftp_send_response(client, "550 Delete failed\r\n");
        return;
    }
    
    FTP_LOG("FTP: DELE - deleted: %s\n", filepath);
    ftp_send_response(client, FTP_RESP_250_FILE_OK);
}

/**
 * Handle RNFR command - rename from (step 1 of 2)
 * Stores the source filename and waits for RNTO
 */
static void ftp_cmd_rnfr(ftp_client_t *client, const char *arg) {
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
        snprintf(filepath, sizeof(filepath), "%s/%s", client->cwd, arg);
    }
    
    // Check if file/directory exists
    FILINFO fno;
    FRESULT res = f_stat(filepath, &fno);
    
    if (res != FR_OK) {
        FTP_LOG("FTP: RNFR - file not found: %s (err=%d)\n", filepath, res);
        ftp_send_response(client, "550 File not found\r\n");
        client->pending_rename = false;
        return;
    }
    
    // Store source filename for RNTO command
    strncpy(client->rename_from, filepath, FTP_FILENAME_MAX - 1);
    client->rename_from[FTP_FILENAME_MAX - 1] = '\0';
    client->pending_rename = true;
    
    FTP_LOG("FTP: RNFR - ready to rename: %s\n", filepath);
    ftp_send_response(client, "350 File exists, ready for destination name\r\n");
}

/**
 * Handle RNTO command - rename to (step 2 of 2)
 * Performs the actual rename operation using stored source filename
 */
static void ftp_cmd_rnto(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error: filename required\r\n");
        client->pending_rename = false;
        return;
    }
    
    // Check if RNFR was issued first
    if (!client->pending_rename) {
        ftp_send_response(client, "503 Bad sequence of commands (use RNFR first)\r\n");
        return;
    }
    
    // Build destination path
    char dest_path[512];
    if (arg[0] == '/') {
        // Absolute path
        strncpy(dest_path, arg, sizeof(dest_path) - 1);
        dest_path[sizeof(dest_path) - 1] = '\0';
    } else {
        // Relative path
        snprintf(dest_path, sizeof(dest_path), "%s/%s", client->cwd, arg);
    }
    
    // Perform the rename
    FRESULT res = f_rename(client->rename_from, dest_path);
    
    // Clear pending rename flag
    client->pending_rename = false;
    
    if (res != FR_OK) {
        FTP_LOG("FTP: RNTO - rename failed: %s -> %s (err=%d)\n", 
               client->rename_from, dest_path, res);
        
        if (res == FR_EXIST) {
            ftp_send_response(client, "550 Destination already exists\r\n");
        } else {
            ftp_send_response(client, "550 Rename failed\r\n");
        }
        return;
    }
    
    FTP_LOG("FTP: RNTO - renamed: %s -> %s\n", client->rename_from, dest_path);
    ftp_send_response(client, FTP_RESP_250_FILE_OK);
}

/**
 * Handle MKD command - make directory
 * Also handles XMKD (alternative command name)
 */
static void ftp_cmd_mkd(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error: directory name required\r\n");
        return;
    }
    
    // Build full directory path
    char dirpath[512];
    if (arg[0] == '/') {
        // Absolute path
        strncpy(dirpath, arg, sizeof(dirpath) - 1);
        dirpath[sizeof(dirpath) - 1] = '\0';
    } else {
        // Relative path
        snprintf(dirpath, sizeof(dirpath), "%s/%s", client->cwd, arg);
    }
    
    // Create the directory
    FRESULT res = f_mkdir(dirpath);
    
    if (res != FR_OK) {
        FTP_LOG("FTP: MKD - mkdir failed: %s (err=%d)\n", dirpath, res);
        
        if (res == FR_EXIST) {
            ftp_send_response(client, "550 Directory already exists\r\n");
        } else if (res == FR_NO_PATH) {
            ftp_send_response(client, "550 Parent directory does not exist\r\n");
        } else {
            ftp_send_response(client, "550 Create directory failed\r\n");
        }
        return;
    }
    
    FTP_LOG("FTP: MKD - created directory: %s\n", dirpath);
    
    // Response format: 257 "pathname" created
    char response[600];  // Large enough for 512-byte path + formatting
    snprintf(response, sizeof(response), "257 \"%s\" created\r\n", dirpath);
    ftp_send_response(client, response);
}

/**
 * Handle RMD command - remove directory
 * Also handles XRMD (alternative command name)
 */
static void ftp_cmd_rmd(ftp_client_t *client, const char *arg) {
    if (!arg || strlen(arg) == 0) {
        ftp_send_response(client, "501 Syntax error: directory name required\r\n");
        return;
    }
    
    // Build full directory path
    char dirpath[512];
    if (arg[0] == '/') {
        // Absolute path
        strncpy(dirpath, arg, sizeof(dirpath) - 1);
        dirpath[sizeof(dirpath) - 1] = '\0';
    } else {
        // Relative path
        snprintf(dirpath, sizeof(dirpath), "%s/%s", client->cwd, arg);
    }
    
    // Check if directory exists and is actually a directory
    FILINFO fno;
    FRESULT res = f_stat(dirpath, &fno);
    
    if (res != FR_OK) {
        FTP_LOG("FTP: RMD - directory not found: %s (err=%d)\n", dirpath, res);
        ftp_send_response(client, "550 Directory not found\r\n");
        return;
    }
    
    // Check if it's actually a directory
    if (!(fno.fattrib & AM_DIR)) {
        FTP_LOG("FTP: RMD - not a directory: %s\n", dirpath);
        ftp_send_response(client, "550 Not a directory (use DELE for files)\r\n");
        return;
    }
    
    // Remove the directory (must be empty)
    res = f_unlink(dirpath);
    
    if (res != FR_OK) {
        FTP_LOG("FTP: RMD - remove failed: %s (err=%d)\n", dirpath, res);
        
        if (res == FR_DENIED) {
            ftp_send_response(client, "550 Directory not empty\r\n");
        } else {
            ftp_send_response(client, "550 Remove directory failed\r\n");
        }
        return;
    }
    
    FTP_LOG("FTP: RMD - removed directory: %s\n", dirpath);
    ftp_send_response(client, FTP_RESP_250_FILE_OK);
}

/**
 * Handle NOOP command - no operation (keepalive)
 */
static void ftp_cmd_noop(ftp_client_t *client) {
    FTP_LOG("FTP: NOOP - keepalive\n");
    ftp_send_response(client, "200 OK\r\n");
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
    
    FTP_LOG("FTP: Received command: '%s'\n", cmd);
    
    // Parse command (first word)
    char *arg = strchr(cmd, ' ');
    if (arg) {
        *arg = '\0';
        arg++;
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
            FTP_LOG("FTP: User '%s' requested login\n", client->username);
        } else {
            ftp_send_response(client, FTP_RESP_500_UNKNOWN);
        }
    }
    else if (strcmp(cmd, "PASS") == 0) {
        if (client->state == FTP_STATE_USER_OK) {
            if (strcmp(client->username, FTP_USER) == 0 && 
                arg && strcmp(arg, FTP_PASSWORD) == 0) {
                client->state = FTP_STATE_LOGGED_IN;
                strcpy(client->cwd, "/");
                ftp_send_response(client, FTP_RESP_230_LOGIN_OK);
                FTP_LOG("FTP: User '%s' logged in successfully\n", client->username);
            } else {
                client->state = FTP_STATE_IDLE;
                ftp_send_response(client, FTP_RESP_530_LOGIN_FAILED);
                FTP_LOG("FTP: Login failed for user '%s'\n", client->username);
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
        FTP_LOG("FTP: Client disconnecting\n");
        ftp_close_client(client);
    }
    else if (strcmp(cmd, "SYST") == 0) {
        ftp_send_response(client, FTP_RESP_215_SYSTEM);
    }
    else if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0) {
        ftp_send_response_fmt(client, FTP_RESP_257_PWD, client->cwd);
    }
    else if (strcasecmp(cmd, "TYPE") == 0) {
        if (arg && (arg[0] == 'A' || arg[0] == 'a')) {
            ftp_send_response(client, "504 ASCII mode not supported. Use TYPE I.\r\n");
        } else {
            ftp_send_response(client, "200 Type set to I.\r\n");
        }
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
    else if (strcmp(cmd, "STOR") == 0) {
        ftp_cmd_stor(client, arg);
    }
    else if (strcmp(cmd, "DELE") == 0) {
        ftp_cmd_dele(client, arg);
    }
    else if (strcmp(cmd, "RNFR") == 0) {
        ftp_cmd_rnfr(client, arg);
    }
    else if (strcmp(cmd, "RNTO") == 0) {
        ftp_cmd_rnto(client, arg);
    }
    else if (strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0) {
        ftp_cmd_mkd(client, arg);
    }
    else if (strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0) {
        ftp_cmd_rmd(client, arg);
    }
    else if (strcmp(cmd, "NOOP") == 0) {
        ftp_cmd_noop(client);
    }
    else if (strcmp(cmd, "MDTM") == 0) {
        ftp_cmd_mdtm(client, arg);
    }
    else if (strcmp(cmd, "MFMT") == 0) {
        ftp_cmd_mfmt(client, arg);
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
        FTP_LOG("FTP: Unknown/unimplemented command: %s\n", cmd);
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
        FTP_LOG("FTP: Connection closed by client\n");
        ftp_close_client(client);
        return ERR_OK;
    }
    
    if (p->tot_len > 0) {
        FTP_LOG("FTP: Received %d bytes\n", p->tot_len);
        
        // Copy received data to command buffer (simple line-based protocol)
        uint16_t available = sizeof(client->cmd_buffer) - client->cmd_len - 1;
        uint16_t to_copy = (p->tot_len < available) ? p->tot_len : available;
        
        pbuf_copy_partial(p, client->cmd_buffer + client->cmd_len, to_copy, 0);
        client->cmd_len += to_copy;
        client->cmd_buffer[client->cmd_len] = '\0';  // Null-terminate
        
        FTP_LOG("FTP: Command buffer length: %d bytes\n", client->cmd_len);
        
        // Process complete lines (commands end with \r\n or \n)
        char *line_start = client->cmd_buffer;
        char *line_end;
        
        while ((line_end = strchr(line_start, '\n')) != NULL) {
            *line_end = '\0';  // Replace '\n' with null terminator
            
            // Process this command if not empty
            if (strlen(line_start) > 0) {
                ftp_process_command(client, line_start);
            }
            
            // Move to next line
            line_start = line_end + 1;
        }
        
        // Keep any partial command at start of buffer
        if (line_start > client->cmd_buffer) {
            size_t remaining = strlen(line_start);
            memmove(client->cmd_buffer, line_start, remaining + 1);
            client->cmd_len = remaining;
        }
        
        FTP_LOG("FTP: Command buffer after processing: '%s' (len=%d)\n",
               client->cmd_buffer, client->cmd_len);
        
        // Acknowledge received data
        cyw43_arch_lwip_begin();
        tcp_recved(tpcb, p->tot_len);
        cyw43_arch_lwip_end();
    }
    
    // Free the received buffer
    pbuf_free(p);
    return ERR_OK;
}

/**
 * TCP error callback
 */
static void ftp_error(void *arg, err_t err) {
    ftp_client_t *client = (ftp_client_t *)arg;
    
    if (!client) {
        FTP_LOG("FTP: TCP error %d (no client)\n", err);
        return;
    }
    
    int slot = (int)(client - ftp_clients);
    LWIP_UNUSED_ARG(slot);  // Used only in FTP_LOG (debug builds)
    FTP_LOG("FTP[%p]: TCP error %d on slot %d\n", client, err, slot);
    
    if (!client->active) {
        FTP_LOG("FTP[%p]: Client already inactive, ignoring error\n", client);
        return;
    }
    
    // CRITICAL: Close data connection FIRST before marking inactive!
    // Otherwise slot appears "free" but data connection still exists
    ftp_close_data_connection(client);
    
    // Close any open file
    if (client->retr_file_open) {
        f_close(&client->retr_file);
        client->retr_file_open = false;
        FTP_LOG("FTP[%p]: Closed open file handle\n", client);
    }
    
    // Close any open upload file
    if (client->stor_file_open) {
        f_close(&client->stor_file);
        client->stor_file_open = false;
        FTP_LOG("FTP[%p]: Closed open upload file handle\n", client);
    }
    
    // Free file buffer
    if (client->file_buffer) {
        free(client->file_buffer);
        client->file_buffer = NULL;
        FTP_LOG("FTP[%p]: Freed file buffer\n", client);
    }
    
    // Now mark slot as free
    client->pcb = NULL;
    client->active = false;
    
    FTP_LOG("FTP[%p]: Slot %d cleaned up and marked FREE\n", client, slot);
}

/**
 * Close client connection
 */
static void ftp_close_client(ftp_client_t *client) {
    if (!client || !client->active) {
        return;
    }
    
    FTP_LOG("FTP: Closing client connection\n");
    
    // Close data connection first
    ftp_close_data_connection(client);
    
    // Close control connection
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
    client->cwd[0] = '\0';
    client->pending_stor = false;
    client->pending_rename = false;
    client->active = false;
}

/**
 * TCP accept callback
 */
static err_t ftp_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    LWIP_UNUSED_ARG(arg);
    
    if (err != ERR_OK || newpcb == NULL) {
        FTP_LOG("FTP: Accept error, err=%d, newpcb=%p\n", err, newpcb);
        return ERR_VAL;
    }
    
    FTP_LOG("FTP: New client connection from %s:%d\n",
           ipaddr_ntoa(&newpcb->remote_ip), newpcb->remote_port);
    
    // Find an available client slot
    ftp_client_t *client = NULL;
    for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
        if (!ftp_clients[i].active) {
            client = &ftp_clients[i];
            break;
        }
    }
    
    if (!client) {
        FTP_LOG("FTP: Maximum number of clients reached, rejecting connection\n");
        cyw43_arch_lwip_begin();
        tcp_close(newpcb);
        cyw43_arch_lwip_end();
        return ERR_MEM;
    }
    
    // Initialize client structure
    memset(client, 0, sizeof(ftp_client_t));
    client->pcb = newpcb;
    client->state = FTP_STATE_IDLE;
    client->active = true;
    strcpy(client->cwd, "/");
    
    // Set callbacks
    tcp_arg(newpcb, client);
    tcp_recv(newpcb, ftp_recv);
    tcp_err(newpcb, ftp_error);
    
    // Send welcome message
    ftp_send_response(client, FTP_RESP_220_WELCOME);
    
    return ERR_OK;
}

/**
 * Initialize FTP server with FatFS
 */
bool ftp_server_init(FATFS *fs) {
    printf("FTP: Initializing server on port %d\n", FTP_PORT);
    
    if (!fs) {
        FTP_LOG("FTP: FatFS filesystem required\n");
        return false;
    }
    
    g_fs = fs;  // Store filesystem pointer
    memset(ftp_clients, 0, sizeof(ftp_clients));
    
    // Create new TCP PCB for FTP server
    cyw43_arch_lwip_begin();
    ftp_server_pcb = tcp_new();
    cyw43_arch_lwip_end();
    
    if (!ftp_server_pcb) {
        FTP_LOG("FTP: Failed to create server PCB\n");
        return false;
    }
    
    // Bind FTP server to port 21 on any IP
    err_t err;
    cyw43_arch_lwip_begin();
    err = tcp_bind(ftp_server_pcb, IP_ADDR_ANY, FTP_PORT);
    cyw43_arch_lwip_end();
    
    if (err != ERR_OK) {
        FTP_LOG("FTP: Failed to bind server PCB, err=%d\n", err);
        cyw43_arch_lwip_begin();
        tcp_close(ftp_server_pcb);
        cyw43_arch_lwip_end();
        ftp_server_pcb = NULL;
        return false;
    }
    
    // Put into listening state
    cyw43_arch_lwip_begin();
    ftp_server_pcb = tcp_listen(ftp_server_pcb);
    cyw43_arch_lwip_end();
    
    if (!ftp_server_pcb) {
        FTP_LOG("FTP: Failed to listen on server PCB\n");
        return false;
    }
    
    // Set accept callback
    cyw43_arch_lwip_begin();
    tcp_accept(ftp_server_pcb, ftp_accept);
    cyw43_arch_lwip_end();
    
    printf("FTP: Server started successfully\n");
    
    return true;
}

/**
 * Process FTP server (placeholder)
 */
void ftp_server_process(void) {
    // CRITICAL: This function is currently lightweight. The FTP server uses
    // lwIP callbacks for control and data connections. This function is provided
    // as a hook for future periodic tasks if needed.
}

/**
 * Shutdown FTP server
 */
void ftp_server_shutdown(void) {
    FTP_LOG("FTP: Shutting down server\n");
    
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
    
    FTP_LOG("FTP: Server shutdown complete\n");
}