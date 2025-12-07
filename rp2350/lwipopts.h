#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/************************************************************
 * SYSTEM CONFIGURATION (NO OS)
 ************************************************************/

#define NO_SYS                          1     /* Raw API only */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_PROVIDE_ERRNO              1

/* Some internal lightweight protection is still useful */
#define SYS_LIGHTWEIGHT_PROT            1

/* Timers required for TCP, DHCP, ARP, retransmit, etc. */
#define LWIP_TIMERS                     1
#define LWIP_MPU_COMPATIBLE             1


/************************************************************
 * CORE PROTOCOL MODULES
 ************************************************************/

#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_DNS                        1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_DHCP                       1

#define LWIP_TIMEVAL_PRIVATE            0


/************************************************************
 * MEMORY CONFIGURATION (RP2350: 520 KB RAM)
 * lwIP HEAP — used for PCBs, TCP segments, pbufs, timers, etc.
 *
 * Recommended: 100–160 KB for high-throughput Wi-Fi FTP
 ************************************************************/

#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (128 * 1024)   /* 128 KB heap */


/************************************************************
 * MEMP MEMORY POOLS — PCBs, segments, pbuf headers
 ************************************************************/

#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_TCP_PCB                10     /* 2 clients + server + data PCBs */
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                128    /* Increased for big tx window */
#define MEMP_NUM_SYS_TIMEOUT            20


/************************************************************
 * PBUF POOL — RX PACKETS (Wi-Fi MTU = 1500)
 ************************************************************/

#define PBUF_POOL_SIZE                  32
#define PBUF_POOL_BUFSIZE               1600   /* 1520 minimal, 1600 safer */


/************************************************************
 * TCP SETTINGS — PERFORMANCE TUNING
 ************************************************************/

/* MTU 1500 → TCP MSS ~1460 (IPv4) */
#define TCP_MSS                         1460

/* TCP Receive Window (RWND)
 * Larger window = fewer stalls at Wi-Fi layer.
 * 32 * MSS ≈ 46 KB
 */
#define TCP_WND                         (32 * TCP_MSS)

/* TCP Send Buffer (SND_BUF)
 * Bigger = lwIP can queue more data before waiting for ACKs.
 * 16 * MSS ≈ 23 KB
 */
#define TCP_SND_BUF                     (16 * TCP_MSS)

/* Send queue segments: rule of thumb = 2 × (SND_BUF / MSS) */
#define TCP_SND_QUEUELEN                (4 * TCP_SND_BUF / TCP_MSS)

/* Oversize writes improve throughput during streaming */
#define TCP_OVERSIZE                    TCP_MSS

/* Disable OOSEQ buffering to save RAM */
#define TCP_QUEUE_OOSEQ                 0

/* Keepalives useful for Wi-Fi */
#define LWIP_TCP_KEEPALIVE              1


/************************************************************
 * WINDOW SCALING (optional)
 * If you want RWND > 64 KB, enable this.
 * 
 * For now RWND ≈ 46 KB → scaling not needed.
 ************************************************************/

#define LWIP_WND_SCALE                  0
/* #define TCP_RCV_SCALE                0 */


/************************************************************
 * CHECKSUM SETTINGS — SOFTWARE
 ************************************************************/

#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1


/************************************************************
 * DEBUGGING (optional)
 ************************************************************/

#define LWIP_DEBUG                      0
/* Enable to debug TCP window issues */
/* #define TCP_DEBUG                   LWIP_DBG_ON */
/* #define TCP_OUTPUT_DEBUG           LWIP_DBG_ON */

#endif /* __LWIPOPTS_H__ */
