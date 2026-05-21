#include "CpapWebServer.h"
#include "Logger.h"
#include "UploadFSM.h"
#include "version.h"
#include "web_ui_gz.h"
#include "WebStatus.h"
#include "setup_html_gz.h"
#include <time.h>
#include <SD_MMC.h>
#include <LittleFS.h>

// Global trigger flags
volatile bool g_triggerUploadFlag = false;
volatile bool g_forceRecentOnlyFlag = false;
volatile bool g_resetStateFlag = false;
volatile bool g_softRebootFlag = false;

// Monitoring trigger flags
volatile bool g_monitorActivityFlag = false;

// Cooperative upload abort flag — set when config lock is requested during an active upload
volatile bool g_abortUploadFlag = false;
volatile bool g_stopMonitorFlag = false;

// External FSM state (defined in main.cpp)
extern UploadState currentState;
extern unsigned long stateEnteredAt;
extern unsigned long cooldownStartedAt;
extern volatile bool uploadTaskRunning;
extern bool g_apSetupMode;

// CPU load globals (defined in main.cpp, updated by idle hooks)
extern volatile uint32_t g_idleCount0, g_idleCount1;
extern uint32_t g_cpuLoad0, g_cpuLoad1;

// PCNT capability flag (defined in main.cpp, set during boot)
extern bool g_pcntCapable;

namespace {
static constexpr unsigned long kUploadUiMinIntervalMs = 400;
static constexpr unsigned long kUploadNotFoundMinIntervalMs = 250;
static constexpr size_t kRecentTabSightingsCount = 12;
static constexpr unsigned long kSseOwnerLeaseMs = 30000;

struct RecentTabSighting {
    uint16_t tabId;
    uint32_t seenAtMs;
};

static RecentTabSighting g_recentTabSightings[kRecentTabSightingsCount] = {};
static uint8_t g_recentTabWriteIndex = 0;
static WiFiClient g_sseClient;
static volatile bool g_sseActive = false;
static volatile uint32_t g_sseLastPushedIndex = 0;
static volatile uint16_t g_sseOwnerTid = 0;
static volatile uint32_t g_sseOwnerIid = 0;
static volatile uint32_t g_sseOwnerLastSeenMs = 0;

enum UploadUiSlot : uint8_t {
    kUploadUiSlotRoot = 0,
    kUploadUiSlotLogs,
    kUploadUiSlotConfig,
    kUploadUiSlotStatus,
    kUploadUiSlotMonitor,
    kUploadUiSlotNotFound,
    kUploadUiSlotCount
};

bool isUploadUiRateLimited(UploadUiSlot slot, bool uploadInProgress, unsigned long minIntervalMs) {
    static unsigned long lastServedMs[kUploadUiSlotCount] = {0};

    if (!uploadInProgress) {
        lastServedMs[slot] = 0;
        return false;
    }

    const unsigned long now = millis();
    if (lastServedMs[slot] != 0 && (now - lastServedMs[slot]) < minIntervalMs) {
        return true;
    }

    lastServedMs[slot] = now;
    return false;
}

void sendUploadRateLimitResponse(WebServer* server,
                                 const char* contentType = "text/plain",
                                 const char* body = "Busy during upload; slow down refresh") {
    if (!server) {
        return;
    }
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Connection", "close");
    server->sendHeader("Retry-After", "1");
    server->send(429, contentType, body);
    // Proactively release socket resources under upload-time pressure.
    server->client().stop();
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool parseCompactTabId(const String& value, uint16_t* outTabId) {
    if (!outTabId || value.length() < 4) return false;
    uint16_t tabId = 0;
    for (int i = 0; i < 4; i++) {
        int nibble = hexNibble(value[i]);
        if (nibble < 0) return false;
        tabId = (tabId << 4) | (uint16_t)nibble;
    }
    *outTabId = tabId;
    return true;
}

bool getRequestTabId(WebServer* server, uint16_t* outTabId) {
    if (!server || !outTabId) return false;
    if (server->hasArg("tid") && parseCompactTabId(server->arg("tid"), outTabId)) return true;
    if (server->hasHeader("X-Tab-Id") && parseCompactTabId(server->header("X-Tab-Id"), outTabId)) return true;
    return false;
}

bool parseCompactInstanceId(const String& value, uint32_t* outInstanceId) {
    if (!outInstanceId || value.length() < 8) return false;
    uint32_t instanceId = 0;
    for (int i = 0; i < 8; i++) {
        int nibble = hexNibble(value[i]);
        if (nibble < 0) return false;
        instanceId = (instanceId << 4) | (uint32_t)nibble;
    }
    *outInstanceId = instanceId;
    return instanceId != 0;
}

bool getRequestInstanceId(WebServer* server, uint32_t* outInstanceId) {
    if (!server || !outInstanceId) return false;
    if (server->hasArg("iid") && parseCompactInstanceId(server->arg("iid"), outInstanceId)) return true;
    if (server->hasHeader("X-Tab-Instance") && parseCompactInstanceId(server->header("X-Tab-Instance"), outInstanceId)) return true;
    return false;
}

void recordRecentTabSighting(WebServer* server) {
    uint16_t tabId = 0;
    if (!getRequestTabId(server, &tabId)) return;
    g_recentTabSightings[g_recentTabWriteIndex].tabId = tabId;
    g_recentTabSightings[g_recentTabWriteIndex].seenAtMs = millis();
    g_recentTabWriteIndex = (uint8_t)((g_recentTabWriteIndex + 1) % kRecentTabSightingsCount);
}

void buildRecentTabsField(char* output, size_t outputSize, unsigned long nowMs) {
    if (!output || outputSize == 0) return;
    output[0] = '\0';
    size_t used = 0;
    for (size_t n = 0; n < kRecentTabSightingsCount; n++) {
        int idx = (int)((g_recentTabWriteIndex + kRecentTabSightingsCount - 1 - n) % kRecentTabSightingsCount);
        const RecentTabSighting& sighting = g_recentTabSightings[idx];
        if (sighting.seenAtMs == 0) continue;
        unsigned long secondsAgo = (nowMs >= sighting.seenAtMs) ? ((nowMs - sighting.seenAtMs) / 1000UL) : 0UL;
        int wrote = snprintf(output + used, outputSize - used, "%s%04X:%lu",
                             used ? "," : "", (unsigned)sighting.tabId, secondsAgo);
        if (wrote <= 0 || (size_t)wrote >= (outputSize - used)) break;
        used += (size_t)wrote;
    }
}

bool isSseOwnerAlive(unsigned long nowMs) {
    if (!g_sseActive || g_sseOwnerTid == 0 || g_sseOwnerIid == 0 || !g_sseClient.connected()) return false;
    return (nowMs - g_sseOwnerLastSeenMs) <= kSseOwnerLeaseMs;
}

void clearSseOwner() {
    g_sseActive = false;
    g_sseOwnerTid = 0;
    g_sseOwnerIid = 0;
    g_sseOwnerLastSeenMs = 0;
}
}

// Constructor
CpapWebServer::CpapWebServer(Config* cfg, UploadStateManager* state,
                             ScheduleManager* schedule, 
                             WiFiManager* wifi)
    : server(nullptr),
      config(cfg),
      stateManager(state),
      smbStateManager(nullptr),
      cloudStateManager(nullptr),
      scheduleManager(schedule),
      wifiManager(wifi),
      trafficMonitor(nullptr),
      sdManager(nullptr)
#ifdef ENABLE_OTA_UPDATES
      , otaManager(nullptr)
#endif
{
}

// Destructor
CpapWebServer::~CpapWebServer() {
    if (server) {
        server->stop();
        delete server;
    }
}

// Initialize and start the web server
bool CpapWebServer::begin() {
    LOG("[WebServer] Initializing web server on port 80...");
    
    server = new WebServer(80);

    // Collect Host header so requests to *.local can be redirected to the device IP.
    const char* headerKeys[] = {"Host", "X-Tab-Id", "X-Tab-Instance"};
    server->collectHeaders(headerKeys, 3);
    
    // Register request handlers
    server->on("/", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        if (g_apSetupMode) {
            this->handleSetupPage();
        } else {
            this->handleRoot();
        }
    });
    server->on("/setup", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleSetupPage();
    });
    server->on("/trigger-upload", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleTriggerUpload();
    });
    
    // HTML Views
    server->on("/status", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleStatusPage();
    });
    server->on("/config", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleConfigPage();
    });
    server->on("/logs", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleLogs();
    });
    server->on("/monitor", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleMonitorPage();
    });
    
    // APIs
    server->on("/api/status", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiStatus();
    });
    server->on("/api/wifi-scan", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiWifiScan();
    });
    server->on("/api/config", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiConfig();
    });
    server->on("/api/logs", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogs();
    });
    server->on("/api/logs/buffer", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogs();
    });
    server->on("/api/logs/poll", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogs();
    });
    server->on("/api/monitor-start", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleMonitorStart();
    });
    server->on("/api/monitor-stop", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleMonitorStop();
    });
    server->on("/api/sd-activity", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleSdActivity();
    });
    // /api/diagnostics removed — cpu0/cpu1 merged into /api/status
    server->on("/reset-state", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleResetState();
    });
    server->on("/soft-reboot", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleSoftReboot();
    });
    server->on("/api/config-raw",  HTTP_GET,  [this]() { this->handleApiConfigRawGet(); });
    server->on("/api/config-raw",  HTTP_POST, [this]() { this->handleApiConfigRawPost(); });
    server->on("/api/logs/saved", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogsSaved();
    });
    server->on("/api/logs/download-all", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogsSaved();
    });
    server->on("/api/logs/full", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogsFull();
    });
    server->on("/api/logs/recent", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogsFull();
    });
    server->on("/api/logs/file0", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogsFile0();
    });
    server->on("/api/logs/stream", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleApiLogsStream();
    });
    
#ifdef ENABLE_OTA_UPDATES
    // OTA handlers
    server->on("/ota", [this]() {
        if (this->redirectToIpIfMdnsRequest()) return;
        this->handleOTAPage();
    });
    server->on("/ota-upload", HTTP_POST, 
               [this]() { this->handleOTAUploadComplete(); },
               [this]() { this->handleOTAUpload(); });
    server->on("/ota-url", HTTP_POST, [this]() { this->handleOTAURL(); });
#endif
    
    // Handle common browser requests silently
    server->on("/favicon.ico", [this]() { 
        if (this->redirectToIpIfMdnsRequest()) return;
        // Avoid empty-content WebServer warnings and keep this path cheap.
        server->sendHeader("Connection", "close");
        server->send(404, "text/plain", "Not found");
    });

    // ── Captive portal detection endpoints ──
    // Android, Apple, and Windows probe these URLs to detect captive portals.
    // In AP mode: redirect to /setup so the "Sign in to network" popup works.
    // In STA mode: return expected responses so devices don't think we're captive.
    auto captiveHandler = [this]() {
        if (g_apSetupMode) {
            String url = "http://" + WiFi.softAPIP().toString() + "/setup";
            server->sendHeader("Location", url, true);
            server->send(302, "text/plain", "");
            server->client().stop();
        } else {
            // Return expected "connected" response so device doesn't nag
            server->send(204, "", "");
        }
    };
    server->on("/generate_204", captiveHandler);         // Android
    server->on("/gen_204", captiveHandler);               // Android alt
    server->on("/hotspot-detect.html", [this]() {         // Apple
        if (g_apSetupMode) {
            String url = "http://" + WiFi.softAPIP().toString() + "/setup";
            server->sendHeader("Location", url, true);
            server->send(302, "text/plain", "");
            server->client().stop();
        } else {
            server->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
        }
    });
    server->on("/connecttest.txt", captiveHandler);       // Windows
    server->on("/redirect", captiveHandler);              // Windows alt
    server->on("/ncsi.txt", [this]() {                    // Windows NCSI
        if (g_apSetupMode) {
            String url = "http://" + WiFi.softAPIP().toString() + "/setup";
            server->sendHeader("Location", url, true);
            server->send(302, "text/plain", "");
            server->client().stop();
        } else {
            server->send(200, "text/plain", "Microsoft NCSI");
        }
    });
    
    server->onNotFound([this]() { this->handleNotFound(); });
    
    // Start the server
    server->begin();
    
    LOG("[WebServer] Web server started successfully");
    LOG("[WebServer] Available endpoints:");
    LOG("[WebServer]   GET  /                  - Dashboard (SPA)");
    LOG("[WebServer]   GET  /status             - Status page (SPA)");
    LOG("[WebServer]   GET  /config             - Config page (SPA)");
    LOG("[WebServer]   GET  /logs               - Logs page (SPA)");
    LOG("[WebServer]   GET  /monitor            - Monitor page (SPA)");
    LOG("[WebServer]   GET  /trigger-upload     - Force immediate upload");
    LOG("[WebServer]   GET  /soft-reboot        - Soft reboot (skips cold-boot delay)");
    LOG("[WebServer]   GET  /reset-state        - Clear upload state and reboot");
    LOG("[WebServer]   GET  /api/status         - Live status JSON");
    LOG("[WebServer]   GET  /api/config         - Config snapshot JSON");
    LOG("[WebServer]   GET  /api/logs           - In-memory log buffer (plain text)");
    LOG("[WebServer]   GET  /api/logs/saved     - Download persisted LittleFS logs");
    LOG("[WebServer]   GET  /api/logs/full      - NAND + circular buffer backfill");
    LOG("[WebServer]   GET  /api/logs/stream    - SSE live log stream");
    LOG("[WebServer]   GET  /api/sd-activity    - SD bus activity samples JSON");
    LOG("[WebServer]   GET  /api/config-raw     - Raw config.txt contents");
    LOG("[WebServer]   POST /api/config-raw     - Save raw config.txt");
    LOG("[WebServer]   GET  /api/monitor-start  - Start SD activity monitoring");
    LOG("[WebServer]   GET  /api/monitor-stop   - Stop SD activity monitoring");
#ifdef ENABLE_OTA_UPDATES
    LOG("[WebServer]   GET  /ota               - OTA firmware update page");
    LOG("[WebServer]   POST /ota-upload         - Upload firmware binary");
    LOG("[WebServer]   POST /ota-url            - Trigger OTA from URL");
#endif
    
    return true;
}

// Process incoming HTTP requests
void CpapWebServer::handleClient() {
    if (server) {
        // Always service sockets to avoid stale descriptors and connection backlog.
        server->handleClient();
    }
}

bool CpapWebServer::isUploadInProgress() const {
    return uploadTaskRunning;
}

bool CpapWebServer::redirectToIpIfMdnsRequest() {
    if (!server || server->method() != HTTP_GET) {
        return false;
    }

    String host = server->header("Host");
    if (host.isEmpty()) {
        return false;
    }

    host.trim();
    int colonPos = host.indexOf(':');
    if (colonPos > 0) {
        host = host.substring(0, colonPos);
    }
    host.toLowerCase();

    if (!host.endsWith(".local")) {
        return false;
    }

    if (!wifiManager || !wifiManager->isConnected()) {
        return false;
    }

    String ip = wifiManager->getIPAddress();
    if (ip.isEmpty() || ip == "Not connected") {
        return false;
    }

    String uri = server->uri();
    if (uri.isEmpty()) {
        uri = "/";
    }

    String location = "http://" + ip + uri;
    LOG_DEBUGF("[WebServer] Redirecting mDNS request %s -> %s", host.c_str(), location.c_str());

    server->sendHeader("Location", location, true);
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Connection", "close");
    server->send(302, "text/plain", "Redirecting to device IP");
    return true;
}

// GET / - Serve static SPA from PROGMEM. Zero heap allocation.
void CpapWebServer::handleRoot() {
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Connection", "close");
    server->sendHeader("Content-Encoding", "gzip");
    server->send_P(200, "text/html; charset=utf-8", (const char*)web_ui_gz, web_ui_gz_len);
}

// GET /trigger-upload - Force immediate upload
void CpapWebServer::handleTriggerUpload() {
    LOG("[WebServer] Upload trigger requested via web interface");

    // Force Upload outside a "fresh-eligible" period: allow but restrict to
    // recent data only.  Covers two cases with one predicate:
    //   • Scheduled mode outside the upload window.
    //   • Smart  mode inside the quiet period (before SMART_START_HOUR).
    // In both cases canUploadFreshData() returns false, so the FSM would
    // otherwise bail with "No data category eligible, releasing".  The user
    // explicitly pressed Force Upload and has been warned via the Danger
    // Zone copy, so honour the request with FRESH_ONLY semantics and the
    // usual EXCLUSIVE_ACCESS_MINUTES time budget.
    if (scheduleManager && !scheduleManager->isManualMode() && !scheduleManager->canUploadFreshData()) {
        const char* why = scheduleManager->isSmartMode()
            ? "Smart mode quiet period"
            : "Scheduled mode outside window";
        LOGF("[WebServer] %s — force upload limited to recent data", why);
        g_forceRecentOnlyFlag = true;
    }

    // Set global trigger flag — FSM picks this up in the next loop iteration
    g_triggerUploadFlag = true;

    addCorsHeaders(server);
    server->send(200, "application/json",
        "{\"status\":\"success\",\"message\":\"Upload triggered. Check the Logs tab for progress.\"}");
}

// GET /status - JSON status information (Legacy - Removed, use handleApiStatus)


// GET /soft-reboot - Reboot immediately, skipping cold-boot delays
void CpapWebServer::handleSoftReboot() {
    LOG("[WebServer] Soft reboot requested via web interface");
    addCorsHeaders(server);
    server->send(200, "application/json",
        "{\"status\":\"success\",\"message\":\"Rebooting now (waits skipped)...\"}");
    g_softRebootFlag = true;
}

// GET /reset-state - Clear upload state
void CpapWebServer::handleResetState() {
    LOG("[WebServer] State reset requested via web interface");
    
    // Set global reset flag
    g_resetStateFlag = true;
    
    // Add CORS headers
    addCorsHeaders(server);
    
    String response = "{\"status\":\"success\",\"message\":\"Upload state will be reset. Check the Logs tab for confirmation.\"}";
    server->send(200, "application/json", response);
}

// GET /api/config - serve pre-built static config snapshot. Zero heap allocation.
void CpapWebServer::handleApiConfig() {
    addCorsHeaders(server);
    server->send(200, "application/json", g_webConfigBuf);
}

void CpapWebServer::handleSetupPage() {
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Connection", "close");
    server->sendHeader("Content-Encoding", "gzip");
    server->send_P(200, "text/html", (const char*)setup_html_gz, setup_html_gz_len);
}

void CpapWebServer::handleApiWifiScan() {
    addCorsHeaders(server);
    server->sendHeader("Cache-Control", "no-store");
    
    int n = WiFi.scanNetworks(false, false); // sync scan, skip hidden
    if (n == WIFI_SCAN_FAILED) {
        server->send(500, "application/json", "{\"error\":\"Scan failed\"}");
        return;
    }
    
    // Deduplicate by SSID, keeping strongest RSSI per unique network name.
    // Uses fixed stack-allocated table — no heap fragmentation.
    static constexpr int kMaxDedup = 32;
    struct ScanEntry { char ssid[33]; int rssi; int auth; };
    ScanEntry dedup[kMaxDedup];
    int dedupCount = 0;
    
    for (int i = 0; i < n; ++i) {
        String ssidStr = WiFi.SSID(i);
        if (ssidStr.isEmpty()) continue; // skip hidden networks
        
        int rssi = WiFi.RSSI(i);
        int auth = WiFi.encryptionType(i);
        
        // Check if SSID already seen — update if stronger signal
        bool found = false;
        for (int j = 0; j < dedupCount; ++j) {
            if (ssidStr == dedup[j].ssid) {
                if (rssi > dedup[j].rssi) {
                    dedup[j].rssi = rssi;
                    dedup[j].auth = auth;
                }
                found = true;
                break;
            }
        }
        if (!found && dedupCount < kMaxDedup) {
            strncpy(dedup[dedupCount].ssid, ssidStr.c_str(), sizeof(dedup[0].ssid) - 1);
            dedup[dedupCount].ssid[sizeof(dedup[0].ssid) - 1] = '\0';
            dedup[dedupCount].rssi = rssi;
            dedup[dedupCount].auth = auth;
            dedupCount++;
        }
    }
    
    // Sort by RSSI descending (simple insertion sort for tiny array)
    for (int i = 1; i < dedupCount; i++) {
        ScanEntry key = dedup[i];
        int j = i - 1;
        while (j >= 0 && dedup[j].rssi < key.rssi) {
            dedup[j + 1] = dedup[j];
            j--;
        }
        dedup[j + 1] = key;
    }
    
    // Stream deduplicated JSON response
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "application/json", "");
    server->sendContent("[");
    
    for (int i = 0; i < dedupCount; ++i) {
        String ssid = String(dedup[i].ssid);
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        
        String json = "{";
        json += "\"ssid\":\"" + ssid + "\",";
        json += "\"rssi\":" + String(dedup[i].rssi) + ",";
        json += "\"auth\":" + String(dedup[i].auth);
        json += "}";
        
        if (i < dedupCount - 1) json += ",";
        server->sendContent(json);
    }
    
    server->sendContent("]");
    WiFi.scanDelete();
}

// Handle 404 errors
void CpapWebServer::handleNotFound() {
    String uri = server->uri();

    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Connection", "close");
    
    if (g_apSetupMode) {
        String url = "http://" + wifiManager->getIPAddress() + "/setup";
        server->sendHeader("Location", url, true);
        server->send(302, "text/plain", "");
        server->client().stop();
        return;
    }
    
    // Silently handle common browser requests that we don't care about
    if (uri == "/favicon.ico" || uri == "/apple-touch-icon.png" || 
        uri == "/apple-touch-icon-precomposed.png" || uri == "/robots.txt") {
        server->send(404, "text/plain", "Not found");
        if (isUploadInProgress()) {
            server->client().stop();
        }
        return;
    }

    // During uploads keep 404 handling allocation-free and rate-limited.
    if (isUploadUiRateLimited(kUploadUiSlotNotFound, isUploadInProgress(), kUploadNotFoundMinIntervalMs)) {
        server->send(404, "text/plain", "Not found");
        server->client().stop();
        return;
    }
    if (isUploadInProgress()) {
        server->send(404, "text/plain", "Not found");
        server->client().stop();
        return;
    }
    
    // Log unexpected 404s
    LOG_DEBUGF("[WebServer] 404 Not Found: %s", uri.c_str());
    
    String message = "{\"status\":\"error\",\"message\":\"Endpoint not found\",\"path\":\"" + uri + "\"}";
    server->send(404, "application/json", message);
}

// Static helper: Add CORS headers to response
void CpapWebServer::addCorsHeaders(::WebServer* server) {
    server->sendHeader("Access-Control-Allow-Origin", "*");
    server->sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server->sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Connection", "close");
}

// Helper: Get uptime as formatted string
String CpapWebServer::getUptimeString() {
    unsigned long seconds = millis() / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;
    
    String uptime = "";
    if (days > 0) {
        uptime += String(days) + "d ";
    }
    uptime += String(hours % 24) + "h ";
    uptime += String(minutes % 60) + "m ";
    uptime += String(seconds % 60) + "s";
    
    return uptime;
}

// Helper: Get current time as formatted string
String CpapWebServer::getCurrentTimeString() {
    time_t now;
    time(&now);
    
    if (now < 1000000000) {
        return "Not synchronized";
    }
    
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    return String(buffer);
}

// Helper: Get count of pending files (estimate)
int CpapWebServer::getPendingFilesCount() {
    if (!stateManager) {
        return 0;
    }
    
    // Count files in incomplete DATALOG folders
    // Note: This requires SD card access, which we don't have here
    // Return -1 to indicate "unknown" rather than misleading 0
    return -1;
}

// Helper: Get count of pending DATALOG folders
int CpapWebServer::getPendingFoldersCount() {
    if (!stateManager) {
        return 0;
    }
    
    return stateManager->getPendingFoldersCount();
}

// Helper class to adapt WebServer for chunked streaming via Print interface
class ChunkedPrint : public Print {
    WebServer* _server;
public:
    ChunkedPrint(WebServer* server) : _server(server) {}
    
    size_t write(uint8_t c) override {
        return write(&c, 1);
    }
    
    size_t write(const uint8_t *buffer, size_t size) override {
        if (size == 0) return 0;
        
        // Manually write chunk header (size in hex + CRLF)
        WiFiClient client = _server->client();
        client.print(String(size, HEX));
        client.write("\r\n", 2);
        
        // Write chunk data
        size_t written = client.write(buffer, size);
        
        // Write chunk footer (CRLF)
        client.write("\r\n", 2);
        
        return written;
    }
};

// GET /logs - serve SPA (client-side rendering handles logs tab)
void CpapWebServer::handleLogs() {
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Connection", "close");
    server->sendHeader("Content-Encoding", "gzip");
    server->send_P(200, "text/html; charset=utf-8", (const char*)web_ui_gz, web_ui_gz_len);
}

// GET /api/logs - Legacy alias for circular-buffer logs
void CpapWebServer::handleApiLogs() {
    recordRecentTabSighting(server);
    addCorsHeaders(server);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/plain; charset=utf-8", " ");
    ChunkedPrint chunkedOutput(server);
    Logger::getInstance().printLogs(chunkedOutput);
    server->sendContent("");
}

// GET /api/logs/saved — legacy alias for full log download
void CpapWebServer::handleApiLogsSaved() {
    recordRecentTabSighting(server);
    Logger::getInstance().dumpSavedLogsPeriodic(nullptr);

    addCorsHeaders(server);
    server->sendHeader("Content-Disposition", "attachment; filename=\"cpap_logs.txt\"");
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/plain; charset=utf-8", " ");
    ChunkedPrint chunked(server);

    // Stream all syslog rotation files (oldest → newest, chronological)
    size_t nandBytes = Logger::getInstance().streamSavedLogs(chunked);

    // Append any circular-buffer content not yet flushed (race window)
    Logger::getInstance().printLogs(chunked);

    if (nandBytes == 0) {
        chunked.print("[No saved log files found on internal flash]\n");
    }
    server->sendContent("");
}

// GET /api/logs/full — legacy alias for recent-history backfill
void CpapWebServer::handleApiLogsFull() {
    recordRecentTabSighting(server);
    addCorsHeaders(server);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/plain; charset=utf-8", " ");
    ChunkedPrint chunked(server);

    Logger::getInstance().dumpSavedLogsPeriodic(nullptr);
    Logger::getInstance().streamSavedLogs(chunked, 1);
    Logger::getInstance().printLogs(chunked);

    server->sendContent("");
}

// GET /api/logs/file0 — stream latest persisted file only (no circular buffer)
void CpapWebServer::handleApiLogsFile0() {
    recordRecentTabSighting(server);
    Logger::getInstance().dumpSavedLogsPeriodic(nullptr);

    addCorsHeaders(server);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/plain; charset=utf-8", " ");
    ChunkedPrint chunked(server);
    size_t nandBytes = Logger::getInstance().streamSavedLogs(chunked, 1);
    if (nandBytes == 0) {
        chunked.print("[No saved log files found on internal flash]\n");
    }
    server->sendContent("");
}

// GET /api/logs/stream — SSE endpoint for live log push.
// Takes over the client socket and stores it globally for main-loop push.
// Declared in CpapWebServer.h; the actual push happens via pushSseLogs() called from loop().
void CpapWebServer::handleApiLogsStream() {
    recordRecentTabSighting(server);

    uint16_t requestTabId = 0;
    if (!getRequestTabId(server, &requestTabId)) {
        addCorsHeaders(server);
        server->send(400, "text/plain; charset=utf-8", "Missing or invalid tab ID");
        return;
    }

    uint32_t requestInstanceId = 0;
    if (!getRequestInstanceId(server, &requestInstanceId)) {
        addCorsHeaders(server);
        server->send(400, "text/plain; charset=utf-8", "Missing or invalid tab instance ID");
        return;
    }

    const unsigned long nowMs = millis();
    if (isSseOwnerAlive(nowMs) && g_sseOwnerIid != requestInstanceId) {
        addCorsHeaders(server);
        server->send(409, "text/plain; charset=utf-8", "Another tab owns the live log stream");
        return;
    }

    if (g_sseActive && g_sseClient.connected()) {
        g_sseClient.stop();
    }
    clearSseOwner();

    g_sseClient = server->client();

    g_sseClient.print("HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/event-stream\r\n"
                       "Cache-Control: no-cache\r\n"
                       "Connection: keep-alive\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "\r\n");

    g_sseLastPushedIndex = Logger::getInstance().getHeadIndex();
    g_sseOwnerTid = requestTabId;
    g_sseOwnerIid = requestInstanceId;
    g_sseOwnerLastSeenMs = nowMs;
    g_sseActive = true;
}

// GET /config - serve SPA
void CpapWebServer::handleConfigPage() {
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Connection", "close");
    server->sendHeader("Content-Encoding", "gzip");
    server->send_P(200, "text/html; charset=utf-8", (const char*)web_ui_gz, web_ui_gz_len);
}
// GET /status - serve SPA
void CpapWebServer::handleStatusPage() {
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Connection", "close");
    server->sendHeader("Content-Encoding", "gzip");
    server->send_P(200, "text/html; charset=utf-8", (const char*)web_ui_gz, web_ui_gz_len);
}
// GET /api/status - rebuild snapshot on demand then serve it.
// Must NOT rely on the main-loop periodic rebuild: during blocking uploads
// the main loop is frozen, so g_webStatusBuf would be permanently stale.
// updateStatusSnapshot() is stack-only (no heap) and safe to call here.
void CpapWebServer::handleApiStatus() {
    recordRecentTabSighting(server);
    updateStatusSnapshot();
    addCorsHeaders(server);
    server->send(200, "application/json", g_webStatusBuf);
}

// ---------------------------------------------------------------------------
// updateStatusSnapshot() — called every ~3 s from main loop.
// Assembles status JSON into g_webStatusBuf using snprintf only (no heap).
// ---------------------------------------------------------------------------
void CpapWebServer::updateStatusSnapshot() {
    char timeBuf[32] = "Not synchronized";
    time_t now; time(&now);
    if (now >= 1000000000) {
        struct tm tm; localtime_r(&now, &tm);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm);
    }
    unsigned long upSec = millis() / 1000;
    unsigned long inStateSec = (millis() - stateEnteredAt) / 1000;
    const char* st = getStateName(currentState);
    int rssi = 0; bool wifiConn = false;
    char wifiSsid[33] = {0};
    if (wifiManager && wifiManager->isConnected()) {
        wifiConn = true;
        rssi = wifiManager->getSignalStrength();
        String s = WiFi.SSID();
        strncpy(wifiSsid, s.c_str(), sizeof(wifiSsid) - 1);
    }
    String wifiSsidJson = escapeJson(String(wifiSsid));
    // Per-backend folder counts — both backends render their own row in the UI.
    //
    // Sourcing: prefer the probe snapshot (universe = folders-in-MAX_DAYS,
    // synced = fully-in-sync-for-this-backend) because it gives the dashboard
    // a stable, meaningful ratio.  The probe is refreshed every session by
    // FileUploader::hasWorkToUpload().  Before the first probe has run, fall
    // back to the legacy state-manager math so boot-time snapshots are still
    // coherent (completed / completed+incomplete).
    //
    // During an active upload phase the probe snapshot is frozen (it only
    // updates at the pre-session, post-cloud, and post-session seams).  To
    // prevent the progress bar from oscillating between the frozen probe
    // count and probe+1 as fractional live folder progress ticks up and down,
    // we add newly-completed folders since the last probe snapshot to `done`.
    // This gives smooth, monotonic progress that matches the actual upload.
    //
    // `pending` still reflects empty-folder bookkeeping and is reported as-is.
    auto readBackendCounts = [](UploadStateManager* sm,
                                int& done, int& total, int& pending,
                                unsigned long& lastTs, bool liveActive) {
        if (!sm) return;
        pending = sm->getPendingFoldersCount();
        lastTs  = sm->getLastUploadTimestamp();
        int u = sm->getProbeUniverse();
        int s = sm->getProbeSynced();
        if (u >= 0 && s >= 0) {
            done = s;
            // Live delta: add folders completed since the snapshot was taken.
            // Only folders that are NEW to the completed set are counted here;
            // re-uploaded already-completed folders are handled by the next
            // probe refresh, so the live count may briefly under-count them
            // (progress bar shows "uploading" anyway).
            if (liveActive) {
                int snapComp = sm->getProbeSnapshotCompletedCount();
                int currComp = sm->getCompletedFoldersCount();
                if (snapComp >= 0 && currComp > snapComp) {
                    done += (currComp - snapComp);
                    if (done > u) done = u;
                }
            }
            total = u;
        } else {
            done  = sm->getCompletedFoldersCount();
            total = done + sm->getIncompleteFoldersCount();
        }
    };

    int cloudDone = 0, cloudTotal = 0, cloudPending = 0;
    unsigned long cloudLastTs = 0;
    bool cloudEnabled = (cloudStateManager != nullptr);

    int smbDone = 0, smbTotal = 0, smbPending = 0;
    unsigned long smbLastTs = 0;
    bool smbEnabled = (smbStateManager != nullptr);

    long nextUp = -1; bool timeSynced = false; bool inWindow = false; bool smartQuiet = false;
    if (scheduleManager) {
        nextUp = scheduleManager->getSecondsUntilNextUpload();
        timeSynced = scheduleManager->isTimeSynced();
        inWindow = scheduleManager->isInUploadWindow();
        smartQuiet = scheduleManager->isSmartQuietPeriod();
    }
    // CPU load computation (moved from handleApiDiagnostics — merged into status)
    static uint32_t prevIdle0 = 0, prevIdle1 = 0;
    static unsigned long prevCpuMs = 0;
    unsigned long nowMs = millis();
    if (prevCpuMs > 0 && (nowMs - prevCpuMs) >= 500) {
        uint32_t d0 = g_idleCount0 - prevIdle0;
        uint32_t d1 = g_idleCount1 - prevIdle1;
        uint32_t maxD = (d0 > d1) ? d0 : d1;
        if (maxD > 0) {
            g_cpuLoad0 = 100 - (uint32_t)((uint64_t)d0 * 100 / maxD);
            g_cpuLoad1 = 100 - (uint32_t)((uint64_t)d1 * 100 / maxD);
        }
    }
    prevIdle0 = g_idleCount0;
    prevIdle1 = g_idleCount1;
    prevCpuMs = nowMs;

    // Live per-file progress from the upload task.  The phased orchestrator
    // runs CLOUD then SMB within one session, so each backend has its own
    // live-status slot; the legacy live_* fields below mirror whichever is
    // currently active.
    char cloudLiveFolder[33] = "";
    int  cloudLiveUp = 0, cloudLiveTotal = 0; bool cloudLiveActive = false;
    if (g_cloudSessionStatus.uploadActive) {
        strncpy(cloudLiveFolder, (const char*)g_cloudSessionStatus.currentFolder, sizeof(cloudLiveFolder) - 1);
        cloudLiveUp = g_cloudSessionStatus.filesUploaded;
        cloudLiveTotal = g_cloudSessionStatus.filesTotal;
        cloudLiveActive = true;
    }
    char smbLiveFolder[33] = "";
    int  smbLiveUp = 0, smbLiveTotal = 0; bool smbLiveActive = false;
    if (g_smbSessionStatus.uploadActive) {
        strncpy(smbLiveFolder, (const char*)g_smbSessionStatus.currentFolder, sizeof(smbLiveFolder) - 1);
        smbLiveUp = g_smbSessionStatus.filesUploaded;
        smbLiveTotal = g_smbSessionStatus.filesTotal;
        smbLiveActive = true;
    }

    // Now that live flags are known, compute per-backend counts.
    readBackendCounts(cloudStateManager, cloudDone, cloudTotal, cloudPending, cloudLastTs, cloudLiveActive);
    readBackendCounts(smbStateManager,   smbDone,   smbTotal,   smbPending,   smbLastTs,   smbLiveActive);

    char recentTabs[128];
    buildRecentTabsField(recentTabs, sizeof(recentTabs), nowMs);

    // ── Compute live UTC offset including DST ──
    // localtime_r uses the TZ env var (set from TZ_STRING via tzset()) so this
    // automatically accounts for DST without any lookup table.
    int tzOffsetMinutes = 0;
    if (config) {
        if (!config->getTzString().isEmpty()) {
            time_t t = now;
            struct tm local_tm, utc_tm;
            localtime_r(&t, &local_tm);
            gmtime_r(&t, &utc_tm);
            // Convert both to epoch-seconds to find the offset in whole minutes
            time_t local_t = mktime(&local_tm);
            time_t utc_t = mktime(&utc_tm);
            tzOffsetMinutes = (int)((local_t - utc_t) / 60);
        } else {
            tzOffsetMinutes = config->getGmtOffsetHours() * 60;
        }
    }

    char buf[WEB_STATUS_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"in_state_sec\":%lu,\"uptime\":%lu"
        ",\"time\":\"%s\",\"time_synced\":%s"
        ",\"free_heap\":%u,\"max_alloc\":%u"
        ",\"wifi\":%s,\"rssi\":%d,\"wifi_ssid\":\"%s\""
        ",\"backends\":{"
            "\"cloud\":{\"enabled\":%s,\"done\":%d,\"total\":%d,\"pending\":%d,\"last_ts\":%lu"
                ",\"live\":{\"active\":%s,\"folder\":\"%s\",\"up\":%d,\"total\":%d}}"
            ",\"smb\":{\"enabled\":%s,\"done\":%d,\"total\":%d,\"pending\":%d,\"last_ts\":%lu"
                ",\"live\":{\"active\":%s,\"folder\":\"%s\",\"up\":%d,\"total\":%d}}"
        "}"
        ",\"next_upload\":%ld"
        ",\"in_window\":%s,\"smart_quiet\":%s,\"smart_config_invalid\":%s"
        ",\"cpu0\":%u,\"cpu1\":%u"
        ",\"pcnt_capable\":%s"
        ",\"has_probed\":%s"
        ",\"tz_offset_minutes\":%d"
        ",\"recent_tabs\":\"%s\""
        ",\"hostname\":\"%s\""
        ",\"ap_setup\":%s"
        ",\"firmware\":\"%s\"}",
        st, inStateSec, upSec,
        timeBuf, timeSynced ? "true" : "false",
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
        wifiConn ? "true" : "false", rssi, wifiSsidJson.c_str(),
        cloudEnabled ? "true" : "false", cloudDone, cloudTotal, cloudPending,
            (unsigned long)cloudLastTs,
            cloudLiveActive ? "true" : "false", cloudLiveFolder, cloudLiveUp, cloudLiveTotal,
        smbEnabled ? "true" : "false", smbDone, smbTotal, smbPending,
            (unsigned long)smbLastTs,
            smbLiveActive ? "true" : "false", smbLiveFolder, smbLiveUp, smbLiveTotal,
        nextUp,
        inWindow ? "true" : "false",
        smartQuiet ? "true" : "false",
        (config && config->isSmartConfigInvalid()) ? "true" : "false",
        (unsigned)g_cpuLoad0, (unsigned)g_cpuLoad1,
        g_pcntCapable ? "true" : "false",
        stateManager ? (stateManager->hasBeenProbed() ? "true" : "false") : "false",
        tzOffsetMinutes,
        recentTabs,
        config ? config->getHostname().c_str() : "cpap",
        g_apSetupMode ? "true" : "false",
        FIRMWARE_VERSION);
    if (n > 0 && n < (int)sizeof(buf)) {
        memcpy(g_webStatusBuf, buf, n + 1);
    }
}

// ---------------------------------------------------------------------------
// initConfigSnapshot() — called once at boot after Config is loaded.
// ---------------------------------------------------------------------------
void CpapWebServer::initConfigSnapshot() {
    if (!config) return;
    char buf[WEB_CONFIG_BUF_SIZE];
    bool hasCloud = config->hasCloudEndpoint();
    int n = snprintf(buf, sizeof(buf),
        "{\"wifi_ssid\":\"%s\",\"hostname\":\"%s\""
        ",\"endpoint_type\":\"%s\",\"endpoint_user\":\"%s\""
        ",\"upload_mode\":\"%s\""
        ",\"upload_start_hour\":%d,\"upload_end_hour\":%d"
        ",\"smart_start_hour\":%d"
        ",\"inactivity_seconds\":%d"
        ",\"exclusive_access_minutes\":%d,\"cooldown_minutes\":%d"
        ",\"gmt_offset_hours\":%d,\"tz_string\":\"%s\",\"tz_name\":\"%s\",\"ntp_server\":\"%s\""
        ",\"max_days\":%d,\"recent_folder_days\":%d"
        ",\"cloud_configured\":%s"
        ",\"brownout_detect_mode\":\"%s\""
        ",\"firmware\":\"%s\"}",
        config->getWifiSSID(0).c_str(),
        config->getHostname().c_str(),
        config->getEndpointType().c_str(),
        config->getEndpointUser().c_str(),
        config->getUploadMode().c_str(),
        config->getUploadStartHour(), config->getUploadEndHour(),
        config->getSmartStartHour(),
        config->getInactivitySeconds(),
        config->getExclusiveAccessMinutes(), config->getCooldownMinutes(),
        config->getGmtOffsetHours(),
        config->getTzString().c_str(), config->getTzName().c_str(), config->getNtpServer().c_str(),
        config->getMaxDays(), config->getRecentFolderDays(),
        hasCloud ? "true" : "false",
        config->getBrownoutDetectMode() == BrownoutDetectMode::OFF ? "OFF" : 
        (config->getBrownoutDetectMode() == BrownoutDetectMode::RELAXED ? "RELAXED" : "ENABLED"),
        FIRMWARE_VERSION);
    if (n > 0 && n < (int)sizeof(buf)) {
        memcpy(g_webConfigBuf, buf, n + 1);
    }
}

// Update manager references (needed after uploader recreation)
void CpapWebServer::updateManagers(UploadStateManager* state, ScheduleManager* schedule) {
    stateManager = state;
    scheduleManager = schedule;
}

void CpapWebServer::setSmbStateManager(UploadStateManager* sm) { smbStateManager = sm; }

void CpapWebServer::setCloudStateManager(UploadStateManager* sm) { cloudStateManager = sm; }

void CpapWebServer::setSdManager(SDCardManager* sd) { sdManager = sd; }

// ---------------------------------------------------------------------------
// GET /api/config-raw — return raw contents of /config.txt as text/plain
// ---------------------------------------------------------------------------
void CpapWebServer::handleApiConfigRawGet() {
    addCorsHeaders(server);
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server->sendHeader("Connection", "close");

    if (!sdManager) {
        server->send(503, "application/json", "{\"error\":\"SD manager unavailable\"}");
        return;
    }

    // Borrow SD control if not already held (e.g. CPAP has card, no upload running)
    bool tookControl = false;
    if (!sdManager->hasControl()) {
        if (!sdManager->takeControl()) {
            server->send(503, "application/json", "{\"error\":\"SD unavailable — CPAP may be using it\"}");
            return;
        }
        tookControl = true;
    }

    fs::FS& sd = sdManager->getFS();
    File f = sd.open("/config.txt", FILE_READ);
    if (!f) {
        if (tookControl) sdManager->releaseControl();
        server->send(404, "application/json", "{\"error\":\"config.txt not found\"}");
        return;
    }
    
    String raw = f.readString();
    f.close();
    if (tookControl) sdManager->releaseControl();

    server->send(200, "text/plain", raw);
}

// ---------------------------------------------------------------------------
// POST /api/config-raw — write new config.txt content (max 4096 bytes)
// Body: raw text/plain  |  Returns: {"ok":true} or {"error":"..."}
// ---------------------------------------------------------------------------
static constexpr size_t CONFIG_RAW_MAX_BYTES = 4096;

void CpapWebServer::handleApiConfigRawPost() {
    addCorsHeaders(server);
    server->sendHeader("Cache-Control", "no-store");
    server->sendHeader("Connection", "close");

    if (!sdManager) {
        server->send(503, "application/json", "{\"error\":\"SD manager unavailable\"}");
        return;
    }
    size_t bodyLen = server->arg("plain").length();
    if (bodyLen == 0) {
        server->send(400, "application/json", "{\"error\":\"Empty body\"}");
        return;
    }
    if (bodyLen > CONFIG_RAW_MAX_BYTES) {
        server->send(413, "application/json", "{\"error\":\"Content too large (max 4096 bytes)\"}");
        return;
    }

    // Borrow SD control
    bool tookControl = false;
    if (!sdManager->hasControl()) {
        if (!sdManager->takeControl()) {
            server->send(503, "application/json", "{\"error\":\"SD unavailable — CPAP may be using it\"}");
            return;
        }
        tookControl = true;
    }

    const String& body = server->arg("plain");

    // Strip trailing blank lines to prevent newline accumulation across saves
    String trimmedBody = body;
    while (trimmedBody.length() > 0 &&
           (trimmedBody.charAt(trimmedBody.length() - 1) == '\n' ||
            trimmedBody.charAt(trimmedBody.length() - 1) == '\r')) {
        // Find the start of the last line
        int lastNl = trimmedBody.lastIndexOf('\n', trimmedBody.length() - 2);
        if (lastNl < 0) lastNl = -1;
        // Check if the last line is blank
        String lastLine = trimmedBody.substring(lastNl + 1);
        lastLine.trim();
        if (lastLine.length() == 0) {
            trimmedBody = trimmedBody.substring(0, lastNl + 1);
        } else {
            break;
        }
    }
    // Ensure exactly one trailing newline
    if (trimmedBody.length() > 0 && trimmedBody.charAt(trimmedBody.length() - 1) != '\n') {
        trimmedBody += '\n';
    }

    size_t unmaskedLen = trimmedBody.length();
    const String& unmasked = trimmedBody;
    fs::FS& sd = sdManager->getFS();

    // Write atomically: write to temp file, then rename
    sd.remove("/config.txt.tmp");
    File f = sd.open("/config.txt.tmp", FILE_WRITE);
    if (!f) {
        if (tookControl) sdManager->releaseControl();
        server->send(500, "application/json", "{\"error\":\"Failed to open temp file for writing\"}");
        return;
    }

    size_t written = 0;
    while (written < unmaskedLen) {
        size_t toWrite = unmaskedLen - written;
        if (toWrite > 512) toWrite = 512;
        size_t n = f.write((const uint8_t*)unmasked.c_str() + written, toWrite);
        if (n == 0) break;
        written += n;
    }

    if (written != unmaskedLen) {
        sd.remove("/config.txt.tmp");
        f.close();
        if (tookControl) sdManager->releaseControl();
        server->send(500, "application/json", "{\"error\":\"Write incomplete\"}");
        return;
    }

    f.close();

    sd.remove("/config.txt");
    if (!sd.rename("/config.txt.tmp", "/config.txt")) {
        if (tookControl) sdManager->releaseControl();
        server->send(500, "application/json", "{\"error\":\"Rename failed\"}");
        return;
    }

    if (tookControl) sdManager->releaseControl();
    LOG("[WebServer] config.txt updated via web UI");
    server->send(200, "application/json", "{\"ok\":true,\"message\":\"Saved successfully. CRITICAL: You MUST physically eject and reinsert the SD card into the CPAP machine now to prevent data corruption!\"}");
}

// Set WiFi manager reference
void CpapWebServer::setWiFiManager(WiFiManager* wifi) {
    wifiManager = wifi;
}

// Set TrafficMonitor reference
void CpapWebServer::setTrafficMonitor(TrafficMonitor* tm) {
    trafficMonitor = tm;
}

// Helper: Escape special characters for JSON string
String CpapWebServer::escapeJson(const String& str) {
    String escaped = "";
    escaped.reserve(str.length() + 20);  // Reserve extra space for escape sequences
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        switch (c) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                // Handle control characters (0x00-0x1F)
                if (c >= 0 && c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    escaped += buf;
                } else {
                    escaped += c;
                }
                break;
        }
    }
    
    return escaped;
}

#ifdef ENABLE_OTA_UPDATES
// Set OTA manager reference
void CpapWebServer::setOTAManager(OTAManager* ota) {
    otaManager = ota;
}

// GET /ota - OTA update page
void CpapWebServer::handleOTAPage() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);

    auto sendChunk = [this](const String& s) {
        server->sendContent(s);
    };

    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Firmware Update - CPAP Auto-Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "*{box-sizing:border-box;margin:0;padding:0}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#0f1923;color:#c7d5e0;min-height:100vh;padding:20px}";
    html += ".wrap{max-width:900px;margin:0 auto}";
    html += "h1{font-size:1.6em;color:#fff;margin-bottom:4px}";
    html += ".subtitle{color:#66c0f4;font-size:0.9em;margin-bottom:20px}";
    html += ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;margin-bottom:20px}";
    html += ".card{background:#1b2838;border:1px solid #2a475e;border-radius:10px;padding:18px}";
    html += ".card h2{font-size:0.85em;text-transform:uppercase;letter-spacing:1px;color:#66c0f4;margin-bottom:12px;border-bottom:1px solid #2a475e;padding-bottom:8px}";
    html += ".row{display:flex;justify-content:space-between;padding:5px 0;font-size:0.88em}";
    html += ".row .k{color:#8f98a0}.row .v{color:#c7d5e0;font-weight:500;text-align:right}";
    html += ".btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:10px 18px;border-radius:6px;font-size:0.85em;font-weight:600;text-decoration:none;border:none;cursor:pointer;transition:all 0.2s;width:100%}";
    html += ".btn-primary{background:#66c0f4;color:#0f1923}.btn-primary:hover{background:#88d0ff}";
    html += ".btn-secondary{background:#2a475e;color:#c7d5e0;width:auto}.btn-secondary:hover{background:#3a5a7e}";
    html += ".btn-danger{background:#c0392b;color:#fff}.btn-danger:hover{background:#e04030}";
    html += ".alert{background:#3a2a1a;border:1px solid #aa6622;border-radius:8px;padding:14px;margin-bottom:16px}";
    html += ".alert h3{color:#ffaa44;font-size:0.9em;margin-bottom:6px}";
    html += ".alert ul{padding-left:20px;color:#c7d5e0;font-size:0.85em}";
    html += ".alert li{margin-bottom:4px}";
    html += ".form-group{margin-bottom:15px}";
    html += ".form-group label{display:block;margin-bottom:6px;color:#8f98a0;font-size:0.9em}";
    html += ".form-group input{width:100%;padding:10px;background:#0f1923;border:1px solid #2a475e;color:#fff;border-radius:6px;font-size:0.9em}";
    html += ".form-group input:focus{outline:none;border-color:#66c0f4}";
    html += ".status-msg{margin-top:10px;font-size:0.9em;min-height:1.2em}";
    html += ".status-msg.success{color:#44ff44}";
    html += ".status-msg.error{color:#ff4444}";
    html += ".status-msg.info{color:#66c0f4}";
    html += ".actions{margin-top:20px}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='wrap'>";
    
    server->send(200, "text/html; charset=utf-8", html);

    html = "<h1>Firmware Update</h1>";
    html += "<p class='subtitle'>System Maintenance</p>";
    sendChunk(html);
    
    html = "<div class='cards'>";
    
    // Current version info
    html += "<div class='card'>";
    html += "<h2>Current Status</h2>";
    if (otaManager) {
        html += "<div class='row'><span class='k'>Current Version</span><span class='v'>" + otaManager->getCurrentVersion() + "</span></div>";
    } else {
        html += "<div class='row'><span class='k'>Current Version</span><span class='v'>Unknown</span></div>";
    }
    html += "</div>";
    
    // Warning message
    html += "<div class='card' style='grid-column:1/-1'>";
    html += "<h2>Important Safety Information</h2>";
    html += "<div class='alert'>";
    html += "<h3>WARNING</h3>";
    html += "<ul>";
    html += "<li><strong>Do not power off</strong> the device during update</li>";
    html += "<li><strong>Ensure stable WiFi</strong> connection before starting</li>";
    html += "<li><strong>Do NOT remove SD card</strong> from CPAP machine during update</li>";
    html += "<li>Update process takes 1-2 minutes</li>";
    html += "<li>Device will restart automatically when complete</li>";
    html += "</ul>";
    html += "</div>";
    html += "</div>";
    
    // Check if update is in progress
    if (otaManager && otaManager->isUpdateInProgress()) {
        html += "<div class='card' style='grid-column:1/-1'>";
        html += "<h2>Update In Progress</h2>";
        html += "<p style='color:#ffaa44'>A firmware update is currently in progress. Please wait for it to complete.</p>";
        html += "</div>";
    } else {
        // File upload method
        html += "<div class='card'>";
        html += "<h2>Method 1: File Upload</h2>";
        html += "<form id='uploadForm' enctype='multipart/form-data'>";
        html += "<div class='form-group'>";
        html += "<label for='firmwareFile'>Select firmware file (.bin):</label>";
        html += "<input type='file' id='firmwareFile' name='firmware' accept='.bin' required>";
        html += "</div>";
        html += "<button type='submit' class='btn btn-primary'>Upload & Install</button>";
        html += "<div id='uploadStatus' class='status-msg'></div>";
        html += "</form>";
        html += "</div>";
        
        // URL download method
        html += "<div class='card'>";
        html += "<h2>Method 2: URL Download</h2>";
        html += "<form id='urlForm'>";
        html += "<div class='form-group'>";
        html += "<label for='firmwareURL'>Firmware URL:</label>";
        html += "<input type='url' id='firmwareURL' name='url' placeholder='https://github.com/.../firmware.bin' required>";
        html += "</div>";
        html += "<button type='submit' class='btn btn-primary'>Download & Install</button>";
        html += "<div id='downloadStatus' class='status-msg'></div>";
        html += "</form>";
        html += "</div>";
    }
    
    html += "</div>"; // end cards
    
    html += "<div class='actions'>";
    html += "<a href='/' class='btn btn-secondary'>&larr; Back to Status</a>";
    html += "</div>";
    
    // JavaScript for handling uploads
    html += "<script>";
    html += "let updateInProgress = false;";
    
    // File upload handler
    html += "document.getElementById('uploadForm')?.addEventListener('submit', function(e) {";
    html += "  e.preventDefault();";
    html += "  if (updateInProgress) return;";
    html += "  const fileInput = document.getElementById('firmwareFile');";
    html += "  if (!fileInput.files[0]) { alert('Please select a file'); return; }";
    html += "  uploadFirmware(fileInput.files[0]);";
    html += "});";
    
    // URL download handler
    html += "document.getElementById('urlForm')?.addEventListener('submit', function(e) {";
    html += "  e.preventDefault();";
    html += "  if (updateInProgress) return;";
    html += "  const url = document.getElementById('firmwareURL').value;";
    html += "  if (!url) { alert('Please enter a URL'); return; }";
    html += "  downloadFirmware(url);";
    html += "});";
    
    // Upload function
    html += "function uploadFirmware(file) {";
    html += "  updateInProgress = true;";
    html += "  var uploadComplete = false;";
    html += "  setStatus('uploadStatus', 'info', 'Uploading firmware... 0%');";
    html += "  const formData = new FormData();";
    html += "  formData.append('firmware', file);";
    html += "  const xhr = new XMLHttpRequest();";
    html += "  xhr.upload.addEventListener('progress', function(e) {";
    html += "    if (e.lengthComputable) {";
    html += "      const percent = Math.round((e.loaded / e.total) * 100);";
    html += "      if (percent >= 100) uploadComplete = true;";
    html += "      setStatus('uploadStatus', 'info', 'Uploading firmware... ' + percent + '%');";
    html += "    }";
    html += "  });";
    html += "  xhr.addEventListener('load', function() {";
    html += "    try {";
    html += "      const data = JSON.parse(xhr.responseText);";
    html += "      handleResult(data, 'uploadStatus');";
    html += "    } catch(e) { handleResult({success:false, message:'Invalid response'}, 'uploadStatus'); }";
    html += "  });";
    html += "  xhr.addEventListener('error', function() {";
    html += "    if (uploadComplete) {";
    html += "      handleResult({success:true, message:'Firmware uploaded. Device is restarting...'}, 'uploadStatus');";
    html += "    } else {";
    html += "      handleResult({success:false, message:'Network error'}, 'uploadStatus');";
    html += "    }";
    html += "  });";
    html += "  xhr.open('POST', '/ota-upload');";
    html += "  xhr.send(formData);";
    html += "}";
    
    // Download function
    html += "function downloadFirmware(url) {";
    html += "  updateInProgress = true;";
    html += "  setStatus('downloadStatus', 'info', 'Downloading firmware... (this may take a minute)');";
    html += "  const formData = new FormData();";
    html += "  formData.append('url', url);";
    html += "  fetch('/ota-url', { method: 'POST', body: formData })";
    html += "    .then(response => response.json())";
    html += "    .then(data => handleResult(data, 'downloadStatus'))";
    html += "    .catch(error => handleResult({success:false, message:String(error)}, 'downloadStatus'));";
    html += "}";
    
    // Result handlers
    html += "function setStatus(id, type, msg) {";
    html += "  const el = document.getElementById(id);";
    html += "  if(el) { el.className = 'status-msg ' + type; el.textContent = msg; }";
    html += "}";
    
    html += "function handleResult(data, statusId) {";
    html += "  updateInProgress = false;";
    html += "  if (data.success) {";
    html += "    setStatus(statusId, 'success', 'Success! ' + data.message);";
    html += "    let timeLeft = 30;";
    html += "    setInterval(() => {";
    html += "      timeLeft--;";
    html += "      if (timeLeft <= 0) window.location.href = '/';";
    html += "      else setStatus(statusId, 'success', 'Success! Redirecting to dashboard in ' + timeLeft + 's...');";
    html += "    }, 1000);";
    html += "  } else {";
    html += "    setStatus(statusId, 'error', 'Failed: ' + data.message);";
    html += "  }";
    html += "}";
    
    html += "</script>";
    html += "</div></body></html>";
    sendChunk(html);
    
    server->sendContent("");
}

// POST /ota-upload - Handle firmware file upload (chunk callback)
// IMPORTANT: Do NOT call server->send() here — the WebServer framework
// calls handleOTAUploadComplete() after all chunks are received and expects
// that function to send the one-and-only HTTP response.  Sending a response
// from both handlers produces duplicate Content-Length headers.
void CpapWebServer::handleOTAUpload() {
    static bool uploadError = false;
    static bool updateFinished = false;
    
    if (!otaManager) {
        uploadError = true;
        return;
    }
    
    HTTPUpload& upload = server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        LOG_DEBUGF("[OTA] UPLOAD_FILE_START - filename: %s, totalSize: %u", upload.filename.c_str(), upload.totalSize);
        uploadError = false;
        updateFinished = false;
        
        if (upload.totalSize == 0) {
            LOG_WARN("[OTA] Total size is 0, using chunked upload mode");
        }
        
        if (!otaManager->startUpdate(upload.totalSize)) {
            LOG_ERROR("[OTA] Failed to start update");
            uploadError = true;
        }
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!uploadError && !otaManager->writeChunk(upload.buf, upload.currentSize)) {
            LOG_ERROR("[OTA] Failed to write chunk");
            uploadError = true;
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        LOG_DEBUGF("[OTA] UPLOAD_FILE_END - totalSize: %u, uploadError: %s", 
                   upload.totalSize, uploadError ? "true" : "false");
        
        if (uploadError) {
            otaManager->abortUpdate();
        } else if (otaManager->finishUpdate()) {
            LOG("[OTA] Update completed successfully");
            updateFinished = true;
        } else {
            LOG_ERROR("[OTA] Failed to finish update");
            uploadError = true;
        }
        
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        LOG_WARN("[OTA] UPLOAD_FILE_ABORTED");
        otaManager->abortUpdate();
        uploadError = true;
    }
}

// POST /ota-upload - Completion handler (sends the sole HTTP response)
// Called by WebServer after the last upload chunk callback returns.
void CpapWebServer::handleOTAUploadComplete() {
    if (!otaManager) {
        server->send(500, "application/json", "{\"success\":false,\"message\":\"OTA manager not initialized\"}");
        return;
    }
    
    String error = otaManager->getLastError();
    if (!error.isEmpty()) {
        server->send(500, "application/json", "{\"success\":false,\"message\":\"Upload failed: " + error + "\"}");
        return;
    }
    
    // Success — send response, then restart
    server->send(200, "application/json", "{\"success\":true,\"message\":\"Update completed! Device will restart in 3 seconds.\"}");
    delay(3000);
    ESP.restart();
}

// POST /ota-url - Handle firmware download from URL
void CpapWebServer::handleOTAURL() {
    if (!otaManager) {
        server->send(500, "application/json", "{\"success\":false,\"message\":\"OTA manager not initialized\"}");
        return;
    }
    
    if (otaManager->isUpdateInProgress()) {
        server->send(400, "application/json", "{\"success\":false,\"message\":\"Update already in progress\"}");
        return;
    }
    
    if (!server->hasArg("url")) {
        server->send(400, "application/json", "{\"success\":false,\"message\":\"URL parameter required\"}");
        return;
    }
    
    String url = server->arg("url");
    LOG_DEBUGF("[OTA] Starting download from URL: %s", url.c_str());
    
    if (otaManager->updateFromURL(url)) {
        LOG("[OTA] Update completed successfully, restarting...");
        server->send(200, "application/json", "{\"success\":true,\"message\":\"Update completed! Device will restart in 3 seconds.\"}");
        
        // Restart after a short delay
        delay(3000);
        ESP.restart();
    } else {
        LOG_ERROR("[OTA] Failed to update from URL");
        String error = otaManager->getLastError();
        server->send(500, "application/json", "{\"success\":false,\"message\":\"Update failed: " + error + "\"}");
    }
}

#endif // ENABLE_OTA_UPDATES

// ============================================================================
// SD Activity Monitor Handlers
// ============================================================================

void CpapWebServer::handleMonitorStart() {
    addCorsHeaders(server);
    g_monitorActivityFlag = true;
    server->send(200, "application/json", "{\"success\":true,\"message\":\"Monitoring started\"}");
}

void CpapWebServer::handleMonitorStop() {
    addCorsHeaders(server);
    g_stopMonitorFlag = true;
    server->send(200, "application/json", "{\"success\":true,\"message\":\"Monitoring stopped\"}");
}

void CpapWebServer::handleSdActivity() {
    addCorsHeaders(server);
    
    if (!trafficMonitor) {
        server->send(500, "application/json", "{\"error\":\"TrafficMonitor not available\"}");
        return;
    }

    // During uploads return a compact payload with no sample history.
    // This keeps the endpoint alive for UI health checks while avoiding heavy String churn.
    if (isUploadInProgress()) {
        static unsigned long lastCompactRefreshMs = 0;
        static char compactJson[256] = {0};
        const unsigned long now = millis();
        if (compactJson[0] == '\0' || now - lastCompactRefreshMs >= 1000) {
            snprintf(compactJson,
                     sizeof(compactJson),
                     "{\"last_pulse_count\":%lu,\"consecutive_idle_ms\":%lu,\"longest_idle_ms\":%lu,\"total_active_samples\":%lu,\"total_idle_samples\":%lu,\"is_busy\":%s,\"sample_count\":%d,\"samples\":[],\"degraded\":true}",
                     (unsigned long)trafficMonitor->getLastPulseCount(),
                     (unsigned long)trafficMonitor->getConsecutiveIdleMs(),
                     (unsigned long)trafficMonitor->getLongestIdleMs(),
                     (unsigned long)trafficMonitor->getTotalActiveSamples(),
                     (unsigned long)trafficMonitor->getTotalIdleSamples(),
                     trafficMonitor->isBusy() ? "true" : "false",
                     trafficMonitor->getSampleCount());
            lastCompactRefreshMs = now;
        }

        server->send(200, "application/json", compactJson);
        return;
    }
    
    // Build JSON with current stats and recent samples
    String json = "{";
    json += "\"last_pulse_count\":" + String(trafficMonitor->getLastPulseCount()) + ",";
    json += "\"consecutive_idle_ms\":" + String((unsigned long)trafficMonitor->getConsecutiveIdleMs()) + ",";
    json += "\"longest_idle_ms\":" + String((unsigned long)trafficMonitor->getLongestIdleMs()) + ",";
    json += "\"total_active_samples\":" + String((unsigned long)trafficMonitor->getTotalActiveSamples()) + ",";
    json += "\"total_idle_samples\":" + String((unsigned long)trafficMonitor->getTotalIdleSamples()) + ",";
    json += "\"is_busy\":" + String(trafficMonitor->isBusy() ? "true" : "false") + ",";
    json += "\"sample_count\":" + String(trafficMonitor->getSampleCount()) + ",";
    
    // Include the last N samples from the circular buffer
    json += "\"samples\":[";
    int count = trafficMonitor->getSampleCount();
    int head = trafficMonitor->getSampleHead();
    int maxSamples = TrafficMonitor::MAX_SAMPLES;
    const ActivitySample* buffer = trafficMonitor->getSampleBuffer();
    
    // Output samples oldest-first (from tail to head)
    int start = (count < maxSamples) ? 0 : head;
    int outputCount = min(count, 60);  // Limit to last 60 seconds for web response
    int skip = count - outputCount;
    
    bool first = true;
    for (int i = 0; i < count; i++) {
        if (i < skip) continue;
        int idx = (start + i) % maxSamples;
        if (!first) json += ",";
        json += "{\"t\":" + String(buffer[idx].timestamp);
        json += ",\"p\":" + String(buffer[idx].pulseCount);
        json += ",\"a\":" + String(buffer[idx].active ? 1 : 0) + "}";
        first = false;
    }
    
    json += "]}";
    
    server->send(200, "application/json", json);
}

// handleApiDiagnostics() removed — cpu0/cpu1 merged into /api/status

void CpapWebServer::handleMonitorPage() {
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server->sendHeader("Connection", "close");
    server->sendHeader("Content-Encoding", "gzip");
    server->send_P(200, "text/html; charset=utf-8", (const char*)web_ui_gz, web_ui_gz_len);
}

// ============================================================================
// SSE Log Push — call from main loop every ~100-200ms
// ============================================================================
void pushSseLogs() {
    if (!g_sseActive) return;

    if (!g_sseClient.connected()) {
        clearSseOwner();
        return;
    }

    Logger& logger = Logger::getInstance();
    uint32_t currentHead = logger.getHeadIndex();
    unsigned long now = millis();
    static unsigned long lastDataPushAt = 0;
    const bool uploadInProgress = uploadTaskRunning;
    const unsigned long minPushIntervalMs = uploadInProgress ? 250 : 0;

    if (currentHead == g_sseLastPushedIndex) {
        // No new log data — send periodic keepalive to prevent idle timeout
        static unsigned long lastKeepalive = 0;
        if (now - lastKeepalive >= 15000) {
            lastKeepalive = now;
            if (g_sseClient.connected()) {
                g_sseClient.print(": keepalive\n\n");
                g_sseOwnerLastSeenMs = now;
            } else {
                clearSseOwner();
            }
        }
        return;
    }

    if (minPushIntervalMs && (now - lastDataPushAt) < minPushIntervalMs) {
        return;
    }

    // Read new bytes from circular buffer and push as SSE data events.
    // We read under the logger's mutex via printLogsTail-style approach,
    // but simplified: we know exactly the range [g_sseLastPushedIndex..currentHead).
    uint32_t newBytes = currentHead - g_sseLastPushedIndex;
    if (newBytes > LOG_BUFFER_SIZE) {
        // Buffer wrapped — we lost some, start from current tail
        newBytes = LOG_BUFFER_SIZE;
        g_sseLastPushedIndex = currentHead - LOG_BUFFER_SIZE;
    }

    // Cap per-push to avoid blocking the loop too long
    const uint32_t maxBytesPerPush = uploadInProgress ? 512 : 2048;
    if (newBytes > maxBytesPerPush) newBytes = maxBytesPerPush;

    // Build SSE "data:" lines from the buffer content.
    // SSE format: "data: <line>\n\n" for each line.
    // We'll batch into a single "data:" block for efficiency.
    char chunk[320];  // "data: " + up to ~300 chars + "\n\n"
    size_t chunkPos = 0;

    // Write "data: " prefix
    memcpy(chunk, "data: ", 6);
    chunkPos = 6;

    for (uint32_t i = 0; i < newBytes; i++) {
        size_t physicalPos = (g_sseLastPushedIndex + i) % LOG_BUFFER_SIZE;
        char c = Logger::s_logBuffer[physicalPos];

        if (c == '\n') {
            // End this SSE event and start a new one
            chunk[chunkPos++] = '\n';
            chunk[chunkPos++] = '\n';
            // Flush chunk
            if (g_sseClient.connected()) {
                g_sseClient.write((const uint8_t*)chunk, chunkPos);
                g_sseOwnerLastSeenMs = now;
            } else {
                clearSseOwner();
                break;
            }
            // Start next "data: " prefix
            memcpy(chunk, "data: ", 6);
            chunkPos = 6;
        } else {
            chunk[chunkPos++] = c;
            // Flush if chunk is getting full (leave room for \n\n)
            if (chunkPos >= sizeof(chunk) - 3) {
                chunk[chunkPos++] = '\n';
                chunk[chunkPos++] = '\n';
                if (g_sseClient.connected()) {
                    g_sseClient.write((const uint8_t*)chunk, chunkPos);
                    g_sseOwnerLastSeenMs = now;
                } else {
                    clearSseOwner();
                    break;
                }
                memcpy(chunk, "data: ", 6);
                chunkPos = 6;
            }
        }
    }

    // Flush any remaining partial data
    if (g_sseActive && chunkPos > 6) {
        chunk[chunkPos++] = '\n';
        chunk[chunkPos++] = '\n';
        if (g_sseClient.connected()) {
            g_sseClient.write((const uint8_t*)chunk, chunkPos);
            g_sseOwnerLastSeenMs = now;
        } else {
            clearSseOwner();
        }
    }

    lastDataPushAt = now;
    g_sseLastPushedIndex += newBytes;
}
