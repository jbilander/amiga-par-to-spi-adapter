#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

// Use the Raw API (not Sequential API)
#define NO_SYS                         1
#define LWIP_NETCONN                   0
#define LWIP_SOCKET                    0

// Provide errno manually (since NO_SYS=1)
#define LWIP_PROVIDE_ERRNO             1

// Disable sys layer features not available without an OS
#define SYS_LIGHTWEIGHT_PROT           0

// Disable or adjust any threading or semaphore features
#define LWIP_TCPIP_CORE_LOCKING        0
#define LWIP_NETIF_API                 0
#define LWIP_NETIF_LOOPBACK            0

// Keep timers functional
#define LWIP_TIMERS                    1
#define LWIP_MPU_COMPATIBLE            1

// Optional, depending on your stack usage
#define LWIP_DNS                       1
#define LWIP_ICMP                      1
#define LWIP_RAW                       1
#define LWIP_TCP                       1
#define LWIP_UDP                       1

#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_DHCP 1

#endif