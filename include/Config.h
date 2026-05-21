#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <FS.h>

// Power management enums
enum class WifiTxPower {
    POWER_LOWEST,  // -1 dBm — router on same nightstand
    POWER_LOW,     //  2 dBm — router within 1-2 metres
    POWER_MID,     //  5 dBm — default, typical bedroom (3-5m)
    POWER_HIGH,    //  8.5 dBm — router in adjacent room
    POWER_MAX      // 10 dBm — through walls, last resort (PHY-capped)
};

enum class WifiPowerSaving {
    SAVE_NONE,     // No power saving — maximum responsiveness, highest power
    SAVE_MID,      // WIFI_PS_MIN_MODEM — default, wakes every DTIM for broadcasts
    SAVE_MAX       // WIFI_PS_MAX_MODEM — maximum WiFi savings, may miss mDNS queries
};

// Conditionally include Preferences for ESP32 or use mock for testing
#ifdef UNIT_TEST
    #include "MockPreferences.h"
#else
    #include <Preferences.h>
#endif

enum class BrownoutDetectMode {
    ENABLED,
    RELAXED,
    OFF
};

class Config {
public:
    // Max line length for config file
    static const size_t MAX_LINE_LENGTH = 256;

    // Maximum number of configured WiFi networks (WIFI_SSID_1 .. WIFI_SSID_N).
    static constexpr int WIFI_MAX_NETWORKS = 4;

    struct WiFiCredential {
        String ssid;
        String password;
    };

private:
    WiFiCredential wifiNetworks[WIFI_MAX_NETWORKS];
    int wifiNetworkCount;  // populated slots (0..WIFI_MAX_NETWORKS)
    String hostname;  // mDNS hostname (defaults to "cpap")
    String ntpServer; // Custom NTP server
    String schedule;
    String endpoint;
    String endpointType;  // SMB, CLOUD, SMB,CLOUD
    String endpointUser;
    String endpointPassword;
    int gmtOffsetHours;
    String tzString;    // POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
    String tzName;      // IANA timezone name, e.g. "Australia/Melbourne" (UI only, not used by firmware)
    bool saveLogs;
    bool debugMode;
    bool isValid;
    
    // Cloud upload settings
    String cloudClientId;
    String cloudClientSecret;
    String cloudTeamId;
    String cloudBaseUrl;
    int cloudDeviceId;
    int maxDays;
    int recentFolderDays;
    bool cloudInsecureTls;
    bool smbPreserveTimestamps;
    
    // Upload FSM settings
    String uploadMode;             // "scheduled" or "smart"
    int uploadStartHour;           // 0-23, start of upload window
    int uploadEndHour;             // 0-23, end of upload window
    int inactivitySeconds;         // Z: seconds of bus silence before upload
    int exclusiveAccessMinutes;    // X: max minutes of exclusive SD access
    int cooldownMinutes;           // Y: minutes to release SD between upload cycles
    bool enable1BitSdMode;         // Whether to use 1-bit SDIO mode instead of 4-bit
    bool stealthRestore;            // Restore card state via stealth after upload (AS10 only)
    bool minimizeReboots;           // Skip elective reboots between upload sessions
    bool flushLogsDuringUpload;      // Continue periodic log flushes during uploads (default: false)
    int smartStartHour;              // 0-23, hour when Smart mode begins monitoring (quiet period ends)
    bool smartConfigInvalid;         // true when SMART_START_HOUR is inside upload window — mode treated as SCHEDULED
    
    // Cached endpoint type flags (computed once during loadFromSD)
    bool _hasSmbEndpoint;
    bool _hasCloudEndpoint;
    bool _hasWebdavEndpoint;
    
    // Power management settings
    int cpuSpeedMhz;
    WifiTxPower wifiTxPower;
    WifiPowerSaving wifiPowerSaving;
    BrownoutDetectMode brownoutDetectMode;
    
    // Remote syslog (UDP)
    String syslogHost;           // empty = disabled
    uint16_t syslogPort;
    
    // Credential masking mode flags
    bool maskCredentials;
    bool credentialsInFlash;
    
    // Preferences object for secure credential storage
    Preferences preferences;
    
    // Preferences constants
    static const char* PREFS_NAMESPACE;
    static const char* PREFS_KEY_WIFI_PASS;       // slot 0 (legacy key, preserved for back-compat)
    static const char* PREFS_KEY_WIFI_PASS_2;     // slot 1
    static const char* PREFS_KEY_WIFI_PASS_3;     // slot 2
    static const char* PREFS_KEY_WIFI_PASS_4;     // slot 3
    static const char* PREFS_KEY_ENDPOINT_PASS;
    static const char* PREFS_KEY_CLOUD_SECRET;
    static const char* CENSORED_VALUE;

    // Returns the NVS key for the WiFi password at slot idx, or nullptr if out of range.
    static const char* prefsKeyForWifiSlot(int idx);
    
    // Preferences initialization and cleanup methods
    bool initPreferences();
    void closePreferences();
    
    // Credential storage and retrieval methods
    bool storeCredential(const char* key, const String& value);
    String loadCredential(const char* key, const String& defaultValue);
    bool isCensored(const String& value);
    
    // Config file censoring method
    bool censorConfigFile(fs::FS &sd);
    
    // Credential migration method
    bool migrateToSecureStorage(fs::FS &sd);

    // Parsing helpers
    void parseLine(String& line);
    void setConfigValue(String key, String value);
    String trimComment(String line);
    void validateAndNormalize();

public:
    Config();
    ~Config();
    
    bool loadFromSD(fs::FS &sd);
    bool loadFromString(const String& rawConfig);
    void overrideUploadMode(const String& mode);
    
    // Multi-slot accessors. idx in [0, getWifiNetworkCount()).
    int getWifiNetworkCount() const;
    const String& getWifiSSID(int idx) const;
    const String& getWifiPassword(int idx) const;

    const String& getHostname() const;
    const String& getNtpServer() const;
    const String& getSchedule() const;
    const String& getEndpoint() const;
    const String& getEndpointType() const;
    const String& getEndpointUser() const;
    const String& getEndpointPassword() const;
    int getGmtOffsetHours() const;
    const String& getTzString() const;
    const String& getTzName() const;
    bool getSaveLogs() const;
    bool getDebugMode() const;
    bool valid() const;
    bool isLoaded() const { return valid(); }
    void loadDefaults();
    
    // Cloud upload getters
    const String& getCloudClientId() const;
    const String& getCloudClientSecret() const;
    const String& getCloudTeamId() const;
    const String& getCloudBaseUrl() const;
    int getCloudDeviceId() const;
    int getMaxDays() const;
    int getRecentFolderDays() const;
    bool getCloudInsecureTls() const;
    bool getSmbPreserveTimestamps() const;
    bool hasCloudEndpoint() const;
    bool hasSmbEndpoint() const;
    bool hasWebdavEndpoint() const;
    
    // Upload FSM getters
    const String& getUploadMode() const;
    int getUploadStartHour() const;
    int getUploadEndHour() const;
    int getInactivitySeconds() const;
    int getExclusiveAccessMinutes() const;
    int getCooldownMinutes() const;
    bool getEnable1BitSdMode() const;
    bool getStealthRestore() const;
    bool getMinimizeReboots() const;
    bool getFlushLogsDuringUpload() const;
    int getSmartStartHour() const;
    bool isSmartMode() const;          // returns false when smartConfigInvalid
    bool isManualMode() const;
    bool isSmartConfigInvalid() const;
    
    // Power management getters
    int getCpuSpeedMhz() const;
    WifiTxPower getWifiTxPower() const;
    WifiPowerSaving getWifiPowerSaving() const;
    BrownoutDetectMode getBrownoutDetectMode() const;
    
    // Remote syslog getters
    const String& getSyslogHost() const;
    uint16_t getSyslogPort() const;
    
    // Credential masking mode getters
    bool isMaskingCredentials() const;
    bool areCredentialsInFlash() const;
    
private:
    // Helper methods for enum conversion
    static WifiTxPower parseWifiTxPower(const String& str);
    static WifiPowerSaving parseWifiPowerSaving(const String& str);
    static String wifiTxPowerToString(WifiTxPower power);
    static String wifiPowerSavingToString(WifiPowerSaving saving);
};

#endif // CONFIG_H
