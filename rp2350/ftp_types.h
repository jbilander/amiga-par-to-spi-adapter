/* ftp_types.h - FTP server types and constants for Pico 2 W */

#ifndef FTP_TYPES_H
#define FTP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "lwip/ip_addr.h"

// ============================================================================
// FTP Protocol Constants
// ============================================================================

#define FTP_CTRL_PORT           21      // Standard FTP control port
#define FTP_DATA_PORT_MIN       50000   // Passive mode port range start
#define FTP_DATA_PORT_MAX       50010   // Passive mode port range end

#define FTP_CMD_BUFFER_SIZE     256     // Maximum FTP command line length
#define FTP_PATH_MAX_LEN        255     // Maximum path length
#define FTP_FILENAME_MAX        255     // Maximum filename length
#define FTP_USERNAME_MAX        32      // Maximum username length
#define FTP_PASSWORD_MAX        32      // Maximum password length
#define FTP_RESPONSE_MAX        256     // Maximum response line length

#define FTP_TRANSFER_BUFFER     4096    // File transfer buffer size (4KB)

#define FTP_TIMEOUT_MS          300000  // 5 minute timeout (300000ms)
#define FTP_MAX_USERS           4       // Maximum number of users

// ============================================================================
// FTP Commands (Enum)
// ============================================================================

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
    FTP_CMD_MFMT,       // Modify file modification time
    FTP_CMD_MFCT,       // Modify file creation time
    FTP_CMD_XMKD,       // Make directory (alternative)
    FTP_CMD_XRMD,       // Remove directory (alternative)
} ftp_command_t;

// ============================================================================
// FTP Client State Machine
// ============================================================================

typedef enum {
    FTP_STATE_IDLE,         // Waiting for USER command
    FTP_STATE_USER,         // Username received, waiting for PASS
    FTP_STATE_AUTHENTICATED // Authenticated, ready for commands
} ftp_client_state_t;

// ============================================================================
// FTP Transfer Type
// ============================================================================

typedef enum {
    FTP_TYPE_ASCII,         // ASCII mode (with CRLF conversion)
    FTP_TYPE_BINARY         // Binary mode (no conversion)
} ftp_transfer_type_t;

// ============================================================================
// FTP Data Connection Mode
// ============================================================================

typedef enum {
    FTP_DATA_MODE_NONE,     // No data connection established
    FTP_DATA_MODE_PASSIVE,  // Passive mode (PASV) - we listen
    FTP_DATA_MODE_ACTIVE    // Active mode (PORT) - we connect
} ftp_data_mode_t;

// ============================================================================
// FTP Transfer State
// ============================================================================

typedef enum {
    FTP_TRANSFER_NONE,      // No transfer in progress
    FTP_TRANSFER_LIST,      // Sending directory listing
    FTP_TRANSFER_RETR,      // Sending file (download)
    FTP_TRANSFER_STOR       // Receiving file (upload)
} ftp_transfer_state_t;

// ============================================================================
// FTP User Structure
// ============================================================================

typedef struct {
    char username[FTP_USERNAME_MAX];
    char password[FTP_PASSWORD_MAX];
} ftp_user_t;

// ============================================================================
// FTP Path Structure
// ============================================================================

typedef struct {
    char path[FTP_PATH_MAX_LEN];    // Current working directory
} ftp_path_t;

// ============================================================================
// FTP Session Structure (one client)
// ============================================================================

typedef struct {
    // Control connection
    int ctrl_sock;                      // Control socket (-1 if not connected)
    ip_addr_t client_addr;              // Client IP address
    uint16_t client_port;               // Client port
    
    // State machine
    ftp_client_state_t state;           // Current authentication state
    char username[FTP_USERNAME_MAX];    // Username being authenticated
    
    // Working directory
    ftp_path_t cwd;                     // Current working directory
    
    // Transfer settings
    ftp_transfer_type_t transfer_type;  // ASCII or Binary
    ftp_data_mode_t data_mode;          // Passive or Active
    
    // Data connection (passive mode)
    int data_listen_sock;               // Passive mode listening socket
    int data_sock;                      // Active data connection socket
    uint16_t pasv_port;                 // Port we're listening on (passive)
    
    // Data connection (active mode)
    ip_addr_t active_client_addr;       // Client IP for active mode
    uint16_t active_client_port;        // Client port for active mode
    
    // Transfer state
    ftp_transfer_state_t transfer_state; // Current transfer operation
    void *transfer_context;             // Transfer-specific context (e.g., FIL*)
    
    // Command parsing
    char cmd_buffer[FTP_CMD_BUFFER_SIZE]; // Incoming command buffer
    uint16_t cmd_len;                   // Current command length
    
    // Rename operation (RNFR/RNTO)
    bool rename_from_set;               // RNFR command received
    char rename_from[FTP_PATH_MAX_LEN]; // Source path for rename
    
    // Timeout tracking
    uint32_t last_activity_ms;          // Last activity timestamp
} ftp_session_t;

// ============================================================================
// FTP Server Structure
// ============================================================================

typedef struct {
    // Listening socket
    int listen_sock;                    // Control connection listening socket
    
    // User database
    ftp_user_t users[FTP_MAX_USERS];    // User credentials
    uint8_t user_count;                 // Number of users configured
    
    // Session (single client support)
    ftp_session_t session;              // Active client session
    
    // Passive port allocation
    uint16_t next_pasv_port;            // Next passive port to try
} ftp_server_t;

#endif // FTP_TYPES_H
