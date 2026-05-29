# CPAP AutoSync v4.3.2 — Manual Mode "Upload All Data" Fix

> **OTA upgrades from v4.0, v4.1, v4.1.1, v4.1.2, v4.1.3, v4.2, v4.2.1, v4.3, or v4.3.1 are fully supported.** There are no partition-table or config.txt changes required in this release.

---

## What's Fixed in v4.3.2

### 📤 Manual Mode Force Upload Now Actually Uploads Old Folders

In Manual mode, the dashboard documents the **Force Upload** button as *"triggers a full upload of all available data now."* However, any DATALOG folder older than `RECENT_FOLDER_DAYS` (default: 2 days) was silently skipped, leaving it permanently un-uploaded with no way to recover it from the UI.

**Symptoms users saw:**

* Dashboard stuck showing **`1 left`** / **`1 on NAS pending`** that never cleared.
* Pressing **Force Upload** repeatedly: it would run briefly (uploading only recent folders), then immediately drop into **COOLDOWN**, with the backlog count unchanged.
* The work probe reporting actionable work as `0` while the synced count stayed one short of the universe (e.g. `universe=61 smbSynced=60`).

**Root cause:**

A Manual-mode Force Upload correctly sets its data filter to `ALL_DATA`, but the actual old-folder upload loops (and the pre-session work probe) were additionally gated on `ScheduleManager::canUploadOldData()`. That method **always returns `false` in Manual mode** (Manual mode has no scheduled upload window), so the "old folders" pass never executed. Any folder that aged past the recent window before being uploaded — for example, after several days without a forced upload, or after a mid-upload crash/reboot — became stranded.

This behavior was made visible by the v4.3.1 backlog-counting fix: the dashboard now accurately reported the genuinely-incomplete folder, exposing the underlying gate that prevented it from ever being uploaded.

**The fix:**

A new `ScheduleManager::canProcessOldData()` helper now governs whether old (non-recent) folders are processed:

* **Scheduled / Smart modes:** behavior is **identical** to before — old folders are processed only inside the upload window (`canProcessOldData()` returns exactly `canUploadOldData()`).
* **Manual mode:** old folders are processed, because a Manual run only ever happens via an explicit user-initiated Force Upload (an intentional "upload everything"). This is gated on `isTimeSynced()` so that recent/MAX_DAYS folder-name math is only applied with a valid clock.

`ScheduleManager::canUploadOldData()` itself is left unchanged, so all schedule/window-detection logic (upload-window-open detection, `isUploadEligible()`) keeps its original semantics. The change is strictly confined to Manual mode.

### 🩹 Recovering a Previously-Stranded Folder

After updating to v4.3.2, simply press **Force Upload** in Manual mode. The previously-stranded old folder will now upload, and the dashboard backlog (`1 on NAS pending`) will clear on the next work-probe refresh.

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` or its IP address.
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.3.2.bin` asset from the Releases page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.3.2.bin` from the Releases page.
2. Open the ESP Web Flasher in Chrome/Edge.
3. Connect your ESP32 via USB and select its serial port.
4. Click **Erase** to clear the flash. This resets all settings.
5. Set the flash address to `0x0` and select the downloaded `.bin` file.
6. Click **Program** and wait for the flash to complete.
7. The device will reboot into setup mode. Follow the on-screen instructions to configure WiFi and upload settings.

---

## Known Limitations

Unchanged from v4.3.1.

---

## Changelog Summary (since v4.3.1)

- **FSM / Uploader**: Manual mode Force Upload now uploads old (non-recent) DATALOG folders, matching its documented "upload all available data" behavior. Previously folders older than `RECENT_FOLDER_DAYS` were silently skipped and could become permanently stranded.
- **ScheduleManager**: Added `canProcessOldData()` helper that permits old-folder processing during a Manual Force Upload (clock-synced) while leaving `canUploadOldData()` and all Scheduled/Smart window semantics unchanged.
- **No change** to Scheduled or Smart mode behavior.
