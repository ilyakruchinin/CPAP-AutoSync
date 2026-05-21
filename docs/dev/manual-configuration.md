# Advanced Manual Configuration

While the **Setup Wizard** is the recommended way to configure CPAP AutoSync, you can manually create or edit the `config.txt` file on the root of your SD card.

## Credential Security

By default, credentials are migrated to the device's secure flash memory (NVS) and replaced with `***STORED_IN_FLASH***` in `config.txt`. This provides improved security while keeping the configuration file easy to manage.

**To update a password:** Replace `***STORED_IN_FLASH***` in your `config.txt` (or via the web UI) with the new plaintext value. The device will automatically re-migrate it to secure storage on the next boot.

**Optional: plaintext mode**
If you prefer to keep your passwords as plaintext in `config.txt` (e.g., for easier debugging), add `MASK_CREDENTIALS = false` to your configuration.

> **Warning:** A full (non-OTA) firmware flash erases NVS. If you are using default masking, you must re-enter your passwords after a full flash. OTA updates are not affected.

---

## Configuration Reference

### Network Settings

**WIFI_SSID** (required)
- Your WiFi network name
- Alias for `WIFI_SSID_1` - the first slot in the multi-network table
- Example: `WIFI_SSID = HomeNetwork`
- Note: ESP32 only supports 2.4GHz WiFi (not 5GHz)

**WIFI_PASSWORD** (required)
- Your WiFi password
- Alias for `WIFI_PASSWORD_1`
- Example: `WIFI_PASSWORD = MySecurePassword123`

**WIFI_SSID_1 .. WIFI_SSID_4 / WIFI_PASSWORD_1 .. WIFI_PASSWORD_4** (optional)
- Up to 4 WiFi networks can be configured. The device picks the strongest available one at boot and roams between them as signal degrades.
- Example:
  ```
  WIFI_SSID_1 = HomeNetwork
  WIFI_PASSWORD_1 = primary-password
  WIFI_SSID_2 = MobileHotspot
  WIFI_PASSWORD_2 = backup-password
  ```
- `WIFI_SSID` / `WIFI_PASSWORD` (no suffix) are aliases for slot 1 - using either form is fine for single-network setups.
- Each slot's password is stored in its own NVS key when `MASK_CREDENTIALS=true`. Slot 1 uses the legacy `wifi_pass` key for back-compat with existing flashed devices; slots 2–4 use `wifi_pass_2` / `wifi_pass_3` / `wifi_pass_4`.

**HOSTNAME** (optional, default: "cpap")
- Device hostname for local network discovery
- Access via: `http://hostname.local` (e.g., `http://cpap.local`)
- Example: `HOSTNAME = airsense11`


### Upload Destination

**ENDPOINT** (required)
- Network location where files will be uploaded
- Format: `//server/share` or `//server/share/folder`
- Examples:
  - Windows PC: `ENDPOINT = //192.168.1.100/cpap_backups`
  - NAS device: `ENDPOINT = //nas.local/backups`
  - With subfolder: `ENDPOINT = //192.168.1.5/backups/cpap_data`

**ENDPOINT_TYPE** (required)
- Type of upload destination
- Values: 
  - `SMB` - Upload to network share
  - `CLOUD` - Upload to SleepHQ
  - `SMB,CLOUD` - Upload to both (simultaneously)

**CLOUD_CLIENT_ID** (required for CLOUD)
- Your SleepHQ Client ID (this is **NOT** your username)
- Example: `CLOUD_CLIENT_ID = your-client-id`

**CLOUD_CLIENT_SECRET** (required for CLOUD)
- Your SleepHQ Client Secret (this is **NOT** your password)
- Example: `CLOUD_CLIENT_SECRET = your-client-secret`

> ⚠️ **How to get your SleepHQ API Keys**
> 1. A **SleepHQ Pro** (paid) subscription is required to use the API.
> 2. Go to your SleepHQ Dashboard: https://sleephq.com/account
> 3. Click **"Account Settings"** in the bottom-left corner.
> 4. Scroll down to the **"API Keys"** section and click **"Add API Key"**.
> 5. Copy the generated `Client UID` into your config as `CLOUD_CLIENT_ID`
> 6. Copy the generated `Client Secret` into your config as `CLOUD_CLIENT_SECRET`

**ENDPOINT_USER** (required for SMB)
- Username for the network share
- Example: `ENDPOINT_USER = john` or `ENDPOINT_USER = DOMAIN\john`
- Leave empty/omit for guest access (if share allows)

**ENDPOINT_PASSWORD** (required for SMB)
- Password for the network share
- Example: `ENDPOINT_PASSWORD = password123`
- Leave empty/omit for guest access

**SMB_PRESERVE_TIMESTAMPS** (optional, default: true)
- When `true` (default), preserves original file timestamps from the SD card on the NAS via `SMB2_SET_INFO`
- When `false`, files appear on the NAS with the upload time
- Only enable if accurate historical dates on the backup are required
- Adds ~1–5 ms per file on a local NAS

### Schedule Settings

**UPLOAD_MODE** (optional, default: "smart")
- `scheduled`: uploads in the configured time window
- `smart` (recommended): starts shortly after therapy ends (activity + inactivity detection)
- `manual`: never uploads automatically; triggers on-demand via the dashboard ("Force Upload")

**UPLOAD_START_HOUR** (optional, default: 9)
- Start of upload window (0-23, local time)

**UPLOAD_END_HOUR** (optional, default: 21)
- End of upload window (0-23, local time)
- If start == end, uploads are allowed 24/7

**INACTIVITY_SECONDS** (optional, default: 62)
- Required SD bus idle time before smart mode starts uploading
- Range: 10-3600

**EXCLUSIVE_ACCESS_MINUTES** (optional, default: 5)
- Maximum time per upload session while holding SD access
- Range: 1-30

**COOLDOWN_MINUTES** (optional, default: 10)
- Pause between upload sessions
- Range: 1-60

**MAX_DAYS** (optional, default: 365)
- Maximum number of days in the past to check for upload eligibility
- Range: 1-366
- Helps prevent infinite loops on very old data and manages memory usage
- **Note:** Requires valid time synchronization (NTP) to function correctly

**GMT_OFFSET_HOURS** (optional, default: 0)
- Your timezone offset from GMT/UTC in hours
- Used for local time calculations (upload window + status display)
- Examples:
  - `0` = UTC/GMT
  - `-8` = Pacific Time (PST)
  - `-5` = Eastern Time (EST)
  - `+1` = Central European Time (CET)
  - `+10` = Australian Eastern Time (AEST)
- For daylight saving time, adjust the offset (e.g., `-7` for PDT instead of `-8` for PST)

### Power Management Settings

> Power defaults are optimized for AirSense 11 compatibility (minimal current draw). Most users should not need to change these.

**CPU_SPEED_MHZ** (optional, default: 80)
- CPU frequency in MHz (80, 160, 240)
- At the default 80 MHz, DFS is disabled (CPU locked) — no frequency transitions, lowest power
- Set to 160 to re-enable DFS (80–160 MHz) for faster TLS handshakes on non-constrained hardware

**WIFI_TX_PWR** (optional, default: "MID")
- WiFi transmit power level:
  - `LOWEST` — -1 dBm (router on same nightstand)
  - `LOW` — 2 dBm (router within 1–2 metres)
  - `MID` — 5 dBm (default, typical bedroom placement ~3–5 m)
  - `HIGH` — 8.5 dBm (router in adjacent room)
  - `MAX` — 10 dBm (through walls, last resort)
- Increase if you experience WiFi connection issues

**WIFI_PWR_SAVING** (optional, default: "MID")
- WiFi power saving mode:
  - `NONE` — No power saving (maximum responsiveness)
  - `MID` — MIN_MODEM (default, wakes every DTIM — preserves mDNS)
  - `MAX` — MAX_MODEM (lowest power but may miss mDNS queries)

**BROWNOUT_DETECT** (optional, default: enabled)
- Set to `RELAXED` to temporarily disable the hardware brownout detector during WiFi connection, re-enabling it afterwards.
- Set to `OFF` to disable it permanently (use only as a last resort).
- When disabled (set to `OFF`), a warning banner is shown on the web dashboard.

> **Note:** 802.11b is disabled at the firmware level to reduce peak power draw. Bluetooth is also fully disabled. These are not configurable.

### Debugging Settings

**DEBUG** (optional, default: false)
- Enable verbose diagnostic logging at runtime (no re-flash required)
- When `true`, adds per-folder pre-flight scan lines to the log:
  `[FileUploader] Pre-flight scan: folder=20260219 completed=1 pending=0 recent=1`
- Also appends `[res fh= ma= fd=]` heap and resource stats to every log line
- Useful for diagnosing upload scheduling issues; disable when not needed
- Example: `DEBUG = true`

**PERSISTENT_LOGS** (optional, default: false)
- Persist logs to internal flash (4-file rotation: `syslog.0..3.txt`, 32 KB each, 128 KB total on LittleFS) for retrieval across reboots
- Logs flush every **30 seconds**, continuously — including during active uploads — and immediately before every reboot
- Use the **⬇ Download All Logs** button on the Logs tab to download persisted + current log files directly to your browser
- Useful for diagnosing issues that only appear after a reboot or crash
- Automatically disabled if flash write operations fail
- Example: `PERSISTENT_LOGS = true`



By default, credentials are migrated to the device's secure flash memory (NVS) and replaced with `***STORED_IN_FLASH***` in `config.txt`. This provides improved security while keeping the configuration file easy to manage.

**To update a password:** Replace `***STORED_IN_FLASH***` in your `config.txt` (or via the web UI) with the new plaintext value. The device will automatically re-migrate it to secure storage on the next boot.

**Optional: plaintext mode**
If you prefer to keep your passwords as plaintext in `config.txt` (e.g., for easier debugging), add `MASK_CREDENTIALS = false` to your configuration.

> **Warning:** A full (non-OTA) firmware flash erases NVS. If you are using default masking, you must re-enter your passwords after a full flash. OTA updates are not affected.

---

## Common Configuration Examples

### SleepHQ (Cloud Only)
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = password
ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = your-client-id
CLOUD_CLIENT_SECRET = your-client-secret
UPLOAD_MODE = smart
GMT_OFFSET_HOURS = 0
```

### Dual Upload (SMB + SleepHQ)
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = password

ENDPOINT_TYPE = SMB,CLOUD
ENDPOINT = //nas.local/backups
ENDPOINT_USER = user
ENDPOINT_PASSWORD = pass
SMB_PRESERVE_TIMESTAMPS = true

CLOUD_CLIENT_ID = your-client-id
CLOUD_CLIENT_SECRET = your-client-secret

UPLOAD_MODE = smart
GMT_OFFSET_HOURS = 0
```

### US Pacific Time (PST/PDT)
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = password
ENDPOINT = //192.168.1.100/cpap
ENDPOINT_TYPE = SMB
ENDPOINT_USER = john
ENDPOINT_PASSWORD = password
UPLOAD_MODE = scheduled
UPLOAD_START_HOUR = 8
UPLOAD_END_HOUR = 22
INACTIVITY_SECONDS = 62
EXCLUSIVE_ACCESS_MINUTES = 5
COOLDOWN_MINUTES = 10
GMT_OFFSET_HOURS = -8
```

### Europe (CET)
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = password
ENDPOINT = //nas.local/backups
ENDPOINT_TYPE = SMB
ENDPOINT_USER = user
ENDPOINT_PASSWORD = password
UPLOAD_MODE = scheduled
UPLOAD_START_HOUR = 7
UPLOAD_END_HOUR = 21
INACTIVITY_SECONDS = 62
EXCLUSIVE_ACCESS_MINUTES = 5
COOLDOWN_MINUTES = 10
GMT_OFFSET_HOURS = 1
```

### NAS with Guest Access
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = password
ENDPOINT = //192.168.1.50/public
ENDPOINT_TYPE = SMB
ENDPOINT_USER = 
ENDPOINT_PASSWORD = 
SMB_PRESERVE_TIMESTAMPS = true
UPLOAD_MODE = smart
UPLOAD_START_HOUR = 8
UPLOAD_END_HOUR = 22
INACTIVITY_SECONDS = 62
EXCLUSIVE_ACCESS_MINUTES = 5
COOLDOWN_MINUTES = 10
GMT_OFFSET_HOURS = 0
```

---

