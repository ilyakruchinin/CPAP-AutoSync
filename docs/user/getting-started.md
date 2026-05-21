# ESP32 CPAP AutoSync - User Guide

This package contains precompiled firmware for automatically uploading CPAP data from your SD card to a network share or the Cloud (**SleepHQ**).

Built with **extreme ease of use** in mind:
- 😎 **No coding required**: Flash the firmware once via USB using a simple web-based tool, then manage everything through your web browser.
- 📱 **Setup Wizard**: No need to edit text files. Connect to the device's WiFi and follow a visual, step-by-step setup on your phone or computer.
- 🧠 **Auto-Detects Hardware**: Automatically detects if your CPAP supports Smart Mode (AirSense 11) or falls back safely (AirSense 10).
- ✅ **Error-Proof Configuration**: The wizard validates your upload schedule to prevent impossible configurations and SD card errors.

---

## 🚀 Get Started in 3 Steps (Seriously, It's This Easy!)

### Step 1: Flash the Firmware
Upload the firmware right from your web browser - no coding required (details below in [Quick Start](#quick-start)).

### Step 2: Power On & Connect
Insert the card into your CPAP and power it on. On your phone or PC, connect to the WiFi network named **`CPAP-AutoSync`**.

### Step 3: Configure via Visual Wizard
A setup page should open automatically. Follow the wizard to enter your WiFi and destination details.

---

**That's it!** The device will save, restart, and begin uploading your data.

**💻 Web Dashboard:** Once running, access the dashboard at **`http://cpap.local`** to monitor uploads, view logs, and manage settings.

**No SD card reader needed. No complex setup. Just a simple visual wizard.**

---

## Quick Start

### 0. Initialize the SD card

Insert the SD card in your CPAP machine and allow for the CPAP machine to format it.

### 1. Upload Firmware

**Important:** Connect the SD WIFI PRO to the development board with switches set to:
- Switch 1: OFF
- Switch 2: ON

**Browser-based web flasher:**

Use a **desktop Chromium-based browser** (Chrome, Microsoft Edge, or Opera). *Safari and Firefox are not supported.*

1. Download the **`firmware-ota-***.bin`** file from the Releases page. ⚠️ **Do NOT use** `firmware-ota-upgrade-***.bin` — that file is for OTA upgrades only.
2. Open `https://esptool.spacehuhn.com/` in Chrome, Edge, or Opera.
3. Connect the ESP32 board by USB.
4. Click **Connect** and choose the serial port for the board.
   - **Windows:** `USB Serial (COM5)`, `USB-SERIAL CH340 (COMx)`, etc.
   - **Mac/Linux:** `/dev/cu.usbserial-*` or `/dev/ttyUSB0`
   - *If you are not sure which port is correct, click Connect first, then plug in the board. The new port that appears is usually the right one.*
5. Delete any existing rows if needed (by clicking the "bucket" icon on the right), then click **Add** once.
6. Make sure the address is **`0x0`**.
7. Select the downloaded **`firmware-ota-***.bin`** file.
8. Click **Erase** (required for first-time setup).
9. Click **Program**.

> **Issues flashing or finding the port?** See [Advanced Flashing & Troubleshooting](../dev/advanced-flashing.md).

### 2. Visual Configuration (Recommended)

**No SD card reader is required for configuration.**

1. Insert the card into your CPAP machine and power it on.
2. On your smartphone or computer, search for the WiFi network named **`CPAP-AutoSync`** and connect to it.
3. A setup page should automatically open (Captive Portal). If it does not, open your browser and go to **`http://192.168.4.1`**.
4. Follow the visual wizard to enter your WiFi and upload details.
5. Click **Save and Restart**.

### 3. Power On & Dashboard

After configuring, the device will restart and connect to your home WiFi. It will then automatically:
1. Connect to WiFi
2. Sync time with internet
3. Wait for upload eligibility based on your schedule
4. Upload new files to your network share or SleepHQ

**Dashboard Access:** Open **[http://cpap.local](http://cpap.local)** in your browser to view progress and logs.

> [!NOTE]
> **Manual Configuration:** If you prefer to manually edit files rather than using the wizard, see the [Advanced Manual Configuration](../dev/manual-configuration.md) guide.

---

## Web Dashboard Features

The firmware includes a simple web interface accessible at **`http://cpap.local`** (or via its IP address).

- **Live Dashboard**: See real-time upload progress, active schedules, and overall system status.
- **Logs**: View what the device is currently doing or download past logs to diagnose issues.
- **Configuration Editor**: Need to change a password or adjust your upload schedule? You can edit your settings directly from your browser without removing the SD card from the CPAP.
- **SD Access Monitor**: Check if your CPAP machine is currently actively writing to the SD card.
- **Wireless Updates (OTA)**: Upload new firmware updates directly through the browser.

---

## Simplified Troubleshooting

### ⚠️ ESP32 Power Issues (Random Reboots)

If your device randomly reboots (shows "REBOOTING" constantly) or drops WiFi, it is likely experiencing a power dip. The CPAP machine's SD card slot provides limited power.

**Fix:** Move your WiFi router closer (or use a WiFi extender/mesh node). The device automatically reduces its transmission power by default to save energy. If it struggles to reach the router, it will fail to connect.

> *For deeper power troubleshooting steps, see the [Advanced Configuration](../dev/manual-configuration.md) guide.*

### ⚠️ SD Card Errors — Use Scheduled or Manual Mode

> **If your CPAP machine is showing "SD Card Error" or "SD Card Removed" messages, switch to `UPLOAD_MODE = scheduled` or `manual` immediately.**

The default `smart` upload mode detects SD bus activity to decide when it is safe to take the card. On some CPAP models, this activity detection may not work perfectly, causing the uploader to take the SD card at the wrong moment.

**Fix:** Use the web dashboard (or setup wizard) to change your mode from **Smart** to **Scheduled** or **Manual**. In Scheduled mode, the uploader **only runs during the specific hours you set** (e.g. 9 AM–11 PM) and completely avoids checking the card while you sleep. In Manual mode, the system remains in `IDLE` permanently and only runs when you click the **Force Upload** button, ensuring the CPAP machine has uncontested access to the SD card at all other times.

### WiFi Connection Issues
- **Device doesn't connect to WiFi:** Ensure your WiFi is 2.4GHz (ESP32 doesn't support 5GHz). Try moving the router closer.
- **Cloud Upload Failed:** You are likely using your SleepHQ account password instead of an API Key. Generate an API key from the **Account Settings -> API Keys** section on the SleepHQ dashboard (requires Pro subscription).

---

## Developer & Advanced Documentation

Looking for the technical details? We've moved them to their own dedicated documents:
- ⚙️ **[Advanced Manual Configuration](../dev/manual-configuration.md)** - Raw `config.txt` keys, SleepHQ/SMB templates, power limits.
- 🏗️ **[Architecture and Internals](../dev/architecture.md)** - SD stealth mode details, directory structure, upload loop logic.
- 🔌 **[Advanced Flashing & Troubleshooting](../dev/advanced-flashing.md)** - Fallback upload scripts (`.bat`/`.sh`), finding serial ports, reading device logs via serial monitor.

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
