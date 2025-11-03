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

From `spisd/device.c` around line 186, the following commands **must be treated as critical** and wrapped with the SD/FS semaphore:

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

> Note: Only commands actually present in `device.c` are included. Directory creation/removal commands like `TD_MKDIR` or `TD_RMDIR` do not exist in this driver version.

---

## RP2350 Firmware Logic (Multi-Client Safe using Semaphore)

```c
#include "pico/sem.h" // RP2040 SDK semaphore
#include "SdFat.h" // or FatFS

semaphore_t sd_fs_sem;

void init_sd_fs_sem() {
    sem_init(&sd_fs_sem, 1, 1); // binary semaphore initialized to 1
}

void acquire_sd_fs() {
    sem_acquire_blocking(&sd_fs_sem);
}

void release_sd_fs() {
    sem_release(&sd_fs_sem);
}

void check_dirty_and_refresh() {
    if (atomic_exchange(&media_dirty_from_amiga, 0) > 0) {
        acquire_sd_fs();
        // FatFS example
        f_mount(NULL, "", 0);
        f_mount(&FatFs, "", 1);
        // SdFat example
        sd.end();
        sd.begin();
        release_sd_fs();
    }
}
```

---

### Core 1: Parallel SPI Interface (Amiga) Example with Semaphore

```c
void core1_par_spi_main() {
    par_spi_init();

    while(1) {
        par_spi_poll();

        switch(cmd) {
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
                acquire_sd_fs();
                handle_command(cmd, ...); // perform write, format, remove, clear, reset, or metadata refresh
                release_sd_fs();

                par_spi_send_command(CMD_DIRTY); // notify FTP core
                break;
            default:
                // non-critical commands, read-only or status
                break;
        }
    }
}
```

---

### Notes & Recommendations

* Semaphore ensures only one core accesses the SD card at a time, providing safer multi-core operation than a spinlock.
* Flush all writes before releasing the semaphore (f_sync() or sd.sync()).
* Use the RP2040 SDK semaphore API (`sem_init`, `sem_acquire_blocking`, `sem_release`).
* Fat95 version ≥ 3.18 is required for TD_UPDATE.

---

### Diagram

```
Amiga --- CMD_DIRTY ---> RP2350 FTP core
RP2350 FTP core --- CMD_REFRESH ---> Amiga
```

Result: Safe, atomic, bidirectional SD card access across cores using a semaphore for synchronization.
