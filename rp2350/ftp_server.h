/* ftp_server.h - FTP server for Pico 2 W with FatFS backend */

#ifndef FTP_SERVER_H
#define FTP_SERVER_H

#include "ftp_types.h"
#include "ff.h"  // FatFS

// ============================================================================
// FTP Server API
// ============================================================================

/**
 * Initialize FTP server
 * @param server Server structure to initialize
 * @param fs FatFS filesystem pointer
 * @return true on success, false on failure
 */
bool ftp_server_init(ftp_server_t *server, FATFS *fs);

/**
 * Add a user to the server
 * @param server Server structure
 * @param username Username (max 31 chars)
 * @param password Password (max 31 chars)
 * @return true on success, false if user list full
 */
bool ftp_server_add_user(ftp_server_t *server, const char *username, const char *password);

/**
 * Start FTP server (begin listening)
 * @param server Server structure
 * @return true on success, false on failure
 */
bool ftp_server_begin(ftp_server_t *server);

/**
 * Handle FTP server operations (call regularly in main loop)
 * @param server Server structure
 */
void ftp_server_handle(ftp_server_t *server);

/**
 * Stop FTP server and close all connections
 * @param server Server structure
 */
void ftp_server_stop(ftp_server_t *server);

/**
 * Check if a client is connected
 * @param server Server structure
 * @return true if client connected
 */
bool ftp_server_has_client(const ftp_server_t *server);

// ============================================================================
// Internal Functions (exposed for testing)
// ============================================================================

/**
 * Send FTP response to client
 * @param session Client session
 * @param code FTP response code (e.g., 220, 331)
 * @param message Response message
 */
void ftp_send_response(ftp_session_t *session, int code, const char *message);

/**
 * Process received command line
 * @param server Server structure
 * @param session Client session
 */
void ftp_process_command(ftp_server_t *server, ftp_session_t *session);

/**
 * Close client session
 * @param session Client session
 */
void ftp_close_session(ftp_session_t *session);

#endif // FTP_SERVER_H
