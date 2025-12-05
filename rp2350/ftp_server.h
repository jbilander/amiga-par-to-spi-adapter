/**
 * ftp_server.h - Function declarations for raw lwIP FTP server
 * 
 * Part of Pico 2 W FTP Server using raw lwIP API
 * Non-blocking event-driven architecture with FatFS integration
 */

#ifndef FTP_SERVER_H
#define FTP_SERVER_H

#include "ftp_types.h"
#include "ff.h"  // FatFS

// ============================================================================
// FTP Server Initialization and Control
// ============================================================================

/**
 * Initialize FTP server
 * Sets up the TCP listening socket on port 21
 * 
 * @param fs Pointer to mounted FatFS filesystem
 * @return true on success, false on failure
 */
bool ftp_server_init(FATFS *fs);

/**
 * Shutdown FTP server
 * Closes all connections and frees resources
 */
void ftp_server_shutdown(void);

#endif // FTP_SERVER_H