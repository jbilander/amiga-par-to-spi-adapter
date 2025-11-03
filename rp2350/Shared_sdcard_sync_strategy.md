# Shared SD Card Synchronization Strategy: RP2350 ↔ Amiga

## Overview

This strategy enables safe bidirectional access to a single SD card shared between:

* **RP2350 MCU (FTP server)** — Core 0: runs SdFat or FatFS filesystem
* **Amiga (via spisd.device)** — Core 1: presents the SD card as a block device using fat95

Both cores share the same SPI bus. The goal is to allow **all commands** currently used in `device.c` to be handled safely, with critical commands protected by a semaphore.

---

## Key Principles

1. **Single active filesystem per core:** Each core maintains its own filesystem in RAM. All writes must be flushed before notifying the other side.
2. **Bidirectional “media-dirty” signaling:**

   * RP2350 writes → Amiga refresh (TD_UPDATE)
   * Amiga writes → RP2350 refresh (SdFat or FatFS remount/reopen)
3. **Global semaphore for SD/FS access:** All filesystem-modifying operations use a semaphore to ensure only one core accesses the SD card at a time.

---

## Command Classification and Handling

| Command           | Critical? | Handling Notes                                                            |
| ----------------- | --------- | ------------------------------------------------------------------------- |
| CMD_RESET         | YES       | Acquire semaphore → reset driver/SD → flush → release → notify other core |
| CMD_READ          | NO        | Read-only; handle normally; optional brief semaphore for consistent read  |
| CMD_WRITE         | YES       | Acquire semaphore → write → flush → release → notify                      |
| CMD_UPDATE        | YES       | Acquire semaphore → refresh FAT/dir → release → notify                    |
| CMD_CLEAR         | YES       | Acquire semaphore → clear blocks → flush → release → notify               |
| TD_MOTOR          | NO        | Drive motor on/off; handle normally                                       |
| TD_FORMAT         | YES       | Acquire semaphore → format → flush → release → notify                     |
| TD_REMOVE         | YES       | Acquire semaphore → remove file/dir → flush → release → notify            |
| TD_CHANGENUM      | NO        | Metadata only; handle normally                                            |
| TD_CHANGESTATE    | NO        | Status only; handle normally                                              |
| TD_PROTSTATUS     | NO        | Status only; handle normally                                              |
| TD_GETDRIVETYPE   | NO        | Status only; handle normally                                              |
| TD_ADDCHANGEINT   | NO        | Subscribe to change interrupts; handle normally                           |
| TD_REMCHANGEINT   | NO        | Remove change interrupts; handle normally                                 |
| TD_GETGEOMETRY    | NO        | Query geometry; handle normally                                           |
| TD_READ64         | NO        | Read-only; handle normally; optional semaphore for consistency            |
| TD_WRITE64        | YES       | Acquire semaphore → write → flush → release → notify                      |
| TD_FORMAT64       | YES       | Acquire semaphore → format variant → flush → release → notify             |
| NSCMD_DEVICEQUERY | NO        | Status/control only; handle normally                                      |
| NSCMD_TD_READ64   | NO        | Read-only; handle normally; optional semaphore for consistency            |
| NSCMD_TD_WRITE64  | YES       | Acquire semaphore → write → flush → release → notify                      |

---

## RP2350 Firmware Integration

### Main Firmware (`main.c`)

```c
#include "pico/multicore.h"
#include "pico/sem.h"
#include "SdFat.h"
#include "par_spi.h"

semaphore_t sd_fs_sem;

int main() {
    sd.begin();
    init_sd_fs_sem();
    ftp_server_init();

    // Launch par_spi on Core 1
    multicore_launch_core1(par_spi_main_core1);

    while(1) {
        check_dirty_and_refresh();

        acquire_sd_fs();
        // FTP server operations
        f_open(...);
        f_write(...);
        f_close(...);
        release_sd_fs();

        ftp_server_loop();
    }
}
```

### Core 1: par_spi.c Integration

```c
#include "par_spi.h"
#include "pico/sem.h"
#include "sd_fs_sem.h"

void par_spi_main_core1() {
    par_spi_init();

    while(1) {
        uint8_t cmd = par_spi_poll();

        switch(cmd) {
            // Semaphore control
            case CMD_ACQUIRE_SEM:
                sem_acquire_blocking(&sd_fs_sem);
                par_spi_send_status(STATUS_OK);
                break;
            case CMD_RELEASE_SEM:
                sem_release(&sd_fs_sem);
                par_spi_send_status(STATUS_OK);
                break;

            // Critical commands
            case CMD_RESET:
            case CMD_WRITE:
            case CMD_UPDATE:
            case CMD_CLEAR:
            case TD_FORMAT:
            case TD_REMOVE:
            case TD_WRITE64:
            case TD_FORMAT64:
            case NSCMD_TD_WRITE64:
                sem_acquire_blocking(&sd_fs_sem);
                handle_command(cmd, ...);
                sem_release(&sd_fs_sem);
                par_spi_send_command(CMD_DIRTY);
                break;

            // Read-only / status / control commands
            case CMD_READ:
            case TD_READ64:
            case NSCMD_TD_READ64:
            case TD_MOTOR:
            case TD_CHANGENUM:
            case TD_CHANGESTATE:
            case TD_PROTSTATUS:
            case TD_GETDRIVETYPE:
            case TD_ADDCHANGEINT:
            case TD_REMCHANGEINT:
            case TD_GETGEOMETRY:
            case NSCMD_DEVICEQUERY:
                handle_command(cmd, ...);
                break;

            default:
                // unknown command handling
                break;
        }
    }
}
```

### Notes

* All currently used commands in `device.c` are accounted for.
* Semaphore is used only for commands that modify SD card content or filesystem metadata.
* Read and status commands are handled normally; optional semaphore acquisition can ensure consistency if required.

---

### Diagram

```
Core 0 (FTP) --- check_dirty_and_refresh() ---> refresh local FS
Core 1 (par_spi) --- CMD_ACQUIRE_SEM / CMD_DIRTY ---> Core 0 semaphore
Amiga SPI commands ----> Core 1 handled for all commands
```

This ensures **full command handling with safe multi-core SD card synchronization**.
