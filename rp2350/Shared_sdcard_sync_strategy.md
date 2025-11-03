# RP2350 Dual-Core SD Card Synchronization with Amiga

## Overview
This setup shares a single SD card between:

- **Amiga** — running `spisd.device` (fat95 filesystem in RAM)  
- **RP2350 Core 0** — FTP server (direct SD card access via SdFat/FatFS)  
- **RP2350 Core 1** — parallel SPI interface (`par_spi.c`) acting as a **block-level relay**  

Goals:

- Preserve **current `par_spi.c` behavior** for Amiga.  
- Allow **FTP server writes** safely without corrupting Amiga view.  
- Ensure **bidirectional synchronization**:
  - FTP server writes → Amiga refresh
  - Amiga writes → FTP server refresh
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
     |  +--------------------+    +--------------------+ |
     |            \__________________________/           |
     |                   Shared SPI Bus                  |
     +-----------------------------+---------------------+
                                   |
                                   v
                          +----------------+
                          |    SD Card     |
                          +----------------+

---

## Core 0 — FTP Server (SdFat) with Semaphore

```c++
#include "SdFat.h"
#include "pico/sync.h"
#include <stdatomic.h>
#include "par_spi.h"

SdFat sd;
semaphore_t sd_semaphore;
atomic_bool sd_writing = false;
atomic_uint media_dirty_from_amiga = 0;

void ftp_init() {
    sem_init(&sd_semaphore, 1, 1); // binary semaphore
    sd.begin();
}

// Handle STOR / FTP writes
void ftp_write_file(const char* filename, const uint8_t* data, size_t length) {
    sem_acquire_blocking(&sd_semaphore);
    atomic_store(&sd_writing, true);

    File f = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        f.write(data, length);
        f.sync();  // flush buffers
        f.close();
    }

    // Notify Amiga to refresh fat95
    par_spi_send_command(CMD_REFRESH);

    atomic_store(&sd_writing, false);
    sem_release(&sd_semaphore);
}

// Periodically check if Amiga wrote to SD
void check_dirty_from_amiga() {
    if (atomic_exchange(&media_dirty_from_amiga, 0) > 0) {
        sem_acquire_blocking(&sd_semaphore);
        atomic_store(&sd_writing, true);

        sd.end();
        sd.begin(); // refresh filesystem

        atomic_store(&sd_writing, false);
        sem_release(&sd_semaphore);
    }
}

// Handle commands from Core 1 / Amiga
void handle_command_from_amiga(uint8_t cmd) {
    if (cmd == CMD_DIRTY) {
        atomic_store(&media_dirty_from_amiga, 1);
    }
}
```

---

## Core 1 — par_spi.c (transparent SPI bridge)

```c
// Main loop: relay all SPI traffic without decoding
while (1) {
    handle_request();  // existing SPI relay

    gpio_set_dir_in_masked(0xff);
    gpio_clr_mask(0xff);

    gpio_put(PIN_ACT, 1);
    gpio_put(PIN_LED, 0);

    while (spi_is_busy(spi0))
        tight_loop_contents();

    if (spi_is_readable(spi0))
        (void)spi_get_hw(spi0)->dr;

    // Optional: stall Amiga writes while FTP is writing
    while (atomic_load(&sd_writing))
        tight_loop_contents();
}
```

> No command decoding in Core 1 — it is purely a transparent SPI bridge.

---

## Amiga — `device.c` / `spisd.device`

### New command definitions

```c
#define CMD_REFRESH 0xF0  // FTP server wrote
#define CMD_DIRTY   0xF1  // Amiga wrote
```

### process_request() integration

```c
void process_request(struct IOExtTD *ioreq)
{
    UWORD cmd = ioreq->iotd_Req.io_Command;
    LONG err = 0;

    switch (cmd) {

        case TD_READ:
        case TD_READ64:
        case NSCMD_TD_READ64:
            err = sd_read_blocks(ioreq);
            break;

        case TD_WRITE:
        case TD_WRITE64:
        case NSCMD_TD_WRITE64:
            err = sd_write_blocks(ioreq);
            send_dirty_notification_to_rp2350();  // Notify FTP server
            break;

        case TD_FORMAT:
        case TD_FORMAT64:
        case TD_REMOVE:
            err = sd_format_or_remove(ioreq);
            send_dirty_notification_to_rp2350();
            break;

        case CMD_REFRESH:
            DoPktAction(ACTION_FLUSH, ioreq);
            DoPktAction(ACTION_DIE, ioreq);
            mount_device();  // remount fat95
            break;

        case CMD_RESET:
            reset_sd_card();
            break;

        default:
            err = default_sd_command(ioreq);
            break;
    }

    ioreq->iotd_Req.io_Error = err;
    ioreq->iotd_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ReplyMsg(&ioreq->iotd_Req.io_Message);
}
```

### send_dirty_notification_to_rp2350()

```c
static void send_dirty_notification_to_rp2350(void)
{
    UBYTE cmd = CMD_DIRTY;
    send_command_to_rp2350(cmd);  // use existing SPI/parallel send function
}
```

---

## Bidirectional Synchronization Flow

| Source        | Action                                  | How handled                                      |
|---------------|----------------------------------------|-------------------------------------------------|
| FTP server    | Write, format, remove via SdFat        | Acquire semaphore, write, send `CMD_REFRESH`  |
| Amiga         | Writes via `spisd.device`              | Send `CMD_DIRTY` to Core 0                     |
| Core 0        | Needs latest data                       | Check `media_dirty_from_amiga` → refresh SdFat |
| Amiga         | Needs latest FTP changes                | Core 0 sends `CMD_REFRESH`                     |
| Core 1        | Transparent relay                       | No decoding required                             |

---

### Notes

- Core 1 remains fully transparent; no logic change.  
- Core 0 uses a semaphore and atomic flags to protect SD card writes.  
- Amiga fat95 filesystem is refreshed using `ACTION_FLUSH + ACTION_DIE + mount_device()`.  
- This ensures safe atomic writes, consistent bidirectional synchronization, and minimal firmware changes.
