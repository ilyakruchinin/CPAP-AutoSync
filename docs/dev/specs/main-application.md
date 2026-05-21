# Main Application Controller

## Overview
The main application controller (`main.cpp`) orchestrates the entire CPAP AutoSync system, managing initialization, the upload state machine, web interface, and system lifecycle events.

## Core Responsibilities

### System Initialization
- **Early power reduction**: CPU throttled to 80 MHz and Bluetooth memory released as the very first instructions in `setup()`
- Hardware pin configuration and peripheral setup
- Component initialization (WiFi, SD card, uploaders, web server)
- Configuration loading and validation
- Fast-boot detection using `esp_reset_reason()`
- **WiFi power optimization**: 802.11b disabled, TX power set before association, power saving enabled after connect
- **DFS configuration**: `esp_pm_configure()` enables automatic CPU scaling (80-160 MHz)

### Upload State Machine (FSM)
- Implements the core upload logic with states: IDLE, LISTENING, ACQUIRING, UPLOADING, RELEASING, COOLDOWN
- **LISTENING State Dynamics**: 
  - Normally waits for `INACTIVITY_SECONDS` of bus silence (indicating the CPAP has ended therapy).
  - If `g_noWorkSuppressed` is true, the behavior inverts: it waits indefinitely for **new bus activity** (the CPAP going *into* therapy) before it will even consider timing an idle period to go *out* of therapy. 
  - Features an edge-trigger that instantly overrides suppression the moment a daily schedule window opens, ensuring old data is safely scanned even if no CPAP therapy occurred during the day.
- Supports both "smart" and "scheduled" upload modes
- Manages SD card exclusive access with timeout handling
- Coordinates between SMB and Cloud upload passes
- **TLS pre-warm + PCNT re-check** in `uploadTaskFunction` (Core 0) before SD mount — see `docs/specs/tls-prewarm-pcnt-recheck.md`

### Heap Management & Recovery
- **Conditional-reboot strategy**: `handleReleasing()` calls `esp_restart()` only when real upload work was done
- When `FileUploader` returns `NOTHING_TO_DO` (all backends fully synced), FSM skips the reboot and enters `COOLDOWN` directly via `g_nothingToUpload` flag — prevents endless reboot cycles when data is already synced
- **`MINIMIZE_REBOOTS` config key**: When `true`, skips elective soft-reboots after upload sessions and reuses the existing runtime (COOLDOWN → LISTENING loop). Mandatory reboots (watchdog, user-triggered state reset / soft reboot, OTA) still occur. Logs a warning if `max_alloc` drops below 35 KB
- **Pre-reboot log preservation**: Before every `esp_restart()`, NAND periodic flush + pre-reboot dump are called to ensure all final log lines (session summary, reboot reason) are persisted to `syslog.A.txt` and `/last_reboot_log.txt`
- Fast-boot path (`ESP_RST_SW`) skips cold-boot stabilization delays and Smart Wait
- Each session runs both backends sequentially (CLOUD then SMB) — TLS pre-warmed before SD mount for clean heap, released before SMB phase to prevent socket conflicts

### Web Interface Integration
- Progressive Web App (PWA) with pre-allocated buffers
- Real-time status monitoring and manual controls
- OTA firmware updates
- SD activity monitoring and log viewing

## Key Features

### Fast-Boot Detection
```cpp
bool fastBoot = (esp_reset_reason() == ESP_RST_SW);
if (fastBoot) {
    LOG("[FastBoot] Software reset — skipping stabilization + Smart Wait");
}
```

### Conditional Reboot in handleReleasing
```cpp
void handleReleasing() {
    sdManager.releaseControl();
    if (g_nothingToUpload) {          // pre-flight found no work
        g_nothingToUpload = false;
        cooldownStartedAt = millis();
        transitionTo(UploadState::COOLDOWN);  // no reboot
        return;
    }
    // Real upload completed — reboot to restore heap
    delay(200);
    esp_restart();
}
```

### Early Suppression (UploadResult::NOTHING_TO_DO & COMPLETE)
- `NOTHING_TO_DO` is returned when pre-flight scan finds no pending work; `COMPLETE` is returned when an upload session successfully exhausts all pending folders.
- In both cases, the FSM sets `g_nothingToUpload = true` (to skip the elective reboot) and `g_noWorkSuppressed = true` to prevent the FSM from redundantly probing the SD card until new work arrives.
- `RELEASING` state skips the reboot and enters `COOLDOWN` instead.

### Config Edit Lock (`g_configEditLock`)
```cpp
bool g_configEditLock = false;          // set via POST /api/config-lock
unsigned long g_configEditLockAt = 0;  // millis() when lock acquired
const unsigned long CONFIG_EDIT_LOCK_TIMEOUT_MS = 30 * 60 * 1000;  // 30 min
```
- When `true`, `handleListening()` returns early and does NOT transition to `ACQUIRING` — FSM stays in `LISTENING` until lock is released or expires
- Auto-expires after 30 minutes to prevent accidentally blocking uploads forever
- Cannot be acquired while an upload is in progress (HTTP 409 from `/api/config-lock`)
- Released automatically after a successful Save or Save & Reboot from the web UI

### Upload Modes
- **Smart Mode**: Continuous loop, uploads recent data anytime, old data only in upload window. Includes a dynamic edge-trigger that instantly clears `g_noWorkSuppressed` the moment the daily schedule window opens, ensuring it never sleeps through old-data uploads.
- **Scheduled Mode**: Only uploads within configured time window, enters IDLE between windows
- **Manual Mode**: Never uploads automatically. The device stays in the `IDLE` state permanently. An upload is only initiated when the user manually triggers a "Force Upload" from the web interface. Once the forced upload is complete, the device runs a cooldown period and then returns directly to `IDLE` (without entering the `LISTENING` loop or marking the day completed). Forced uploads in manual mode always target all available data (no recent-only restriction).

## Global Objects
- `Config` - Configuration management
- `SDCardManager` - SD card access control
- `WiFiManager` - Network connectivity
- `FileUploader` - Upload orchestration
- `TrafficMonitor` - SD bus activity detection
- `CpapWebServer` - Web interface (optional)
- `OTAManager` - OTA updates (optional)

## Lifecycle
1. **Boot**: Immediate CPU throttle (80 MHz) + BT memory release, detect reset reason, initialize components
2. **Setup**: Load config, apply TX power early, connect WiFi (802.11b disabled), enable DFS, start web server
3. **Loop**: Run FSM with state-appropriate `vTaskDelay()` yields (enables DFS), handle web requests, monitor heap
4. **Upload**: TLS pre-warm (cloud-only) → PCNT re-check → SD mount → pre-flight scan → phased upload (CLOUD → SMB) → SD release → reboot; if no work or PCNT re-check fails, go to cooldown
5. **Recovery**: Soft reboot after every real upload session restores contiguous heap

## Power Management
- **Boot**: CPU at 80 MHz from first instruction, Bluetooth memory released
- **WiFi**: 802.11b disabled (OFDM only), TX power default 5 dBm, MIN_MODEM sleep default
- **DFS**: CPU scales 80-160 MHz automatically via `esp_pm_configure()`
- **Loop yields**: State-appropriate `vTaskDelay()` calls (10-100ms) allow DFS to engage
- **Compile-time**: `CONFIG_BT_ENABLED=n`, `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`, `CONFIG_PM_ENABLE=y`

## Configuration Dependencies
- All timing parameters (inactivity, exclusive access, cooldown)
- Upload mode and window settings
- Backend endpoints (SMB, Cloud, WebDAV)
- Power management settings (CPU speed, TX power, power saving mode)

## Integration Points
- **UploadFSM**: Core state machine logic
- **FileUploader**: Backend upload orchestration
- **TrafficMonitor**: SD activity detection for smart mode
- **WiFiManager**: Network connectivity for cloud operations
- **WebServer**: User interface and manual controls
