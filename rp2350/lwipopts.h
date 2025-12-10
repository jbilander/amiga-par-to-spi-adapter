#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/************************************************************
 * SYSTEM CONFIGURATION (NO OS)
 ************************************************************/

#define NO_SYS                          1     /* Raw API only */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_PROVIDE_ERRNO              1

#define SYS_LIGHTWEIGHT_PROT            1
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
 ************************************************************/

/*
 * lwIP heap used for PCBs, TCP segments, pbufs, timers, etc.
 * FTP with 2 clients needs a large heap to avoid fragmentation stalls.
 * 
 * 128 KB is a solid value and leaves plenty for:
 * FreeRTOS, stacks, SD buffers, and your FTP file buffers.
 */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (128 * 1024)

/************************************************************
 * MEMP MEMORY POOLS — PCBs, segments, headers
 ************************************************************/

#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_TCP_PCB                10
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                256
#define MEMP_NUM_SYS_TIMEOUT            20

/************************************************************
 * PBUF POOL — RX PACKETS (Wi-Fi MTU = 1500)
 ************************************************************/
#define PBUF_POOL_SIZE                  32
#define PBUF_POOL_BUFSIZE               1600   /* safe for full Ethernet frame */

/************************************************************
 * TCP SETTINGS — HIGH THROUGHPUT
 ************************************************************/

/* MTU 1500 → MSS = 1460 */
#define TCP_MSS                         1460

/*
 * TCP Receive Window:
 * 32 × MSS ≈ 46.7 KB
 * Large window prevents WiFi stalls.
 */
#define TCP_WND                         (32 * TCP_MSS)

/*
 * TCP Send Buffer:
 * 24 × MSS ≈ 35 KB
 * Big enough to allow multiple 8 KB SD chunks in-flight.
 */
#define TCP_SND_BUF                     (24 * TCP_MSS)

/*
 * TCP Send Queue Length:
 * Rule: 4 × SND_BUF/MSS
 */
#define TCP_SND_QUEUELEN                (4 * (TCP_SND_BUF / TCP_MSS))

/* Oversize writes help during streaming from SD */
#define TCP_OVERSIZE                    TCP_MSS

/* Disable OOSEQ buffering to save RAM */
#define TCP_QUEUE_OOSEQ                 0

/* Keepalives help stability on Wi-Fi */
#define LWIP_TCP_KEEPALIVE              1

/************************************************************
 * WINDOW SCALING
 * RWND < 64 KB → scaling OFF is correct.
 ************************************************************/
#define LWIP_WND_SCALE                  0

/************************************************************
 * CHECKSUM SETTINGS
 ************************************************************/
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1

/************************************************************
 * DEBUGGING
 ************************************************************/
#define LWIP_DEBUG                      0
/* #define TCP_DEBUG                    LWIP_DBG_ON */
/* #define TCP_OUTPUT_DEBUG            LWIP_DBG_ON */

#endif /* __LWIPOPTS_H__ */
