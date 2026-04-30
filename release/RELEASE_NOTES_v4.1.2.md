# CPAP AutoSync v4.1.2 — Upload Retry Fix (Complete)

> **OTA upgrades from v4.0, v4.1, or v4.1.1 are fully supported.** There are no partition-table or config.txt changes in this release.

## What's Fixed in v4.1.2

### 🔄 Complete Fix for Stalled Upload Retries

v4.1.1 introduced logic to continue retrying uploads when backends still have incomplete folders, but the check used an unreliable data source (`totalFoldersCount`) that was never populated when a backend failed to connect. **v4.1.2 completes the fix.**

**The root cause in v4.1.1:** The `hasIncompleteFolders()` check relied on `getIncompleteFoldersCount()`, which computes `totalFoldersCount - completedCount - pendingCount`. However, `totalFoldersCount` is only set during the upload phase's folder scan — which **never runs** when a backend fails to connect (e.g., NAS unreachable). With `totalFoldersCount` stuck at 0, the function always returned "no incomplete folders," and retries were still suppressed.

**The v4.1.2 fix:** `hasIncompleteFolders()` now uses the **work probe snapshot** (`probeUniverse` vs `probeSynced`) instead. The work probe runs both before and after every upload session regardless of backend connectivity, so it always has accurate counts. For example, if the probe reports `universe=11, smbSynced=9`, the function correctly detects 2 incomplete folders and prevents retry suppression.

**Practical impact:** If your NAS is temporarily unreachable (rebooting, network issue, firewall), the device will now correctly retry on the next cooldown cycle instead of waiting indefinitely for CPAP bus activity.

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` (or its IP address).
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.1.2.bin` asset from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.1.2.bin` from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page.
2. Open the [ESP Web Flasher](https://esp.huhn.me/) in Chrome/Edge.
3. Connect your ESP32 via USB and select its serial port.
4. Click **Erase** to clear the flash (this resets all settings).
5. Set the flash address to `0x0` and select the downloaded `.bin` file.
6. Click **Program** and wait for the flash to complete.
7. The device will reboot into setup mode — follow the on-screen instructions to configure WiFi and upload settings.

---

## Known Limitations

Unchanged from v4.0 — see `RELEASE_NOTES_v4.0.md` for details.

---

## Changelog Summary (since v4.1.1)

- **Fix**: `hasIncompleteFolders()` now uses the work probe snapshot (`probeUniverse` vs `probeSynced`) instead of the upload-phase `totalFoldersCount`, which was never set when a backend failed to connect. This completes the retry suppression fix from v4.1.1.

## Cumulative Changelog (since v4.1)

- **Fix**: Upload retry suppression now checks whether backends still have incomplete folders before activating. Previously, suppression was unconditional after any `COMPLETE` or `NOTHING_TO_DO` result, which could stall uploads indefinitely when a backend was temporarily unreachable or the session timed out with work remaining. *(v4.1.1)*
- **Fix**: The incomplete-folders check now uses the work probe snapshot instead of an internal counter that depended on the upload phase running successfully — which doesn't happen when a backend fails to connect. *(v4.1.2)*
