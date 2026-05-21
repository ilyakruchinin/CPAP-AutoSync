# CPAP AutoSync v4.3 — Manual Upload Mode

> **OTA upgrades from v4.0, v4.1, v4.1.1, v4.1.2, v4.1.3, v4.2, or v4.2.1 are fully supported.** There are no partition-table or config.txt changes required in this release.

---

## What's New in v4.3

### 🖐️ Manual Upload Mode

For users who want to disable all automatic scheduled uploads and prevent any routine SD card checks, a new **Manual Mode** has been introduced.

* **Behavior**: When set to `UPLOAD_MODE = manual`, the system stays in the `IDLE` state at all times. It does not perform schedule window checks, automatic uploads, or routine SD card checks.
* **On-Demand Trigger**: Uploads are initiated exclusively on-demand by clicking the **Force Upload** button on the web dashboard (or via the `/api/trigger-upload` endpoint).
* **Full Data Upload**: A manual upload in this mode always performs a full data scan and upload (`ALL_DATA`), bypassing any recent-only filters.
* **Flow**: Idle ➔ Force Upload ➔ Uploading ➔ Releasing ➔ Cooldown ➔ Idle.
* **Repeatable**: Manual mode does not mark the day completed after an upload, allowing multiple manual uploads in a single day.
* **Cooldowns**: Standard cooldown periods are still fully respected after the upload session finishes before returning to `IDLE`.
* **Web UI Support**: A new "Manual Mode" option has been added to the Setup Wizard (which automatically hides the scheduling sliders when selected), and the dashboard status displays `—` for the next scheduled upload.

---

## What's Fixed in v4.3 (from v4.2)

### 📏 PCNT Glitch Filter Adjusted for 20 MHz SD Bus

The PCNT (Pulse Counter) peripheral is used to detect CPAP SD card bus activity without CPU overhead. A hardware glitch filter rejects electrical noise so that only genuine SD bus transitions are counted.

The previous filter threshold was **125 ns** (10 APB cycles), which was chosen conservatively to reject noise. However, the AirSense 11 runs its SD bus clock at **20 MHz**, producing valid data transitions as short as **50 ns** per bit. This meant the filter was 2.5× the minimum valid pulse width and could silently discard legitimate SD edges — particularly during short burst transactions.

In practice the system worked because:
- Real data patterns produce many pulses wider than 125 ns (consecutive identical bits create no edges; only alternating-bit patterns hit the 50 ns minimum).
- The activity detector only needs ≥1 edge per 100 ms sample window, so partial filtering was tolerable.

However, very short SD transactions (a few bytes) at 20 MHz could theoretically be filtered out entirely, causing the device to miss brief CPAP activity bursts and potentially acquire the SD card during an active transaction.

**The fix:** The glitch filter threshold has been reduced from **125 ns to 40 ns** (~3 APB cycles at 80 MHz). This is comfortably below the 50 ns SD bit period, ensuring all valid SD bus edges pass through while still rejecting sub-cycle electrical noise.

The change applies to both the early-boot PCNT (`EarlyPCNT`) and the runtime traffic monitor (`TrafficMonitor`).

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` or its IP address.
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.3.bin` asset from the Releases page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.3.bin` from the Releases page.
2. Open the ESP Web Flasher in Chrome/Edge.
3. Connect your ESP32 via USB and select its serial port.
4. Click **Erase** to clear the flash. This resets all settings.
5. Set the flash address to `0x0` and select the downloaded `.bin` file.
6. Click **Program** and wait for the flash to complete.
7. The device will reboot into setup mode. Follow the on-screen instructions to configure WiFi and upload settings.

---

## Known Limitations

Unchanged from v4.2.

---

## Changelog Summary (since v4.2)

- **FSM**: Added `manual` upload mode to disable automated triggers and SD checks.
- **Web UI**: Added "Manual Mode" selection, status masking, and next-upload display handling.
- **PCNT**: Reduced glitch filter threshold from 125 ns to 40 ns to avoid rejecting valid 20 MHz SD bus edges. Applied to both `EarlyPCNT` and `TrafficMonitor`.
