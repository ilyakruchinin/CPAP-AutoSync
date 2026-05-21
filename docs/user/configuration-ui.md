# Configuration Reference

All settings are read from `/config.txt` on the SD card at boot. The file uses simple `KEY = VALUE` syntax. Lines starting with `#` are treated as comments. Values may be optionally wrapped in single or double quotes.

---

## 1. Network

| Key | Default | Description |
|---|---|---|
| `WIFI_SSID` | *(required)* | WiFi network name to connect to. |
| `WIFI_PASSWORD` | *(empty)* | WiFi password. Supports all characters including `@`, `!`, `#`. By default, the password is migrated to encrypted flash (NVS) and censored in the config file on the first successful boot. |
| `HOSTNAME` | `cpap` | mDNS hostname. Device becomes reachable at `http://<hostname>.local`. |
| `NTP_SERVER` | *(empty)* | Custom NTP server for time synchronization. If empty, the device uses the NTP server provided by the router (DHCP Option 42), falling back to `pool.ntp.org` if none is provided. |

---

## 2. Upload Destination (Endpoint)

| Key | Default | Description |
|---|---|---|
| `ENDPOINT` | *(required)* | Upload destination. SMB share: `//server/share`. Cloud: `https://sleephq.com` (or leave empty when `ENDPOINT_TYPE=CLOUD`). |
| `ENDPOINT_TYPE` | *(auto-detected)* | Comma-separated list of active backends: `SMB`, `CLOUD`, or `SMB,CLOUD`. If omitted, type is inferred from `ENDPOINT` value. |
| `ENDPOINT_USER` | *(empty)* | SMB username. |
| `ENDPOINT_PASSWORD` | *(empty)* | SMB password. Migrated to encrypted flash by default. |
| `SMB_PRESERVE_TIMESTAMPS` | `true` | Set to `false` to use upload time instead of original file timestamps on the NAS. Enabled by default (files appear with their original sleep-study date on the backup). |

---

## 3. SleepHQ Cloud Upload

Only required when `ENDPOINT_TYPE` includes `CLOUD`.

| Key | Default | Description |
|---|---|---|
| `CLOUD_CLIENT_ID` | *(required for cloud)* | OAuth2 client ID from SleepHQ developer settings. |
| `CLOUD_CLIENT_SECRET` | *(required for cloud)* | OAuth2 client secret. Migrated to encrypted flash by default. |
| `CLOUD_TEAM_ID` | *(auto-discovered)* | SleepHQ team ID. If omitted, auto-discovered via `GET /api/v1/me` on each upload session. Set explicitly to skip the discovery round-trip. |
| `CLOUD_DEVICE_ID` | `0` | SleepHQ device ID to associate imports with. `0` = let SleepHQ auto-assign. |
| `CLOUD_BASE_URL` | `https://sleephq.com` | SleepHQ API base URL. Only change if using a self-hosted instance. |
| `CLOUD_INSECURE_TLS` | `false` | Set to `true` to skip TLS certificate validation. **Not recommended for production.** |
| `MAX_DAYS` | `365` | Maximum number of past days to upload. DATALOG folders older than this are ignored. |
| `RECENT_FOLDER_DAYS` | `2` | Number of days considered "recent" (today + yesterday by default). Recent folders are re-scanned for changes on every upload cycle. |

---

## 4. Upload Schedule

| Key | Default | Description |
|---|---|---|
| `UPLOAD_MODE` | `smart` | Upload strategy. `smart` = continuous monitoring, upload whenever CPAP is idle. `scheduled` = only upload within the configured time window. `manual` = remain in IDLE state permanently; only upload when "Force Upload" is pressed on the dashboard. **Note:** Smart mode requires 4-bit SD bus activity (DAT3). On AirSense 10 CPAPs (which use 1-bit SD), the firmware automatically detects this at boot and overrides `smart` to `scheduled`. The web dashboard shows an indicator when this override is active. |
| `UPLOAD_START_HOUR` | `9` | Start of upload window (0–23, local time). Ignored in smart and manual modes for fresh data. |
| `UPLOAD_END_HOUR` | `21` | End of upload window (0–23, local time). Set equal to `UPLOAD_START_HOUR` for a 24/7 always-open window. Ignored in manual mode. |
| `SMART_START_HOUR` | `6` | The hour (0–23, local time) when Smart mode begins monitoring for CPAP inactivity. Between `UPLOAD_END_HOUR` and this hour, Smart mode stays idle — no upload attempts, safe for therapy interruptions. `0` = midnight (not disabled). Set equal to `UPLOAD_END_HOUR` to disable the quiet period. Only applies when `UPLOAD_MODE=smart`; ignored in scheduled and manual modes. |
| `TZ_STRING` | *(empty)* | POSIX TZ string for automatic DST handling. When set, this takes precedence over `GMT_OFFSET_HOURS`. Examples: `EST5EDT,M3.2.0,M11.1.0` (US Eastern), `CET-1CEST,M3.5.0,M10.5.0/3` (EU Central), `AEST-10AEDT,M10.1.0,M4.1.0/3` (Australia Eastern), `JST-9` (Japan, no DST). See [POSIX TZ format](https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html) for full syntax. |
| `TZ_NAME` | *(empty)* | IANA timezone name written by the Setup Wizard (e.g. `Australia/Melbourne`). Used only by the web UI to pre-select the correct timezone in the dropdown — the firmware ignores this key and always uses `TZ_STRING`. You do not need to set this manually; it is managed automatically when saving through the Setup Wizard. |
| `GMT_OFFSET_HOURS` | `0` | Timezone offset from UTC in whole hours (e.g. `11` for AEDT, `-5` for EST). Used for NTP time and upload window calculation. |

> **Tip**: In `scheduled` mode, the device holds the SD card only during the upload window. In `manual` mode, the device remains in `IDLE` at all times and never accesses the card automatically. Both modes give the CPAP machine uncontested access, making them the safest configurations for avoiding SD card errors.

---

## 5. Upload Timing & Behaviour

| Key | Default | Range | Description |
|---|---|---|---|
| `INACTIVITY_SECONDS` | `62` | 10–3600 | Seconds of SD bus silence required before the device attempts to take SD card control. Increase if your CPAP accesses the card frequently during warm-up. |
| `EXCLUSIVE_ACCESS_MINUTES` | `5` | 1–30 | Maximum minutes the device holds exclusive SD card control per upload session. The session ends early if all work is done. |
| `COOLDOWN_MINUTES` | `5` | 1–60 | Minutes to wait (SD card released) between upload cycles before starting the next inactivity check. |
<!-- MINIMIZE_REBOOTS is an internal / developer-only flag. It controls whether
     the FSM performs an elective soft-reboot after each upload session. The
     default (`true`) skips the reboot and reuses the runtime; a heap safety
     valve at `max_alloc < 32 KB` forces a reboot regardless of this flag.
     Not surfaced to end users; edit via raw `config.txt` only if diagnosing
     heap issues. -->


---

## 6. Power Management

Power defaults are optimised for AirSense 11 compatibility (low peak current). Most users should not need to change these.

| Key | Default | Options | Description |
|---|---|---|---|
| `CPU_SPEED_MHZ` | `80` | `80`, `160`, `240` | ESP32 CPU clock speed. At the default 80 MHz, DFS is disabled (CPU locked) — no frequency transitions, lowest power. Set to `160` to re-enable DFS (80–160 MHz) for faster TLS handshakes on non-constrained hardware. |
| `WIFI_TX_PWR` | `MID` | `LOWEST` (-1 dBm), `LOW` (2 dBm), `MID` (5 dBm), `HIGH` (8.5 dBm), `MAX` (10 dBm) | WiFi transmit power. `MID` (5 dBm) is the default — sufficient for typical bedroom placement (~3–5 m). Use `LOWEST` or `LOW` only if your router is very close. Increase to `HIGH` or `MAX` if your router is further away or through walls. |
| `WIFI_PWR_SAVING` | `MID` | `NONE`, `MID`, `MAX` | WiFi power-saving mode. `MID` (MIN_MODEM) wakes every DTIM for broadcasts — preserves mDNS while saving ~90 mA idle. `MAX` saves slightly more but may miss mDNS queries. |
| `BROWNOUT_DETECT` | `ENABLED` | `ENABLED`, `RELAXED`, `OFF` | Set to `RELAXED` to temporarily disable the hardware brownout detector during WiFi connection (the highest peak-current phase), re-enabling it afterwards. Set to `OFF` to disable it permanently. **Use only as a last resort** for devices that reset frequently due to power drops. When set to `OFF`, the device will not reset on voltage sags, risking data corruption. A persistent warning banner is shown on the web dashboard if set to `OFF`. |

> **Note:** 802.11b is disabled at the firmware level (OFDM only). This is not configurable and eliminates the 370 mA peak TX scenario. Bluetooth is also fully disabled at compile time. Auto light-sleep is enabled — the CPU sleeps between WiFi DTIM intervals in IDLE and COOLDOWN states, reducing idle current to ~2–3 mA. TLS uses hardware-accelerated AES-128-GCM only (ChaCha20 and AES-256 are disabled at compile time for lower CPU load).

---

## 7. Security & Credentials

| Key | Default | Description |
|---|---|---|
| `MASK_CREDENTIALS` | `true` | When `true` (default), credentials are migrated from `config.txt` to ESP32 NVS flash and replaced with `***STORED_IN_FLASH***`. When `false`, credentials remain as plaintext in `config.txt` — the safest option for development but less secure. **Note:** The old key `STORE_CREDENTIALS_PLAIN_TEXT` is no longer recognised. |

---

## 8. SD Card & System

| Key | Default | Description |
|---|---|---|
| `ENABLE_1BIT_SD_MODE` | `false` | If `true`, the ESP32 will mount the SD card in 1-bit mode instead of 4-bit mode. This reduces bus toggling current during ESP-side uploads, but forces a brief 4-bit compatibility remount before handing the card back to the CPAP machine (which expects a 4-bit negotiated state). Leave `false` (default) for the most reliable, lowest-spike CPAP handoff. Enable only if you want to experiment with ESP-side power reduction and your CPAP does not throw SD errors during handoff. |
| `STEALTH_RESTORE` | `true` | When `true` (default), the firmware restores the SD card to Standby state via a stealth sequence (no CMD0) after each upload, before handing the card back to the CPAP. This prevents AirSense 10 machines from power-cycling the ESP32 after upload. The restore re-initializes the SDMMC host without sending CMD0, verifies the card at the known RCA, forces 1-bit mode, and deselects. **Only active on AS10** (no effect on AS11). Set to `false` to disable if it causes issues on your hardware. |

---

## 9. Debugging

| Key | Default | Description |
|---|---|---|
| `PERSISTENT_LOGS` | `false` | Set to `true` to periodically flush the in-memory log buffer to the ESP32's internal `LittleFS` partition (4-file rotation: `syslog.0..3.txt`, 32 KB each, 128 KB total). Logs are flushed every **10 seconds** while the device is idle; periodic flushes are deferred during active uploads by default (see `FLUSH_LOGS_DURING_UPLOAD`). Use the **⬇ Download All Logs** button on the Logs tab to download persisted + current log files to your browser. Logs are written to internal flash only, never to the SD card. **Note:** Regardless of this setting, the log buffer is still flushed to LittleFS before reboot, and to `/uploader_error.txt` on the SD card if a boot-time failure prevents WiFi connectivity. If high-volume logging overruns the 8 KB RAM circular buffer before the next LittleFS flush, the persisted syslog will include a `LOG NOTICE` marker noting that some lines were skipped and how many bytes were affected. |
| `FLUSH_LOGS_DURING_UPLOAD` | `false` | When `false` (default), periodic log flushes to internal flash are paused while an upload is running. This reduces SPI flash write contention with SD reads, TLS encryption, and WiFi traffic — the safest setting for power-constrained setups. When `true`, logs continue to flush every 10 seconds even during uploads, eliminating `LOG NOTICE` gaps in the persisted syslog at the cost of slightly higher power draw and flash wear during upload sessions. |
| `DEBUG` | `false` | Set to `true` to enable verbose diagnostics: (1) per-folder `Pre-flight scan` lines in the upload log, and (2) `[res fh= ma= fd=]` heap/file-descriptor stats appended to every log line. Leave `false` in normal operation to keep logs concise. |

---

## 10. Remote Syslog (UDP)

Optional. Streams all device log lines to a remote syslog server (e.g. rsyslog, syslog-ng, Graylog, Papertrail) via UDP. Useful for persistent off-device log collection, fleet monitoring, and webhook triggers. Syslog is fire-and-forget — if WiFi is down or the server is unreachable, messages are silently dropped (the local circular buffer and LittleFS rotation remain the durable stores).

| Key | Default | Description |
|---|---|---|
| `SYSLOG_HOST` | *(empty)* | IPv4 address of the syslog server (e.g. `192.168.1.100`). Feature is disabled when empty. Must be an IP address, not a hostname (avoids DNS resolution overhead). |
| `SYSLOG_PORT` | `514` | UDP port to send syslog messages to. Only change if your server listens on a non-standard port. |

> **Note:** Syslog output begins after WiFi is connected. Pre-WiFi boot logs are not sent remotely but are captured in the local circular buffer and flushed to LittleFS if `PERSISTENT_LOGS=true`. Log lines are sent in RFC 3164 (BSD syslog) format using facility `local0` with severity mapped from the firmware's `[INFO]`/`[WARN]`/`[ERROR]` prefixes. The device hostname (from the `HOSTNAME` config key) is used as the syslog TAG.

---

## Removed / Legacy Keys

The following keys are **no longer used** by the firmware. They will generate a `WARN: Unknown config key` log message if present in `config.txt`.

| Key | Reason |
|---|---|
| `STORE_CREDENTIALS_PLAIN_TEXT` | **Renamed in v2.0i.** Use `MASK_CREDENTIALS` instead (note: inverted semantics — `MASK_CREDENTIALS = true` enables NVS masking, whereas the old key's `true` meant plaintext). |
| `BOOT_DELAY_SECONDS` | **Removed in v0.9.2.** Cold-boot electrical stabilization is now hardcoded to 15 seconds (a chicken-and-egg problem: the delay happens before the SD card is read, so this value could never actually be applied). |
| `SCHEDULE` | **Legacy.** Parsed and stored but not used by any firmware logic. Superseded by `UPLOAD_MODE`, `UPLOAD_START_HOUR`, and `UPLOAD_END_HOUR`. |
| `GMT_OFFSET` | **Removed.** Use `GMT_OFFSET_HOURS`. |
| `SAVE_LOGS` | **Removed.** Use `PERSISTENT_LOGS`. |
| `LOG_TO_SD_CARD` | **Removed.** Use `PERSISTENT_LOGS`. |
| `ENABLE_SD_CMD0_RESET` | **Removed.** Use `ENABLE_1BIT_SD_MODE` for experimental low-power mode instead. |

---

## Credential Security

By default (`MASK_CREDENTIALS = true`), credentials are migrated to the device's secure flash memory (NVS) on the first successful boot.

**The migration process:**
1. Migrates `WIFI_PASSWORD`, `ENDPOINT_PASSWORD`, and `CLOUD_CLIENT_SECRET` to encrypted ESP32 NVS (flash).
2. Replaces those values in `config.txt` with the placeholder `***STORED_IN_FLASH***`.
3. On subsequent boots, loads credentials from NVS instead of `config.txt`.

> [!WARNING]
> A full (non-OTA) firmware flash erases NVS. If masking is enabled (default), you must re-enter your passwords in `config.txt` (or via the Setup Wizard) after a full flash. OTA updates are not affected.

**Plaintext Mode:**
If you prefer to keep your passwords as plaintext in `config.txt`, set `MASK_CREDENTIALS = false`. This is the safest option for developers who frequently perform full flashes.

If `MASK_CREDENTIALS` is `false` and `config.txt` contains `***STORED_IN_FLASH***` as a password, the firmware logs an error prompting you to re-enter the password. It does **not** attempt to recover from NVS in this mode.

---

## 11. Initial Setup (Web Wizard)

If no valid WiFi configuration is found on the SD card (i.e. first boot or "Reset State" triggered), the device enters **SoftAP Mode**:

1. **Broadcast**: It creates a WiFi network named `CPAP-Setup`.
2. **Captive Portal**: Most devices will automatically open the setup page upon connection. If not, browse to `http://192.168.4.1`.
3. **Wizard**: The Visual Setup Wizard allows you to scan for WiFi networks, enter credentials, and configure upload destinations.
4. **Persistence**: All settings entered in the wizard are written back to `config.txt` on the SD card.
