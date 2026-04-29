# sdcard_mount

Resilient SDSPI mount wrapper around `esp_board_manager`'s `fs_sdcard` device.
Handles bus pull-up setup, power-up settling, retry/frequency fallback,
hot-plug detection, and clear failure diagnostics.

## What this component does

| Responsibility | Notes |
|---|---|
| Bypass `esp_board_manager` v0.5.x sub-entry name collision | Both `dev_fs_fat_sub_spi.c` and `dev_display_lcd_sub_spi.c` register a sub-entry called `"spi"`; we drive `esp_vfs_fat_sdspi_mount` directly so we don't dispatch into the wrong handler. |
| Power-up settling (≥1 ms Vdd, ≥74 dummy clocks) | Done with a 20 ms `vTaskDelay` and CS deasserted before the first command. |
| Retry ladder | Try board-configured frequency → 4 MHz → 1 MHz. |
| Distinguish signal failure from filesystem failure | Lets the FS branch short-circuit retries (pointless to lower SCLK if `f_mount` rejected the BPB). |
| Hot-removal probe | `sdcard_mount_check_alive()` issues a small read; on failure it force-unmounts. |
| Hot-insertion remount with cooldown | `sdcard_mount_alive_or_remount()` retries every ≥5 s. |

## Supported filesystems

| Filesystem | Status | Note |
|---|---|---|
| FAT12 | OK | Tiny cards (≤4 MB). |
| FAT16 | OK | Up to ~2 GB partitions. |
| FAT32 | **Recommended** | Up to ~32 GB on Windows-formatter; up to ~256 GB with third-party tools (guiformat, mkfs.fat -F32). Single-file size limit 4 GiB − 1. |
| exFAT | **Not supported** | ESP-IDF v5.5 ships `FF_FS_EXFAT 0` in `components/fatfs/src/ffconf.h`. SDXC cards (≥64 GB) ship pre-formatted as exFAT and **must be reformatted as FAT32** to work. |
| NTFS / ext* / HFS+ / APFS | Not supported | FATFS only. |

### Card-size guidance

| Card capacity | Default factory FS | Action required |
|---|---|---|
| ≤2 GB SDSC | FAT12/16 | None — works as-is. |
| 4–32 GB SDHC | FAT32 | None. |
| 64 GB–2 TB SDXC | exFAT | **Reformat to FAT32 before use.** Windows hides the FAT32 option for >32 GB volumes — use `guiformat.exe`, `mkfs.fat -F 32 /dev/sdX1`, or the SD Association SD Memory Card Formatter (only formats ≤32 GB as FAT32 by default; larger volumes will need a 3rd-party tool). |

The on-device error message points the user to the same guidance:

```
E (xxxx) sdcard_mount: SD card detected at /sdcard but FAT mount failed.
E (xxxx) sdcard_mount: Cause: unsupported filesystem.
E (xxxx) sdcard_mount:   Supported: FAT12 / FAT16 / FAT32.
E (xxxx) sdcard_mount:   NOT supported: exFAT (SDXC default), NTFS, ext*.
E (xxxx) sdcard_mount:   -> Reformat the card as FAT32.
E (xxxx) sdcard_mount:   -> Cards >32 GB ship pre-formatted as exFAT;
E (xxxx) sdcard_mount:        use a tool such as guiformat / mkfs.fat -F32.
```

#### If you really want exFAT support

Patch `$IDF_PATH/components/fatfs/src/ffconf.h`:

```c
#define FF_FS_EXFAT             1
```

This requires `FF_USE_LFN >= 1` (already enabled by default in IDF) and
discards C89 compatibility in FATFS itself. We don't ship this patch
because (a) it bloats the firmware, (b) it requires you to maintain a
local IDF patch across upgrades, and (c) Microsoft's exFAT patents only
became OIN-licensed in 2019 — re-check your distribution model before
shipping.

## File-size and count limits (FAT32)

| Limit | Value |
|---|---|
| Maximum single file size | 4 GiB − 1 byte |
| Maximum files per directory | ~65 535 (with LFN, fewer due to long-name overhead) |
| Cluster size on a 64 GB volume formatted FAT32 | 32 KiB or 64 KiB depending on tool |
| `max_files` open simultaneously | 5 (set in `board_devices.yaml → vfs_config.max_files`) |

## SPI bus sharing with the display

The NM-CYD-C5 board wires the ST7789 LCD, XPT2046 touch controller and the
SD slot to the same `spi_display` peripheral (SPI2_HOST). Coordination
requires **two layers**:

1. **Per-transaction arbitration (`spi_master`).** Each device has its own
   CS pin and `spi_master` queues every transaction so two devices cannot
   start a transfer at the exact same instant.
2. **Coarse-grained burst arbitration (`spi_bus_arbiter`).** `spi_master`
   alone is **not enough** when one client uses a *queued / DMA* path
   (`esp_lcd_panel_io_spi`, which returns from `draw_bitmap` before the
   ISR completes the transfer) while another client uses a *polling* path
   (`esp_vfs_fat_sdspi_mount` and friends, which call
   `spi_device_polling_transmit`). The polling driver releases the
   bus_lock between sub-transfers; if an LCD DMA transaction is still
   physically running on the hardware, the next polling write hits the
   `spi_hal_setup_trans` assert:
   ```
   assert failed: spi_hal_setup_trans  spi_hal_iram.c:134
       (spi_ll_get_running_cmd(hw) == 0)
   ```
   To prevent this, `sdcard_mount` and the LCD draw paths take a single
   recursive mutex (`spi_bus_arbiter_lock` / `_unlock`) around every
   burst (each `esp_vfs_fat_*` call, each `esp_lcd_panel_draw_bitmap`,
   each HTTP file-handler chunk that targets `/sdcard`). The lock is
   recursive so nested LCD-flush wait loops do not self-deadlock.
3. **Init ordering.** `sdcard_mount_init()` is called *after* the LCD/touch
   are fully initialised so the SD initialisation sequence (with its
   400 kHz handshake) does not race the LCD's high-rate boot flush.

Performance impact is intentional: a 320×240 full-screen flush at 40 MHz
(~100 ms) will block SD I/O for that duration, and a multi-MB upload to
`/sdcard` will pause LCD updates until each `fwrite` chunk finishes. This
is the price of SPI bus time-sharing on this board; do not remove the
arbiter unless the LCD and SD are moved to separate SPI hosts.

The `display_arbiter` component is a *separate* concern (ownership of the
panel between Lua UI and the emote engine) and does not touch SPI
arbitration.

## Public API

See `include/sdcard_mount.h`. The most useful entry points:

| Function | Purpose |
|---|---|
| `sdcard_mount_init()` | One-shot mount with retries. |
| `sdcard_mount_is_mounted()` / `_get_mount_point()` | State query. |
| `sdcard_mount_get_usage(total, free)` | Capacity report. |
| `sdcard_mount_check_alive()` | Probe + auto-unmount on removal. |
| `sdcard_mount_alive_or_remount()` | Probe + cooldown-throttled remount. Wired as the `alive_cb` of the `/sdcard` HTTP virtual mount. |
| `sdcard_mount_force_unmount()` | Tear down (releases SPI peripheral refcount). |
