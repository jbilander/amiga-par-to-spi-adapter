# Shared SD Card Synchronization Strategy: RP2350 ↔ Amiga

## Overview

This strategy enables **safe bidirectional access** to a single SD card shared between:

* **RP2350 MCU (FTP server)** — Core 0: runs **SdFat** (or FatFS) filesystem
* **Amiga (via spisd.device)** — Core 1: presents the SD card as a block device using fat95

Both cores share the same SPI bus. The goal is to allow **writes from either side** without corrupting data and without unmounting/remounting unnecessarily.

---

## Key Principles

1. **Single active filesystem per core:**
   Each core maintains its own filesystem in RAM. All writes must be flushed before notifying the other side.

2. **Bidirectional “media-dirty” signaling:**

   * RP2350 writes → Amiga refresh (`TD_UPDATE`)
   * Amiga writes → RP2350 refresh (SdFat or FatFS remount/reopen)

3. **No new hardware pins required:**
   Existing parallel interface pins are preserved. Notifications are done via **custom protocol opcodes**.

---

## Protocol Opcodes

| Direction      | Opcode               | Purpose                                   | Effect                                                                               |
| -------------- | -------------------- | ----------------------------------------- | ------------------------------------------------------------------------------------ |
| RP2350 → Amiga | `CMD_REFRESH` (0xF0) | Request Amiga to refresh fat95 filesystem | TD_UPDATE; directories and FAT re-read in place, volume stays mounted                |
| Amiga → RP2350 | `CMD_DIRTY` (0xF1)   | Notify FTP core that media changed        | RP2350 sets dirty counter/flag; SdFat or FatFS remounts or reopens volume to refresh |

---

## RP2350 Firmware Logic (Multi-Client Safe)

Use an **atomic counter** for multi-client safety.

```c
#include <stdatomic.h>

// Count of dirty notifications from Amiga
atomic_uint media_dirty_from_amiga = 0;

// When Amiga sends CMD_DIRTY:
atomic_fetch_add(&media_dirty_from_amiga, 1);

// Before serving FTP requests (directory listing, file access):
if (atomic_exchange(&media_dirty_from_amiga, 0) > 0) {
    // At least one dirty notification occurred → refresh filesystem

    // For FatFS:
    f_mount(NULL, "", 0);   // unmount
    f_mount(&FatFs, "", 1); // remount, re-read FAT and directories

    // For SdFat:
    sd.end();       // close volume and release resources
    sd.begin();     // re-initialize SD card and reload FAT/directory structures
}
```

After FTP writes:

```c
f_close(&fil);          // flush all writes
par_spi_send_command(CMD_REFRESH); // notify Amiga
```

---

## Amiga (spisd.device) Logic

* Handle `CMD_REFRESH` by issuing **TD_UPDATE** to fat95:

```c
case CMD_REFRESH:
    spisd_refresh_filesystem();
    break;

void spisd_refresh_filesystem(void)
{
    // Issue TD_UPDATE to fat95
    struct IOExtTD *ioreq = CreateIORequest(...);
    if (ioreq) {
        ioreq->iotd_Req.io_Command = TD_UPDATE;
        DoIO((struct IORequest*)ioreq);
        DeleteIORequest((struct IORequest*)ioreq);
    }
}
```

* After any Amiga write (`TD_WRITE`), send `CMD_DIRTY` to RP2350:

```c
case TD_WRITE:
    result = spisd_do_write(ioreq);
    if (result == OK)
        par_spi_send_command(CMD_DIRTY);
    break;
```

---

## Sequence Example

| Step | Actor                     | Action                              | Result                                                                   |
| ---- | ------------------------- | ----------------------------------- | ------------------------------------------------------------------------ |
| 1    | Amiga                     | Writes `FILE1.TXT`                  | TD_WRITE                                                                 |
| 2    | spisd.device              | Sends `CMD_DIRTY`                   | RP2350 increments atomic dirty counter                                   |
| 3    | FTP client                | Requests directory listing          | RP2350 sees counter > 0 → remounts/reopens filesystem → sees `FILE1.TXT` |
| 4    | FTP writes `FILE2.TXT`    | Flush + `CMD_REFRESH`               | Amiga fat95 sees TD_UPDATE → `FILE2.TXT` appears                         |
| 5    | Both sides remain mounted | Both sides now see latest directory | No unmounts at Amiga, minimal downtime at FTP server                     |

---

## Notes & Recommendations

* Always flush filesystem writes before sending notifications (`f_sync()` / `f_close()`).
* Only one core should write at a time; the atomic counter ensures multi-client FTP safety.
* For **SdFat**, there is no built-in refresh API — reinitialize the SD card object (`sd.end()` + `sd.begin()`) to reload FAT and directories after external changes.
* For **FatFS**, remounting (`f_mount(NULL)` + `f_mount()`) clears caches and ensures filesystem coherence.
* Optional optimization: batch writes and send one notification per batch.
* Fat95 version ≥ 3.18 is required for `TD_UPDATE`.
* No hardware changes required; uses existing parallel port protocol.

---

## Diagram

```
Amiga --- CMD_DIRTY ---> RP2350 FTP core
RP2350 FTP core --- CMD_REFRESH ---> Amiga
```

This strategy ensures **real-time directory updates**, **safe bidirectional access**, and **multi-client safe handling** for both FatFS and SdFat without unmounting the Amiga volume.
