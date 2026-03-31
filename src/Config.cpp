#include "Config.h"
#include "Logger.h"

// Define static constants for Preferences
const char* Config::PREFS_NAMESPACE = "cpap_creds";
const char* Config::PREFS_KEY_WIFI_PASS = "wifi_pass";
const char* Config::PREFS_KEY_ENDPOINT_PASS = "endpoint_pass";
const char* Config::PREFS_KEY_CLOUD_SECRET = "cloud_secret";
const char* Config::CENSORED_VALUE = "***STORED_IN_FLASH***";

Config::Config() : 
    hostname("cpap"),		// Default: cpap
    gmtOffsetHours(0),		// Default: UTC
    saveLogs(false),		// Default: do not persist logs (debugging only)
    debugMode(false),		// Default: suppress verbose pre-flight and heap stats
    isValid(false),
    
    // Cloud upload defaults
    cloudBaseUrl("https://sleephq.com"),
    cloudDeviceId(0),
    maxDays(365),		// Default: upload only last 365 days
    recentFolderDays(2),	// Default: re-check today + yesterday
    cloudInsecureTls(false),	// Default: use root CA validation
    
    // Upload FSM defaults
    uploadMode("smart"),
    uploadStartHour(9),
    uploadEndHour(21),
    inactivitySeconds(62),
    exclusiveAccessMinutes(5),
    cooldownMinutes(10),
    enable1BitSdMode(false),	// Default to safer 4-bit mode
    minimizeReboots(true),
    flushLogsDuringUpload(false), // Default: defer log flushes during uploads
    
    _hasSmbEndpoint(false),
    _hasCloudEndpoint(false),
    _hasWebdavEndpoint(false),
    
    maskCredentials(false),	// Default: plaintext — credentials stay in config.txt
    credentialsInFlash(false),	// Will be set during loadFromSD
    
    // Power management defaults (optimized for AirSense 11 compatibility)
    cpuSpeedMhz(80),					// Default: 80MHz (minimum for WiFi, saves ~30-40mA)
    wifiTxPower(WifiTxPower::POWER_MID),		// Default: 5.0dBm (typical bedroom placement, reduces peak current)
    wifiPowerSaving(WifiPowerSaving::SAVE_MID),		// Default: MIN_MODEM (preserves mDNS)
    brownoutDetectMode(BrownoutDetectMode::ENABLED)	// Default: brownout detection enabled
{}

Config::~Config() {
    closePreferences();
}

bool Config::initPreferences() {
    // Attempt to open Preferences namespace in read-write mode
    if (!preferences.begin(PREFS_NAMESPACE, false)) {
        LOG_ERROR("Failed to initialize Preferences namespace");
        LOG("Falling back to plain text credential storage");
        // Fall back to plain text mode on failure
        maskCredentials = false;
        credentialsInFlash = false;
        return false;
    }
    
    LOG_DEBUG("Preferences initialized successfully");
    LOG_DEBUGF("Using Preferences namespace: %s", PREFS_NAMESPACE);
    return true;
}

void Config::closePreferences() {
    // Close Preferences to free resources
    preferences.end();
    LOG_DEBUG("Preferences closed");
}

bool Config::storeCredential(const char* key, const String& value) {
    // Validate that the credential is not empty
    if (value.isEmpty()) {
        LOGF("WARNING: Attempted to store empty credential for key '%s'", key);
        return false;
    }
    
    // Attempt to write the credential to Preferences
    size_t written = preferences.putString(key, value);
    
    if (written == 0) {
        LOGF("ERROR: Failed to store credential '%s' in Preferences", key);
        return false;
    }
    
    LOG_DEBUGF("Credential '%s' stored successfully in Preferences (%d bytes)", key, written);
    return true;
}

String Config::loadCredential(const char* key, const String& defaultValue) {
    // Attempt to read the credential from Preferences
    String value = preferences.getString(key, defaultValue);
    
    // Check if we got the default value (key not found)
    if (value == defaultValue) {
        LOG_DEBUGF("WARNING: Credential '%s' not found in Preferences, using default", key);
    } else {
        // Validate that the retrieved credential is not empty
        if (value.isEmpty()) {
            LOG_DEBUGF("WARNING: Credential '%s' retrieved from Preferences is empty, using default", key);
            return defaultValue;
        }
        LOG_DEBUGF("Credential '%s' loaded successfully from Preferences", key);
    }
    
    return value;
}

bool Config::isCensored(const String& value) {
    // Check if the value matches the censored placeholder
    return value.equals(CENSORED_VALUE);
}

// Helper to trim whitespace from a String
static String trim(String s) {
    s.trim();
    return s;
}

// Helper to remove comments from a line
// NOTE: Only call this on complete lines BEFORE the key=value split.
// It is intentionally NOT called on values — passwords/SSIDs can contain '#'.
String Config::trimComment(String line) {
    // Handle hash style comments
    int commentPos = line.indexOf('#');
    if (commentPos != -1) {
        return line.substring(0, commentPos);
    }
    
    return line;
}

// Helper to parse a line and set config values
void Config::parseLine(String& line) {
    line = trim(line); // Trim leading/trailing whitespace

    if (line.isEmpty()) {
        return; // Skip empty lines
    }

    // Skip pure comment lines (start with '#').
    // Do NOT strip '#' from values — WiFi passwords and SSIDs can contain '#'.
    if (line.charAt(0) == '#') {
        return;
    }

    int equalsPos = line.indexOf('=');
    if (equalsPos == -1) {
        // Only warn if line is not empty and doesn't look like a section header [SECTION]
        if (line.length() > 0 && line.charAt(0) != '[') {
            LOGF("WARN: Config line '%s' has no '='. Skipping.", line.c_str());
        }
        return;
    }

    String key = trim(line.substring(0, equalsPos));
    String value = trim(line.substring(equalsPos + 1));
    
    // Remove optional quotes around value
    if (value.length() >= 2) {
        if ((value.charAt(0) == '"' && value.charAt(value.length()-1) == '"') ||
            (value.charAt(0) == '\'' && value.charAt(value.length()-1) == '\'')) {
            value = value.substring(1, value.length()-1);
        }
    }

    setConfigValue(key, value);
}

// Helper to set config values based on key (case-insensitive)
void Config::setConfigValue(String key, String value) {
    key.toUpperCase(); // Convert key to uppercase for case-insensitive comparison

    if (key == "WIFI_SSID") {
        wifiSSID = value;
    } else if (key == "WIFI_PASSWORD") {
        wifiPassword = value;
    } else if (key == "HOSTNAME") {
        hostname = value.substring(0, 32);  //limit to 32
    } else if (key == "SCHEDULE") {
        schedule = value;
    } else if (key == "ENDPOINT") {
        endpoint = value;
    } else if (key == "ENDPOINT_TYPE") {
        endpointType = value;
    } else if (key == "ENDPOINT_USER") {
        endpointUser = value;
    } else if (key == "ENDPOINT_PASSWORD") {
        endpointPassword = value;
    } else if (key == "GMT_OFFSET_HOURS") {
        gmtOffsetHours = value.toInt();
    } else if (key == "PERSISTENT_LOGS") {
        saveLogs = (value.equalsIgnoreCase("true") || value.toInt() == 1);
    } else if (key == "DEBUG") {
        debugMode = (value.equalsIgnoreCase("true") || value.toInt() == 1);
    } else if (key == "CLOUD_CLIENT_ID") {
        cloudClientId = value;
    } else if (key == "CLOUD_CLIENT_SECRET") {
        cloudClientSecret = value;
    } else if (key == "CLOUD_TEAM_ID") {
        cloudTeamId = value;
    } else if (key == "CLOUD_BASE_URL") {
        cloudBaseUrl = value;
    } else if (key == "CLOUD_DEVICE_ID") {
        cloudDeviceId = value.toInt();
    } else if (key == "MAX_DAYS") {
        maxDays = value.toInt();
    } else if (key == "RECENT_FOLDER_DAYS") {
        recentFolderDays = value.toInt();
    } else if (key == "CLOUD_INSECURE_TLS") {
        cloudInsecureTls = (value.equalsIgnoreCase("true") || value.toInt() == 1);
    } else if (key == "UPLOAD_MODE") {
        uploadMode = value;
    } else if (key == "UPLOAD_START_HOUR") {
        uploadStartHour = value.toInt();
    } else if (key == "UPLOAD_END_HOUR") {
        uploadEndHour = value.toInt();
    } else if (key == "INACTIVITY_SECONDS") {
        inactivitySeconds = value.toInt();
    } else if (key == "EXCLUSIVE_ACCESS_MINUTES") {
        exclusiveAccessMinutes = value.toInt();
    } else if (key == "COOLDOWN_MINUTES") {
        cooldownMinutes = value.toInt();
    } else if (key == "ENABLE_1BIT_SD_MODE") {
        enable1BitSdMode = (value.equalsIgnoreCase("true") || value == "1");
    } else if (key == "CPU_SPEED_MHZ") {
        cpuSpeedMhz = value.toInt();
    } else if (key == "WIFI_TX_PWR") {
        wifiTxPower = parseWifiTxPower(value);
    } else if (key == "WIFI_PWR_SAVING") {
        wifiPowerSaving = parseWifiPowerSaving(value);
    } else if (key == "MASK_CREDENTIALS") {
        maskCredentials = (value.equalsIgnoreCase("true") || value.toInt() == 1);
    } else if (key == "MINIMIZE_REBOOTS") {
        minimizeReboots = (value.equalsIgnoreCase("true") || value.toInt() == 1);
    } else if (key == "FLUSH_LOGS_DURING_UPLOAD") {
        flushLogsDuringUpload = (value.equalsIgnoreCase("true") || value.toInt() == 1);
    } else if (key == "BROWNOUT_DETECT") {
        if (value.equalsIgnoreCase("off")) {
            brownoutDetectMode = BrownoutDetectMode::OFF;
        } else if (value.equalsIgnoreCase("relaxed")) {
            brownoutDetectMode = BrownoutDetectMode::RELAXED;
        } else {
            brownoutDetectMode = BrownoutDetectMode::ENABLED;
        }
    } else {
        LOG_WARNF("Unknown config key: %s", key.c_str());
    }
}

bool Config::censorConfigFile(fs::FS &sd) {
    LOG_DEBUG("Starting config file censoring operation");
    
    // Use a temporary file to write the censored version
    String tempPath = "/config.tmp";
    String configPath = "/config.txt";
    
    File configFile = sd.open(configPath, FILE_READ);
    if (!configFile) {
        LOG_ERROR("Cannot open config.txt for reading during censoring");
        return false;
    }
    
    File tempFile = sd.open(tempPath, FILE_WRITE);
    if (!tempFile) {
        LOG_ERROR("Cannot open temp file for writing during censoring");
        configFile.close();
        return false;
    }
    
    bool errorOccurred = false;
    
    while (configFile.available()) {
        String line = configFile.readStringUntil('\n');
        // readStringUntil strips the delimiter, but we need to handle CR if present
        line.trim(); 
        
        String trimmedLine = trimComment(line);
        trimmedLine.trim();
        
        // Check if this line contains a secret key
        bool isSecret = false;
        int equalsPos = trimmedLine.indexOf('=');
        
        if (equalsPos != -1 && trimmedLine.length() > 0 && trimmedLine.charAt(0) != '#' && trimmedLine.substring(0, 2) != "//") {
            String key = trimmedLine.substring(0, equalsPos);
            key.trim();
            key.toUpperCase();
            
            if (key == "WIFI_PASSWORD" || key == "ENDPOINT_PASSWORD" || key == "CLOUD_CLIENT_SECRET") {
                // It's a secret, reconstruct the line with censored value
                String originalKey = line.substring(0, line.indexOf('=')); // Keep original casing/spacing
                tempFile.println(originalKey + " = " + String(CENSORED_VALUE));
                isSecret = true;
            }
        }
        
        if (!isSecret) {
            // Write original line
            tempFile.println(line);
        }
    }
    
    configFile.close();
    tempFile.close();
    
    if (errorOccurred) {
        sd.remove(tempPath);
        return false;
    }
    
    // Replace original file with temp file
    sd.remove(configPath);
    if (!sd.rename(tempPath, configPath)) {
        LOG_ERROR("Failed to replace config.txt with censored version");
        return false;
    }
    
    LOG_DEBUG("Config file censored successfully");
    LOG_DEBUG("Credentials are now stored securely in flash memory");
    
    return true;
}

bool Config::migrateToSecureStorage(fs::FS &sd) {
    LOG("========================================");
    LOG("Starting credential migration to secure storage");
    LOG("========================================");
    
    // Step 1: Validate that credentials are not empty
    if (wifiPassword.isEmpty() && endpointPassword.isEmpty() && cloudClientSecret.isEmpty()) {
        LOG_WARN("All credentials are empty, skipping migration");
        return false;
    }
    
    // Step 2: Store credentials in Preferences
    bool success = true;
    
    if (!wifiPassword.isEmpty() && !isCensored(wifiPassword)) {
        LOG_DEBUG("Storing WiFi password in Preferences...");
        if (!storeCredential(PREFS_KEY_WIFI_PASS, wifiPassword)) success = false;
    }
    
    if (!endpointPassword.isEmpty() && !isCensored(endpointPassword)) {
        LOG_DEBUG("Storing endpoint password in Preferences...");
        if (!storeCredential(PREFS_KEY_ENDPOINT_PASS, endpointPassword)) success = false;
    }
    
    if (!cloudClientSecret.isEmpty() && !isCensored(cloudClientSecret)) {
        LOG_DEBUG("Storing cloud client secret in Preferences...");
        if (!storeCredential(PREFS_KEY_CLOUD_SECRET, cloudClientSecret)) success = false;
    }
    
    if (!success) {
        LOG_ERROR("Failed to store some credentials");
        LOG("Migration aborted - keeping plain text credentials");
        return false;
    }
    
    // Step 3: Verify credentials
    LOG_DEBUG("Verifying stored credentials...");
    if (!wifiPassword.isEmpty() && !isCensored(wifiPassword)) {
        if (loadCredential(PREFS_KEY_WIFI_PASS, "") != wifiPassword) success = false;
    }
    if (!endpointPassword.isEmpty() && !isCensored(endpointPassword)) {
        if (loadCredential(PREFS_KEY_ENDPOINT_PASS, "") != endpointPassword) success = false;
    }
    if (!cloudClientSecret.isEmpty() && !isCensored(cloudClientSecret)) {
        if (loadCredential(PREFS_KEY_CLOUD_SECRET, "") != cloudClientSecret) success = false;
    }
    
    if (!success) {
        LOG_ERROR("Credential verification failed");
        return false;
    }
    
    // Step 4: Censor config.txt
    if (!censorConfigFile(sd)) {
        LOG_ERROR("Failed to censor config.txt");
        return false;
    }
    
    LOG("========================================");
    LOG("Credential migration completed successfully");
    LOG("Credentials are now stored securely in flash memory");
    LOG("config.txt has been updated with censored values");
    LOG("========================================");
    
    return true;
}

// Main function to load configuration from SD card
bool Config::loadFromSD(fs::FS &sd) {
    LOG("========================================");
    LOG("Loading configuration from SD card");
    LOG("========================================");
    
    // Step 1: Read and parse config.txt
    File configFile = sd.open("/config.txt", FILE_READ);
    if (!configFile) {
        LOG_ERROR("Failed to open config.txt");
        return false;
    }

    // Reset members to defaults before loading
    // (Constructor already set defaults, but good practice if called multiple times)
    
    while (configFile.available()) {
        String line = configFile.readStringUntil('\n');
        parseLine(line);
    }
    configFile.close();

    LOG("Config file parsed successfully");
    
    // Step 2: Handle secure storage logic
    
    if (!maskCredentials) {
        LOG_DEBUG("========================================");
        LOG_DEBUG("PLAIN TEXT MODE: Credentials stored in config.txt");
        LOG_DEBUG("========================================");
        credentialsInFlash = false;
        
        // Detect leftover censored placeholders from a previous MASK_CREDENTIALS=true session.
        // This happens when a user upgrades firmware or disables masking after credentials
        // were already migrated to NVS. Log a loud error so the user knows to re-enter them.
        bool anyCensored = false;
        if (isCensored(wifiPassword)) {
            LOG_ERROR("WiFi password is '***STORED_IN_FLASH***' but MASK_CREDENTIALS is off.");
            LOG_ERROR("NVS data may be lost after a full (non-OTA) flash. Please re-enter your WiFi password in config.txt.");
            anyCensored = true;
        }
        if (isCensored(endpointPassword)) {
            LOG_ERROR("Endpoint password is '***STORED_IN_FLASH***' but MASK_CREDENTIALS is off.");
            LOG_ERROR("Please re-enter your endpoint password in config.txt.");
            anyCensored = true;
        }
        if (isCensored(cloudClientSecret)) {
            LOG_ERROR("Cloud client secret is '***STORED_IN_FLASH***' but MASK_CREDENTIALS is off.");
            LOG_ERROR("Please re-enter your cloud client secret in config.txt.");
            anyCensored = true;
        }
        if (anyCensored) {
            LOG_ERROR("========================================");
            LOG_ERROR("ACTION REQUIRED: One or more credentials contain the placeholder");
            LOG_ERROR("'***STORED_IN_FLASH***'. These cannot be loaded because MASK_CREDENTIALS");
            LOG_ERROR("is off (or was never set). Edit config.txt on the SD card and replace");
            LOG_ERROR("the placeholder with your actual password/secret.");
            LOG_ERROR("========================================");
        }
    } else {
        LOG_DEBUG("========================================");
        LOG_DEBUG("MASK MODE: Credentials will be stored in flash memory (NVS)");
        LOG_DEBUG("========================================");
        
        // Initialize Preferences
        if (!initPreferences()) {
            LOG_ERROR("Failed to initialize Preferences");
            LOG("Falling back to plain text mode for this session");
            maskCredentials = false;
            credentialsInFlash = false;
        } else {
            // Check loaded credentials for censorship
            bool wifiCensored = isCensored(wifiPassword);
            bool endpointCensored = isCensored(endpointPassword);
            bool cloudSecretCensored = isCensored(cloudClientSecret);
            
            // If censored, load from preferences
            if (wifiCensored) {
                wifiPassword = loadCredential(PREFS_KEY_WIFI_PASS, "");
                LOG("Loaded WiFi password from flash");
            }
            if (endpointCensored) {
                endpointPassword = loadCredential(PREFS_KEY_ENDPOINT_PASS, "");
                LOG("Loaded endpoint password from flash");
            }
            if (cloudSecretCensored) {
                cloudClientSecret = loadCredential(PREFS_KEY_CLOUD_SECRET, "");
                LOG("Loaded cloud client secret from flash");
            }
            
            credentialsInFlash = (wifiCensored || endpointCensored || cloudSecretCensored);
            
            // Check for migration needed (plaintext credentials present in mask mode)
            bool needsMigration = false;
            if (!wifiPassword.isEmpty() && !wifiCensored) needsMigration = true;
            if (!endpointPassword.isEmpty() && !endpointCensored) needsMigration = true;
            if (!cloudClientSecret.isEmpty() && !cloudSecretCensored) needsMigration = true;
            
            if (needsMigration) {
                LOG("New plain text credentials detected in mask mode - attempting migration");
                if (migrateToSecureStorage(sd)) {
                    credentialsInFlash = true;
                }
            }
        }
    }
    
    // Step 3: Validate configuration
    
    // Validate SSID length (WiFi standard limit is 32 characters)
    if (wifiSSID.length() > 32) {
        LOG_ERROR("SSID exceeds maximum length of 32 characters");
        LOGF("SSID length: %d characters", wifiSSID.length());
        LOG("Truncating SSID to 32 characters");
        wifiSSID = wifiSSID.substring(0, 32);
    }
    
    // Compute cached endpoint type flags from the (possibly comma-separated) endpointType string
    {
        String upper = endpointType;
        upper.toUpperCase();
        _hasSmbEndpoint = (upper.indexOf("SMB") >= 0);
        _hasCloudEndpoint = (upper.indexOf("CLOUD") >= 0 || upper.indexOf("SLEEPHQ") >= 0);
        _hasWebdavEndpoint = (upper.indexOf("WEBDAV") >= 0);
    }
    
    bool hasValidEndpoint = false;
    if (hasSmbEndpoint()) {
        bool smbValid = !endpoint.isEmpty();
        if (!smbValid) {
            LOG_WARN("SMB endpoint configured but ENDPOINT is empty - SMB backend will be disabled for this run");
            _hasSmbEndpoint = false;
        } else {
            hasValidEndpoint = true;
        }
    }
    if (hasWebdavEndpoint()) {
        bool webdavValid = !endpoint.isEmpty();
        if (!webdavValid) {
            LOG_WARN("WEBDAV endpoint configured but ENDPOINT is empty - WEBDAV backend will be disabled for this run");
            _hasWebdavEndpoint = false;
        } else {
            hasValidEndpoint = true;
        }
    }
    if (hasCloudEndpoint()) {
        bool cloudValid = !cloudClientId.isEmpty();
        if (!cloudValid) {
            LOG_ERROR("CLOUD endpoint configured but CLOUD_CLIENT_ID is empty");
        }
        hasValidEndpoint = hasValidEndpoint || cloudValid;
    }
    if (!hasSmbEndpoint() && !hasCloudEndpoint() && !hasWebdavEndpoint()) {
        if (endpointType.isEmpty() && !endpoint.isEmpty()) {
            // Legacy: default to SMB when ENDPOINT is set but ENDPOINT_TYPE is empty
            LOG_WARN("ENDPOINT_TYPE not set, defaulting to SMB for backward compatibility");
            endpointType = "SMB";
            _hasSmbEndpoint = true;
            hasValidEndpoint = true;
        } else if (!endpointType.isEmpty()) {
            // Non-empty type that isn't SMB, CLOUD, or WEBDAV
            hasValidEndpoint = !endpoint.isEmpty();
        }
    }
    
    // Validation of numeric ranges
    if (maxDays <= 0) { maxDays = 365; }
    else if (maxDays > 366) { maxDays = 366; }
    
    if (recentFolderDays < 0) { recentFolderDays = 2; }
    
    if (uploadMode != "scheduled" && uploadMode != "smart") { uploadMode = "smart"; }
    
    if (uploadStartHour < 0 || uploadStartHour > 23) { uploadStartHour = 9; }
    if (uploadEndHour < 0 || uploadEndHour > 23) { uploadEndHour = 21; }
    
    if (inactivitySeconds < 10) { inactivitySeconds = 10; }
    else if (inactivitySeconds > 3600) { inactivitySeconds = 3600; }
    
    if (exclusiveAccessMinutes < 1) { exclusiveAccessMinutes = 1; }
    else if (exclusiveAccessMinutes > 30) { exclusiveAccessMinutes = 30; }
    
    if (cooldownMinutes < 1) { cooldownMinutes = 1; }
    else if (cooldownMinutes > 60) { cooldownMinutes = 60; }
    
    if (cpuSpeedMhz < 80) { cpuSpeedMhz = 80; }
    else if (cpuSpeedMhz > 240) { cpuSpeedMhz = 240; }
    
    isValid = !wifiSSID.isEmpty() && hasValidEndpoint;
    
    if (isValid) {
        LOG("========================================");
        LOG("Configuration loaded successfully");
        LOGF("Endpoint type: %s", endpointType.c_str());
        LOGF("Backends active this run: SMB=%s CLOUD=%s WEBDAV=%s",
             hasSmbEndpoint() ? "YES" : "NO",
             hasCloudEndpoint() ? "YES" : "NO",
             hasWebdavEndpoint() ? "YES" : "NO");
        LOG_DEBUGF("Storage mode: %s", maskCredentials ? "MASKED (NVS)" : "PLAIN TEXT");
        LOG_DEBUGF("Credentials in flash: %s", credentialsInFlash ? "YES" : "NO");
        // ... (logging continues)
        LOG("========================================");
    } else {
        LOG_ERROR("Configuration validation failed");
        LOG("Check WIFI_SSID and ENDPOINT/CLOUD_CLIENT_ID settings");
    }
    
    return isValid;
}

const String& Config::getWifiSSID() const { return wifiSSID; }
const String& Config::getWifiPassword() const { return wifiPassword; }
const String& Config::getHostname() const { return hostname; }
const String& Config::getSchedule() const { return schedule; }
const String& Config::getEndpoint() const { return endpoint; }
const String& Config::getEndpointType() const { return endpointType; }
const String& Config::getEndpointUser() const { return endpointUser; }
const String& Config::getEndpointPassword() const { return endpointPassword; }
int Config::getGmtOffsetHours() const { return gmtOffsetHours; }
bool Config::getSaveLogs() const { return saveLogs; }
bool Config::getDebugMode() const { return debugMode; }
bool Config::valid() const { return isValid; }

// Credential storage mode getters
bool Config::isMaskingCredentials() const { return maskCredentials; }
bool Config::areCredentialsInFlash() const { return credentialsInFlash; }

// Cloud upload getters
const String& Config::getCloudClientId() const { return cloudClientId; }
const String& Config::getCloudClientSecret() const { return cloudClientSecret; }
const String& Config::getCloudTeamId() const { return cloudTeamId; }
const String& Config::getCloudBaseUrl() const { return cloudBaseUrl; }
int Config::getCloudDeviceId() const { return cloudDeviceId; }
int Config::getMaxDays() const { return maxDays; }
int Config::getRecentFolderDays() const { return recentFolderDays; }
bool Config::getCloudInsecureTls() const { return cloudInsecureTls; }

bool Config::hasCloudEndpoint() const { return _hasCloudEndpoint; }
bool Config::hasSmbEndpoint() const { return _hasSmbEndpoint; }
bool Config::hasWebdavEndpoint() const { return _hasWebdavEndpoint; }

// Power management getters
int Config::getCpuSpeedMhz() const { return cpuSpeedMhz; }
WifiTxPower Config::getWifiTxPower() const { return wifiTxPower; }
WifiPowerSaving Config::getWifiPowerSaving() const { return wifiPowerSaving; }
BrownoutDetectMode Config::getBrownoutDetectMode() const { return brownoutDetectMode; }

// Upload FSM getters
const String& Config::getUploadMode() const { return uploadMode; }
int Config::getUploadStartHour() const { return uploadStartHour; }
int Config::getUploadEndHour() const { return uploadEndHour; }
int Config::getInactivitySeconds() const { return inactivitySeconds; }
int Config::getExclusiveAccessMinutes() const { return exclusiveAccessMinutes; }
int Config::getCooldownMinutes() const { return cooldownMinutes; }
bool Config::getEnable1BitSdMode() const { return enable1BitSdMode; }
bool Config::getMinimizeReboots() const { return minimizeReboots; }
bool Config::getFlushLogsDuringUpload() const { return flushLogsDuringUpload; }
bool Config::isSmartMode() const { return uploadMode == "smart"; }

// Helper methods for enum conversion
WifiTxPower Config::parseWifiTxPower(const String& str) {
    String s = str;
    s.toUpperCase();
    if (s == "MAX") return WifiTxPower::POWER_MAX;
    if (s == "HIGH") return WifiTxPower::POWER_HIGH;
    if (s == "MID") return WifiTxPower::POWER_MID;
    if (s == "LOW") return WifiTxPower::POWER_LOW;
    if (s == "LOWEST") return WifiTxPower::POWER_LOWEST;
    return WifiTxPower::POWER_MID; // Default: 5 dBm
}

WifiPowerSaving Config::parseWifiPowerSaving(const String& str) {
    String s = str;
    s.toUpperCase();
    if (s == "NONE") return WifiPowerSaving::SAVE_NONE;
    if (s == "MID") return WifiPowerSaving::SAVE_MID;
    if (s == "MAX") return WifiPowerSaving::SAVE_MAX;
    return WifiPowerSaving::SAVE_MID; // Default: MIN_MODEM
}

String Config::wifiTxPowerToString(WifiTxPower power) {
    switch (power) {
        case WifiTxPower::POWER_MAX: return "MAX";
        case WifiTxPower::POWER_HIGH: return "HIGH";
        case WifiTxPower::POWER_MID: return "MID";
        case WifiTxPower::POWER_LOW: return "LOW";
        case WifiTxPower::POWER_LOWEST: return "LOWEST";
        default: return "UNKNOWN";
    }
}

String Config::wifiPowerSavingToString(WifiPowerSaving saving) {
    switch (saving) {
        case WifiPowerSaving::SAVE_NONE: return "NONE";
        case WifiPowerSaving::SAVE_MID: return "MID";
        case WifiPowerSaving::SAVE_MAX: return "MAX";
        default: return "UNKNOWN";
    }
}
