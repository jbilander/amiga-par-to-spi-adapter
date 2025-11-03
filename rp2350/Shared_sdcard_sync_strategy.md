# RP2350 Dual-Core SD Card Synchronization with Amiga

## Overview
This setup shares a single SD card between:

- **Amiga** — running `spisd.device` (fat95 filesystem in RAM)  
- **RP2350 Core 0** — FTP server (direct SD card access via SdFat/FatFS)  
- **RP2350 Core 1** — parallel SPI interface (`par_spi.c`) acting as a **block-level bridge**  

Key goals:

- Preserve **current `par_spi.c` behavior** for the Amiga.  
- Allow **FTP server writes** without corrupting Amiga view.  
- Ensure Amiga sees **latest filesystem updates** via `CMD_REFRESH`.  
- Provide **atomic access** via a semaphore.

---

## Architecture Diagram
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

---

## Core 0 — FTP Server with Semaphore

```c++
#include "SdFat.h"
#include "pico/sync.h"
#include <stdatomic.h>
#include "par_spi.h"

SdFat sd;
semaphore_t sd_semaphore;
atomic_bool sd_writing = false;

void ftp_init() {
    sem_init(&sd_semaphore, 1, 1); // binary semaphore
    sd.begin();
}

void ftp_write_file(const char* filename, const uint8_t* data, size_t length) {
    sem_acquire_blocking(&sd_semaphore);
    atomic_store(&sd_writing, true);

    File f = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        f.write(data, length);
        f.sync();
        f.close();
    }

    par_spi_send_command(CMD_REFRESH); // notify Amiga fat95 cache

    atomic_store(&sd_writing, false);
    sem_release(&sd_semaphore);
}
```

> Notes:
> - All SD card writes (file creation, modification, formatting, removal) **acquire the semaphore**.  
> - After write, **CMD_REFRESH** is sent to Amiga to update fat95.  

---

## Core 1 — par_spi.c (Minimal Changes)

```c
#include "par_spi.h"
#include <stdatomic.h>

extern semaphore_t sd_semaphore;
extern atomic_bool sd_writing;

void handle_amiga_command(int cmd, command_params_t* params) {
    switch(cmd) {
        // Critical writes: wait if FTP server is writing
        case TD_WRITE:
        case TD_WRITE64:
        case NSCMD_TD_WRITE64:
        case TD_FORMAT:
        case TD_FORMAT64:
        case TD_REMOVE:
        case CMD_CLEAR:
        case CMD_UPDATE:
        case CMD_RESET:
            while (atomic_load(&sd_writing)) {
                tight_loop_contents(); // wait for Core 0 write to finish
            }
            // proceed with existing block-level write logic
            do_block_write(params->sector, params->buffer, params->count);
            break;

        // Reads: optionally block if FTP write active
        case CMD_READ:
        case TD_READ64:
        case NSCMD_TD_READ64:
            while (atomic_load(&sd_writing)) {
                tight_loop_contents();
            }
            do_block_read(params->sector, params->buffer, params->count);
            break;

        default:
            // handle other commands unchanged
            process_non_critical_command(cmd, params);
            break;
    }
}
```

> Notes:
> - **No SdFat calls in Core 1** — it remains a block-level bridge.  
> - Reads can optionally spin while Core 0 writes.  
> - Writes/format/remove commands **wait for Core 0 semaphore** if active.  

---

## Amiga Side — `spisd.device` CMD_REFRESH

Inside `process_request()`:

```c
#define CMD_REFRESH 0xF0

void process_request(IORequest* req) {
    int cmd = req->cmd;

    switch(cmd) {
        // existing commands
        case CMD_REFRESH:
            // Invalidate fat95 caches so latest SD card contents are visible
            if (fat95_invalidate_cache() != 0) {
                kprintf("spisd.device: CMD_REFRESH failed\n");
            }
            fat95_refresh_directories(); // optional
            break;

        default:
            // other commands unchanged
            break;
    }
}
```

> - `process_request()` is called by `begin_io()`.  
> - No other changes are needed in `spisd.device`.  

---

## Command Flow Summary

| Source        | Operation                    | Semaphore | Notes |
|---------------|-----------------------------|-----------|-------|
| FTP server    | Write, format, remove       | ✅        | Acquire semaphore |
| FTP server    | Read                        | ❌        | Optional: block if needed |
| Amiga         | Write, format, remove       | Wait if FTP active | Uses block-level SPI |
| Amiga         | Read                        | Wait if FTP active | Uses block-level SPI |
| Core 0 → Amiga| CMD_REFRESH                 | ❌        | Updates fat95 cache |

---

## Notes

- This approach **keeps `par_spi.c` intact** — only minor spinlocks added to block Amiga writes/reads if Core 0 is writing.  
- All **FTP server writes** are safe and flushed before notifying Amiga.  
- **Amiga fat95 filesystem** sees updates via `CMD_REFRESH` without remounting.  
- No changes to Amiga block command protocol, minimal impact on existing firmware.  
- Core 0 handles **SdFat/FatFS**, Core 1 remains **block-level SPI bridge**.

