/*
 * spitest.c
 *
 * Small test program that uses the spi-lib API to send bytes
 * to the RP2350 via the parallel-to-SPI adapter.
 *
 * Uses functions from spi.h / spi.c in this repo:
 *   int spi_initialize(void (*change_isr)());
 *   void spi_set_speed(long speed);
 *   void spi_write(const unsigned char *buf, unsigned long size);
 *   void spi_shutdown();
 *
 * Compile with the same flags/toolchain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spi.h"

/* Busy-wait delay (approx) - adjust loops for timing on your Amiga */
static void short_delay_ms(unsigned int ms)
{
    volatile unsigned long i, j;
    for (i = 0; i < ms; ++i) {
        for (j = 0; j < 6000; ++j) {
            /* burn some cycles, approximate delay */
            //asm volatile("" ::: "memory");
        }
    }
}

static void send_sequence(const unsigned char *buf, unsigned long len, const char *label)
{
    printf("=== Sending %s, %lu bytes ===\n", label, len);
    /* spi_write will manage the REQ/CLK/ACT handshake internally */
    spi_write((const unsigned char *)buf, len);
    printf("-> sent\n");
    /* small gap so Pico can process/debug output if needed */
    short_delay_ms(50);
}

/* Helper: generate incremental pattern and send */
static void send_incremental(unsigned long size)
{
    unsigned char *b = malloc(size);
    if (!b) {
        printf("malloc failed for size %lu\n", size);
        return;
    }
    for (unsigned long i = 0; i < size; ++i) b[i] = (unsigned char)(i & 0xFF);
    send_sequence(b, size, "incremental pattern");
    free(b);
}

int main(int argc, char **argv)
{
    printf("spitest: starting\n");

    /* initialize spi library. We don't need change_isr for this test, pass NULL */
    int initres = spi_initialize(NULL);
    if (initres < 0) {
        printf("spi_initialize failed: %d\n", initres);
        return 1;
    }

    /* test slow speed first */
    spi_set_speed(SPI_SPEED_SLOW);
    printf("Set SPI speed: SLOW\n");

    /* Test A: single bytes (WRITE1 path expected) */
    {
        unsigned char testA[] = { 0x00, 0xFF, 0xAA, 0x55, 0x12, 0x34, 0xAB, 0xCD };
        send_sequence(testA, sizeof(testA), "short single-bytes");
    }

    /* small pause */
    short_delay_ms(200);

    /* Test B: medium-length burst (should exercise WRITE2) */
    send_incremental(200);   /* adjust size: e.g. 200 bytes */

    short_delay_ms(200);

    /* Test C: interactive mode - read hex pairs from stdin and send them immediately */
    printf("\nInteractive hex input mode.\n");
    printf("Type hex pairs separated by spaces or newlines, finish line with ENTER.\n");
    printf("Example: 'DE AD BE EF' -> will send 4 bytes.\n");
    printf("Type 'q' or Ctrl-C to quit interactive mode.\n\n");

    char line[512];
    while (1) {
        printf("> ");
        if (!fgets(line, sizeof(line), stdin)) break;

        /* trim newline */
        char *p = line;
        while (*p && (*p == ' ' || *p == '\t')) ++p;
        if (*p == 'q' || *p == 'Q') break;

        unsigned char outbuf[256];
        unsigned int outlen = 0;

        char *tok = strtok(line, " \t\r\n");
        while (tok && outlen < sizeof(outbuf)) {
            unsigned int val;
            if (sscanf(tok, "%x", &val) == 1) {
                outbuf[outlen++] = (unsigned char)(val & 0xFF);
            } else {
                printf("Ignored token: %s\n", tok);
            }
            tok = strtok(NULL, " \t\r\n");
        }

        if (outlen) {
            send_sequence(outbuf, outlen, "interactive bytes");
        }
    }

    printf("Interactive done. Cleaning up.\n");

    spi_shutdown();

    printf("spitest: exit\n");
    return 0;
}
