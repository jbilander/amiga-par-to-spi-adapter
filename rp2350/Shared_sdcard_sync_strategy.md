# Shared SD Card Synchronization Strategy: RP2350 ↔ Amiga

## Overview
This document describes how to safely share a single SD card between an Amiga computer and an RP2350 microcontroller with two cores — one acting as a block-level interface bridge for the Amiga, and the other running an FTP server that also accesses the SD card. It integrates the current `spisd.device` commands with a hybrid semaphore strategy and uses proper header-based integration for `par_spi.c`.

## Architecture Summary
- **RP2350 Core 0 (FTP Server):** Runs SdFat (or FatFS), handles FTP commands (STOR, RETR), flushes files, sends `CMD_REFRESH` to Amiga.
- **RP2350 Core 1 (Parallel SPI Bridge):** Implements Amiga parallel port protocol using `par_spi.c` (via `par_spi.h`), provides block-level access, sends `CMD_DIRTY` when Amiga writes/format/remove.
- **Amiga (fat95 filesystem):** Holds FAT16/FAT32 state in RAM, performs block-level I/O via `spisd.device`. Responds to CMD_REFRESH by flushing/remounting filesystem.
- **Synchronization:** Hybrid semaphore + atomic flag ensures writes are atomic, reads are non-blocking unless a write is in progress.

## Data Flow Diagram
             +-----------------------------+
             |           Amiga             |
             |     fat95 filesystem        |
             +--------------+--------------+
                            |
                            | Parallel Port (Block I/O)
                            v
     +---------------------------------------------------+
     |                    RP2350 MCU                    |
     |                                                   |
     |  +--------------------+    +--------------------+ |
     |  |   Core 1           |    |   Core 0           | |
     |  | par_spi.c          |    | FTP Server (SdFat) | |
     |  | Parallel ↔ SPI I/F |    | Filesystem Access  | |
     |  +---------+----------+    +----------+---------+ |
     |            \\__________________________/           |
     |                   Shared SPI Bus                  |
     +-----------------------------+---------------------+
                                   |
                                   v
                          +----------------+
                          |    SD Card     |
                          +----------------+

## Synchronization Mechanism
Hybrid Lock Strategy: Writes acquire semaphore, reads wait only if a write is active. Atomic flag `sd_writing` indicates active write.

```c
#include <stdatomic.h>
#include <pico/sync.h>

semaphore_t sd_semaphore;
atomic_bool sd_writing = false;

void acquire_write() {
    sem_acquire_blocking(&sd_semaphore);
    atomic_store(&sd_writing, true);
}

void release_write() {
    atomic_store(&sd_writing, false);
    sem_release(&sd_semaphore);
}

void read_sd_block_safe(...) {
    while (atomic_load(&sd_writing)) {
        tight_loop_contents();
    }
    do_block_read(...);
}
```

## Core 0 (FTP Server) — SdFat Example
```c++
#include "SdFat.h"
#include "pico/multicore.h"
#include "par_spi.h"

extern semaphore_t sd_semaphore;
extern atomic_bool sd_writing;

SdFat sd;
File ftp_file;

void ftp_handle_stor(const char* filename, const uint8_t* data, size_t length) {
    acquire_write();
    if (!sd.begin()) { release_write(); return; }

    ftp_file.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    ftp_file.write(data, length);
    ftp_file.sync();
    ftp_file.close();

    par_spi_send_command(CMD_REFRESH); // notify Amiga
    release_write();
}
```

(FatFS alternative)
```c
/*
f_mount(&FatFs, "", 1);
f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
f_write(&fil, data, length, &bw);
f_sync(&fil);
f_close(&fil);
*/
```

## Core 1 (Parallel SPI Bridge) — par_spi.c Integration with device.c commands
```c
#include "par_spi.h"    // par_spi_init(), par_spi_get_command(), par_spi_send_command()
#include "device.h"     // handle_device_command()
#include <stdatomic.h>
#include <pico/sync.h>

extern semaphore_t sd_semaphore;
extern atomic_bool sd_writing;

void core1_main() {
    par_spi_init();

    while (true) {
        int cmd = par_spi_get_command();

        switch(cmd) {
            // Reads (non-blocking unless write active)
            case CMD_READ:
            case TD_READ64:
            case NSCMD_TD_READ64:
                read_sd_block_safe(...);
                break;

            // Critical writes / FAT changes / formatting / remove / resets
            case CMD_WRITE:
            case TD_WRITE:
            case TD_WRITE64:
            case NSCMD_TD_WRITE64:
            case TD_FORMAT:
            case TD_FORMAT64:
            case CMD_CLEAR:
            case TD_REMOVE:
            case CMD_UPDATE:
            case CMD_RESET:
            case TD_MOTOR:
            case TD_CHANGENUM:
            case TD_CHANGESTATE:
            case TD_PROTSTATUS:
            case TD_GETDRIVETYPE:
            case TD_ADDCHANGEINT:
            case TD_REMCHANGEINT:
            case TD_GETGEOMETRY:
            case NSCMD_DEVICEQUERY:
                acquire_write();
                handle_device_command(cmd, ...);
                release_write();
                if (cmd != CMD_READ && cmd != TD_READ64 && cmd != NSCMD_TD_READ64) {
                    par_spi_send_command(CMD_DIRTY); // notify FTP core
                }
                break;

            default:
                handle_non_critical_command(cmd);
                break;
        }
    }
}
```

## Filesystem Synchronization Sequence
1. **FTP Server → Amiga:** Writes file(s), flushes, sets `sd_writing=false`, sends CMD_REFRESH. Amiga flushes/remounts FAT.
2. **Amiga → FTP Server:** Writes via fat95 through Core 1. Core 1 sends CMD_DIRTY. FTP core refreshes filesystem (`sd.end(); sd.begin();`).

## Summary Table (Hybrid Lock)

| Operation | Needs Semaphore | Notes |
|-----------|----------------|-------|
| FTP read | ❌ unless sd_writing active | Safe if no write ongoing |
| FTP write | ✅ | Block reads/writes |
| Amiga read | ❌ unless sd_writing active | Safe if no write ongoing |
| Amiga write / format / mkdir / delete | ✅ | Acquire semaphore |
| Filesystem refresh / remount | ✅ | Acquire semaphore |

## Minimal par_spi.h
```c
#ifndef PAR_SPI_H
#define PAR_SPI_H

#include <stdint.h>

void par_spi_init(void);
int par_spi_get_command(void);
void par_spi_send_command(uint8_t cmd);
void do_block_read(uint32_t sector, uint8_t* buffer);
void do_block_write_or_format(uint32_t sector, const uint8_t* buffer, uint32_t count);

#endif
```

## Minimal device.h
```c
#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

void handle_device_command(int cmd, void* params);
void handle_non_critical_command(int cmd);

#endif
```

## Summary
- Maximizes read concurrency while maintaining atomic writes.
- Semaphore + atomic flag prevents read/write races.
- Integrates cleanly with existing `par_spi.c` and `device.c` commands.
- Ensures both FTP clients and Amiga see consistent filesystem state.
