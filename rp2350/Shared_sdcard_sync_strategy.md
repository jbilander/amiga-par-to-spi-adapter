# Shared SD Card Synchronization Strategy: RP2350 ↔ Amiga

## Overview

This strategy enables safe bidirectional access to a single SD card shared between:

* **RP2350 MCU (FTP server)** — Core 0: runs SdFat or FatFS filesystem
* **Amiga (via spisd.device)** — Core 1: presents the SD card as a block device using fat95

Both cores share the same SPI bus. The goal is to allow writes, file creation, and formatting from either side without corrupting data.

---

## Key Principles

1. **Single active filesystem per core:** Each core maintains its own filesystem in RAM. All writes must be flushed before notifying the other side.
2. **Bidirectional “media-dirty” signaling:**

   * RP2350 writes → Amiga refresh (TD_UPDATE)
   * Amiga writes → RP2350 refresh (SdFat or FatFS remount/reopen)
3. **Global semaphore for SD/FS access:** All filesystem operations (writes, file creation, FAT updates, formatting) use a semaphore to ensure only one core accesses the SD card at a time.

---

## Supported Critical Commands

| Command          | Description                    | Semaphore Handling                                                        |
| ---------------- | ------------------------------ | ------------------------------------------------------------------------- |
| TD_WRITE         | Standard sector write          | acquire semaphore → write → flush → release semaphore → notify other core |
| TD_WRITE64       | Multi-sector write             | same as TD_WRITE                                                          |
| NSCMD_TD_WRITE64 | Custom large write             | same as TD_WRITE                                                          |
| TD_FORMAT        | Format filesystem              | acquire semaphore → format → flush → release → notify                     |
| TD_FORMAT64      | Large format variant           | same as TD_FORMAT                                                         |
| CMD_WRITE        | Custom write command           | same as TD_WRITE                                                          |
| CMD_UPDATE       | Metadata refresh / FAT reload  | acquire semaphore → refresh metadata → release → notify                   |
| CMD_CLEAR        | Clear sectors / zero blocks    | acquire semaphore → clear → flush → release → notify                      |
| TD_REMOVE        | File or directory deletion     | acquire semaphore → remove → flush → release → notify                     |
| CMD_RESET        | Reset interface / driver state | acquire semaphore → reset → release → notify                              |
| CMD_ACQUIRE_SEM  | Explicit acquire semaphore     | acquire semaphore → respond OK                                            |
| CMD_RELEASE_SEM  | Explicit release semaphore     | release semaphore → respond OK                                            |

---

## RP2350 Firmware: Core 0 (FTP Server) and Core 1 (par_spi.c) Integration

### 1. Main Firmware (`main.c`) integrating par_spi.c

```c
#include "pico/multicore.h"
#include "pico/sem.h"
#include "SdFat.h"
#include "par_spi.h" // include current par_spi.c header

semaphore_t sd_fs_sem;

int main() {
    // Initialize SD card and semaphore
    sd.begin();
    init_sd_fs_sem();
    ftp_server_init(); // Core 0 FTP server

    // Launch par_spi application on Core 1
    multicore_launch_core1(par_spi_main_core1);

    while(1) {
        // Check if Amiga has modified media
        check_dirty_and_refresh();

        // FTP server operations
        acquire_sd_fs();
        f_open(...);
        f_write(...);
        f_close(...);
        release_sd_fs();

        ftp_server_loop();
    }
}
```

### 2. Core 1: par_spi.c integrated for semaphore handling

```c
#include "par_spi.h"
#include "pico/sem.h"
#include "sd_fs_sem.h" // shared semaphore

void par_spi_main_core1() {
    par_spi_init();

    while(1) {
        uint8_t cmd = par_spi_poll();

        switch(cmd) {
            case CMD_ACQUIRE_SEM:
                sem_acquire_blocking(&sd_fs_sem);
                par_spi_send_status(STATUS_OK);
                break;

            case CMD_RELEASE_SEM:
                sem_release(&sd_fs_sem);
                par_spi_send_status(STATUS_OK);
                break;

            case TD_WRITE:
            case TD_WRITE64:
            case NSCMD_TD_WRITE64:
            case TD_FORMAT:
            case TD_FORMAT64:
            case CMD_WRITE:
            case CMD_UPDATE:
            case CMD_CLEAR:
            case TD_REMOVE:
            case CMD_RESET:
                sem_acquire_blocking(&sd_fs_sem);
                handle_command(cmd, ...); // original par_spi.c command handling
                sem_release(&sd_fs_sem);
                par_spi_send_command(CMD_DIRTY); // notify FTP core
                break;

            default:
                // Read-only or status commands handled normally
                break;
        }
    }
}
```

**Notes:**

* The existing `par_spi.c` logic is preserved inside `handle_command()` calls.
* Semaphore ensures that any critical SD card operation from the Amiga is serialized with FTP server operations.
* Core 1 is exclusively running `par_spi_main_core1`, while Core 0 runs FTP server and handles media refresh.

---

### Diagram

```
Core 0 (FTP) --- check_dirty_and_refresh() ---> refresh local FS
Core 1 (par_spi) --- CMD_ACQUIRE_SEM / CMD_DIRTY ---> Core 0 semaphore
Amiga SPI commands ----> Core 1 handled via semaphore
```

This integration allows the **current par_spi.c implementation to run on Core 1** safely while using the semaphore to synchronize SD card access with the FTP server on Core 0.
