#include "ScheduleManager.h"
#include "Logger.h"
#if defined(ESP32)
#include "esp_sntp.h"
#endif

extern bool g_heapRecoveryBoot;  // defined in main.cpp (RTC_DATA_ATTR)

ScheduleManager::ScheduleManager() :
    uploadStartHour(8),
    uploadEndHour(22),
    smartStartHour(6),
    uploadMode("scheduled"),
    uploadHour(12),
    uploadCompletedToday(false),
    lastCompletedDay(-1),
    lastUploadTimestamp(0),
    ntpServer("pool.ntp.org"),
    gmtOffsetHours(0)
{}

void ScheduleManager::setNtpServer(const String& server) {
    if (server.length() > 0) {
        ntpServer = server;
    }
}

bool ScheduleManager::begin(const String& mode, int startHour, int endHour,
                            int smartStart, int gmtOffset,
                            const String& tz, const String& ntp) {
    this->uploadMode = mode;
    this->uploadStartHour = startHour;
    this->uploadEndHour = endHour;
    this->smartStartHour = smartStart;
    this->gmtOffsetHours = gmtOffset;
    this->tzString = tz;
    this->ntpServer = ntp;
    
    // Also set legacy uploadHour for backward compat
    this->uploadHour = startHour;
    
    if (tzString.length() > 0) {
        LOGF("[Schedule] Mode: %s, Window: %d:00-%d:00, SmartStart: %d:00, TZ: %s",
             mode.c_str(), startHour, endHour, smartStart, tzString.c_str());
    } else {
        LOGF("[Schedule] Mode: %s, Window: %d:00-%d:00, SmartStart: %d:00, GMT%+d",
             mode.c_str(), startHour, endHour, smartStart, gmtOffset);
    }
    LOGF("[Schedule] NTP server: %s", ntpServer.c_str());
    
    syncTime();
    return true;
}

bool ScheduleManager::begin(int uploadHour, int gmtOffsetHours) {
    // Legacy overload: create a 2-hour window from the single upload hour
    this->uploadHour = uploadHour;
    
    if (uploadHour < 0 || uploadHour > 23) {
        LOG("Invalid upload hour, using default (12)");
        this->uploadHour = 12;
    }
    
    return begin("scheduled", this->uploadHour, (this->uploadHour + 2) % 24, this->smartStartHour, gmtOffsetHours);
}

bool ScheduleManager::syncTime() {
    LOGF("[NTP] Starting time sync with server: %s", ntpServer.c_str());
    LOGF("[NTP] TZ string: %s", tzString.c_str());
    LOGF("[NTP] GMT offset: %d hours", gmtOffsetHours);
    
    // Allow network to stabilize after WiFi connection.
    // Skip on heap-recovery reboots — WiFi re-connects to a known AP in <1 s.
    if (g_heapRecoveryBoot) {
        LOG("[NTP] [FastBoot] Skipping network-stabilize delay");
    } else {
        LOG("[NTP] Waiting 2 seconds for network to stabilize...");
        delay(2000);
    }
    
    // Skip ICMP ping pre-check to reduce dependency footprint.
    // ICMP reachability is not required for NTP (uses UDP/123).
    LOG("[NTP] Proceeding directly with UDP NTP sync (ICMP pre-check disabled)");
    
    // Check if DHCP gave us a server (esp_sntp_servermode_dhcp(true) was called before WiFi connect)
    bool hasDhcpServer = false;
    const ip_addr_t* dhcpServer = esp_sntp_getserver(0);
    if (dhcpServer && !ip_addr_isany(dhcpServer)) {
        hasDhcpServer = true;
    }
    
    const char* serverToUse = nullptr;
    
    // Configure DHCP Option 42 (NTP) based on server override
    if (ntpServer.length() > 0 && ntpServer != "pool.ntp.org") {
#if defined(ESP32)
        esp_sntp_servermode_dhcp(0);
#endif
        LOGF("[NTP] Using configured override: %s", ntpServer.c_str());
        serverToUse = ntpServer.c_str();
    } else if (hasDhcpServer) {
        LOGF("[NTP] Using DHCP-provided server: " IPSTR, IP2STR(&dhcpServer->u_addr.ip4));
        serverToUse = nullptr; // Tells configTzTime not to overwrite server 0
    } else {
        LOG("[NTP] No DHCP server found, using fallback: pool.ntp.org");
        serverToUse = "pool.ntp.org";
    }

    String tzVal;
    if (tzString.length() > 0) {
        LOGF("[NTP] Using POSIX timezone: %s", tzString.c_str());
        tzVal = tzString;
    } else {
        // note inverted sign, TZ "UTC-5" is "GMT+5"
        char tz[24];
        snprintf(tz, sizeof(tz), "UTC%d", -gmtOffsetHours);
        LOGF("[NTP] Using GMT offset: %d hours (TZ=%s)", gmtOffsetHours, tz);
        tzVal = tz;
    }

    // configTzTime natively handles SNTP init, parses the TZ string natively, 
    // and correctly preserves DHCP SNTP servers if serverToUse is nullptr.
    configTzTime(tzVal.c_str(), serverToUse);
    

    // Wait for time to be set (with timeout)
    int retries = 0;
    const int maxRetries = 20;  // Increased timeout
    
    LOG("[NTP] Waiting for time synchronization...");
    while (retries < maxRetries) {
        time_t now = time(nullptr);
        LOGF("[NTP] Retry %d/%d: Current timestamp: %lu", retries + 1, maxRetries, (unsigned long)now);
        
        if (now > 24 * 3600) {  // Time is set if it's past Jan 1, 1970 + 1 day
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                LOG("[NTP] Time synchronized successfully!");
                LOGF("[NTP] Current time: %04d-%02d-%02d %02d:%02d:%02d", 
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
                return true;
            } else {
                LOG("[NTP] WARNING: Timestamp valid but getLocalTime failed");
            }
        }
        delay(1000);  // Increased from 500ms to 1000ms for high-latency networks
        retries++;
    }
    
    LOG("[NTP] ERROR: Failed to sync time after maximum retries");
    LOG("[NTP] Possible causes:");
    LOG("[NTP]   - Network firewall blocking NTP (UDP port 123)");
    LOG("[NTP]   - DNS resolution failure for pool.ntp.org");
    LOG("[NTP]   - No internet connectivity");
    LOG("[NTP]   - NTP server unreachable from this network");
    return false;
}

// ============================================================================
// Window-based scheduling (new FSM methods)
// ============================================================================

bool ScheduleManager::isInUploadWindow() {
    if (isManualMode()) return false;
    if (!isTimeSynced()) return false;
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return false;
    
    // If start == end, window is always open (24/7)
    if (uploadStartHour == uploadEndHour) return true;
    
    int currentHour = timeinfo.tm_hour;
    
    if (uploadStartHour < uploadEndHour) {
        // Normal window: e.g., 8-22
        return currentHour >= uploadStartHour && currentHour < uploadEndHour;
    } else {
        // Cross-midnight window: e.g., 22-6
        return currentHour >= uploadStartHour || currentHour < uploadEndHour;
    }
}

bool ScheduleManager::isSmartQuietPeriod() {
    if (!isSmartMode()) return false;
    if (!isTimeSynced()) return false;
    
    // If smartStartHour == uploadEndHour, quiet period is disabled (24/7 active)
    if (smartStartHour == uploadEndHour) return false;
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return false;
    
    int currentHour = timeinfo.tm_hour;
    
    // Quiet period: from uploadEndHour to smartStartHour
    // This typically crosses midnight (e.g., 21:00 → 06:00)
    if (uploadEndHour <= smartStartHour) {
        // Same-day range (e.g., end=6, start=9 — unlikely but valid)
        return currentHour >= uploadEndHour && currentHour < smartStartHour;
    } else {
        // Cross-midnight range (e.g., end=21, start=6)
        return currentHour >= uploadEndHour || currentHour < smartStartHour;
    }
}

bool ScheduleManager::canUploadFreshData() {
    if (isManualMode()) return false;
    if (!isTimeSynced()) return false;
    
    if (isSmartMode()) {
        // Smart mode: fresh data can upload anytime EXCEPT during quiet period
        return !isSmartQuietPeriod();
    }
    // Scheduled mode: fresh data only within upload window
    return isInUploadWindow();
}

bool ScheduleManager::canUploadOldData() {
    if (isManualMode()) return false;
    if (!isTimeSynced()) return false;
    
    // Both modes: old data only within upload window
    return isInUploadWindow();
}

bool ScheduleManager::canProcessOldData() {
    // Manual mode keeps canUploadOldData() == false so the scheduler/window
    // logic (e.g. main.cpp window-open detection, isUploadEligible) stays
    // pure.  But a manual run only ever happens via an explicit Force Upload,
    // which is documented as "upload all available data now" — so old
    // (non-recent) folders MUST be processed.  Gate on isTimeSynced() because
    // recent/MAX_DAYS folder-name math is meaningless without a valid clock.
    if (isManualMode()) return isTimeSynced();
    return canUploadOldData();
}

bool ScheduleManager::isUploadEligible(bool hasFreshData, bool hasOldData) {
    if (isManualMode()) return false;
    if (!isTimeSynced()) return false;
    
    // In scheduled mode, check if already completed today
    if (!isSmartMode() && isDayCompleted()) {
        return false;
    }
    
    // Check if any data category is eligible right now
    if (hasFreshData && canUploadFreshData()) return true;
    if (hasOldData && canUploadOldData()) return true;
    
    return false;
}

void ScheduleManager::markDayCompleted() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        lastCompletedDay = timeinfo.tm_yday;
        uploadCompletedToday = true;
    }
    lastUploadTimestamp = time(nullptr);
    LOGF("[Schedule] Day marked as completed (yday=%d)", lastCompletedDay);
}

bool ScheduleManager::isDayCompleted() {
    if (!uploadCompletedToday) return false;
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return false;
    
    // Reset if we're on a new day
    if (timeinfo.tm_yday != lastCompletedDay) {
        uploadCompletedToday = false;
        return false;
    }
    return true;
}

// ============================================================================
// Legacy methods (delegate to new logic)
// ============================================================================

bool ScheduleManager::isUploadTime() {
    if (!isTimeSynced()) {
        LOG("Time not synced, cannot check upload schedule");
        return false;
    }
    
    // Use new window check + day completion
    if (isDayCompleted()) return false;
    return isInUploadWindow();
}

void ScheduleManager::markUploadCompleted() {
    markDayCompleted();
    LOGF("Upload marked as completed at timestamp: %lu", lastUploadTimestamp);
}

unsigned long ScheduleManager::getSecondsUntilNextUpload() {
    if (isManualMode()) {
        return (unsigned long)-1;
    }
    if (!isTimeSynced()) {
        return 0;
    }
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return 0;
    
    // Calculate seconds until upload window opens
    int currentHour = timeinfo.tm_hour;
    int hoursUntil;
    
    if (uploadStartHour <= uploadEndHour) {
        // Normal window
        if (currentHour < uploadStartHour) {
            hoursUntil = uploadStartHour - currentHour;
        } else if (currentHour >= uploadEndHour) {
            hoursUntil = (24 - currentHour) + uploadStartHour;
        } else {
            return 0; // Currently in window
        }
    } else {
        // Cross-midnight window
        if (currentHour >= uploadStartHour || currentHour < uploadEndHour) {
            return 0; // Currently in window
        }
        hoursUntil = uploadStartHour - currentHour;
        if (hoursUntil < 0) hoursUntil += 24;
    }
    
    // Approximate: hours * 3600 minus current minutes/seconds
    return (unsigned long)hoursUntil * 3600 - timeinfo.tm_min * 60 - timeinfo.tm_sec;
}

// ============================================================================
// Time utilities
// ============================================================================

bool ScheduleManager::isTimeSynced() const {
    return time(nullptr) > 1000000000;
}

unsigned long ScheduleManager::getLastUploadTimestamp() const {
    return lastUploadTimestamp;
}

void ScheduleManager::setLastUploadTimestamp(unsigned long timestamp) {
    lastUploadTimestamp = timestamp;
}

String ScheduleManager::getCurrentLocalTime() const {
    if (!isTimeSynced()) {
        return "Time not synchronized";
    }
    
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (!localtime_r(&now, &timeinfo)) {
        return "Failed to get local time";
    }
    
    char buffer[48];
    if (tzString.length() > 0) {
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d (%s)",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 timeinfo.tm_isdst > 0 ? "DST" : "STD");
    } else {
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d (GMT%+d)",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 gmtOffsetHours);
    }

    
    return String(buffer);
}

// ============================================================================
// Getters for web UI
// ============================================================================

const String& ScheduleManager::getUploadMode() const { return uploadMode; }
int ScheduleManager::getUploadStartHour() const { return uploadStartHour; }
int ScheduleManager::getSmartStartHour() const { return smartStartHour; }
int ScheduleManager::getUploadEndHour() const { return uploadEndHour; }
bool ScheduleManager::isSmartMode() const { return uploadMode == "smart"; }
bool ScheduleManager::isManualMode() const { return uploadMode == "manual"; }
