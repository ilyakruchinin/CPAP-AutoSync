# CPAP AutoSync v4.3.1 — Connection Failure and Retrying Fixes

> **OTA upgrades from v4.0, v4.1, v4.1.1, v4.1.2, v4.1.3, v4.2, v4.2.1, or v4.3 are fully supported.** There are no partition-table or config.txt changes required in this release.

---

## What's Fixed in v4.3.1

This release fixes a subtle bug in the FSM retry logic that caused the uploader to prematurely stop polling when a connection or initialization failure occurred (e.g., during WiFi drops or internet downtime) in a dual-backend or SMART setup.

### 🔌 Connection and Initialization Failures Now Correctly Flagged

Previously, if the Cloud (SleepHQ) initialization failed or the persistent SMB session setup failed, the system would gracefully skip the affected phase, but it would **not** set `sessionHadFailure = true`.

As a result, if the other configured backend (or a previous phase) completed successfully, the overall session was mistakenly recorded as a complete success rather than an incomplete run. This triggered "no-work suppression" (meaning the uploader would stop polling until new CPAP physical SD card/PCNT bus activity was detected), leaving the failed backend's data pending.

**The fix:** The system now sets `sessionHadFailure = true` immediately if either the Cloud or SMB uploader fails to connect, authenticate, or initialize. This guarantees that the session correctly exits as an incomplete/failure state, resetting the retry trigger so that the uploader continues to poll periodically after the standard COOLDOWN period until all backends are fully in sync.

### 📐 Fixed Folder Synced/Incomplete Backlog Calculations

A fundamental bug in the state manager backlog count was identified:

1. During the directory scan, `scanDatalogFolders` was called to find incomplete folders.
2. The count of these incomplete folders (typically `1` or `0` during a routine poll) was written to the state manager as the *total* folders count: `sm->setTotalFoldersCount(folders.size())`.
3. In `UploadStateManager::getIncompleteFoldersCount()`, the calculation `totalFoldersCount - completedCount - pendingCount` was computed.
4. Because `totalFoldersCount` was overwritten to a tiny number (e.g., `1`), while `completedCount` (all-time list of completed folder days) was much larger (e.g., `9`), the calculation produced a negative number (e.g. `1 - 9 - 0 = -8`), which was clamped to `0`.
5. The FSM was tricked into thinking `hasIncompleteFolders()` was `false`, causing it to conclude that all folders were successfully complete.

**The fix:**
- Updated the directory scan to assign `sm->setTotalFoldersCount(eligibleFolderCount)` (the actual total count of directories on the card within the MAX_DAYS window, including completed ones) rather than the incomplete-only list size.
- Hardened `getIncompleteFoldersCount()` to query the authoritative, dual-backend-aware work probe snapshot (`probeUniverse - probeSynced`) if a probe has run. This provides a direct, highly-resilient, and robust backlog count that is immune to phase skips or connection crashes.

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` or its IP address.
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.3.1.bin` asset from the Releases page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.3.1.bin` from the Releases page.
2. Open the ESP Web Flasher in Chrome/Edge.
3. Connect your ESP32 via USB and select its serial port.
4. Click **Erase** to clear the flash. This resets all settings.
5. Set the flash address to `0x0` and select the downloaded `.bin` file.
6. Click **Program** and wait for the flash to complete.
7. The device will reboot into setup mode. Follow the on-screen instructions to configure WiFi and upload settings.

---

## Known Limitations

Unchanged from v4.3.

---

## Changelog Summary (since v4.3)

- **FSM**: Connection and initialization failures for either Cloud (SleepHQ) or SMB are now flagged, preventing incorrect "all folders complete" statuses when backends fail to connect.
- **FSM**: Fixed folder backlog logic by setting total folders using the actual physical directory count (`eligibleFolderCount`) rather than the filtered incomplete list size.
- **FSM**: Hardened `getIncompleteFoldersCount()` to query the robust, dual-backend-aware work probe snapshot (`probeUniverse - probeSynced`) if available.
