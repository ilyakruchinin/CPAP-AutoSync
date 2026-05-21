#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
#include <esp_bt.h>
#endif
#include <esp_pm.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <soc/rtc_cntl_reg.h>
#include <esp32/rom/rtc.h>
#include <esp_freertos_hooks.h>
#include <esp_partition.h>
#include <Preferences.h>
#include <LittleFS.h>

#include "Config.h"
#include "SDCardManager.h"
#include "WiFiManager.h"
#include "FileUploader.h"
#include "Logger.h"
#include "pins_config.h"
#include "version.h"
#include "EarlyPCNT.h"

#include "TrafficMonitor.h"
#include "UploadFSM.h"
#include "TlsArena.h"
#include <ESPmDNS.h>

// True when esp_restart() was the reset cause (ESP_RST_SW).
// Any programmatic restart means the CPAP machine was already idle and
// voltages are stable, so cold-boot delays (stabilization, Smart Wait,
// NTP settle) can be skipped.  Set once in setup() from esp_reset_reason().
bool g_heapRecoveryBoot = false;

// ── POWER: Brownout-recovery degraded boot ──
// When the previous reset was ESP_RST_BROWNOUT, boot in a reduced-but-reachable
// mode: no mDNS, lowest TX power, MAX power save. SSE remains active (lightest
// transport). Automatically clears after one successful upload cycle or on next
// clean boot.
bool g_brownoutRecoveryBoot = false;

// ── POWER: Timed mDNS ──
// mDNS runs for 60 seconds after boot/reconnect then stops to eliminate
// continuous multicast group membership and associated radio wakes.
unsigned long g_mdnsStartTime = 0;
const unsigned long MDNS_ACTIVE_DURATION_MS = 60000;  // 60 seconds
bool g_mdnsTimedOut = false;

#ifdef ENABLE_OTA_UPDATES
#include "OTAManager.h"
#endif

#ifdef ENABLE_WEBSERVER
#include "CpapWebServer.h"
#endif

// ============================================================================
// Global Objects
// ============================================================================
#include "NetworkHints.h"

Config config;
SDCardManager sdManager;
WiFiManager wifiManager;
FileUploader* uploader = nullptr;
TrafficMonitor trafficMonitor;
NetworkHints networkHints;

#ifdef ENABLE_OTA_UPDATES
OTAManager otaManager;
#endif

#ifdef ENABLE_WEBSERVER
CpapWebServer* webServer = nullptr;
#endif

// ============================================================================
// Upload FSM State
// ============================================================================
UploadState currentState = UploadState::IDLE;
unsigned long stateEnteredAt = 0;
unsigned long cooldownStartedAt = 0;
bool uploadCycleHadTimeout = false;
bool g_nothingToUpload = false;  // Set when pre-flight finds no work — skip reboot, go to cooldown

// ── No-work suppression ──
// When the last upload cycle found NOTHING_TO_DO, suppress further attempts
// until new PCNT bus activity is detected (CPAP wrote new data to SD card).
// This prevents pointless SD mount + scan cycles every 2 minutes when the
// CPAP hasn't produced any new data — a significant power savings.
bool g_noWorkSuppressed = false;

// ── PCNT capability: true if CPAP uses 4-bit SD (DAT3 pulses detected) ──
// Persisted to NVS on power-on boot; read from NVS on soft/watchdog reboots.
// When false, "smart" upload mode is not available (forced to "scheduled").
bool g_pcntCapable = false;
bool g_apSetupMode = false;
// AP mode is only permitted on cold-boot (power-on / external hard reset).
// Soft reboots (ESP_RST_SW), watchdog resets, and panics must never start AP.
// Set once in setup() from esp_reset_reason() — never changes after boot.
bool g_apModeAllowed = false;

// Monitoring mode flags
bool monitoringRequested = false;
bool stopMonitoringRequested = false;

// IDLE state periodic check
unsigned long lastIdleCheck = 0;
const unsigned long IDLE_CHECK_INTERVAL_MS = 60000;  // 60 seconds

// FreeRTOS upload task (runs upload on separate core for web server responsiveness)
volatile bool uploadTaskRunning = false;
volatile bool uploadTaskComplete = false;
volatile UploadResult uploadTaskResult = UploadResult::ERROR;
TaskHandle_t uploadTaskHandle = nullptr;

// ── Static task stack: allocated in .bss, never touches the dynamic heap ──
// This eliminates the 12KB heap allocation that was fragmenting the largest
// contiguous block (ma dropping from ~45KB to ~36KB, causing SD mount failures).
static StackType_t uploadTaskStack[12288 / sizeof(StackType_t)];
static StaticTask_t uploadTaskTCB;

// Software watchdog: upload task updates this heartbeat; main loop kills task if stale
volatile unsigned long g_uploadHeartbeat = 0;
const unsigned long UPLOAD_WATCHDOG_TIMEOUT_MS = 120000;  // 2 minutes

// Power management lock — held in active states to prevent auto light-sleep.
// Released in IDLE and COOLDOWN so the CPU can enter light-sleep between DTIM intervals.
esp_pm_lock_handle_t g_pmLock = nullptr;

// ── CPU load measurement via FreeRTOS idle hooks ──
// Idle hooks increment counters every time the idle task runs on each core.
// The diagnostics endpoint samples these counters to compute load %.
volatile uint32_t g_idleCount0 = 0, g_idleCount1 = 0;
uint32_t g_cpuLoad0 = 0, g_cpuLoad1 = 0;  // 0-100 percent, updated every 2s
static bool _idleHook0() { g_idleCount0 = g_idleCount0 + 1; return true; }
static bool _idleHook1() { g_idleCount1 = g_idleCount1 + 1; return true; }

// ── Reboot reason helper ──
// Stores a human-readable reason in NVS before esp_restart() so the next
// boot can log it clearly.  The reason is read and cleared early in setup().
static void setRebootReason(const char* reason) {
    Preferences p;
    p.begin("cpap_flags", false);
    p.putString("reboot_why", reason);
    p.end();
}

struct UploadTaskParams {
    FileUploader* uploader;
    SDCardManager* sdManager;
    int maxMinutes;
    DataFilter filter;
    bool forceTriggered;  // true when upload was manually triggered via web UI
    bool reducedRetries;  // true when outside scheduled hours (fewer retries, fast fail)
};

// ============================================================================
// Global State (legacy + shared)
// ============================================================================
unsigned long lastNtpSyncAttempt = 0;
const unsigned long NTP_RETRY_INTERVAL_MS = 5 * 60 * 1000;  // 5 minutes

unsigned long lastWifiReconnectAttempt = 0;
unsigned long lastSdCardRetry = 0;

// Persistent log flush timing
unsigned long lastLogFlushTime = 0;
const unsigned long LOG_FLUSH_INTERVAL_MS = 10 * 1000;  // 10 seconds

// Runtime debug mode: set from config DEBUG=true after config load.
// Gates [res fh= ma= fd=] heap suffix on all log lines and verbose pre-flight output.
bool g_debugMode = false;

#ifdef ENABLE_WEBSERVER
// External trigger flags (defined in WebServer.cpp)
extern volatile bool g_triggerUploadFlag;
extern volatile bool g_forceRecentOnlyFlag;
extern volatile bool g_resetStateFlag;

// Latched copy of g_triggerUploadFlag — set when the FSM transitions to
// ACQUIRING due to a web-UI trigger, before g_triggerUploadFlag is cleared.
// Read once in handleUploading() to propagate into UploadTaskParams.
static bool g_uploadWasForceTriggered = false;

// Monitoring trigger flags (defined in WebServer.cpp)
extern volatile bool g_monitorActivityFlag;
extern volatile bool g_stopMonitorFlag;
extern volatile bool g_abortUploadFlag;
#endif

// ============================================================================
// FSM Helper
// ============================================================================
void transitionTo(UploadState newState) {
    LOGF("[FSM] %s -> %s", getStateName(currentState), getStateName(newState));
    
    // ── POWER: Manage PM lock + PCNT for auto light-sleep ──
    // Hold the lock in active states (CPU must stay awake for PCNT, SD I/O, network).
    // Release in IDLE so auto light-sleep can engage between DTIM intervals.
    // Also suspend/resume the PCNT unit — its ESP_PM_APB_FREQ_MAX lock blocks light-sleep.
    // NOTE: COOLDOWN is no longer treated as a low-power state to prevent the PCNT
    // from missing CPAP activity while the FSM is waiting out the cooldown timer.
    bool newStateIsLowPower = (newState == UploadState::IDLE);
    bool oldStateIsLowPower = (currentState == UploadState::IDLE);
    
    if (newStateIsLowPower && !oldStateIsLowPower) {
        trafficMonitor.suspend();
        if (g_pmLock) {
            esp_pm_lock_release(g_pmLock);
        }
        LOG_DEBUG("[POWER] Low-power state: PM lock released, PCNT suspended — light-sleep enabled");
        #if CORE_DEBUG_LEVEL >= 3
        esp_pm_dump_locks(stdout);
        #endif
    } else if (!newStateIsLowPower && oldStateIsLowPower) {
        if (g_pmLock) {
            esp_pm_lock_acquire(g_pmLock);
        }
        trafficMonitor.resume();
        LOG_DEBUG("[POWER] Active state: PM lock acquired, PCNT resumed — light-sleep inhibited");
    }
    
    currentState = newState;
    stateEnteredAt = millis();
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Extract panic details from the coredump partition after a WDT/panic reset.
 * Scans for the ESP_PANIC_DETAILS ELF note in the raw coredump data and
 * returns the human-readable panic reason (e.g. "Task watchdog got triggered.
 * The following tasks/users did not reset the watchdog in time: - upload (CPU 0)").
 * Returns empty string if no coredump or no panic details found.
 */
static String extractPanicDetailsFromCoredump() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x03, "coredump");
    if (!part) return "";

    // Quick check: erased flash is all 0xFF
    uint8_t magic[4];
    if (esp_partition_read(part, 0, magic, 4) != ESP_OK) return "";
    if (magic[0] == 0xFF && magic[1] == 0xFF) return "";  // Empty/erased

    // Scan for "ESP_PANIC_DETAILS" note name in the raw coredump ELF
    static const char MARKER[] = "ESP_PANIC_DETAILS";
    const size_t MLEN = sizeof(MARKER) - 1;  // 17 chars
    const size_t CHUNK = 512;
    uint8_t buf[CHUNK + 32];  // overlap for cross-chunk boundary matches

    size_t limit = (part->size < 65536) ? part->size : 65536;
    for (size_t off = 0; off < limit; off += CHUNK) {
        size_t readSz = CHUNK + 32;
        if (off + readSz > limit) readSz = limit - off;
        if (readSz < MLEN) break;
        if (esp_partition_read(part, off, buf, readSz) != ESP_OK) break;

        for (size_t i = 0; i + MLEN <= readSz; i++) {
            if (memcmp(buf + i, MARKER, MLEN) != 0) continue;

            // Found marker. ELF NOTE layout:
            //   namesz(4) + descsz(4) + type(4) + name(aligned to 4) + desc
            // The header starts 12 bytes before the name.
            size_t absPos = off + i;
            if (absPos < 12) continue;

            uint32_t descsz;
            if (esp_partition_read(part, absPos - 8, &descsz, 4) != ESP_OK) continue;
            if (descsz == 0 || descsz > 256) continue;

            // "ESP_PANIC_DETAILS\0" = 18 bytes → aligned to 20
            size_t descAbsOff = absPos + ((MLEN + 1 + 3) & ~3);

            char text[257];
            size_t toRead = (descsz < sizeof(text) - 1) ? descsz : sizeof(text) - 1;
            if (esp_partition_read(part, descAbsOff, text, toRead) != ESP_OK) continue;
            text[toRead] = '\0';
            return String(text);
        }
    }
    return "";
}

/**
 * Convert ESP32 reset reason to human-readable string
 * Useful for diagnosing power issues, crashes, and unexpected resets
 */
const char* getResetReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "Unknown";
        case ESP_RST_POWERON:
            return "Power-on reset (normal startup)";
        case ESP_RST_EXT:
            return "External reset via EN pin";
        case ESP_RST_SW:
            return "Software reset via esp_restart()";
        case ESP_RST_PANIC:
            return "Software panic/exception";
        case ESP_RST_INT_WDT:
            return "Interrupt watchdog timeout";
        case ESP_RST_TASK_WDT:
            return "Task watchdog timeout";
        case ESP_RST_WDT:
            return "Other watchdog timeout";
        case ESP_RST_DEEPSLEEP:
            return "Wake from deep sleep";
        case ESP_RST_BROWNOUT:
            return "Brown-out reset (low voltage)";
        case ESP_RST_SDIO:
            return "SDIO reset";
        default:
            return "Unrecognized reset reason";
    }
}

// ============================================================================
// Pre-Main Constructor: Earliest PCNT + MUX Lock
// Runs before app_main() / setup() — captures the very first CPAP bus pulses.
// Priority 101 (0-100 reserved for system/compiler).
// ============================================================================
__attribute__((constructor(101)))
static void preinitPcntAndMux() {
    // 1. Lock MUX to CPAP immediately — eliminates floating-pin jitter
    gpio_reset_pin((gpio_num_t)SD_SWITCH_PIN);
    gpio_set_direction((gpio_num_t)SD_SWITCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);

    // 2. Release any stale RTC hold on CS_SENSE (legacy ULP firmware residue)
    rtc_gpio_hold_dis((gpio_num_t)CS_SENSE);
    rtc_gpio_deinit((gpio_num_t)CS_SENSE);

    // 3. Start PCNT immediately — capture the very first CPAP bus pulses
    EarlyPCNT::init(CS_SENSE);
}

// ============================================================================
// Setup Function
// ============================================================================
void setup() {
    // ── POWER: Immediate CPU throttle ──
    // Reduce from 240 MHz to 80 MHz before anything else runs.
    // Saves ~30-40 mA during the entire 20+ second boot sequence.
    // 80 MHz is the minimum for WiFi and sufficient for all boot I/O.
    setCpuFrequencyMhz(80);
    
    // ── POWER: Release Bluetooth memory ──
    // Firmware is WiFi-only. With pioarduino (source-compiled), CONFIG_BT_ENABLED=n
    // strips BT at compile time — no runtime release needed and esp_bt.h is absent.
    // With the old precompiled framework, runtime release was the only option.
    uint32_t btHeapBefore = ESP.getFreeHeap();
    uint32_t btMaxAllocBefore = ESP.getMaxAllocHeap();
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif
    uint32_t btHeapAfter = ESP.getFreeHeap();
    uint32_t btMaxAllocAfter = ESP.getMaxAllocHeap();
    
    // Initialize serial port
    Serial.begin(115200);
    
    // ── Install TLS arena allocator before any TLS/WiFi activity ──
    // This must happen before WiFi.begin() or any mbedTLS call.
    // Routes large mbedTLS buffer allocations to a static arena in .bss,
    // preventing TLS from fragmenting the general heap.
    tlsArenaInit();
    
    // MUX lock, RTC hold release, and EarlyPCNT::init() are now in
    // preinitPcntAndMux() — a GCC constructor that runs before app_main().
    // This captures CPAP bus pulses ~500ms earlier than the old position here.
    
    delay(1000);
    LOG("\n\n=== CPAP Data Auto-Uploader ===");
    LOGF("Firmware Version: %s", FIRMWARE_VERSION);
    LOGF("BT memory release: free %u->%u (+%u), max_alloc %u->%u",
         btHeapBefore, btHeapAfter, btHeapAfter - btHeapBefore,
         btMaxAllocBefore, btMaxAllocAfter);
    LOGF("Build Info: %s", BUILD_INFO);
    LOGF("Build Time: %s", FIRMWARE_BUILD_TIME);
    
    // Log reset reason for power/stability diagnostics
    esp_reset_reason_t resetReason = esp_reset_reason();
    LOG_INFOF("Reset reason: %s", getResetReasonString(resetReason));
    
    // ── 2C: Raw per-core reset reasons for finer-grained diagnostics ──
    // rtc_get_reset_reason() gives the low-level RTC reset cause per CPU core,
    // which is more granular than esp_reset_reason() (e.g. distinguishes
    // POWERON_RESET=1 vs RTCWDT_BROWN_OUT_RESET=15 vs SW_CPU_RESET=12).
    {
        int rawCore0 = (int)rtc_get_reset_reason(0);
        int rawCore1 = (int)rtc_get_reset_reason(1);
        LOG_INFOF("Reset reason (raw): Core0=%d Core1=%d", rawCore0, rawCore1);
    }

    // ── Early PCNT checkpoint (after ~1s of boot, before detach to TrafficMonitor) ──
    {
        int earlyPulses = EarlyPCNT::read();
        LOG_INFOF("[PCNT] Early checkpoint (boot+1s): %d pulses on DAT3", earlyPulses);
        if (resetReason == ESP_RST_POWERON) {
            LOG_INFO("[PCNT] Power-on boot — PCNT started in pre-main constructor");
        } else {
            LOG_INFOF("[PCNT] Non-power-on boot (reason=%d) — PCNT may miss init window", (int)resetReason);
        }
    }

    // ── 2B: NVS boot counter ──
    {
        Preferences bootStats;
        bootStats.begin("boot_stats", false);
        uint32_t totalBoots = bootStats.getUInt("total", 0) + 1;
        bootStats.putUInt("total", totalBoots);
        bootStats.end();
        LOG_INFOF("[BOOT] Boot #%u", totalBoots);
    }

    // Register CPU idle hooks for load measurement (before any blocking waits)
    esp_register_freertos_idle_hook_for_cpu(_idleHook0, 0);
    esp_register_freertos_idle_hook_for_cpu(_idleHook1, 1);

    // Check for power-related issues
    if (resetReason == ESP_RST_BROWNOUT) {
        LOG_ERROR("WARNING: System reset due to brown-out (insufficient power supply), this could be caused by:");
        LOG_ERROR(" - the CPAP was disconnected from the power supply");
        LOG_ERROR(" - the card was removed");
        LOG_ERROR(" - the CPAP machine cannot provide enough power");
        // ── POWER: Activate brownout-recovery degraded mode ──
        // Boot in reduced-but-reachable mode: no mDNS, no SSE, lowest TX power,
        // MAX power save. This reduces current draw on hardware that just proved
        // it cannot sustain normal operation.
        g_brownoutRecoveryBoot = true;
        LOG_WARN("[POWER] Brownout-recovery mode ACTIVE — degraded boot to reduce power draw");
    } else if (resetReason == ESP_RST_PANIC) {
        LOG_WARN("System reset due to software panic - check for stability issues");
        String panicDetails = extractPanicDetailsFromCoredump();
        if (!panicDetails.isEmpty()) {
            LOGF("Previous crash: %s", panicDetails.c_str());
        }
    } else if (resetReason == ESP_RST_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_INT_WDT) {
        LOG_WARN("System reset due to watchdog timeout - possible hang or power issue");
        String panicDetails = extractPanicDetailsFromCoredump();
        if (!panicDetails.isEmpty()) {
            LOGF("Previous crash: %s", panicDetails.c_str());
        }
    }

    // ── Guard against LittleFS partition-shrink assertion ──
    // LittleFS asserts (lfs_fs_grow_) if the partition shrank since the last
    // format. This happens when the partition table changes (e.g. adding a
    // coredump partition). We store the last known size in NVS and erase the
    // partition data before mounting if it got smaller.
    {
        const esp_partition_t* lfsPart = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
        if (lfsPart) {
            Preferences lfsMeta;
            lfsMeta.begin("lfs_meta", false);
            uint32_t savedSize = lfsMeta.getUInt("part_size", 0);
            if (savedSize != 0 && lfsPart->size < savedSize) {
                LOG_WARNF("LittleFS partition shrank (%u -> %u) — erasing for reformat",
                          savedSize, (unsigned)lfsPart->size);
                esp_partition_erase_range(lfsPart, 0, lfsPart->size);
            }
            lfsMeta.putUInt("part_size", (uint32_t)lfsPart->size);
            lfsMeta.end();
        }
    }

    // Initialize LittleFS for state and internal logs
    if (!LittleFS.begin(true)) {
        LOG_ERROR("Failed to mount LittleFS - state and logs cannot be saved!");
    } else {
        LOG("LittleFS mounted successfully");
        Logger::getInstance().enableLogSaving(true, &LittleFS); // Enable early to capture boot/detector logs
    }

    // Initialize SD card control
    if (!sdManager.begin()) {
        LOG("Failed to initialize SD card manager");
        return;
    }
    
    // Transfer EarlyPCNT's PCNT unit to TrafficMonitor — single unit, zero gap.
    // EarlyPCNT was started in the pre-main constructor; TrafficMonitor now owns it.
    // The accumulated pulse count is preserved for the checkpoint readings below.
    trafficMonitor.adoptUnit(EarlyPCNT::detach(), CS_SENSE);

    // Determine boot type: software reset (ESP_RST_SW) = soft-reboot / FastBoot.
    // Cold boots (power-on, brownout, watchdog) use distinct reason codes.
    g_heapRecoveryBoot = (esp_reset_reason() == ESP_RST_SW);
    bool fastBoot = g_heapRecoveryBoot;

    // AP mode is only permitted on genuine cold-boots (SD just inserted / CPAP just powered on).
    // Soft-reboots (user clicked Reboot in web UI) and crash-reboots must NOT start AP:
    // the user's phone may be on a different network and an unexpected AP would be confusing.
    {
        esp_reset_reason_t rr = esp_reset_reason();
        g_apModeAllowed = (rr == ESP_RST_POWERON || rr == ESP_RST_EXT || rr == ESP_RST_UNKNOWN);
        LOG_INFOF("[AP] Reset reason %d \u2192 AP mode %s", (int)rr, g_apModeAllowed ? "ALLOWED" : "BLOCKED");
    }

    // ── NVS flags check (always runs — uses Preferences + LittleFS, not SD) ──
    {
        Preferences resetPrefs;
        resetPrefs.begin("cpap_flags", false);
        
        // Display and clear stored reboot reason (set by setRebootReason() before esp_restart).
        // Use isKey() first to avoid noisy NVS "NOT_FOUND" error from getString().
        if (resetPrefs.isKey("reboot_why")) {
            String rebootWhy = resetPrefs.getString("reboot_why", "");
            if (rebootWhy.length() > 0) {
                LOGF("[BOOT] Reboot reason: %s", rebootWhy.c_str());
            }
            resetPrefs.remove("reboot_why");
        }

        // Check if software watchdog killed the upload task last boot
        bool watchdogKill = resetPrefs.getBool("watchdog_kill", false);
        if (watchdogKill) {
            LOG_WARN("=== Previous boot: upload task was killed by software watchdog (hung >2 min) ===");
            resetPrefs.putBool("watchdog_kill", false);
        }
        
        bool resetPending = resetPrefs.getBool("reset_state", false);
        if (resetPending) {
            LOG("=== Completing deferred state reset (flag set before reboot) ===");
            resetPrefs.putBool("reset_state", false);
            resetPrefs.end();
            
            // Delete all known state/summary paths from internal LittleFS only.
            // Paths are relative to the LittleFS mount — do NOT include /littlefs/ prefix.
            static const char* STATE_FILES[] = {
                "/.upload_state.v2.smb",
                "/.upload_state.v2.smb.log",
                "/.upload_state.v2.cloud",
                "/.upload_state.v2.cloud.log",
                "/.backend_summary.smb",
                "/.backend_summary.cloud",
                "/.upload_state.v2",      // legacy: pre-split single-manager path
                "/.upload_state.v2.log",
            };
            bool removedAny = false;
            for (const char* path : STATE_FILES) {
                if (LittleFS.remove(path)) {
                    LOGF("Deleted state file: %s", path);
                    removedAny = true;
                }
            }
            if (!removedAny) {
                LOG_WARN("No state files found (may already be clean)");
            }
            LOG("State reset complete — continuing with fresh start");
        } else {
            resetPrefs.end();
        }
    }

    // ── Boot path: wait for bus silence, stealth config read, load config ──
    {
        // Smart Wait constants — same values for both cold and soft-reboot.
        // 5 s of continuous SD bus silence required before taking control.
        const unsigned long SMART_WAIT_REQUIRED_MS = 5000;

        auto runSmartWait = [&]() {
            LOG("Checking for CPAP SD card activity (Smart Wait)...");
            while (true) {
                trafficMonitor.update();
                delay(10);
                if (trafficMonitor.isIdleFor(SMART_WAIT_REQUIRED_MS)) {
                    LOGF("Smart Wait: %lums of bus silence — CPAP is idle", SMART_WAIT_REQUIRED_MS);
                    break;
                }
            }
        };

        if (fastBoot) {
            LOG("[FastBoot] Software reset — skipping electrical stabilization");
            // Run one update() to drain any accumulated pulses into activity tracking
            trafficMonitor.update();
            LOG_INFOF("[PCNT] Pre-SmartWait (FastBoot): active=%d idle=%lums",
                      trafficMonitor.hasActivityLatch() ? 1 : 0,
                      (unsigned long)trafficMonitor.getConsecutiveIdleMs());
            runSmartWait();
        } else {
            // Run one update() to drain accumulated pulses from the constructor era
            trafficMonitor.update();
            LOG_INFOF("[PCNT] Pre-stabilization: active=%d",
                      trafficMonitor.hasActivityLatch() ? 1 : 0);
            LOG("Waiting 8s for electrical stabilization...");
            delay(8000);
            // Continue sampling during stabilization
            trafficMonitor.update();
            LOG_INFOF("[PCNT] Post-stabilization: activeSamples=%lu idleSamples=%lu latch=%d",
                      (unsigned long)trafficMonitor.getTotalActiveSamples(),
                      (unsigned long)trafficMonitor.getTotalIdleSamples(),
                      trafficMonitor.hasActivityLatch() ? 1 : 0);
            runSmartWait();
        }
        
        // ── PCNT capability detection ──
        // The single PCNT unit has been counting since the pre-main constructor.
        // TrafficMonitor's activity latch captures ANY pulses detected since boot.
        // On power-on boot: write result unconditionally to NVS.
        //   Users may switch cards between AS10 and AS11 — power-on boot is
        //   required when moving the card, so NVS must reflect the current machine.
        // On non-power-on boot: PCNT misses the init window, so read from NVS.
        {
            bool activityDetected = trafficMonitor.hasActivityLatch();
            LOG_INFOF("[PCNT] Activity latch: %s (activeSamples=%lu)",
                      activityDetected ? "YES" : "NO",
                      (unsigned long)trafficMonitor.getTotalActiveSamples());

            Preferences pcntPrefs;
            pcntPrefs.begin("pcnt_cap", false);
            if (resetReason == ESP_RST_POWERON) {
                g_pcntCapable = activityDetected;
                pcntPrefs.putBool("capable", g_pcntCapable);
                LOG_INFOF("[PCNT] Power-on detection: %s",
                          g_pcntCapable ? "CAPABLE (4-bit SD, smart mode OK)" : "NOT CAPABLE (1-bit SD, scheduled only)");
            } else {
                g_pcntCapable = pcntPrefs.getBool("capable", false);
                LOG_INFOF("[PCNT] Using cached capability: %s",
                          g_pcntCapable ? "CAPABLE" : "NOT CAPABLE");
            }
            pcntPrefs.end();
        }

        LOG("Boot delay complete.");

        // ── Config read: unified path for AS10 and AS11 ──
        // SDCardManager::takeControl() calls StealthConfigReader::captureCardState()
        // before SD_MMC.begin() on all devices, capturing the card's RCA and bus
        // width without sending CMD0. After SD_MMC.end(), releaseControl() calls
        // restoreToSavedState() to put the card back exactly as it was found.
        // This replaces the old AS10-only custom FAT32 stealth reader and is safe
        // for both AS10 and AS11 at boot.
        bool configLoaded = false;

        LOG_INFOF("[Config] Loading config via SD init (%s)", g_pcntCapable ? "AS11" : "AS10");
        if (sdManager.takeControl()) {
            configLoaded = config.loadFromSD(sdManager.getFS());
            if (configLoaded) {
                LOG_INFO("[Config] Config loaded successfully");
                Logger::getInstance().checkPreviousBootError(sdManager.getFS());
            } else {
                LOG_WARN("[Config] SD init succeeded but config parse failed");
            }
            sdManager.releaseControl();
        } else {
            LOG_WARN("[Config] SD takeControl failed");
        }

        // ── Fallback: regular SD mount if primary config read failed ──
        if (!configLoaded) {
            LOG("Falling back to regular SD card access for config...");

            while (!sdManager.takeControl()) {
                delay(1000);
            }

            if (!config.loadFromSD(sdManager.getFS())) {
                LOG_ERROR("Failed to load configuration - starting AP Setup Mode...");
                
                Logger::getInstance().dumpToSD(sdManager.getFS(), "/uploader_error.txt", "Config load failure");
                
                sdManager.releaseControl();
                
                bool dumped = Logger::getInstance().dumpSavedLogs("config_load_failed");
                if (!dumped) {
                    LOG_WARN("Failed to persist logs (config_load_failed)");
                }

                digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
                
                g_apSetupMode = true;
            } else {
                Logger::getInstance().checkPreviousBootError(sdManager.getFS());
                sdManager.releaseControl();
                configLoaded = true;
            }
        }
    }

    // ── Config post-processing ──
    LOG("Configuration loaded successfully");

    // ── PCNT gating: force scheduled mode if smart mode is not supported ──
    if (!g_pcntCapable && config.isSmartMode()) {
        LOG_WARN("[PCNT] Smart mode not supported on this CPAP (no DAT3 activity) — forcing scheduled mode");
        config.overrideUploadMode("scheduled");
    }
    g_debugMode = config.getDebugMode();
    if (g_debugMode) {
        LOG_WARN("DEBUG mode enabled — verbose pre-flight logs and heap stats active");
    }
    LOG_DEBUGF("WiFi SSID: %s", config.getWifiSSID(0).c_str());
    LOG_DEBUGF("Endpoint: %s", config.getEndpoint().c_str());

    // Configure LittleFS-backed syslog rotation for optional periodic persistence
    Logger::getInstance().enableLogSaving(config.getSaveLogs(), &LittleFS);
    if (config.getSaveLogs()) {
        Logger::getInstance().dumpSavedLogsPeriodic(nullptr);
    }

    // Apply power management settings from config
    LOG("Applying power management settings...");
    
    // Read target CPU frequency but do NOT apply yet — defer until after WiFi
    // has stabilised to avoid compounding a PLL relock spike with the RF
    // calibration spike during WiFi.mode(WIFI_STA).
    int targetCpuMhz = config.getCpuSpeedMhz();

    // ── POWER: Optionally disable brownout detector ──
    // Must happen BEFORE WiFi init (the highest-current boot phase).
    if (config.getBrownoutDetectMode() == BrownoutDetectMode::OFF || 
        config.getBrownoutDetectMode() == BrownoutDetectMode::RELAXED) {
        LOG_WARNF("BROWNOUT_DETECT=%s — disabling brownout detection per config",
                config.getBrownoutDetectMode() == BrownoutDetectMode::OFF ? "OFF" : "RELAXED");
        if (config.getBrownoutDetectMode() == BrownoutDetectMode::OFF) {
            LOG_WARN("[POWER] WARNING: Device will NOT reset on power drops. Risk of data corruption.");
        }
        CLEAR_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
    }

    // Setup WiFi event handlers for logging
    wifiManager.setupEventHandlers();

    // Load cached per-AP connection hints (BSSID, channel, PMF flag) from NVS.
    networkHints.begin();

    // Defer TX power cap — applied inside connectStation() after WiFi.mode(WIFI_STA)
    // to avoid "Neither AP or STA has been started" warning.
    wifiManager.applyTxPowerEarly(config.getWifiTxPower());

    // Initialize WiFi
    if (g_apSetupMode) {
        // Config failed to load — enter AP mode if cold-boot, otherwise continue without WiFi
        if (g_apModeAllowed) {
            wifiManager.startAP();
        } else {
            LOG_WARN("[AP] Config load failed but reset was not a cold-boot — skipping AP mode");
            g_apSetupMode = false;  // Don't mislead the rest of setup into AP-mode paths
        }
    } else {
        // Attempt 1
        bool connected = wifiManager.connectStation(config);
        if (!connected) {
            LOG_WARN("WiFi connect attempt 1 failed — waiting 3s before retry...");
            delay(3000);
            // Attempt 2
            connected = wifiManager.connectStation(config);
        }

        if (!connected) {
            LOG_ERROR("Failed to connect to WiFi after 2 attempts.");

            // ── EMERGENCY BOOT ERROR DUMP ──
            // The WiFi attempts above ran a ~30s busy-wait that froze the main
            // loop, so the PCNT idle counter is stale. Refresh it before gating.
            trafficMonitor.update();
            bool safeToDump = !g_pcntCapable || trafficMonitor.isIdleFor(2000);
            if (safeToDump) {
                if (sdManager.takeControl()) {
                    Logger::getInstance().dumpToSD(sdManager.getFS(), "/uploader_error.txt", "WiFi connection failure");
                    sdManager.releaseControl();
                }
            } else {
                LOG_WARN("[Boot] CPAP bus active — skipping SD log dump to avoid mid-transaction MUX flip");
            }

            // Re-enable brownout detection if it was only relaxed for boot
            if (config.getBrownoutDetectMode() == BrownoutDetectMode::RELAXED) {
                LOG_INFO("[POWER] WiFi connection phase complete — re-enabling brownout detection");
                SET_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
            }

            if (g_apModeAllowed) {
                LOG("[AP] Cold-boot with failed WiFi — starting AP Setup Mode");
                g_apSetupMode = true;
                wifiManager.startAP();
            } else {
                LOG_WARN("[AP] WiFi failed but reset was not a cold-boot — skipping AP mode");
                LOG_WARN("[AP] ⚠️ If you entered wrong WiFi credentials, power-cycle the device to re-enter setup mode.");
            }
        }
    }
    
    if (!g_apSetupMode) {
        // ── POWER: mDNS and WiFi power settings (brownout-aware) ──
        if (g_brownoutRecoveryBoot) {
            // Brownout-recovery: skip mDNS entirely, force lowest TX power + MAX power save
            LOG_WARN("[POWER] Brownout-recovery: skipping mDNS, forcing lowest TX power + MAX power save");
            wifiManager.applyPowerSettings(WifiTxPower::POWER_LOWEST, WifiPowerSaving::SAVE_MAX);
        } else {
            // Normal boot: small delay lets the 3.3V rail recover from the WiFi
            // association + DHCP burst before mDNS fires its multicast announcement.
            delay(200);
            wifiManager.startMDNS(config.getHostname());
            g_mdnsStartTime = millis();
            g_mdnsTimedOut = false;
            wifiManager.applyPowerSettings(config.getWifiTxPower(), config.getWifiPowerSaving());
        }
        LOG("WiFi power management settings applied");
    }

    // ── Remote syslog (UDP) — enable if configured ──
    if (!config.getSyslogHost().isEmpty()) {
        IPAddress syslogIp;
        if (syslogIp.fromString(config.getSyslogHost())) {
            Logger::getInstance().enableSyslog(syslogIp, config.getSyslogPort(),
                                                config.getHostname().c_str());
            LOGF("[Syslog] UDP syslog enabled → %s:%d",
                 config.getSyslogHost().c_str(), config.getSyslogPort());
        } else {
            LOG_WARNF("[Syslog] Invalid SYSLOG_HOST: %s (must be an IPv4 address)",
                      config.getSyslogHost().c_str());
        }
    }

    // Re-enable brownout detection if it was only relaxed for boot
    if (config.getBrownoutDetectMode() == BrownoutDetectMode::RELAXED) {
        LOG_INFO("[POWER] WiFi connection phase complete — re-enabling brownout detection");
        SET_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
    }
    
    // ── POWER: Configure Dynamic Frequency Scaling (DFS) + Auto Light-Sleep ──
    // With CONFIG_PM_ENABLE=y and CONFIG_FREERTOS_USE_TICKLESS_IDLE=y, the CPU
    // automatically scales between min (40 MHz XTAL) and max, and enters
    // light-sleep when all PM locks are released during FreeRTOS idle periods.
    //
    // The WiFi driver holds its own PM lock at 80 MHz during active RF ops.
    // The FSM PM lock (ESP_PM_CPU_FREQ_MAX) keeps CPU at max during uploads/TLS.
    // The PCNT driver holds ESP_PM_APB_FREQ_MAX while enabled (blocks light-sleep).
    //
    // In IDLE/COOLDOWN states, transitionTo() suspends PCNT (releasing its lock)
    // and releases the FSM lock, allowing both DFS down to 40 MHz and auto
    // light-sleep. FreeRTOS tickless idle handles timer-based wakeup automatically.
    // No GPIO wakeup needed — avoids the PCNT/GPIO interrupt conflict.
    esp_pm_config_t pm_config = {
        .max_freq_mhz = targetCpuMhz,  // Respects CPU_SPEED_MHZ config (default 80)
        .min_freq_mhz = 40,            // XTAL frequency — DFS floor when all PM locks released
        .light_sleep_enable = true      // Auto light-sleep when all PM locks released
    };
    esp_err_t pm_err = esp_pm_configure(&pm_config);
    if (pm_err == ESP_OK) {
        LOGF("Power management: DFS (40-%dMHz) + auto light-sleep ENABLED", targetCpuMhz);
    } else {
        LOGF("PM configuration failed (err=%d), CPU stays at %dMHz", pm_err, getCpuFrequencyMhz());
    }
    
    // ── POWER: Apply CPU frequency AFTER WiFi has stabilised ──
    // During boot, the CPU stays at 80 MHz through WiFi init to avoid
    // compounding a PLL relock spike with the RF calibration spike.
    if (targetCpuMhz != getCpuFrequencyMhz()) {
        setCpuFrequencyMhz(targetCpuMhz);
    }
    LOGF("CPU frequency: %dMHz", getCpuFrequencyMhz());
    
    // ── POWER: Light-sleep wakeup strategy ──
    // No explicit GPIO wakeup is configured. Auto light-sleep wakeup is handled by:
    //   1. FreeRTOS tickless idle timer (wakes at next scheduled tick/task unblock)
    //   2. WiFi driver (wakes on DTIM beacon for modem-sleep)
    // GPIO wakeup on CS_SENSE (GPIO 33) was removed because the PCNT driver owns
    // that pin. Instead, PCNT is suspended in low-power states (releasing its PM
    // lock), and FreeRTOS periodic wakes handle activity polling.
    LOG_DEBUG("[POWER] Light-sleep wakeup: FreeRTOS tickless idle + WiFi DTIM");
    
    // ── POWER: Create PM lock for active FSM states ──
    // Acquired in LISTENING/ACQUIRING/UPLOADING/RELEASING/MONITORING/COMPLETE
    // to prevent light-sleep while PCNT counting, SD I/O, or network I/O is active.
    // Released in IDLE and COOLDOWN to allow light-sleep.
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "fsm_active", &g_pmLock) == ESP_OK) {
        // Start with lock acquired — initial state (LISTENING or IDLE) will
        // release it via transitionTo() if entering a low-power state.
        esp_pm_lock_acquire(g_pmLock);
        LOG_DEBUG("PM lock created and acquired for active states");
    } else {
        LOG_WARN("Failed to create PM lock — light-sleep management unavailable");
        g_pmLock = nullptr;
    }

    if (!g_apSetupMode) {
        // Initialize uploader (no SD card needed — state is on LittleFS)
        uploader = new FileUploader(&config, &wifiManager);
        LOG("Initializing uploader...");
        if (!uploader->begin()) {
            LOG_ERROR("Failed to initialize uploader");
            return;
        }
        g_heapRecoveryBoot = false;  // consumed — only skip delays on this one boot
        LOG("Uploader initialized successfully");
        
#ifdef ENABLE_OTA_UPDATES
        // Initialize OTA manager
        LOG("Initializing OTA manager...");
        if (!otaManager.begin()) {
            LOG_ERROR("Failed to initialize OTA manager");
            return;
        }
        otaManager.setCurrentVersion(VERSION_STRING);
        LOG("OTA manager initialized successfully");
        LOGF("OTA Version: %s", VERSION_STRING);
#endif
        
        // Synchronize time with NTP server
        LOG("Synchronizing time with NTP server...");
        ScheduleManager* sm = uploader->getScheduleManager();
        if (sm && sm->isTimeSynced()) {
            LOG("Time synchronized successfully");
            LOGF("System time: %s", sm->getCurrentLocalTime().c_str());
        } else {
            LOG_DEBUG("Time sync not yet available, will retry every 5 minutes");
            lastNtpSyncAttempt = millis();
        }
    }

#ifdef ENABLE_WEBSERVER
    // Initialize web server
    LOG("Initializing web server...");
    
    if (g_apSetupMode) {
        webServer = new CpapWebServer(&config, nullptr, nullptr, &wifiManager);
        webServer->setSdManager(&sdManager);
        if (webServer->begin()) {
            LOG("Web server started successfully in AP Setup Mode");
        } else {
            LOG_ERROR("Failed to start web server");
        }
    } else {
        // Create web server with references to uploader's internal components
        webServer = new CpapWebServer(&config, 
                                          uploader->getStateManager(),
                                          uploader->getScheduleManager(),
                                          &wifiManager);
        
        if (webServer->begin()) {
            LOG("Web server started successfully");
            LOGF("Access web interface at: http://%s", wifiManager.getIPAddress().c_str());
            
#ifdef ENABLE_OTA_UPDATES
            // Set OTA manager reference in web server
            webServer->setOTAManager(&otaManager);
            LOG_DEBUG("OTA manager linked to web server");
#endif
            
            // Set TrafficMonitor reference in web server for SD Activity Monitor
            webServer->setTrafficMonitor(&trafficMonitor);
            LOG_DEBUG("TrafficMonitor linked to web server");

            webServer->setSdManager(&sdManager);
            LOG_DEBUG("SDCardManager linked to web server for config editor");

            // Give web server access to both backend state managers so
            // updateStatusSnapshot() can render one progress row per backend.
            webServer->setSmbStateManager(uploader->getSmbStateManager());
            webServer->setCloudStateManager(uploader->getCloudStateManager());
            
            // Set web server reference in uploader for responsive handling during uploads
            if (uploader) {
                uploader->setWebServer(webServer);
                LOG_DEBUG("Web server linked to uploader for responsive handling");
            }

            // Build static config snapshot once — served from g_webConfigBuf with zero heap alloc.
            webServer->initConfigSnapshot();
            LOG_DEBUG("[WebStatus] Config snapshot built");
        } else {
            LOG_ERROR("Failed to start web server");
        }
    }
#endif

    if (!g_apSetupMode) {
        // Set initial FSM state based on upload mode
        if (uploader && uploader->getScheduleManager() && uploader->getScheduleManager()->isSmartMode()) {
            if (uploader->getScheduleManager()->isSmartQuietPeriod()) {
                LOG("[FSM] Smart mode — starting in IDLE (quiet period active)");
                transitionTo(UploadState::IDLE);
            } else {
                LOG("[FSM] Smart mode — starting in LISTENING (continuous loop)");
                transitionTo(UploadState::LISTENING);
            }
        } else {
            LOG("[FSM] Scheduled mode — starting in IDLE");
            // IDLE is the correct initial state for scheduled mode
            transitionTo(UploadState::IDLE);
        }

        LOG("Setup complete!");
    } else {
        LOG("Setup complete (AP Setup Mode)!");
    }
}

// ============================================================================
// FSM State Handlers
// ============================================================================

void handleIdle() {
    // IDLE is used in scheduled mode AND Smart mode during quiet period.
    // Smart mode quiet period: IDLE from UPLOAD_END_HOUR until SMART_START_HOUR.
    
    unsigned long now = millis();
    if (now - lastIdleCheck < IDLE_CHECK_INTERVAL_MS) return;
    lastIdleCheck = now;
    
    if (!uploader || !uploader->getScheduleManager()) return;
    
    ScheduleManager* sm = uploader->getScheduleManager();
    
    if (sm->isSmartMode()) {
        // Smart mode: transition to LISTENING when quiet period ends
        if (!sm->isSmartQuietPeriod()) {
            LOG("[FSM] Smart mode — quiet period ended, transitioning to LISTENING");
            trafficMonitor.resetIdleTracking();
            transitionTo(UploadState::LISTENING);
        }
        return;
    }
    
    // In scheduled mode: transition to LISTENING when the upload window opens,
    // even if all known files are marked complete. This ensures new DATALOG
    // folders written by the CPAP since the last upload are discovered during
    // the scan phase of the upload cycle.
    if (!sm->isManualMode() && sm->isInUploadWindow() && !sm->isDayCompleted()) {
        LOG("[FSM] Upload window open — transitioning to LISTENING");
        transitionTo(UploadState::LISTENING);
    }
}

void handleListening() {
    // TrafficMonitor.update() is called in main loop before FSM dispatch
    uint32_t inactivityMs = (uint32_t)config.getInactivitySeconds() * 1000UL;

    // ── No-work suppression ──
    // After a NOTHING_TO_DO result, we suppress further upload attempts until
    // new PCNT bus activity is detected (CPAP wrote new data to SD card).
    // Any non-idle PCNT activity clears the suppression flag, allowing the
    // next idle detection to trigger a new upload cycle.
    if (g_noWorkSuppressed) {
        if (trafficMonitor.isBusy() || trafficMonitor.hasActivityLatch()) {
            // Bus activity detected — CPAP is doing something. Clear suppression
            // so the next idle detection triggers a fresh upload attempt.
            g_noWorkSuppressed = false;
            trafficMonitor.clearActivityLatch();
            LOG("[FSM] No-work suppression cleared — new bus activity detected");
        }

        // AS10 fallback: no PCNT activity possible (DAT3 unused in 1-bit mode),
        // so clear suppression on a timer to enable periodic re-scans for new data.
        // Uses INACTIVITY_SECONDS as the interval — the natural "silence" threshold.
        if (!g_pcntCapable && g_noWorkSuppressed) {
            static unsigned long lastPeriodicClearMs = 0;
            if (lastPeriodicClearMs == 0) lastPeriodicClearMs = millis();
            unsigned long periodicIntervalMs = (unsigned long)config.getInactivitySeconds() * 1000UL;
            if (millis() - lastPeriodicClearMs >= periodicIntervalMs) {
                lastPeriodicClearMs = millis();
                g_noWorkSuppressed = false;
                LOG("[FSM] AS10 periodic check — clearing no-work suppression (no PCNT available)");
            }
        }

        ScheduleManager* sm = uploader->getScheduleManager();

        // Edge-Trigger: If suppressed but the schedule window JUST opened, wake up!
        // This prevents Smart Mode from permanently sleeping through the 8AM-10PM 
        // window if the CPAP wasn't physically used to generate PCNT activity.
        static bool lastWindowOpen = false;
        if (sm) {
            bool currentWindowOpen = sm->canUploadOldData();
            if (currentWindowOpen && !lastWindowOpen && g_noWorkSuppressed) {
                g_noWorkSuppressed = false;
                LOG("[FSM] Schedule window opened \u2014 clearing no-work suppression to scan for old data");
            }
            lastWindowOpen = currentWindowOpen;
        }

        // While suppressed, don't transition to ACQUIRING even if idle
        // (still allow scheduled mode to exit to IDLE if window closes)
        if (sm && !sm->isSmartMode()) {
            if (!sm->isInUploadWindow() || sm->isDayCompleted()) {
                LOG("[FSM] Scheduled mode — window closed or day completed while listening");
                g_noWorkSuppressed = false;
                transitionTo(UploadState::IDLE);
            }
        }
        return;
    }

    // Smart mode logic
    if (config.isSmartMode()) {
        // Check if we've entered the quiet period while listening
        ScheduleManager* sm = uploader->getScheduleManager();
        if (sm && sm->isSmartQuietPeriod()) {
            LOG("[FSM] Smart mode — quiet period started, transitioning to IDLE");
            g_noWorkSuppressed = false;
            transitionTo(UploadState::IDLE);
            return;
        }
        
        if (trafficMonitor.isIdleFor(inactivityMs)) {
            LOGF("[FSM] %ds of bus silence confirmed", config.getInactivitySeconds());
            
            // No network pre-connect here — backends connect on-demand when actual work is confirmed
            // (SMB connects lazily in FileUploader, Cloud connects after preflight)
            transitionTo(UploadState::ACQUIRING);
            return;
        }
        return;
    }
    
    // In scheduled mode, check if the upload window has closed while we were listening
    ScheduleManager* sm = uploader->getScheduleManager();
    if (!sm->isSmartMode()) {
        if (!sm->isInUploadWindow() || sm->isDayCompleted()) {
            LOG("[FSM] Scheduled mode — window closed or day completed while listening");
            transitionTo(UploadState::IDLE);
        } else if (trafficMonitor.isIdleFor(inactivityMs)) {
            LOGF("[FSM] Scheduled mode — %ds of bus silence confirmed", config.getInactivitySeconds());
            
            // No network pre-connect here — backends connect on-demand when actual work is confirmed
            transitionTo(UploadState::ACQUIRING);
        }
    }
}

void handleAcquiring() {
    // The upload task owns the session lifecycle:
    //   PCNT re-check → SD mount → minimal work probe → upload → SD release
    // TLS connects on-demand during the cloud phase — the TLS Arena ensures
    // mbedTLS buffers never fragment the general heap, making mount order irrelevant.
    transitionTo(UploadState::UPLOADING);
}

// FreeRTOS task function — runs on Core 0 so main loop (Core 1) stays responsive
// Owns the full session lifecycle:
//   PCNT re-check → SD mount → minimal work probe → upload → SD release
//
// ── On-Demand TLS (no pre-warm) ──────────────────────────────────────────────
// The TLS Arena (TlsArena.cpp) routes large mbedTLS buffer allocations to a
// static .bss arena, so TLS handshake no longer depends on general heap layout.
// This means we can safely mount the SD card first, probe for work, and only
// connect TLS if cloud work actually exists — eliminating the ~15s TLS+SD
// overhead on no-work cycles.
//
// PCNT re-check runs before SD mount to catch CPAP activity that started
// during the transition from LISTENING → ACQUIRING.
// ──────────────────────────────────────────────────────────────────────────────
void uploadTaskFunction(void* pvParameters) {
    UploadTaskParams* params = (UploadTaskParams*)pvParameters;
    
    // Subscribe this task to the task WDT so esp_task_wdt_reset() calls
    // (from SleepHQUploader, NetworkRecovery) don't log "task not found".
    esp_task_wdt_add(NULL);  // NULL = current task
    
    g_uploadHeartbeat = millis();

    // ── Step 1: PCNT re-check — confirm SD bus is still silent ───────────────
    // The original silence detection happened in handleListening() (62s+ idle).
    // Between that detection and now, the CPAP might have started a new SD
    // transaction.  Re-reading the PCNT idle counter catches this.
    // NOTE: Once SD_MMC.begin() runs, ESP's own bus activity resets the counter,
    // making any post-mount check meaningless.  This is the LAST valid check.
    // When the upload was force-triggered via web UI, the PCNT counter will
    // almost certainly be below threshold (no silence detection preceded it).
    // We still log the value for diagnostics but skip the abort.
    {
        uint32_t inactivityMs = (uint32_t)config.getInactivitySeconds() * 1000UL;
        uint32_t currentIdleMs = trafficMonitor.getConsecutiveIdleMs();

        if (currentIdleMs < inactivityMs) {
            if (params->forceTriggered) {
                LOGF("[Upload] PCNT re-check: idle=%lums < threshold=%lums — below threshold but force-triggered, proceeding",
                     (unsigned long)currentIdleMs, (unsigned long)inactivityMs);
            } else {
                LOGF("[Upload] PCNT re-check FAILED: idle=%lums < threshold=%lums — CPAP may have resumed",
                     (unsigned long)currentIdleMs, (unsigned long)inactivityMs);
                LOG_WARN("[Upload] Aborting upload cycle to avoid SD card conflict");

                uploadTaskResult = UploadResult::NOTHING_TO_DO;
                uploadTaskComplete = true;
                esp_task_wdt_delete(NULL);
                delete params;
                vTaskDelete(NULL);
                return;
            }
        } else {
            LOGF("[Upload] PCNT re-check OK: idle=%lums >= threshold=%lums — safe to acquire SD",
                 (unsigned long)currentIdleMs, (unsigned long)inactivityMs);
        }
    }

    // ── Step 2: Mount SD card ────────────────────────────────────────────────
    LOGF("[Upload] Mounting SD: heap fh=%u ma=%u",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (!params->sdManager->takeControl()) {
        LOG_ERROR("[Upload] Failed to acquire SD card control");
        uploadTaskResult = UploadResult::ERROR;
        uploadTaskComplete = true;
        esp_task_wdt_delete(NULL);
        delete params;
        vTaskDelete(NULL);
        return;
    }
    LOG("[FSM] SD card control acquired");
    esp_task_wdt_reset();
    g_uploadHeartbeat = millis();

    // ── Step 3: Minimal work probe — check if any backend has pending work ───
    // Streaming directory check with minimal heap churn. If no work exists,
    // unmount SD immediately and return NOTHING_TO_DO — no TLS, no task overhead.
    LOGF("[Upload] Work probe: heap fh=%u ma=%u",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    {
        auto workResult = params->uploader->hasWorkToUpload(params->sdManager->getFS());
        esp_task_wdt_reset();
        g_uploadHeartbeat = millis();

        if (!workResult.hasCloudWork && !workResult.hasSmbWork) {
            LOG("[Upload] Work probe: no work for any backend — releasing SD");
            if (params->sdManager->hasControl()) {
                params->sdManager->releaseControl();
            }
            uploadTaskResult = UploadResult::NOTHING_TO_DO;
            uploadTaskComplete = true;
            esp_task_wdt_delete(NULL);
            delete params;
            vTaskDelete(NULL);
            return;
        }
        LOGF("[Upload] Work probe: cloud=%d smb=%d — proceeding with upload",
             workResult.hasCloudWork, workResult.hasSmbWork);
    }

    // ── Step 4: Run phased upload (CLOUD first with on-demand TLS, then SMB) ─
    // TLS connects on-demand in cloud phase's begin() — the TLS Arena ensures
    // mbedTLS buffers come from static .bss, not the general heap.
    LOGF("[Upload] Starting upload session: heap fh=%u ma=%u",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    UploadResult result = params->uploader->runFullSession(
        params->sdManager, params->maxMinutes, params->filter, params->reducedRetries);

    // Step 4b: Refresh the work-probe snapshot with post-upload state while
    // the SD is still mounted.  Without this, the dashboard progress counts
    // stay frozen at their pre-session values until the next upload session
    // runs a fresh probe — so a successful RECENT_FOLDER_DAYS refresh of
    // today's folder would leave the UI stuck at "N-1 / N · 1 left" even
    // though the folder is now fully synced.  The probe is cheap (100–200 ms
    // of SD time for a typical card) and we've already paid the SD-mount
    // cost.
    //
    // TIMEOUT is included because the timer expiring with partial progress
    // is a normal outcome (not an error).  If we skip the probe, the
    // progress bar reverts to the stale pre-session snapshot — e.g. SMB
    // shows 0 / 12 even though two folders were successfully uploaded during
    // the phase.  ERROR is still skipped: the state may be inconsistent and
    // a fresh probe on the next cycle will sort it out.
    FileUploader::WorkProbeResult postWorkResult = {false, false, -1, -1, -1};
    bool postProbeRan = false;
    if ((result == UploadResult::COMPLETE || result == UploadResult::NOTHING_TO_DO ||
         result == UploadResult::TIMEOUT)
        && params->sdManager->hasControl()) {
        postWorkResult = params->uploader->hasWorkToUpload(params->sdManager->getFS());
        postProbeRan = true;
        esp_task_wdt_reset();
        g_uploadHeartbeat = millis();
    }

    if (postProbeRan && result != UploadResult::ERROR) {
        time_t completedAt = time(nullptr);
        if (completedAt >= 1000000000) {
            UploadStateManager* cloudSm = params->uploader->getCloudStateManager();
            UploadStateManager* smbSm = params->uploader->getSmbStateManager();
            bool cloudComplete = !cloudSm || (!postWorkResult.hasCloudWork &&
                                             postWorkResult.universe >= 0 &&
                                             postWorkResult.cloudSynced == postWorkResult.universe);
            bool smbComplete = !smbSm || (!postWorkResult.hasSmbWork &&
                                         postWorkResult.universe >= 0 &&
                                         postWorkResult.smbSynced == postWorkResult.universe);

            if (cloudSm && cloudComplete) {
                cloudSm->setLastUploadTimestamp((unsigned long)completedAt);
                cloudSm->save(LittleFS);
            }
            if (smbSm && smbComplete) {
                smbSm->setLastUploadTimestamp((unsigned long)completedAt);
                smbSm->save(LittleFS);
            }
            if (cloudComplete && smbComplete) {
                ScheduleManager* schedule = params->uploader->getScheduleManager();
                if (schedule) {
                    schedule->setLastUploadTimestamp((unsigned long)completedAt);
                    schedule->markDayCompleted();
                }
                result = UploadResult::COMPLETE;
            }
        }
    }

    // Step 5: Release SD card
    if (params->sdManager->hasControl()) {
        params->sdManager->releaseControl();
    }

    uploadTaskResult = result;
    uploadTaskComplete = true;
    
    esp_task_wdt_delete(NULL);  // Unsubscribe before self-delete
    delete params;
    vTaskDelete(NULL);  // Self-delete
}

void handleUploading() {
    if (!uploader) {
        transitionTo(UploadState::RELEASING);
        return;
    }
    
    if (!uploadTaskRunning) {
        // ── First call: determine filter and spawn upload task ──
        ScheduleManager* sm = uploader->getScheduleManager();
        DataFilter filter;

#ifdef ENABLE_WEBSERVER
        // Force Upload outside scheduled window → recent data only
        bool forceRecent = g_forceRecentOnlyFlag;
        g_forceRecentOnlyFlag = false;
#else
        bool forceRecent = false;
#endif

        if (sm && sm->isManualMode()) {
            filter = DataFilter::ALL_DATA;
            LOG("[FSM] Manual mode force upload — uploading all data");
        } else if (forceRecent) {
            filter = DataFilter::FRESH_ONLY;
            LOG("[FSM] Force upload (recent only) — outside scheduled window");
        } else {
            bool canFresh = sm->canUploadFreshData();
            bool canOld = sm->canUploadOldData();
            
            if (canFresh && canOld) {
                filter = DataFilter::ALL_DATA;
            } else if (canFresh) {
                filter = DataFilter::FRESH_ONLY;
            } else if (canOld) {
                filter = DataFilter::OLD_ONLY;
            } else {
                LOG_WARN("[FSM] No data category eligible, releasing");
                transitionTo(UploadState::RELEASING);
                return;
            }
        }

        LOGF("[FSM] Heap before upload task: fh=%u ma=%u",
             ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        g_abortUploadFlag = false;  // Clear any stale abort request

        // Disable web server handling inside upload task — main loop handles it
        // This prevents concurrent handleClient() calls from two cores
#ifdef ENABLE_WEBSERVER
        uploader->setWebServer(nullptr);
#endif

        // Reduce retries outside scheduled hours to minimize SD card hold time
        bool outsideWindow = false;
        {
            ScheduleManager* sm = uploader->getScheduleManager();
            if (sm) outsideWindow = !sm->isInUploadWindow();
        }

        UploadTaskParams* params = new UploadTaskParams{
            uploader, &sdManager, config.getExclusiveAccessMinutes(), filter,
            g_uploadWasForceTriggered,  // propagate manual-trigger state to upload task
            outsideWindow               // reduced retries when outside upload window
        };
        g_uploadWasForceTriggered = false;  // consumed
        
        uploadTaskComplete = false;
        uploadTaskRunning = true;
        wifiManager.suspendRoaming();  // no roam scans / AP switches mid-upload

        // Relax task watchdog during upload — TLS handshake (5-15s of CPU-intensive
        // crypto) starves IDLE0 on Core 0. Instead of removing IDLE0 from monitoring
        // (which causes "task not found" error spam because IDLE0 still calls
        // esp_task_wdt_reset internally), keep both cores monitored but increase
        // timeout to 30s. IDLE0 feeds the WDT during socket I/O waits.
        {
            esp_task_wdt_config_t wdt_cfg = {
                .timeout_ms = 30000,
                .idle_core_mask = (1 << 0) | (1 << 1),  // Both cores
                .trigger_panic = true
            };
            esp_task_wdt_reconfigure(&wdt_cfg);
        }
        
        // Pin to Core 0 (protocol core) — keeps Core 1 free for main loop + web server
        // Stack: 12KB in static .bss — never touches the dynamic heap.
        // This prevents the task stack from fragmenting the largest contiguous block.
        uploadTaskHandle = xTaskCreateStaticPinnedToCore(
            uploadTaskFunction,  // Task function
            "upload",            // Name
            sizeof(uploadTaskStack) / sizeof(StackType_t),  // Stack depth in words
            params,              // Parameters
            1,                   // Priority (same as loop task)
            uploadTaskStack,     // Static stack buffer
            &uploadTaskTCB,      // Static task TCB
            0                    // Pin to Core 0
        );
        
        if (uploadTaskHandle == nullptr) {
            LOG_ERRORF("[FSM] Failed to create upload task (free=%u, max_alloc=%u) \u2014 releasing",
                       ESP.getFreeHeap(),
                       ESP.getMaxAllocHeap());
            uploadTaskRunning = false;
            wifiManager.resumeRoaming();
            delete params;
            // Restore normal watchdog timeout (task creation failed)
            {
                esp_task_wdt_config_t wdt_cfg = {
                    .timeout_ms = 5000,
                    .idle_core_mask = (1 << 0) | (1 << 1),  // Both cores
                    .trigger_panic = true
                };
                esp_task_wdt_reconfigure(&wdt_cfg);
            }
#ifdef ENABLE_WEBSERVER
            uploader->setWebServer(webServer);
#endif
            transitionTo(UploadState::RELEASING);
        } else {
            LOGF("[FSM] Upload task started on Core 0 (static stack, non-blocking) — heap after: fh=%u ma=%u",
                 ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        }
    } else if (uploadTaskComplete) {
        // ── Task finished: read result and transition ──
        uploadTaskRunning = false;
        wifiManager.resumeRoaming();
        uploadTaskHandle = nullptr;
        g_abortUploadFlag = false;  // Clear abort flag — task has stopped
        
        // Restore normal watchdog timeout now that Core 0 is free
        {
            esp_task_wdt_config_t wdt_cfg = {
                .timeout_ms = 5000,
                .idle_core_mask = (1 << 0) | (1 << 1),  // Both cores
                .trigger_panic = true
            };
            esp_task_wdt_reconfigure(&wdt_cfg);
        }
        
        // Restore web server handling in uploader
#ifdef ENABLE_WEBSERVER
        uploader->setWebServer(webServer);
#endif
        
        UploadResult result = (UploadResult)uploadTaskResult;
        
        switch (result) {
            case UploadResult::COMPLETE:
                g_nothingToUpload = true;
                if (uploader->hasIncompleteFolders()) {
                    LOG("[FSM] Session complete but backends still have incomplete folders \u2014 will retry");
                    g_noWorkSuppressed = false;
                } else {
                    LOG("[FSM] All folders complete \u2014 suppressing retries until new bus activity");
                    g_noWorkSuppressed = true;
                }
                transitionTo(UploadState::COMPLETE);
                break;
            case UploadResult::TIMEOUT:
                uploadCycleHadTimeout = true;
                transitionTo(UploadState::RELEASING);
                break;
            case UploadResult::ERROR:
                LOG_ERROR("[FSM] Upload error occurred");
                transitionTo(UploadState::RELEASING);
                break;
            case UploadResult::NOTHING_TO_DO:
                g_nothingToUpload = true;
                if (uploader->hasIncompleteFolders()) {
                    LOG("[FSM] Work probe found no actionable work but folders remain incomplete — will retry");
                    g_noWorkSuppressed = false;
                } else {
                    LOG("[FSM] Nothing to upload — suppressing retries until new bus activity");
                    g_noWorkSuppressed = true;
                }
                transitionTo(UploadState::RELEASING);
                break;
        }
    }
    // else: task still running — return immediately (non-blocking)
}

void handleReleasing() {
    if (sdManager.hasControl()) {
        sdManager.releaseControl();
    }
    
    // If monitoring was requested during upload, go to MONITORING instead of COOLDOWN
    if (monitoringRequested) {
        monitoringRequested = false;
        trafficMonitor.enableSampleBuffer();  // Allocate buffer for web UI
        trafficMonitor.resetStatistics();
        LOG("[FSM] Monitoring requested during upload — entering MONITORING after release");
        transitionTo(UploadState::MONITORING);
        return;
    }

    // If nothing was uploaded, skip the reboot and go straight to cooldown.
    // This prevents an endless reboot cycle when all backends are already synced.
    if (g_nothingToUpload) {
        g_nothingToUpload = false;
        trafficMonitor.clearActivityLatch();  // Clear ESP-generated activity before entering cooldown
        LOGF("[FSM] Nothing to upload — entering cooldown without reboot (fh=%u ma=%u)",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        cooldownStartedAt = millis();
        transitionTo(UploadState::COOLDOWN);
        return;
    }

    // MINIMIZE_REBOOTS: skip elective reboot and reuse existing runtime.
    // The device enters COOLDOWN → LISTENING and picks up work in the next cycle.
    if (config.getMinimizeReboots()) {
        unsigned fh = (unsigned)ESP.getFreeHeap();
        unsigned ma = (unsigned)ESP.getMaxAllocHeap();
        if (g_debugMode) {
            LOGF("[FSM] MINIMIZE_REBOOTS: skipping elective reboot after upload (fh=%u ma=%u)", fh, ma);
        }
        // Heap safety valve: force reboot if contiguous heap is critically low.
        // 32KB is below the ~36KB minimum needed for TLS handshake and leaves
        // insufficient margin for SMB PDU allocations + lwIP buffers.
        // The 38900-byte floor during active SMB transfers is normal and recovers
        // after transfer completes — this valve only fires if heap stays fragmented.
        if (ma < 32000) {
            LOG_WARN("[FSM] Heap critically fragmented (ma < 32KB) — forcing reboot to restore heap");
            setRebootReason("Heap safety valve (ma < 32KB)");
            Logger::getInstance().flushBeforeReboot();
            delay(200);
            esp_restart();
        } else if (ma < 35000) {
            LOG_WARN("[FSM] Heap fragmented — contiguous block below 35KB. Will reboot if it drops below 32KB.");
        }
        uploadCycleHadTimeout = false;
        trafficMonitor.clearActivityLatch();  // Clear ESP-generated activity before entering cooldown
        cooldownStartedAt = millis();
        transitionTo(UploadState::COOLDOWN);
        return;
    }

    // Default: soft-reboot after a real upload session.
    // A clean reboot restores the full contiguous heap and keeps the FSM simple.
    // The fast-boot path (ESP_RST_SW) skips cold-boot delays.
    LOGF("[FSM] Upload session complete — soft-reboot to restore heap (fh=%u ma=%u)",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    setRebootReason("Heap recovery after upload session");
    Logger::getInstance().flushBeforeReboot();
    delay(200);
    esp_restart();
}

void handleCooldown() {
    unsigned long cooldownMs = (unsigned long)config.getCooldownMinutes() * 60UL * 1000UL;
    
    if (millis() - cooldownStartedAt < cooldownMs) {
        return;  // Non-blocking wait
    }
    
    LOGF("[FSM] Cooldown complete (%d minutes)", config.getCooldownMinutes());
    uploadCycleHadTimeout = false;
    
    ScheduleManager* sm = uploader->getScheduleManager();
    
    if (sm->isSmartMode()) {
        // Smart mode: return to LISTENING unless in quiet period
        if (sm->isSmartQuietPeriod()) {
            LOG("[FSM] Smart mode — cooldown expired during quiet period, transitioning to IDLE");
            transitionTo(UploadState::IDLE);
        } else {
            trafficMonitor.resetIdleTracking();
            LOG("[FSM] Smart mode — returning to LISTENING (continuous loop)");
            transitionTo(UploadState::LISTENING);
        }
    } else if (sm->isManualMode()) {
        LOG("[FSM] Manual mode — cooldown complete, transitioning to IDLE");
        transitionTo(UploadState::IDLE);
    } else {
        // Scheduled mode: return to LISTENING if still in window and day not done
        if (sm->isInUploadWindow() && !sm->isDayCompleted()) {
            trafficMonitor.resetIdleTracking();
            transitionTo(UploadState::LISTENING);
        } else {
            LOG("[FSM] Scheduled mode — window closed or day completed");
            transitionTo(UploadState::IDLE);
        }
    }
}

void handleComplete() {
    // Clear brownout-recovery mode after a successful upload — the device
    // has proven it can sustain a full upload cycle at current power levels.
    if (g_brownoutRecoveryBoot) {
        g_brownoutRecoveryBoot = false;
        LOG_INFO("[POWER] Brownout-recovery mode cleared — successful upload cycle");
    }
    
    ScheduleManager* sm = uploader->getScheduleManager();
    
    if (sm->isSmartMode()) {
        // Smart mode: release → cooldown → listening (continuous loop)
        // Next cycle will scan SD card and discover any new data naturally
        LOG("[FSM] Smart mode complete — continuing loop via RELEASING → COOLDOWN → LISTENING");
        transitionTo(UploadState::RELEASING);
    } else if (sm->isManualMode()) {
        // Manual mode: release → cooldown → idle
        LOG("[FSM] Manual mode complete — transitioning to RELEASING");
        transitionTo(UploadState::RELEASING);
    } else {
        // Scheduled mode: done for today
        sm->markDayCompleted();
        LOG("[FSM] Scheduled mode — day marked as completed");
        transitionTo(UploadState::IDLE);
    }
}

void handleMonitoring() {
    // TrafficMonitor.update() runs as normal (called in main loop)
    // No upload activity, no SD card access
    // Web endpoint /api/sd-activity serves live PCNT sample data
    
    if (stopMonitoringRequested) {
        stopMonitoringRequested = false;
        LOG("[FSM] Monitoring stopped by user");
        trafficMonitor.disableSampleBuffer();  // Free ~2.4KB buffer
        ScheduleManager* sm = uploader ? uploader->getScheduleManager() : nullptr;
        if (sm && sm->isSmartMode()) {
            trafficMonitor.resetIdleTracking();
            transitionTo(UploadState::LISTENING);
        } else {
            transitionTo(UploadState::IDLE);
        }
    }
}

// ============================================================================
// Loop Function
// ============================================================================
void loop() {
    // ── Always-on tasks ──
    
    // Periodic persisted-log flush (every 10 seconds)
    // Uses multi-file rotation on LittleFS — independent of SD_MMC / upload task.
    // ── POWER: By default, skip during active uploads to avoid internal SPI flash
    // writes overlapping with SD reads, TLS encryption, and WiFi TX bursts.
    // Set FLUSH_LOGS_DURING_UPLOAD=true in config.txt to flush during uploads too.
    // flushBeforeReboot() ensures no logs are lost on post-upload reboot.
    if (!uploadTaskRunning || config.getFlushLogsDuringUpload()) {
        unsigned long currentTime = millis();
        if (currentTime - lastLogFlushTime >= LOG_FLUSH_INTERVAL_MS) {
            Logger::getInstance().dumpSavedLogsPeriodic(nullptr);
            lastLogFlushTime = currentTime;
        }
    }
    
    // Update traffic monitor only in states that need activity detection
    // LISTENING: needs idle detection to trigger uploads
    // COOLDOWN: needs to catch activity so the FSM doesn't falsely suppress retries
    // MONITORING: needs live data for web UI
    if (currentState == UploadState::LISTENING || 
        currentState == UploadState::MONITORING ||
        currentState == UploadState::COOLDOWN) {
        trafficMonitor.update();
    }

#ifdef ENABLE_WEBSERVER
    // Handle web server requests
    if (webServer) {
        // Service captive portal DNS in AP mode — without this, phones never get
        // DNS responses and the auto-redirect to the setup page doesn't work.
        wifiManager.processDNS();
        webServer->handleClient();
        // Push SSE log events to connected client (if any).
        // Upload-time throttling is handled inside pushSseLogs() so logs remain
        // near-live without generating continuous high-rate traffic.
        pushSseLogs();
        // Status snapshot is rebuilt on-demand in handleApiStatus() — no periodic
        // rebuild needed. The API handler calls updateStatusSnapshot() before
        // serving, so the data is always fresh when a client requests it.
    }
    
    // ── POWER: Timed mDNS — stop responder after 60 seconds ──
    // mDNS is only needed for initial .local discovery. After the browser
    // resolves the hostname and gets redirected to the IP, mDNS can stop
    // to eliminate multicast group membership and associated radio wakes.
    if (!g_mdnsTimedOut && g_mdnsStartTime > 0 &&
        (millis() - g_mdnsStartTime >= MDNS_ACTIVE_DURATION_MS)) {
        g_mdnsTimedOut = true;
        MDNS.end();
        LOG_DEBUG("[POWER] Timed mDNS stopped after 60 seconds");
    }
    
    // ── Software watchdog for upload task ──
    // If the upload task hasn't sent a heartbeat in UPLOAD_WATCHDOG_TIMEOUT_MS, it's hung.
    // Force-kill it and reboot — vTaskDelete mid-SD-I/O corrupts the SD bus,
    // making remount impossible. A clean reboot is the only reliable recovery.
    if (uploadTaskRunning && g_uploadHeartbeat > 0 &&
        (millis() - g_uploadHeartbeat > UPLOAD_WATCHDOG_TIMEOUT_MS)) {
        LOG_ERROR("[FSM] Upload task appears hung (no heartbeat for 2 minutes) — rebooting");
        
        // Set NVS flag so we can log the reason on next boot
        Preferences wdPrefs;
        wdPrefs.begin("cpap_flags", false);
        wdPrefs.putBool("watchdog_kill", true);
        wdPrefs.end();
        
        if (uploadTaskHandle) {
            vTaskDelete(uploadTaskHandle);
        }
        
        setRebootReason("Upload task hung (software watchdog, >2 min no heartbeat)");
        Logger::getInstance().flushBeforeReboot();
        delay(300);
        esp_restart();
    }
    
    // ── Web trigger handlers (operate independently of FSM) ──
    
    // Check for state reset trigger — takes effect IMMEDIATELY, even during upload.
    // Strategy: set NVS flag → kill upload task → reboot.
    // State files are deleted on next boot with a clean SD card mount.
    // This avoids SD card access after killing a task mid-I/O (which can hang).
    if (g_resetStateFlag) {
        LOG("=== State Reset Triggered via Web Interface ===");
        g_resetStateFlag = false;
        
        // Set NVS flag so state files are deleted on next clean boot
        Preferences resetPrefs;
        resetPrefs.begin("cpap_flags", false);
        resetPrefs.putBool("reset_state", true);
        resetPrefs.end();
        LOG("Reset flag saved to NVS");
        
        // Kill upload task if running (don't touch SD card after this!)
        if (uploadTaskRunning && uploadTaskHandle) {
            LOG_WARN("[FSM] Killing active upload task for state reset");
            vTaskDelete(uploadTaskHandle);
            uploadTaskRunning = false;
            wifiManager.resumeRoaming();
            uploadTaskHandle = nullptr;
        }
        
        // Immediate reboot — state files deleted on next clean boot
        LOG("Rebooting for clean state reset...");
        setRebootReason("State reset requested via Web UI");
        Logger::getInstance().flushBeforeReboot();
        delay(300);  // Brief pause for web response to send
        esp_restart();
    }
    
    // Soft reboot — next boot detects ESP_RST_SW and skips all delays automatically
    if (g_softRebootFlag) {
        LOG("=== Soft Reboot Triggered via Web Interface ===");
        g_softRebootFlag = false;
        setRebootReason("Soft reboot requested via Web UI");
        Logger::getInstance().flushBeforeReboot();
        delay(300);
        esp_restart();
    }

    // Check for upload trigger (force immediate upload — skip inactivity check)
    // Blocked while upload task is running — already uploading
    if (g_triggerUploadFlag && !uploadTaskRunning) {
        LOG("=== Upload Triggered via Web Interface ===");
        g_triggerUploadFlag = false;
        g_uploadWasForceTriggered = true;  // latch for upload task
        g_noWorkSuppressed = false;  // Manual trigger always overrides suppression
        uploadCycleHadTimeout = false;
        transitionTo(UploadState::ACQUIRING);
    }
    
    // Check for monitoring triggers
    if (g_monitorActivityFlag) {
        g_monitorActivityFlag = false;
        monitoringRequested = true;
    }
    if (g_stopMonitorFlag) {
        g_stopMonitorFlag = false;
        stopMonitoringRequested = true;
    }
#endif

    // ── WiFi reconnection (non-blocking with 30 second retry interval) ──
    // GUARD: Do NOT attempt reconnection while upload task is running on Core 0.
    // The upload task manages its own WiFi recovery via tryCoordinatedWifiCycle().
    // Concurrent reconnection from both cores corrupts the lwIP state machine.
    // While in boot-fallback AP mode we keep retrying in the background so the
    // device can self-heal once the router comes back, but skip the retry while
    // an AP client is connected (user is mid-config; don't disrupt the radio).
    // Tracks whether the new-attempt block below relaxed brownout detection
    // for the in-flight attempt.  Roam scans don't relax brownout, so we must
    // not re-enable it (or log "reconnect complete") when a roam-stay decision
    // exits ROAM_SCAN -> CONNECTED.
    static bool brownoutRelaxedThisAttempt = false;

    bool wasInProgress = wifiManager.isConnectInProgress();
    wifiManager.pollConnect();
    if (wasInProgress && !wifiManager.isConnectInProgress()) {
        if (brownoutRelaxedThisAttempt) {
            LOG_INFO("[POWER] WiFi reconnect complete - re-enabling brownout detection");
            SET_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
            brownoutRelaxedThisAttempt = false;
        }
        if (wifiManager.getConnectPhase() == WiFiManager::ConnectPhase::CONNECTED) {
            LOG_DEBUG("WiFi reconnected successfully");
        }
        // FAILED case logs inside terminateConnect via logConnectFailure().
    }

    if (!wifiManager.isConnectInProgress() &&
        !wifiManager.isConnected() && !uploadTaskRunning &&
        wifiManager.getApClientCount() == 0) {
        unsigned long currentTime = millis();
        bool intervalElapsed = (currentTime - lastWifiReconnectAttempt >= wifiManager.getReconnectIntervalMs());
        // PCNT-aware bus-idle gate: defer the RF burst if the CPAP is currently using the SD bus
        // No-op on non-pcnt capable devices
        bool busBusy = g_pcntCapable && !trafficMonitor.isIdleFor(2000);
        if (intervalElapsed && !busBusy) {
            LOG_WARN("WiFi disconnected, attempting to reconnect...");

            if (!config.valid() || config.getWifiNetworkCount() == 0) {
                LOG_ERROR("Cannot reconnect to WiFi: no networks configured");
                lastWifiReconnectAttempt = currentTime;
                return;
            }

            if (config.getBrownoutDetectMode() == BrownoutDetectMode::RELAXED) {
                LOG_INFO("[POWER] Relaxing brownout detection for WiFi reconnect");
                CLEAR_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
                brownoutRelaxedThisAttempt = true;
            }

            wifiManager.beginConnect(config);
            lastWifiReconnectAttempt = currentTime;  // count from attempt start
        }

        // Initialize web server regardless of connection success
        // It will either serve normal UI or AP setup UIFSM while WiFi is down
    }

    // Stable-quiet AP teardown
    // If we booted into AP-fallback (failed STA at boot) and STA has since
    // recovered, drop the AP back down so the radio runs in plain STA again.
    // We reboot rather than tearing down in place: the AP-mode boot path
    // skipped uploader/scheduler init, so a clean reboot through the normal
    // STA path is the simplest way to bring the device fully online.
    if (g_apSetupMode && wifiManager.shouldTearDownAP()) {
        LOG_INFO("[AP] STA reconnected stably for 2 min, no AP clients - rebooting to exit AP mode");
        setRebootReason("AP fallback teardown after STA recovery");
        Logger::getInstance().flushBeforeReboot();
        delay(300);
        esp_restart();
    }

    // ── NTP sync retry ──
    if (uploader) {
        unsigned long currentTime = millis();
        if (currentTime - lastNtpSyncAttempt >= NTP_RETRY_INTERVAL_MS) {
            LOG_DEBUG("Periodic NTP synchronization check...");
            lastNtpSyncAttempt = currentTime;
        }
    }
    
    // ── Monitoring request handling (can interrupt most states) ──
    if (monitoringRequested) {
        if (currentState != UploadState::UPLOADING) {
            monitoringRequested = false;
            stopMonitoringRequested = false;  // discard any stale stop from before monitoring began
            if (currentState == UploadState::ACQUIRING && sdManager.hasControl()) {
                sdManager.releaseControl();
            }
            trafficMonitor.enableSampleBuffer();  // Allocate buffer for web UI
            trafficMonitor.resetStatistics();
            transitionTo(UploadState::MONITORING);
        }
        // If UPLOADING, leave flag set — handleReleasing() will redirect to MONITORING
        // after upload finishes current cycle + mandatory root/SETTINGS files
    }

    // ── FSM dispatch ──
    switch (currentState) {
        case UploadState::IDLE:       handleIdle();       break;
        case UploadState::LISTENING:  handleListening();  break;
        case UploadState::ACQUIRING:  handleAcquiring();  break;
        case UploadState::UPLOADING:  handleUploading();  break;
        case UploadState::RELEASING:  handleReleasing();  break;
        case UploadState::COOLDOWN:   handleCooldown();   break;
        case UploadState::COMPLETE:   handleComplete();   break;
        case UploadState::MONITORING: handleMonitoring(); break;
    }
    
    // ── POWER: Yield CPU so DFS can scale down ──
    // Without explicit yields the loop task never blocks, keeping CPU at max
    // frequency. State-appropriate delays allow the FreeRTOS IDLE task to run,
    // triggering automatic frequency scaling (DFS) when no work is pending.
    switch (currentState) {
        case UploadState::IDLE:
        case UploadState::COOLDOWN:
            vTaskDelay(pdMS_TO_TICKS(100));  // Low-frequency states: 100ms yield
            break;
        case UploadState::LISTENING:
            vTaskDelay(pdMS_TO_TICKS(50));   // TrafficMonitor samples; 50ms is sufficient
            break;
        case UploadState::MONITORING:
        case UploadState::UPLOADING:
            vTaskDelay(pdMS_TO_TICKS(10));   // Responsive states: 10ms yield
            break;
        default:
            vTaskDelay(pdMS_TO_TICKS(10));   // Transient states: brief yield
            break;
    }
}
