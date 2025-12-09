/**
 * ftp_types.h - Type definitions and constants for raw lwIP FTP server
 * 
 * Part of Pico 2 W FTP Server using raw lwIP API
 * Non-blocking event-driven architecture with FatFS integration
 */

#ifndef FTP_TYPES_H
#define FTP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "lwip/tcp.h"
#include "ff.h"  // FatFS

// ============================================================================
// FTP Protocol Constants
// ============================================================================

#define FTP_PORT                21          // Standard FTP control port
#define FTP_DATA_PORT_MIN       50000       // Passive mode port range start
#define FTP_DATA_PORT_MAX       50099       // Passive mode port range end

#define FTP_CMD_BUFFER_SIZE     256         // Maximum FTP command line length
#define FTP_PATH_MAX_LEN        256         // Maximum path length
#define FTP_FILENAME_MAX        256         // Maximum filename length
#define FTP_USERNAME_MAX        32          // Maximum username length
#define FTP_PASSWORD_MAX        32          // Maximum password length

#define FTP_MAX_CLIENTS         8           // Maximum simultaneous clients, 2 means 2 active sessions,
                                            // BUT you need EXTRA slots for cleanup delays!

#define FTP_FILE_BUFFER_MAX     (256*1024)  // 256KB max RAM buffer per transfer

/* FTP transfer tuning: streaming buffer and max TCP chunk size */
#define FTP_STREAM_BUFFER_SIZE   (64 * 1024)   /* 64KB streaming buffer for large files */
#define FTP_MAX_CHUNK_SIZE       8192          /* Max bytes per tcp_write call */

// ============================================================================
// FTP Response Code Strings
// ============================================================================

#define FTP_RESP_150_OPENING_DATA   "150 Opening data connection\r\n"
#define FTP_RESP_200_TYPE_OK        "200 Type set to I\r\n"
#define FTP_RESP_211_FEAT_START     "211-Features:\r\n"
#define FTP_RESP_211_FEAT_END       "211 End\r\n"
#define FTP_RESP_214_HELP           "214 Help: USER PASS QUIT SYST PWD TYPE PASV LIST MLSD NLST CWD CDUP RETR MDTM SIZE FEAT\r\n"
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

// ============================================================================
// FTP Client State Machine
// ============================================================================

/**
 * FTP path structure
 * Used for working directory management
 */
typedef struct {
    char path[FTP_PATH_MAX_LEN];    // Current working directory
} ftp_path_t;

/**
 * FTP command enumeration
 * Used by ftp_parse_command() in ftp_utils.c
 */
typedef enum {
    FTP_CMD_NONE = 0,
    FTP_CMD_USER,       // Username for authentication
    FTP_CMD_PASS,       // Password for authentication
    FTP_CMD_QUIT,       // Disconnect
    FTP_CMD_SYST,       // System type
    FTP_CMD_NOOP,       // No operation (keepalive)
    FTP_CMD_FEAT,       // List features
    FTP_CMD_PWD,        // Print working directory
    FTP_CMD_CWD,        // Change working directory
    FTP_CMD_CDUP,       // Change to parent directory
    FTP_CMD_TYPE,       // Set transfer type (ASCII/Binary)
    FTP_CMD_PASV,       // Enter passive mode
    FTP_CMD_PORT,       // Enter active mode (client IP:port)
    FTP_CMD_LIST,       // List directory (detailed)
    FTP_CMD_NLST,       // Name list (simple)
    FTP_CMD_MLSD,       // Machine-readable list
    FTP_CMD_RETR,       // Retrieve (download) file
    FTP_CMD_STOR,       // Store (upload) file
    FTP_CMD_DELE,       // Delete file
    FTP_CMD_MKD,        // Make directory
    FTP_CMD_RMD,        // Remove directory
    FTP_CMD_RNFR,       // Rename from
    FTP_CMD_RNTO,       // Rename to
    FTP_CMD_ABOR,       // Abort transfer
    FTP_CMD_OPTS,       // Set options
    FTP_CMD_MDTM,       // Get file modification time
    FTP_CMD_SIZE,       // Get file size
    FTP_CMD_MFMT,       // Modify file modification time
    FTP_CMD_MFCT,       // Modify file creation time
    FTP_CMD_XMKD,       // Make directory (alternative)
    FTP_CMD_XRMD,       // Remove directory (alternative)
} ftp_command_t;

/**
 * Client authentication state
 */
typedef enum {
    FTP_STATE_IDLE = 0,         // Initial state, waiting for USER
    FTP_STATE_USER_OK,          // USER received, waiting for PASS
    FTP_STATE_LOGGED_IN         // Authenticated and ready for commands
} ftp_state_t;

// ============================================================================
// FTP Data Connection Structure
// ============================================================================

/**
 * Data connection state (for PASV mode file transfers)
 * Manages the separate TCP connection used for data transfer
 */
typedef struct ftp_data_conn {
    struct tcp_pcb *listen_pcb;             // Listening PCB for PASV mode
    struct tcp_pcb *pcb;                    // Active data connection PCB
    uint16_t port;                          // Port we're listening on (PASV)
    volatile bool waiting_for_connection;   // True when waiting for client to connect
    volatile bool connected;                // True when data connection established
    volatile bool transfer_complete;        // True when transfer done, ready to close
} ftp_data_conn_t;

// ============================================================================
// FTP Client Structure
// ============================================================================

/**
 * FTP client session structure
 * Represents one connected FTP client with control and data connections
 */
typedef struct ftp_client {
    // Control connection
    struct tcp_pcb *pcb;                    // Control connection PCB
    ftp_state_t state;                      // Authentication state
    char cmd_buffer[FTP_CMD_BUFFER_SIZE];   // Incoming command buffer
    uint16_t cmd_len;                       // Current command length
    char username[FTP_USERNAME_MAX];        // Username for authentication
    char cwd[FTP_PATH_MAX_LEN];             // Current working directory
    bool active;                            // Connection active flag
    
    // Data connection
    ftp_data_conn_t data_conn;              // Data connection state
    
    // Pending operations (waiting for data connection)
    bool pending_list;                      // LIST command pending
    bool pending_mlsd;                      // MLSD command pending
    bool pending_retr;                      // RETR command pending
    bool pending_stor;                      // STOR command pending
    bool pending_rename;                    // RNFR received, waiting for RNTO
    char retr_filename[FTP_FILENAME_MAX];   // Filename for pending RETR
    char stor_filename[FTP_FILENAME_MAX];   // Filename for pending STOR
    char rename_from[FTP_FILENAME_MAX];     // Source filename for RNFR/RNTO
    
    // File transfer state (downloads)
    FIL retr_file;                          // FatFS file handle for RETR
    bool retr_file_open;                    // True if file is open for reading
    uint32_t retr_bytes_sent;               // Total bytes sent in transfer
    
    // File transfer state (uploads)
    FIL stor_file;                          // FatFS file handle for STOR
    bool stor_file_open;                    // True if file is open for writing
    uint32_t stor_bytes_received;           // Total bytes received in transfer
    bool stor_use_buffer;                   // True if using RAM buffering for small file
    uint32_t stor_expected_size;            // Expected file size (0 if unknown)
    
    // RAM buffering for efficient transfers (used for both RETR and STOR)
    uint8_t *file_buffer;                   // RAM buffer for file data
    uint32_t file_buffer_size;              // Size of allocated buffer
    uint32_t file_buffer_pos;               // Current position in buffer
    uint32_t buffer_data_len;               // Valid data currently in buffer
    uint32_t buffer_send_pos;               // Streaming: current send position (RETR only)
    bool sending_in_progress;               // Re-entry guard for callbacks
} ftp_client_t;

#endif // FTP_TYPES_H