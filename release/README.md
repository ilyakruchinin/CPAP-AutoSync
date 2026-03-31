# ESP32 CPAP AutoSync - User Guide

This package contains precompiled firmware for automatically uploading CPAP data from your SD card to a network share or the Cloud (**SleepHQ**).

## Table of Contents
- [🚀 Get Started in 3 Steps](#-get-started-in-3-steps-seriously-its-this-easy)
- [What This Does](#what-this-does)
- [Firmware Options](#firmware-options)
- [Quick Start](#quick-start)
  - [0. Initialize SD Card](#0-initialize-the-sd-card)
  - [1. Upload Firmware](#1-upload-firmware)
  - [2. Create Configuration](#2-create-configuration-file)
  - [3. Insert SD Card](#3-insert-sd-card-and-power-on)
- [Configuration Reference](#configuration-reference)
- [Common Configuration Examples](#common-configuration-examples)
- [How It Works](#how-it-works)
- [Finding Your Serial Port](#finding-your-serial-port)
- [Web Interface](#web-interface-optional)
- [Troubleshooting](#troubleshooting)
- [File Structure](#file-structure)
- [Package Contents](#package-contents)
- [Legal & Support](#legal--trademarks)

---

## 🚀 Get Started in 3 Steps (Seriously, It's This Easy!)

### Step 1: Flash the Firmware
Upload the firmware using the included scripts (details below in [Quick Start](#quick-start))

### Step 2: Create `config.txt` 
Create a file named `config.txt` on your SD card with **just these lines**:

**👇👇👇 Click the option you want to use** (or click ▸ to expand additional options):

---

<details>
<summary><b>📤 For Network Share (SMB)</b></summary>

```ini
# WiFi
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword

# Upload Destination
ENDPOINT_TYPE = SMB
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
```

**That's it!** Replace the values with your actual WiFi and network share details.
</details>

<details>
<summary><b>☁️ For Generic Cloud (API Key — e.g. SDBFriends)</b></summary>

```ini
# WiFi
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword

# Generic Cloud
ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = sdbfriends
CLOUD_CLIENT_SECRET = your-sdbfriends-api-key
```

**That's it!** Replace with your actual WiFi and API credentials. This simpler model is specifically tested with **SDBFriends** but is technically compatible with any cloud provider following a similar API-key-based design.
</details>

<details>
<summary><b>🔄 For Multiple Destinations (Dual Upload)</b></summary>

```ini
# WiFi
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword

# SleepHQ
ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = your-sleephq-client-id
CLOUD_CLIENT_SECRET = your-sleephq-client-secret
```

**That's it!** Replace with your actual WiFi and SleepHQ credentials.
</details>

<details>
<summary><b>🔄 For Multiple Destinations (Dual Upload)</b></summary>

```ini
# WiFi
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword

# Dual Upload
ENDPOINT_TYPE = SMB,CLOUD
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
CLOUD_CLIENT_ID = sdbfriends
CLOUD_CLIENT_SECRET = your-sdbfriends-api-key
```

**That's it!** Provide credentials for each destination to upload to multiple targets simultaneously.
</details>

```ini
# WiFi
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword

# Dual Upload
ENDPOINT_TYPE = SMB,CLOUD
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
CLOUD_CLIENT_ID = your-sleephq-client-id
CLOUD_CLIENT_SECRET = your-sleephq-client-secret
```

**That's it!** Provide both SMB and SleepHQ credentials to upload to both destinations.
</details>

---

**All other settings are optional and have smart defaults.**

### Step 3: Insert SD Card & Done!
Insert the SD card into your CPAP machine. The device will automatically:
- ✅ Connect to WiFi
- ✅ Sync time
- ✅ Wait for therapy to end (Smart Mode)
- ✅ Upload your CPAP data

**💻 Web Interface:** Once running, access the dashboard at **`http://cpap.local`** (for 60 seconds after boot, then use the IP address) to monitor uploads, view logs, and manage settings.

**No complex setup. No JSON syntax. Just simple key = value pairs.**

Want more control? See [Configuration Reference](#configuration-reference) for all optional settings.

---

## What This Does

- Automatically uploads CPAP data files from SD card to your network storage or the Cloud (SleepHQ)
- Uploads automatically when therapy ends (Smart Mode) or at a scheduled time
- Respects CPAP machine access to the SD card (short upload sessions)
- **Local Network Discovery (mDNS):** Access the device via `http://cpap.local` instead of its IP address *(Note: To save power, `cpap.local` only resolves during the first 60 seconds after the device boots. Accessing it within this window automatically redirects your browser to the device's actual IP address, so your session won't drop when the 60 seconds are up).*
- Tracks which files have been uploaded (no duplicates)
- Automatically creates directories on remote shares as needed
- **Supported CPAP Machines:** ResMed Series 9, 10, and 11 (other brands not supported)

**Capacity:** The firmware tracks upload state for the last **1 year** (rolling window). Older data is automatically ignored based on the `MAX_DAYS` configuration (default 365). The 8GB SD WIFI PRO card can store approximately **8+ years** (3,000+ days) of CPAP data based on typical usage (~2.7 MB per day).

---

## Firmware

This package includes OTA-enabled firmware with web-based update capability:

- **`firmware-ota.bin`** — Complete firmware (bootloader + partitions + app) for initial USB/Serial flashing
- **`firmware-ota-upgrade.bin`** — App-only binary for subsequent updates via the web interface

**Update methods:**
- **Initial flash (preferred):** Use `firmware-ota.bin` with the browser-based web flasher at `https://esptool.spacehuhn.com/`
- **Initial flash (fallback):** Use the included upload scripts or `esptool` if the browser flasher is not available on your system
- **Upgrades:** Upload `firmware-ota-upgrade.bin` via the web interface at `http://cpap.local/ota`

---

## Quick Start

### 0. Initialize the SD card

Insert the SD card in your CPAP machine and allow for the CPAP machine to format it.

### 1. Upload Firmware

**Important:** Connect the SD WIFI PRO to the development board with switches set to:
- Switch 1: OFF
- Switch 2: ON

**Preferred method: browser-based web flasher**

Use a **desktop Chromium-based browser**:
- **Supported:** Chrome, Microsoft Edge, Opera
- **Not supported:** Firefox, Safari
- **Phones/tablets:** not recommended for this tool

1. Extract this release ZIP to a folder on your computer.
2. Open `https://esptool.spacehuhn.com/` in Chrome, Edge, or Opera.
3. Connect the ESP32 board by USB.
4. Click **Connect** and choose the serial port for the board.
   - On Windows this often looks like **`USB Serial (COM5)`**, **`USB-SERIAL CH340 (COMx)`**, or **`Silicon Labs CP210x USB to UART Bridge (COMx)`**.
   - On macOS this usually looks like **`/dev/cu.usbserial-*`** or **`/dev/cu.SLAB_USBtoUART`**.
   - On Linux this is usually **`/dev/ttyUSB0`** or **`/dev/ttyACM0`**.
   - If you are not sure which port is correct, click **Connect** first, then plug in the board. The new port that appears is usually the right one.
5. Delete any existing rows if needed, then click **Add** once.
6. Make sure the address is **`0x0`**.
7. Select **`firmware-ota.bin`**.
8. Click **Erase**.
9. Click **Program**.

**Important:** `firmware-ota.bin` is the complete image for first-time flashing and any release that requires a full reflash. Do **not** use `firmware-ota-upgrade.bin` in this external flasher for that purpose.

**Fallback: included scripts**

If the browser flasher does not work on your computer, use the included scripts instead.

**Windows:**
```cmd
upload-ota.bat COM3
```

**macOS/Linux:**
```bash
./upload.sh /dev/ttyUSB0
```

Replace `COM3` or `/dev/ttyUSB0` with your actual serial port.

### 2. Create Configuration File

Create a file named `config.txt` in the root of your SD card.

**Option A: Cloud Upload (SleepHQ)**
```ini
WIFI_SSID = YourNetworkName
WIFI_PASSWORD = YourNetworkPassword

ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = your-sleephq-client-id
CLOUD_CLIENT_SECRET = your-sleephq-client-secret

GMT_OFFSET_HOURS = 0
```

**Option B: Network Share (SMB)**
```ini
WIFI_SSID = YourNetworkName
WIFI_PASSWORD = YourNetworkPassword

ENDPOINT_TYPE = SMB
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password

GMT_OFFSET_HOURS = 0
```

**Option C: Generic Cloud Upload (API Key — e.g. SDBFriends)**
```ini
WIFI_SSID = YourNetworkName
WIFI_PASSWORD = YourNetworkPassword

ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = sdbfriends
CLOUD_CLIENT_SECRET = your-sdbfriends-api-key

GMT_OFFSET_HOURS = 0
```

**Syntax Notes:** 
- Lines starting with `#` or `//` are comments.
- Spaces around `=` are optional.
- Keys are case-insensitive.

**Common mistake that causes "SSID is empty" errors:**
Missing `config.txt` file, or using invalid Key-Value syntax.

### 3. Insert SD Card and Power On

Insert the SD card into your CPAP machine's SD slot and power it on. The device will:
1. Connect to WiFi
2. Sync time with internet
3. Wait for upload eligibility based on mode
4. Upload new files to your network share

---

## Configuration Reference

### Network Settings

**WIFI_SSID** (required)
- Your WiFi network name
- Example: `WIFI_SSID = HomeNetwork`
- Note: ESP32 only supports 2.4GHz WiFi (not 5GHz)

**WIFI_PASSWORD** (required)
- Your WiFi password
- Example: `WIFI_PASSWORD = MySecurePassword123`

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
- Your Cloud Client ID or Provider ID.
- For **SleepHQ**: Use your SleepHQ Client UID (NOT your username).
- For **SDBFriends**: Use `sdbfriends` (all lowercase).
- Example: `CLOUD_CLIENT_ID = sdbfriends`

**CLOUD_CLIENT_SECRET** (required for CLOUD)
- Your Cloud Client Secret or API Key.
- For **SleepHQ**: Use your SleepHQ Client Secret (NOT your password).
- For **SDBFriends**: Use your SDBFriends API Key.
- Example: `CLOUD_CLIENT_SECRET = your-api-key`

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

### Schedule Settings

**UPLOAD_MODE** (optional, default: "smart")
- `scheduled`: uploads in the configured time window
- `smart` (recommended): starts shortly after therapy ends (activity + inactivity detection)

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

### Credential Security

By default, credentials stay as plaintext in `config.txt`. This is the safest option — your passwords survive full firmware flashes without any extra steps.

**To update a password:** Edit `config.txt` directly.

**Optional: credential masking**
- Add `MASK_CREDENTIALS = true` to your `config.txt`
- On next boot, passwords will be moved to encrypted ESP32 flash (NVS) and replaced with `***STORED_IN_FLASH***`
- On subsequent boots, credentials are loaded from NVS instead of `config.txt`
- To update a masked password, replace `***STORED_IN_FLASH***` with the new plaintext value — the device will re-migrate it

> **Warning:** A full (non-OTA) firmware flash erases NVS. After such a flash with masking enabled, you must re-enter all passwords in `config.txt`. OTA updates are not affected.

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
UPLOAD_MODE = smart
UPLOAD_START_HOUR = 8
UPLOAD_END_HOUR = 22
INACTIVITY_SECONDS = 62
EXCLUSIVE_ACCESS_MINUTES = 5
COOLDOWN_MINUTES = 10
GMT_OFFSET_HOURS = 0
```

---

## How It Works

### First Boot
1. Device reads `config.txt` from SD card
2. Connects to WiFi network
3. Synchronizes time with internet (NTP)
4. Loads upload history from internal LittleFS state files (if present)

### Daily Upload Cycle
1. Waits for upload eligibility based on configured mode (`UPLOAD_MODE`)
   - Smart mode: shortly after therapy ends (activity + inactivity detection)
   - Scheduled mode: during configured upload window
2. **Pre-flight scan** checks for new/changed files (SD-only, no network)
3. Takes control of SD card (only when CPAP is idle)
4. **Phased dual-backend upload** (optimizes memory usage):
   - **Phase 1 — Cloud**: Upload to SleepHQ (on-demand TLS, highest heap for handshake)
   - **Phase 2 — SMB**: Upload to network share (TLS torn down, clean sockets for libsmb2)
   - Time budget auto-scales to 2× configured minutes when both backends enabled
5. Uploads new/changed files in priority order:
   - **SMB**: Root/SETTINGS files first, then DATALOG folders (newest first)
   - **Cloud**: DATALOG folders first (newest first), then Root/SETTINGS files (only if DATALOG files uploaded)
6. **SMB:** Automatically creates directories on remote share
   **Cloud:** Associates data with your SleepHQ account (OAuth only if needed)
7. Releases SD card after session or time budget exhausted
8. Enters COOLDOWN → LISTENING loop for the next cycle (elective reboots skipped by default; set `MINIMIZE_REBOOTS=false` to restore post-upload reboots)
9. Saves progress to separate internal state files for each backend (`/littlefs/.upload_state.v2.smb`/`.cloud` + journals)

### Smart File Tracking
- **DATALOG folders**: Tracks completion (all files uploaded = done)
- **Root/SETTINGS files**: Tracks checksums (only uploads if changed)
- Never uploads the same file twice

### SD Card Sharing
- **Passive Operation:** Only accesses the card when the CPAP machine is idle (no therapy recording)
- **Short Sessions:** Limits exclusive access time (default 5 minutes) to ensure CPAP can reclaim access if needed
- **Automatic Release:** Releases control immediately after session or if therapy starts

---

## Finding Your Serial Port

**Note:** Connect the SD WIFI PRO to the development board with switches set to:
- Switch 1: OFF
- Switch 2: ON

**Easiest method:** Open the web flasher, click **Connect**, then plug in the board. The newly appeared port is usually the correct one.

### Windows
1. Open Device Manager (Win+X, then select Device Manager)
2. Expand "Ports (COM & LPT)"
3. Look for entries such as:
    - `USB Serial (COM5)`
    - `USB-SERIAL CH340 (COMx)`
    - `Silicon Labs CP210x USB to UART Bridge (COMx)`
4. Note the COM port number (for example `COM3`, `COM4`, `COM5`)

**Tip:** If you use the included Windows upload script, running it without arguments will also show COM port instructions.

### macOS
```bash
ls /dev/cu.*
```
Look for `/dev/cu.usbserial-*`, `/dev/cu.SLAB_USBtoUART`, or another newly appeared USB serial device

### Linux
```bash
ls /dev/ttyUSB* /dev/ttyACM*
```
Usually `/dev/ttyUSB0` or `/dev/ttyACM0`

---

## Web Interface

The firmware includes a web interface accessible at **`http://cpap.local`** (or `http://<device-ip>/`).

### Accessing the Web Interface

Once the device connects to WiFi, open a browser and navigate to `http://cpap.local` *(first 60 seconds after boot only — redirects to IP address)*. All features are accessible through a single-page dashboard with the following tabs:

---

### Dashboard Tab
- **Upload Engine** card: live FSM state with glowing badges (LISTENING, ACQUIRING, UPLOADING, COOLDOWN, MONITORING, etc.), upload mode, window, thresholds, and next full upload countdown
- **System** card: current time, time sync status, heap/alloc stats, WiFi signal strength with color coding, endpoint, GMT offset, uptime
- **Upload Progress**: current file, bytes transferred, folder counts per backend (active + inactive)
- Mode explanation helper with dynamic context (current window state, upload scope)
- **Danger Zone**: Force Upload and Reset State buttons with risk descriptions

### Logs Tab
- **Live streaming** log feed via Server-Sent Events (SSE) with automatic reconnect and fallback polling
- Leaving the Logs tab closes the live stream; returning performs an immediate RAM-buffer catch-up and then resumes SSE live streaming
- Live logs remain near-real-time even during active uploads; SSE is throttled during uploads rather than fully paused
- On first visit, backfills full log history from internal flash (streaming progress indicator with KB counter)
- Automatic reboot detection: re-fetches NAND history when a new boot is detected
- **⬇ Download All Logs** — flushes current logs to flash and downloads all saved + current logs as a single `cpap_logs.txt` file (requires `SAVE_LOGS=true`)
- Copy to clipboard / Clear buffer buttons
- Up to 2000 lines buffered client-side across page reloads

### Config Tab
- Direct editor for `config.txt` on the SD card
- Click **Edit** to unlock the textarea (uploads continue uninterrupted)
- **Save & Reboot** writes the file and reboots the device; browser auto-redirects after 10 seconds
- Passwords replaced with `***STORED_IN_FLASH***` — leave unchanged to keep existing credentials

### SD Access Tab
- Real-time SD bus activity graph (pulses from the SD data lines) with CSV-style activity log
- **Start Monitoring** / **Stop** buttons (monitoring persists across tab switches with a banner on other tabs)
- Monitoring is automatically disabled during active uploads
- **⚙ Profiler Wizard** — measures your CPAP machine’s SD write gap pattern and recommends a safe `INACTIVITY_SECONDS` value. Requires CPAP on and blowing air; breathe continuously during the 2–3 minute measurement.

### System Tab
- Live `Free Heap` and `Max Contiguous Alloc` readings with rolling **2-minute minimum** values
- **CPU load graphs** (Core 0 and Core 1) with 2-minute history
- **Heap History** chart: SVG line graph of free heap and max contiguous allocation over time
- Diagnostics data merged into the status endpoint — single 3-second poll for all dashboard and system data

### OTA Tab *(OTA firmware only)*
- Upload new firmware binary (`firmware-ota-upgrade.bin`) directly from the browser
- Or enter a URL to download firmware from GitHub releases
- Progress bar and automatic device restart after update
- **⚠️ Important:** Ensure stable WiFi; do not power off during an OTA update

---

### Security Warning
⚠️ The web server has no authentication. Only use on a trusted local network.

---

## Troubleshooting

### Firmware Upload Issues

**Browser does not support the web flasher**
- Use Chrome, Microsoft Edge, or Opera on a desktop computer
- Firefox and Safari do not currently support Web Serial
- If needed, use the included upload scripts instead

**No serial port appears in the browser**
- Unplug and reconnect the board after clicking **Connect**
- Check the USB cable (must be a data cable, not charge-only)
- Try a different USB port
- Install USB drivers if needed (CH340 or CP210x)
- Close other programs that may already be using the serial port

**"Python is not installed" (Windows)**
- Download Python from https://python.org
- During installation, check "Add Python to PATH"
- Restart command prompt after installation

**"Failed to connect to ESP32"**
- Verify SD WIFI PRO is connected to development board
- Check switches: Switch 1 OFF, Switch 2 ON
- Check USB cable (must be data cable, not charge-only)
- Try holding the BOOT button during upload

**"Permission denied" (Linux/Mac)**
- Run with sudo: `sudo ./upload.sh /dev/ttyUSB0`
- Or add user to dialout group: `sudo usermod -a -G dialout $USER` (logout/login required)

**"Port not found"**
- Make sure ESP32 is connected
- Install USB drivers (CH340 or CP210x)
- Check Device Manager (Windows) or `ls /dev/tty*` (Linux/Mac)

### WiFi Connection Issues

**Device doesn't connect to WiFi**
- Verify WIFI_SSID and WIFI_PASS are correct
- Ensure WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)
- Check WiFi network is in range
- Try moving device closer to router

**WiFi connects but no internet**
- Check router internet connection
- Verify router allows device to access internet
- Check firewall settings

### ⚠️ ESP32 Brownouts & Power Issues

If your device randomly reboots (shows "REBOOTING" constantly) or drops WiFi, it is likely experiencing a power dip (brownout). The CPAP machine's SD card slot provides limited power, and the ESP32's WiFi can draw too much current.

**Recommended Settings to Fix Power Issues:**

1. **Reduce WiFi Transmit Power (Most Effective)**
   Move your router closer (or use a WiFi extender/mesh node) and set:
   `WIFI_TX_PWR = LOW` or `WIFI_TX_PWR = LOWEST`
   *(Default is MID. Lowering TX power drastically reduces current spikes).*
2. **Keep CPU at Base Speed**
   `CPU_SPEED_MHZ = 80`
   *(This is the default, but ensure you haven't increased it. 80 MHz disables dynamic frequency scaling).*
3. **Try Experimental 1-Bit SD Mode**
   `ENABLE_1BIT_SD_MODE = true`
   *(Reduces the number of active SD data lines, halving bus toggle current during uploads. Note: only use if your CPAP does not throw "SD Card Error" when using this mode).*
4. **Relax or Disable Brownout Detection (Last Resort)**
   `BROWNOUT_DETECT = RELAXED` (first choice)
   *(Temporarily bypasses strict voltage checks only during the initial WiFi power spike, and re-enables it for safer general running. Try this if your device repeatedly fails to connect or logs show a brown-out reset).*
   
   If `RELAXED` still doesn't work, try:
   `BROWNOUT_DETECT = OFF` (absolute last resort)
   *(Prevents the ESP32 from intentionally restarting when voltage drops. Device may still crash if voltage drops too low and risks data corruption).*

---

### ⚠️ SD Card Errors — Use Scheduled Mode

> **If your CPAP machine is showing "SD Card Error" or "SD Card Removed" messages, switch to `UPLOAD_MODE = scheduled` immediately.**

The default `smart` upload mode detects SD bus activity to decide when it is safe to take the card. On some CPAP models or firmware versions the activity detection may not work correctly, causing the uploader to take the SD card at the wrong moment. This results in your CPAP displaying an SD card error.

**Fix:**

```ini
UPLOAD_MODE = scheduled
UPLOAD_START_HOUR = 9
UPLOAD_END_HOUR = 23
```

In scheduled mode the uploader **only runs during the configured window** (e.g. 9 AM–11 PM) and completely avoids uploading while you sleep. This eliminates any possibility of conflicting with your CPAP at night.

> **Tip:** Set `UPLOAD_START_HOUR` and `UPLOAD_END_HOUR` to cover the hours when you are typically awake and not using the CPAP machine.

---

### SD Card Issues

**SD card not detected**
- The SD WIFI PRO is an integrated SD card with ESP32 chip
- Verify the device is properly inserted into the CPAP machine
- Check that the device is receiving power

**config.txt not found**
- Ensure file is named exactly `config.txt` (lowercase)
- Place file in root of SD card (not in a folder)
- Verify file uses correct Key-Value format (see examples)

### Cloud / SleepHQ Issues

**OAuth failed with HTTP 401 (invalid_client)**
- You are likely using your SleepHQ account password instead of an API Key.
- You must generate an API key from the **Account Settings -> API Keys** section on the SleepHQ dashboard.
- A **SleepHQ Pro** subscription is required to generate and use API keys.

**Authentication Failed**
- Verify `CLOUD_CLIENT_ID` and `CLOUD_CLIENT_SECRET` in `config.txt`
- Ensure no extra spaces or hidden characters in the credentials
- Check logs for "401 Unauthorized" or "403 Forbidden" errors

**Upload Failed**
- Check internet connection (Cloud upload requires internet)
- Verify `GMT_OFFSET_HOURS` is correct (timestamps matter)
- View logs for specific API error messages

**Frequent Reboots**
- With the default `MINIMIZE_REBOOTS=true`, elective post-upload reboots are skipped; only mandatory reboots (watchdog, OTA, user-triggered) occur
- If you see "Heap fragmented" warnings in logs, the device will still operate but you can set `MINIMIZE_REBOOTS=false` to restore post-upload heap recovery reboots
- Reboots are seamless (fast-boot path) and preserve upload state

**Nothing Uploads**
- Check if files have actually changed (pre-flight scan skips unchanged files)
- Verify recent completed folders have file size changes
- Check logs for "nothing to upload — skipping" messages

### SMB Connection Issues

**Cannot connect to SMB share**
- Verify ENDPOINT format: `//server/share`
- Test share is accessible from another computer on the same network
- Check ENDPOINT_USER and ENDPOINT_PASS are correct
- Try IP address instead of hostname (e.g., `//192.168.1.100/share` instead of `//nas.local/share`)

**Authentication fails**
- Verify username and password are correct
- For Windows, try: `DOMAIN\\username` format
- Check share permissions allow write access
- Try guest access (empty user/pass) if share allows

### Upload Issues

**Files not uploading**
- For scheduled mode: check current local time is inside `UPLOAD_START_HOUR`-`UPLOAD_END_HOUR`
- For smart mode: verify therapy has ended and inactivity threshold elapsed
- Verify internet connection for time sync
- Check SMB connection is working
- View logs via web interface: `http://<device-ip>/logs`

**Upload incomplete**
- Increase `EXCLUSIVE_ACCESS_MINUTES` if files are large
- Check available space on network share
- Verify `MAX_DAYS` is set correctly

### WiFi / Power Connectivity Issues

- If you still experience issues, check the `WIFI_PWR_SAVING` and `WIFI_TX_PWR` options in `config.txt`
- If your device repeatedly fails to connect or logs show `[ERROR] WARNING: System reset due to brown-out (insufficient power supply)`:
  1. Remove the SD card, put it in a card reader connected to your computer.
  2. Open `config.txt` and add `BROWNOUT_DETECT=RELAXED`.
  3. Put the card back in the CPAP. This allows the ESP to bypass the strict voltage checks during the initial WiFi power spike.
  4. If it still fails, try `BROWNOUT_DETECT=OFF`. 
  *Note: Some rare AirSense 11 machines (particularly those made in Singapore) have power supply tolerances that lead to brief voltage sags when the ESP32 turns on its WiFi radio. The RELAXED mode is specifically designed for these machines.*

**Same files uploading repeatedly**
- Check LittleFS is mounted (look for `LittleFS mounted successfully` in logs)
- Check reset-state was not triggered unexpectedly
- Try reset state via web interface

### Time Sync Issues

**Time not synchronized**
- Verify internet connection
- Check firewall allows NTP (UDP port 123)
- Try different NTP server (requires firmware modification)

**Wrong timezone**
- Verify GMT_OFFSET_HOURS is correct for your location
- Remember to adjust for daylight saving time

### Getting More Information

**View Serial Monitor Output**
```bash
# Windows (using PlatformIO)
pio device monitor

# Linux/Mac
screen /dev/ttyUSB0 115200
# or
sudo pio device monitor
```

**View Logs via Web Interface**
```
http://<device-ip>/logs
```

**Check Upload State**
- Look for `/littlefs/.upload_state.v2.smb`/`.cloud` and their `.log` files in internal LittleFS
- Contains upload history, retry counts, and incremental journal updates for each backend

---

## File Structure

### On SD Card
```
/
├── config.txt               # Your configuration (you create this)
├── Identification.json      # ResMed 11 identification (if present)
├── Identification.crc       # Identification checksum (if present)
├── Identification.tgt       # ResMed 9/10 identification (if present)
├── STR.edf                  # Summary data (if present)
├── DATALOG/                 # Therapy data folders
│   ├── 20241114/           # Date-named folders (YYYYMMDD)
│   │   ├── file1.edf
│   │   └── file2.edf
│   └── 20241113/
└── SETTINGS/                # Settings folder
    ├── CurrentSettings.json
    └── CurrentSettings.crc
```

### Internal LittleFS (ESP32 flash)
```
/littlefs/.upload_state.v2.smb
/littlefs/.upload_state.v2.smb.log
/littlefs/.upload_state.v2.cloud
/littlefs/.upload_state.v2.cloud.log
/littlefs/.backend_summary.smb
/littlefs/.backend_summary.cloud
/littlefs/syslog.0.txt
/littlefs/syslog.1.txt
/littlefs/syslog.2.txt
/littlefs/syslog.3.txt
/littlefs/crash_log.txt
```

### On Network Share
Files are uploaded maintaining the same structure:
```
//server/share/
├── Identification.json
├── Identification.crc
├── Identification.tgt
├── STR.edf
├── DATALOG/
│   ├── 20241114/
│   │   ├── file1.edf
│   │   └── file2.edf
│   └── 20241113/
└── SETTINGS/
    ├── CurrentSettings.json
    └── CurrentSettings.crc
```

---

## Fallback: Upload Scripts and Manual Firmware Upload

The browser-based web flasher is the preferred method for most users.
If it is not available on your system, use the included upload scripts or `esptool` directly:

### Windows
```cmd
REM Install esptool
pip install esptool

REM OTA firmware
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 firmware-ota.bin

REM Standard firmware
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 firmware-standard.bin
```

### macOS/Linux
```bash
# Install esptool
pip install esptool

# OTA firmware
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 firmware-ota.bin

# Standard firmware
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 firmware-standard.bin
```

**Note:** The firmware files are complete images (bootloader + partitions + app) and must be flashed at address `0x0`.

---

## Package Contents

- `firmware-ota.bin` - Complete firmware for initial flashing (1.3MB)
- `firmware-ota-upgrade.bin` - App-only binary for web OTA updates (1.2MB)
- `upload-ota.bat` - Windows fallback upload script
- `upload.sh` - macOS/Linux fallback upload script
- `requirements.txt` - Python dependencies (esptool)
- `config.txt.example.simple` - Minimal configuration (bare essentials)
- `config.txt.example.smb` - SMB/network share configuration
- `config.txt.example.sleephq` - SleepHQ cloud configuration
- `config.txt.example.both` - Dual upload (SMB + SleepHQ)
- `README.md` - This file

---

## Legal & Trademarks

- **SleepHQ** is a trademark of its respective owner. This project is an unofficial client and is not affiliated with, endorsed by, or associated with SleepHQ.
  - This project uses the officially published [SleepHQ API](https://sleephq.com/api-docs) and does not rely on any non-official methods.
  - This project is **not intended to compete** with the official [Magic Uploader](https://shop.sleephq.com/products/magic-uploader-pro). We strongly encourage users to support the platform by purchasing the official solution, which comes with vendor support and requires no technical setup (flashing).
- **ResMed** is a trademark of ResMed. This software is not affiliated with ResMed.
- All other trademarks are the property of their respective owners.

### Disclaimer & No Warranty

**USE AT YOUR OWN RISK.**

This project (including source code, pre-compiled binaries, and documentation) is provided "as is" and **without any warranty of any kind**, express or implied.

**By using this software, you acknowledge and agree that:**
1.  **You are solely responsible** for the safety and operation of your CPAP machine and data.
2.  The authors and contributors **guarantee nothing** regarding the reliability, safety, or suitability of this software.
3.  **We are not liable** for any damage to your CPAP machine, SD card, loss of therapy data, or any other direct or indirect damage resulting from the use of this project.
4.  **Warranty Implication:** Using third-party accessories or software with your medical device may void its warranty. You accept this risk entirely.

This software interacts directly with medical device hardware and file systems. While every effort has been made to ensure safety, bugs or hardware incompatibilities can occur.

**GPL-3.0 License Disclaimer:**
> THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW. EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.

<details>
<summary><b>⚠️ Upgrading from v0.7.x or earlier? (Breaking change in v0.8.0)</b></summary>

The configuration format changed from `config.json` (JSON) to `config.txt` (key-value, one setting per line).

Your existing `config.json` **will not** work. You must create a new `config.txt`:
1. Delete or rename your old `config.json`
2. Create `config.txt` using the examples in this package (`config.txt.example*`)
3. Format: `SETTING_NAME = value` (one setting per line)

The new format is simpler and has no JSON syntax to get wrong.
</details>

## Support

For issues, questions, or contributions, visit the project repository.

**Hardware:** ESP32-PICO-D4 (SD WIFI PRO)  

**Requirements:**
- Python 3.7+ (https://python.org)
- USB cable (data cable, not charge-only)
- SD WIFI PRO development board for initial flashing


