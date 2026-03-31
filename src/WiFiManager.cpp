#include "WiFiManager.h"
#include "Logger.h"
#include "Config.h"  // For power management enums
#include "SDCardManager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>

volatile uint8_t WiFiManager::_lastDisconnectReason = 0;

WiFiManager::WiFiManager() : connected(false), mdnsStarted(false), _pendingTxPower(0), _hasPendingTxPower(false) {}

void WiFiManager::setupEventHandlers() {
    WiFi.onEvent(onWiFiEvent);
    LOG_DEBUG("WiFi event handlers registered");
}

void WiFiManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_READY:
            LOG_DEBUG("WiFi Event: WiFi interface ready");
            break;
            
        case ARDUINO_EVENT_WIFI_SCAN_DONE:
            LOG_DEBUG("WiFi Event: Scan completed");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_START:
            LOG_INFO("WiFi Event: Station mode started");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_STOP:
            LOG_INFO("WiFi Event: Station mode stopped");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            LOG_INFO("WiFi Event: Connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint8_t reason = info.wifi_sta_disconnected.reason;
            _lastDisconnectReason = reason;
            LOG_WARNF("WiFi Event: Disconnected from AP (reason: %d)", reason);
            
            // Log human-readable disconnect reasons
            switch (reason) {
                case WIFI_REASON_UNSPECIFIED:
                    LOG_WARN("Disconnect reason: Unspecified");
                    break;
                case WIFI_REASON_AUTH_EXPIRE:
                    LOG_WARN("Disconnect reason: Authentication expired");
                    break;
                case WIFI_REASON_AUTH_LEAVE:
                    LOG_WARN("Disconnect reason: Deauthenticated (left network)");
                    break;
                case WIFI_REASON_ASSOC_EXPIRE:
                    LOG_WARN("Disconnect reason: Association expired");
                    break;
                case WIFI_REASON_ASSOC_TOOMANY:
                    LOG_WARN("Disconnect reason: Too many associations");
                    break;
                case WIFI_REASON_NOT_AUTHED:
                    LOG_WARN("Disconnect reason: Not authenticated");
                    break;
                case WIFI_REASON_NOT_ASSOCED:
                    LOG_WARN("Disconnect reason: Not associated");
                    break;
                case WIFI_REASON_ASSOC_LEAVE:
                    LOG_WARN("Disconnect reason: Disassociated (left network)");
                    break;
                case WIFI_REASON_ASSOC_NOT_AUTHED:
                    LOG_WARN("Disconnect reason: Association not authenticated");
                    break;
                case WIFI_REASON_DISASSOC_PWRCAP_BAD:
                    LOG_WARN("Disconnect reason: Bad power capability");
                    break;
                case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
                    LOG_WARN("Disconnect reason: Bad supported channels");
                    break;
                case WIFI_REASON_IE_INVALID:
                    LOG_WARN("Disconnect reason: Invalid information element");
                    break;
                case WIFI_REASON_MIC_FAILURE:
                    LOG_WARN("Disconnect reason: MIC failure");
                    break;
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                    LOG_WARN("Disconnect reason: 4-way handshake timeout");
                    break;
                case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
                    LOG_WARN("Disconnect reason: Group key update timeout");
                    break;
                case WIFI_REASON_IE_IN_4WAY_DIFFERS:
                    LOG_WARN("Disconnect reason: IE in 4-way handshake differs");
                    break;
                case WIFI_REASON_GROUP_CIPHER_INVALID:
                    LOG_WARN("Disconnect reason: Invalid group cipher");
                    break;
                case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
                    LOG_WARN("Disconnect reason: Invalid pairwise cipher");
                    break;
                case WIFI_REASON_AKMP_INVALID:
                    LOG_WARN("Disconnect reason: Invalid AKMP");
                    break;
                case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
                    LOG_WARN("Disconnect reason: Unsupported RSN IE version");
                    break;
                case WIFI_REASON_INVALID_RSN_IE_CAP:
                    LOG_WARN("Disconnect reason: Invalid RSN IE capability");
                    break;
                case WIFI_REASON_802_1X_AUTH_FAILED:
                    LOG_WARN("Disconnect reason: 802.1X authentication failed");
                    break;
                case WIFI_REASON_CIPHER_SUITE_REJECTED:
                    LOG_WARN("Disconnect reason: Cipher suite rejected");
                    break;
                case WIFI_REASON_BEACON_TIMEOUT:
                    LOG_WARN("Disconnect reason: Beacon timeout (AP lost)");
                    break;
                case WIFI_REASON_NO_AP_FOUND:
                    LOG_WARN("Disconnect reason: No AP found");
                    break;
                case WIFI_REASON_AUTH_FAIL:
                    LOG_WARN("Disconnect reason: Authentication failed");
                    break;
                case WIFI_REASON_ASSOC_FAIL:
                    LOG_WARN("Disconnect reason: Association failed");
                    break;
                case WIFI_REASON_HANDSHAKE_TIMEOUT:
                    LOG_WARN("Disconnect reason: Handshake timeout");
                    break;
                case WIFI_REASON_CONNECTION_FAIL:
                    LOG_WARN("Disconnect reason: Connection failed");
                    break;
                case WIFI_REASON_AP_TSF_RESET:
                    LOG_WARN("Disconnect reason: AP TSF reset");
                    break;
                case WIFI_REASON_ROAMING:
                    LOG_WARN("Disconnect reason: Roaming");
                    break;
                case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
                    LOG_WARN("Disconnect reason: Association comeback time too long (PMF/802.11w)");
                    break;
                case WIFI_REASON_SA_QUERY_TIMEOUT:
                    LOG_WARN("Disconnect reason: SA query timeout (PMF/802.11w)");
                    break;
                default:
                    LOG_WARNF("Disconnect reason: Unknown (%d)", reason);
                    break;
            }
            break;
        }
            
        case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
            LOG_DEBUG("WiFi Event: Authentication mode changed");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            LOG_INFOF("WiFi Event: Got IP address: %s", WiFi.localIP().toString().c_str());
            break;
            
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            LOG_WARN("WiFi Event: Lost IP address");
            break;
            
        default:
            LOG_DEBUGF("WiFi Event: Unhandled event %d", event);
            break;
    }
}

bool WiFiManager::connectStation(const String& ssid, const String& password, const String& hostname) {
    // Validate SSID before attempting connection
    if (ssid.isEmpty()) {
        LOG_ERROR("Cannot connect to WiFi: SSID is empty");
        Logger::getInstance().dumpSavedLogs("wifi_config_error");
        return false;
    }
    
    if (ssid.length() > 32) {
        LOG_ERROR("Cannot connect to WiFi: SSID exceeds 32 character limit");
        LOGF("SSID length: %d characters", ssid.length());
        Logger::getInstance().dumpSavedLogs("wifi_config_error");
        return false;
    }

    WiFi.setHostname(hostname.c_str());
    
    if (password.isEmpty()) {
        LOG_WARN("WiFi password is empty - attempting open network connection");
    }
    
    LOGF("Connecting to WiFi: %s", ssid.c_str());
    LOGF("SSID length: %d characters", ssid.length());

    WiFi.mode(WIFI_STA);
    
    // ── Power optimization: disable 802.11b (DSSS) ──
    // 802.11b uses up to 370 mA peak TX. Restricting to 802.11g/n (OFDM)
    // caps peak TX current to ~205-250 mA. All modern routers support g/n.
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    LOG_DEBUG("WiFi protocol restricted to 802.11g/n (802.11b disabled)");
    
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Apply deferred TX power AFTER WiFi.begin() — WiFi.mode(WIFI_STA) is async
    // and the STA isn't fully started until the STA_START event fires. begin()
    // blocks until STA is active, so setTxPower() works reliably here.
    // This caps TX power during the association/DHCP phase.
    if (_hasPendingTxPower) {
        WiFi.setTxPower((wifi_power_t)_pendingTxPower);
        LOG_DEBUGF("WiFi TX power applied: %d (deferred from applyTxPowerEarly)", (int)_pendingTxPower);
        _hasPendingTxPower = false;
    }

    _lastDisconnectReason = 0;
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        LOG_DEBUG(".");
        attempts++;
    }

    // ── PMF fallback: reason 208 (ASSOC_COMEBACK_TIME_TOO_LONG) ──
    // ESP-IDF 5.x sets PMF (Protected Management Frames / 802.11w) capable=true
    // by default. Some WiFi 6 / WPA3-transitional routers send an association
    // comeback time that exceeds the ESP-IDF threshold, causing reason 208.
    // This didn't exist in ESP-IDF 4.x. Retry with PMF disabled.
    if (WiFi.status() != WL_CONNECTED &&
        _lastDisconnectReason == WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG) {
        LOG_WARN("PMF association comeback timeout (reason 208) — retrying with PMF disabled");
        wifi_config_t wifi_conf;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_conf) == ESP_OK) {
            wifi_conf.sta.pmf_cfg.capable = false;
            wifi_conf.sta.pmf_cfg.required = false;
            esp_wifi_set_config(WIFI_IF_STA, &wifi_conf);
            esp_wifi_disconnect();
            esp_wifi_connect();
            attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 30) {
                delay(500);
                LOG_DEBUG(".");
                attempts++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                LOG_WARN("Connected after disabling PMF — router may not fully support 802.11w");
            }
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        LOG("\nWiFi connected");
        LOGF("IP address: %s", WiFi.localIP().toString().c_str());
        connected = true;
        return true;
    } else {
        LOG("\nWiFi connection failed");
        LOGF("WiFi status: %d", WiFi.status());
        
        // Log detailed failure reason
        switch (WiFi.status()) {
            case WL_NO_SSID_AVAIL:
                LOG_ERROR("WiFi failure: SSID not found");
                break;
            case WL_CONNECT_FAILED:
                LOG_ERROR("WiFi failure: Connection failed (wrong password?)");
                break;
            case WL_CONNECTION_LOST:
                LOG_ERROR("WiFi failure: Connection lost");
                break;
            case WL_DISCONNECTED:
                LOG_ERROR("WiFi failure: Disconnected");
                break;
            default:
                LOGF("WiFi failure: Unknown status %d", WiFi.status());
                break;
        }
        
        // Persist logs for critical connection failures
        Logger::getInstance().dumpSavedLogs("wifi_connection_failed");
        connected = false;
        return false;
    }
}

bool WiFiManager::isConnected() const { 
    return connected && WiFi.status() == WL_CONNECTED; 
}

void WiFiManager::disconnect() {
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
    }
    WiFi.disconnect();
    connected = false;
}

String WiFiManager::getIPAddress() const {
    if (connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "Not connected";
}

int WiFiManager::getSignalStrength() const {
    if (connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();  // Returns signal strength in dBm
    }
    return 0;
}

String WiFiManager::getSignalQuality() const {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        return "Not connected";
    }
    
    int rssi = WiFi.RSSI();
    
    // Classify signal strength based on RSSI value
    // RSSI ranges: Excellent > -50, Good > -60, Fair > -70, Weak > -80, Very Weak <= -80
    if (rssi > -50) {
        return "Excellent";
    } else if (rssi > -60) {
        return "Good";
    } else if (rssi > -70) {
        return "Fair";
    } else if (rssi > -80) {
        return "Weak";
    } else {
        return "Very Weak";
    }
}

bool WiFiManager::startMDNS(const String& hostname) {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        LOG_WARN("Cannot start mDNS: WiFi not connected");
        return false;
    }

    String name = hostname;

    LOGF("Starting mDNS responder with hostname: %s.local", name.c_str());

    // Ensure stale responder state from prior reconnects is released first.
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
        delay(10);
    }
    
    if (MDNS.begin(name.c_str())) {
        LOG("mDNS responder started successfully");
        // Advertise web server service
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;
        return true;
    } else {
        LOG_ERROR("Failed to start mDNS responder");
        mdnsStarted = false;
        return false;
    }
}

// Power management methods
void WiFiManager::setHighPerformanceMode() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);  // Disable all power saving
        LOG_DEBUG("WiFi set to high performance mode (no power saving)");
    }
}

void WiFiManager::setPowerSaveMode() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MIN_MODEM);  // Enable minimum modem sleep
        LOG_DEBUG("WiFi set to power save mode (WIFI_PS_MIN_MODEM)");
    }
}

void WiFiManager::setMaxPowerSave() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MAX_MODEM);  // Enable maximum modem sleep
        LOG_DEBUG("WiFi set to maximum power save mode (WIFI_PS_MAX_MODEM)");
    }
}
void WiFiManager::applyTxPowerEarly(WifiTxPower txPower) {
    // Store the desired TX power for deferred application inside connectStation().
    // We cannot call WiFi.setTxPower() here because WiFi.mode(WIFI_STA) hasn't
    // been called yet (or hasn't fully started), which causes the warning:
    //   "Neither AP or STA has been started"
    // connectStation() applies it right after WiFi.mode(WIFI_STA).
    wifi_power_t espTxPower;
    switch (txPower) {
        case WifiTxPower::POWER_MAX:    espTxPower = WIFI_POWER_11dBm;       break; // PHY caps at 10 dBm
        case WifiTxPower::POWER_HIGH:   espTxPower = WIFI_POWER_8_5dBm;      break;
        case WifiTxPower::POWER_MID:    espTxPower = WIFI_POWER_5dBm;        break;
        case WifiTxPower::POWER_LOW:    espTxPower = WIFI_POWER_2dBm;        break;
        case WifiTxPower::POWER_LOWEST: espTxPower = WIFI_POWER_MINUS_1dBm;  break;
        default:                        espTxPower = WIFI_POWER_5dBm;        break;
    }
    _pendingTxPower = (int8_t)espTxPower;
    _hasPendingTxPower = true;
    LOG_DEBUGF("WiFi TX power deferred: %d (will apply in connectStation)", (int)espTxPower);
}

void WiFiManager::applyPowerSettings(WifiTxPower txPower, WifiPowerSaving powerSaving) {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        LOG_WARN("Cannot apply power settings - WiFi not connected");
        return;
    }
    
    // Apply TX power setting
    wifi_power_t espTxPower;
    switch (txPower) {
        case WifiTxPower::POWER_MAX:
            espTxPower = WIFI_POWER_11dBm;       // PHY caps at 10 dBm
            LOG_DEBUG("WiFi TX power set to MAX (10dBm, PHY-capped)");
            break;
        case WifiTxPower::POWER_HIGH:
            espTxPower = WIFI_POWER_8_5dBm;      // 8.5dBm
            LOG_DEBUG("WiFi TX power set to HIGH (8.5dBm)");
            break;
        case WifiTxPower::POWER_MID:
            espTxPower = WIFI_POWER_5dBm;        // 5dBm (default)
            LOG_DEBUG("WiFi TX power set to MID (5dBm)");
            break;
        case WifiTxPower::POWER_LOW:
            espTxPower = WIFI_POWER_2dBm;        // 2dBm
            LOG_DEBUG("WiFi TX power set to LOW (2dBm)");
            break;
        case WifiTxPower::POWER_LOWEST:
            espTxPower = WIFI_POWER_MINUS_1dBm;  // -1dBm
            LOG_DEBUG("WiFi TX power set to LOWEST (-1dBm)");
            break;
        default:
            espTxPower = WIFI_POWER_5dBm;
            LOG_WARN("Unknown TX power setting, using MID (5dBm)");
            break;
    }
    WiFi.setTxPower(espTxPower);
    
    // Apply power saving setting
    switch (powerSaving) {
        case WifiPowerSaving::SAVE_NONE:
            WiFi.setSleep(false);
            LOG_DEBUG("WiFi power saving disabled (high performance)");
            break;
        case WifiPowerSaving::SAVE_MID:
            WiFi.setSleep(WIFI_PS_MIN_MODEM);
            LOG_DEBUG("WiFi power saving set to MID (WIFI_PS_MIN_MODEM)");
            break;
        case WifiPowerSaving::SAVE_MAX:
            WiFi.setSleep(WIFI_PS_MAX_MODEM);
            // Set listen_interval so the radio sleeps for multiple DTIM intervals.
            // Without this, MAX_MODEM wakes on every DTIM (~100-300ms), providing
            // minimal benefit over MIN_MODEM. A value of 10 means wake every 10th
            // DTIM beacon, significantly reducing idle radio-on time.
            {
                wifi_config_t wifi_cfg;
                if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
                    wifi_cfg.sta.listen_interval = 10;
                    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
                    LOG_DEBUG("WiFi power saving set to MAX (WIFI_PS_MAX_MODEM, listen_interval=10)");
                } else {
                    LOG_DEBUG("WiFi power saving set to MAX (WIFI_PS_MAX_MODEM, listen_interval unchanged)");
                }
            }
            break;
        default:
            WiFi.setSleep(false);
            LOG_WARN("Unknown power saving setting, disabling power save");
            break;
    }
}
