#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

// ============================================================================
// IMPORTANT: This configuration is ONLY used when in WiFi mode!
// When in Amiga mode, FreeRTOS is never started and Core 1 runs par_spi.c
// with exclusive IRQ handling and strict timing (~200-300ns response).
// ============================================================================

// Use FreeRTOS (ONLY in WiFi mode - initialized in ftp_server_main())
#define NO_SYS                         0  // FreeRTOS provides OS layer
#define LWIP_SOCKET                    1  // Enable BSD socket API
#define LWIP_NETCONN                   1  // Required for socket API

// Provide errno manually
#define LWIP_PROVIDE_ERRNO             1

// FreeRTOS integration
#define SYS_LIGHTWEIGHT_PROT           1
#define LWIP_TCPIP_CORE_LOCKING        1

// Enable threading features (used by FreeRTOS)
#define LWIP_NETIF_API                 1

// Keep timers functional
#define LWIP_TIMERS                    1
#define LWIP_MPU_COMPATIBLE            1

// Protocol support
#define LWIP_DNS                       1
#define LWIP_ICMP                      1
#define LWIP_RAW                       1
#define LWIP_TCP                       1
#define LWIP_UDP                       1
#define LWIP_DHCP                      1

#define LWIP_TIMEVAL_PRIVATE           0

// ============================================================================
// Socket API Configuration for FTP Server
// ============================================================================

// Use lwip_ prefix (not POSIX compatibility mode)
#define LWIP_COMPAT_SOCKETS            0
#define LWIP_POSIX_SOCKETS_IO_NAMES    0

// Socket options for FTP
#define LWIP_SO_RCVTIMEO               1
#define LWIP_SO_SNDTIMEO               1
#define LWIP_SO_RCVBUF                 1
#define LWIP_SO_LINGER                 1

// Increase TCP resources for FTP (control + data connections)
#define MEMP_NUM_NETCONN               8
#define MEMP_NUM_TCP_PCB               8
#define MEMP_NUM_TCP_PCB_LISTEN        4

// TCP window and buffer sizes
#define TCP_MSS                        1460
#define TCP_WND                        (8 * TCP_MSS)
#define TCP_SND_BUF                    (8 * TCP_MSS)
#define MEMP_NUM_TCP_SEG               32

// Memory pool
#define MEM_SIZE                       (16 * 1024)  // 16KB heap
#define PBUF_POOL_SIZE                 24

// ============================================================================
// FreeRTOS Configuration
// ============================================================================

// These match the Pico SDK FreeRTOS examples
#define TCPIP_THREAD_STACKSIZE         2048
#define TCPIP_THREAD_PRIO              8
#define DEFAULT_THREAD_STACKSIZE       512
#define DEFAULT_THREAD_PRIO            1

#endif