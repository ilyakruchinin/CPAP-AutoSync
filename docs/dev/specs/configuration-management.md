# Configuration Management

## Overview
The Configuration module (`Config.cpp/.h`) handles loading, parsing, validation, and secure storage of system configuration from the SD card. Supports key-value format with credential security features.

## Configuration Format
Uses simple key-value format in `config.txt`:
```ini
# Comments start with #
WIFI_SSID = MyNetwork
WIFI_PASSWORD = mypassword
ENDPOINT_TYPE = SMB,CLOUD
ENDPOINT = //192.168.1.100/cpap_backups
```

## Core Features

### Key-Value Parser
- Case-insensitive keys
- Optional spaces around `=` 
- Lines starting with `#` are treated as comments and skipped; `#` within values is **preserved** (valid in passwords/SSIDs)
- Everything after the first `=` on a non-comment line is the value — passwords with `=`, `#`, or spaces are all handled correctly
- Automatic trimming of leading/trailing whitespace from key and value

### Credential Security
- **Plaintext Mode (default)**: Credentials stay in `config.txt` as plaintext — survives full firmware flashes
- **Masked Mode**: Opt-in via `MASK_CREDENTIALS = true` — migrates passwords to ESP32 NVS flash and replaces with `***STORED_IN_FLASH***` in config file
- **Automatic Detection**: Detects password changes and updates secure storage
- **Safety**: If `***STORED_IN_FLASH***` found without `MASK_CREDENTIALS = true`, logs an error prompting re-entry

### Backend Support
- **SMB**: Network shares (Windows, NAS, Samba)
- **Cloud**: SleepHQ direct upload
- **WebDAV**: Planned support (placeholder only)
- **Single-backend sessions**: Each upload session uses one backend (cycling via oldest timestamp)

## Configuration Parameters

### Network Settings
- `WIFI_SSID` / `WIFI_PASSWORD` - WiFi credentials (alias for slot 1)
- `WIFI_SSID_1..4` / `WIFI_PASSWORD_1..4` - up to 4 networks; roaming/failover is implicit when more than one is configured
- `HOSTNAME` - mDNS hostname (default: "cpap")
- `NTP_SERVER` - Custom NTP server (default: empty for DHCP Option 42 with pool.ntp.org fallback)

### Upload Destinations
- `ENDPOINT_TYPE` - SMB, CLOUD, WEBDAV or comma-separated combinations
- `ENDPOINT` - Network path (e.g., `//server/share`)
- `ENDPOINT_USER` / `ENDPOINT_PASSWORD` - SMB credentials
- `SMB_PRESERVE_TIMESTAMPS` - Copy original file timestamps to NAS (default: `true`)
- `CLOUD_CLIENT_ID` / `CLOUD_CLIENT_SECRET` - SleepHQ credentials

### Upload Scheduling
- `UPLOAD_MODE` - "smart", "scheduled", or "manual"
- `UPLOAD_START_HOUR` / `UPLOAD_END_HOUR` - Upload window (0-23)
- `INACTIVITY_SECONDS` - Bus silence required before upload (default: 62)
- `EXCLUSIVE_ACCESS_MINUTES` - Max SD card hold time (default: 5)
- `COOLDOWN_MINUTES` - Pause between sessions (default: 10)

### Data Management
- `MAX_DAYS` - Maximum days to look back (default: 365)
- `RECENT_FOLDER_DAYS` - Fresh data threshold (default: 2)
- `GMT_OFFSET_HOURS` - Timezone offset

### Power Management
- `CPU_SPEED_MHZ` - 80, 160, or 240 (default: 80)
- `WIFI_TX_PWR` - "lowest" (-1dBm), "low" (2dBm), "mid" (5dBm), "high" (8.5dBm), "max" (10dBm) (default: "mid")
- `WIFI_PWR_SAVING` - "none", "mid" (MIN_MODEM), "max" (MAX_MODEM) (default: "mid")
- `BROWNOUT_DETECT` - "off" to disable brownout detector at runtime (default: enabled)

### Debugging
- `PERSISTENT_LOGS` - Enable persistent internal logging (default: false)

## Validation Rules
- **Ranges**: Hours (0-23), minutes (1-60), seconds (10-3600)
- **Clamping**: Invalid values clamped to valid range with warnings
- **Backend Validation**: Empty endpoints disable that backend
- **Required Fields**: WiFi credentials and at least one valid backend

## Secure Storage Implementation
Uses ESP32 Preferences (NVS) for credential storage:
```cpp
const char* PREFS_NAMESPACE = "cpap_creds";
const char* PREFS_KEY_WIFI_PASS = "wifi_pass";
const char* PREFS_KEY_ENDPOINT_PASS = "endpoint_pass";
const char* PREFS_KEY_CLOUD_SECRET = "cloud_secret";
```

## Loading Process
1. Open `config.txt` from SD card
2. Parse key-value pairs line by line
3. Validate and clamp values
4. Load existing credentials from secure storage
5. Detect password changes and update secure storage
6. Censor passwords in config file if in secure mode
7. Set endpoint type flags and validate backends

## Error Handling
- Missing config file: Error and halt
- Invalid syntax: Warning with default values
- Missing credentials: Error for required backends
- Secure storage failures: Fallback to plain text with warning

## Web Config Editor
The Web GUI Config tab provides a live editor for `config.txt` directly on the SD card:
- **GET `/api/config-raw`**: Returns raw `config.txt` content as `text/plain` — borrows SD control if not already held
- **POST `/api/config-raw`**: Writes new content atomically (temp file + rename), max 4096 bytes; blocked during active upload (409)
- Passwords stored in flash appear as `***STORED_IN_FLASH***` — leaving them unchanged preserves existing credentials
- Changes take effect after reboot (`Save & Reboot` button triggers `esp_restart()`)
- SD is borrowed only briefly for the read/write operation and released immediately

## Integration Points
- **main.cpp**: Loads configuration during setup
- **WiFiManager**: Uses WiFi credentials
- **FileUploader**: Uses backend and scheduling settings
- **All uploaders**: Use endpoint credentials
- **WebServer**: Serves config editor via `/api/config-raw`

---

## Stealth Mode for Configuration Loading (Unified)

Configuration loading uses the same stealth-aware `SDCardManager::takeControl()` / `releaseControl()` path on all devices (AS10 and AS11):

1. `captureCardState()` — stealth probe captures RCA, bus width, and card state (no CMD0)
2. `SD_MMC.begin()` — standard mount; config.txt is read via the filesystem
3. `SD_MMC.end()` + `restoreToSavedState()` — card returned to its exact pre-mount state

**Historical note**: `StealthConfigReader::readConfigTxt()` was previously used on AS10 at boot to read `config.txt` via a custom FAT32 parser (avoiding `SD_MMC` entirely). It is superseded by the unified approach above and retained as `#if 0` in `StealthConfigReader.cpp` for reference.
