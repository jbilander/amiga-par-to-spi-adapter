# Shared SD Card Synchronization Strategy: RP2350 ↔ Amiga

## Overview

This strategy enables safe bidirectional access to a single SD card shared between:

* **RP2350 MCU (FTP server)** — Core 0: runs SdFat filesystem (FatFS commented as alternative)
* **Amiga (via spisd.device)** — Core 1: presents the SD card as a block device using fat95

Both cores share the same SPI bus. The goal is to allow **all commands** currently used in `device.c` to be handled safely, with critical commands protected by a semaphore.

Explicit core assignment ensures safe separation of tasks: Core 0 = FTP server, Core 1 = Amiga SPI interface.

---

## Key Principles

1. **Single active filesystem per core:** Each core maintains its own filesystem in RAM. All writes must be flushed before notifying the other side.
2. **Bidirectional “media-dirty” signaling:**

   * RP2350 writes → Amiga refresh (TD_UPDATE + remount)
   * Amiga writes → RP2350 refresh (flush + remount)
3. **Global semaphore for SD/FS access:** All filesystem-modifying operations use a semaphore to ensure only one core accesses the SD card at a time.
4. **Explicit Core Assignment:**

   * Core 0: runs `main()` and the FTP server loop (`ftp_server_init()` / `ftp_server_loop()`).
   * Core 1: runs `par_spi_main_core1()` launched via `multicore_launch_core1()`.

---

## Command Classification and Handling

| Command           | Critical? | Handling Notes                                                                                   |
| ----------------- | --------- | ------------------------------------------------------------------------------------------------ |
| CMD_RESET         | YES       | Acquire semaphore → reset driver/SD → flush → release → notify other core                        |
| CMD_READ          | NO        | Read-only; handle normally; optional brief semaphore for consistent read                         |
| CMD_WRITE         | YES       | Acquire semaphore → write → flush → release → notify                                             |
| CMD_UPDATE        | YES       | Acquire semaphore → refresh FAT/dir → release → notify; use remount if external changes detected |
| CMD_CLEAR         | YES       | Acquire semaphore → clear blocks → flush → release → notify                                      |
| TD_MOTOR          | NO        | Drive motor on/off; handle normally                                                              |
| TD_FORMAT         | YES       | Acquire semaphore → format → flush → release → notify                                            |
| TD_REMOVE         | YES       | Acquire semaphore → remove file/dir → flush → release → notify                                   |
| TD_CHANGENUM      | NO        | Metadata only; handle normally                                                                   |
| TD_CHANGESTATE    | NO        | Status only; handle normally                                                                     |
| TD_PROTSTATUS     | NO        | Status only; handle normally                                                                     |
| TD_GETDRIVETYPE   | NO        | Status only; handle normally                                                                     |
| TD_ADDCHANGEINT   | NO        | Subscribe to change interrupts; handle normally                                                  |
| TD_REMCHANGEINT   | NO        | Remove change interrupts; handle normally                                                        |
| TD_GETGEOMETRY    | NO        | Query geometry; handle normally                                                                  |
| TD_READ64         | NO        | Read-only; handle normally; optional semaphore for consistency                                   |
| TD_WRITE64        | YES       | Acquire semaphore → write → flush → release → notify                                             |
| TD_FORMAT64       | YES       | Acquire semaphore → format variant → flush → release → notify                                    |
| NSCMD_DEVICEQUERY | NO        | Status/control only; handle normally                                                             |
| NSCMD_TD_READ64   | NO        | Read-only; handle normally; optional semaphore for consistency                                   |
| NSCMD_TD_WRITE64  | YES       | Acquire semaphore → write → flush → release → notify                                             |

---

## Multi-Core Refresh Workflow Using SdFat

### Core 0 (FTP Server) Handling Amiga Writes

```c
void check_dirty_and_refresh() {
    if (atomic_exchange(&media_dirty_from_amiga, 0) > 0) {
        acquire_sd_fs(); // semaphore to protect SD card

        // Step 1: Flush all open files (SdFat)
        currentFile.sync();

        // Step 2: Close filesystem instance (SdFat)
        sd.end();
        // FatFS alternative:
        // f_mount(NULL, "", 0);

        // Step 3: Re-mount filesystem (SdFat)
        sd.begin();
        // FatFS alternative:
        // f_mount(&FatFs, "", 1);

        release_sd_fs();
    }
}
```

### Core 0 Example: Handling a STOR Command (FTP Upload)

```c++
void handle_ftp_stor_command(const char* filename, const uint8_t* data, size_t length) {
    acquire_sd_fs(); // Protect SD card during write

    SdFile file;
    if (!file.open(filename, O_RDWR | O_CREAT | O_TRUNC)) {
        // handle error
        release_sd_fs();
        return;
    }

    size_t written = file.write(data, length); // Write FTP data to SD card
    if (written != length) {
        // handle partial write or error
    }

    file.sync();  // flush buffer to SD card
    file.close(); // close file and update FAT

    release_sd_fs();

    // Notify Amiga that media changed
    par_spi_send_command(CMD_DIRTY);
}
```

* `filename` comes from the FTP STOR command.
* `data` contains the uploaded file contents.
* Semaphore ensures **no concurrent writes** from the Amiga core.
* `file.sync()` ensures buffered data is committed before releasing the semaphore.
* `CMD_DIRTY` notifies the Amiga to refresh its filesystem view.

---

### Core 1 (Amiga) Handling FTP Server Writes

```c
void on_ftp_dirty_notification() {
    sem_acquire_blocking(&sd_fs_sem);

    // Step 1: Flush any open files (Fat95)
    for (each open Fat95 file) {
        flush_file_buffers(f); // equivalent to ACTION_FLUSH
    }

    // Step 2: Re-read filesystem state
    do_action_die();      // discard caches
    mount_filesystem();   // re-read FAT, directories

    sem_release(&sd_fs_sem);
}
```

**Notes:**

* TD_UPDATE alone is insufficient for external changes. Always flush and remount.
* Open file handles are lost during remount; any ongoing writes must be closed and reopened.
* Semaphore ensures that remounts and writes are atomic across cores.

---

## RP2350 Firmware Integration

### Main Firmware (`main.c`) - Core 0 (FTP server)

```c
#include "pico/multicore.h"
#include "pico/sem.h"
#include "SdFat.h"
#include "par_spi.h"

semaphore_t sd_fs_sem;

int main() {
    sd.begin();
    init_sd_fs_sem();
    ftp_server_init(); // Runs on Core 0

    // Launch par_spi on Core 1
    multicore_launch_core1(par_spi_main_core1);

    while(1) {
        check_dirty_and_refresh();

        ftp_server_loop(); // may call handle_ftp_stor_command() when receiving STOR commands
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

### Diagram

```
Core 0 (FTP) --- check_dirty_and_refresh() ---> flush + remount FS (SdFat)
Core 0 (FTP) --- STOR command ---> write file via handle_ftp_stor_command()
Core 1 (par_spi) --- CMD_ACQUIRE_SEM / CMD_DIRTY ---> Core 0 semaphore
Amiga SPI commands ----> Core 1 handles all commands
```

This workflow now includes a **realistic FTP STOR file write example**, fully integrated with the multi-core semaphore and filesystem synchronization strategy.
