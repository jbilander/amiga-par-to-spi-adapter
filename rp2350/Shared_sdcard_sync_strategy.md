# Shared SD Card Synchronization Strategy: RP2350 ↔ Amiga

## Overview

This document describes how to safely share a single SD card between an **Amiga computer** and an **RP2350 microcontroller** with **two cores** — one acting as a **block-level interface bridge** for the Amiga, and the other running an **FTP server** that also accesses the SD card.

---

### 🧠 Architecture Summary

- **RP2350 Core 0 (FTP Server):**  
  - Runs a full filesystem using **SdFat** (or FatFS as an alternative).  
  - Handles FTP commands such as `STOR` (file upload) and `RETR` (file download).  
  - Performs filesystem-level reads and writes to the SD card.  
  - Flushes and closes all files after operations, and sends **CMD_REFRESH** to notify the Amiga.

- **RP2350 Core 1 (Parallel SPI Bridge):**  
  - Implements the **Amiga parallel port protocol** using the existing `par_spi.c`.  
  - Provides **block-level access** (read/write sectors) between the Amiga and SD card.  
  - Does **not** maintain a filesystem.  
  - Sends **CMD_DIRTY** to Core 0 whenever the Amiga writes, formats, or otherwise modifies the card.

- **Amiga (fat95 filesystem):**  
  - The Amiga OS holds the **fat95 FAT16/FAT32 filesystem state in RAM**.  
  - It performs **block-level I/O** over the parallel port interface via `spisd.device`.  
  - When notified via `CMD_REFRESH`, the Amiga can **flush and remount** the filesystem to see the latest changes.

- **Synchronization and Safety:**  
  - A **hardware semaphore** ensures only one core accesses the SD card at a time.  
  - **Dirty/refresh signaling** keeps both filesystem views in sync.  
  - Critical filesystem updates (write, format, file create/delete) are atomic and protected.

---

### 📊 Data Flow Diagram

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
     |            \__________________________/           |
     |                   Shared SPI Bus                  |
     +-----------------------------+---------------------+
                                   |
                                   v
                          +----------------+
                          |    SD Card     |
                          +----------------+

---

## Synchronization Mechanism

### Key Concepts

1. **Semaphore-controlled access** — ensures that only one core (FTP or SPI bridge) can access the SD card at a time.  
2. **Dirty flag notifications** — used to inform the other side that filesystem contents have changed.  
3. **Filesystem refresh** — handled through flush, unmount/remount, or reopen logic depending on the filesystem library.

---

### Semaphore Implementation

```c
#include <pico/multicore.h>
#include <pico/sync.h>
#include <atomic>

semaphore_t sd_semaphore;

void acquire_sd_fs() {
    sem_acquire_blocking(&sd_semaphore);
}

void release_sd_fs() {
    sem_release(&sd_semaphore);
}
```

### Dirty Flag Handling

```c
#include <stdatomic.h>

atomic_uint media_dirty_from_amiga = 0;

void notify_dirty_from_amiga() {
    atomic_fetch_add(&media_dirty_from_amiga, 1);
}

void check_dirty_and_refresh(SdFat &sd) {
    if (atomic_exchange(&media_dirty_from_amiga, 0) > 0) {
        acquire_sd_fs();
        // Reinitialize the filesystem
        sd.end();
        sd.begin();
        release_sd_fs();
    }
}
```

---

## Core 0 (FTP Server) — SdFat Example with STOR Command

```c
#include "SdFat.h"
#include "pico/multicore.h"

extern semaphore_t sd_semaphore;
extern void par_spi_send_command(uint8_t cmd);

SdFat sd;
File ftp_file;

void ftp_handle_stor(const char* filename, uint8_t* data, size_t length) {
    acquire_sd_fs();

    if (!sd.begin()) {
        printf("SD init failed\n");
        release_sd_fs();
        return;
    }

    ftp_file.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    ftp_file.write(data, length);
    ftp_file.sync();
    ftp_file.close();

    // Notify Amiga side
    par_spi_send_command(CMD_REFRESH);

    release_sd_fs();
}
```

*(FatFS alternative)*
```c
/*
f_mount(&FatFs, "", 1);
f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
f_write(&fil, data, length, &bw);
f_sync(&fil);
f_close(&fil);
*/
```

---

## Core 1 (Parallel SPI Bridge) — par_spi.c Integration

Core 1 runs the Amiga block-level I/O interface. It handles all commands defined in `device.c`, including reads, writes, and formatting.

Example modification:

```c
void core1_par_spi_main() {
    par_spi_init();

    while (true) {
        int cmd = par_spi_get_command();

        switch (cmd) {
            case CMD_READ:
            case TD_READ64:
            case NSCMD_TD_READ64:
                acquire_sd_fs();
                do_block_read(...);
                release_sd_fs();
                break;

            case CMD_WRITE:
            case TD_WRITE:
            case TD_WRITE64:
            case NSCMD_TD_WRITE64:
            case TD_FORMAT:
            case TD_FORMAT64:
            case CMD_CLEAR:
            case TD_REMOVE:
                acquire_sd_fs();
                do_block_write_or_format(...);
                release_sd_fs();
                par_spi_send_command(CMD_DIRTY);
                break;

            default:
                handle_non_critical_command(cmd);
                break;
        }
    }
}
```

---

## Filesystem Synchronization Sequence

### 1️⃣ FTP Server → Amiga
- FTP server writes file(s) via SdFat.  
- Flush and close all files.  
- Send **CMD_REFRESH** to the Amiga.  
- Amiga performs `ACTION_FLUSH`, then `ACTION_DIE`, and remounts the device to refresh the FAT.

### 2️⃣ Amiga → FTP Server
- Amiga writes via fat95 (through Core 1 block I/O).  
- Core 1 sends **CMD_DIRTY** to Core 0.  
- Core 0 checks dirty flag and calls `sd.end(); sd.begin();` to reload filesystem.

---

### Summary

This system allows:

- Safe, semaphore-controlled shared SD access between two cores.  
- FTP uploads (`STOR`) and Amiga writes (`TD_WRITE`, `TD_FORMAT`, etc.) to coexist.  
- Consistent filesystem state via flush + remount synchronization.  
- Minimal change to existing `spisd.device` and `par_spi.c` structure.

---
