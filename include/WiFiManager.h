#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#include "Config.h"

// Forward declarations for power management enums
enum class WifiTxPower;
enum class WifiPowerSaving;

class WiFiManager {
public:
    // Connection state machine
    enum class ConnectPhase {
        IDLE,       // no attempt in progress
        SCANNING,   // async WiFi.scanNetworks() in progress (multi-SSID only)
        CONNECTING, // WiFi.begin() called with PMF enabled (default), awaiting result
        PMF_RETRY,  // PMF disabled after reason-208 disconnect, awaiting result
        CONNECTED,  // success, terminal
        ROAM_SCAN,  // scan triggered by RSSI degradation while connected
        FAILED      // failure, terminal (timeout, no AP, auth fail...)
    };

private:
    bool connected;
    bool mdnsStarted;
    bool apMode;
    DNSServer dnsServer;
    int8_t _pendingTxPower;   // Deferred TX power (dBm*4), applied during beginConnect()
    bool _hasPendingTxPower;

    // One scan-result entry that matched a configured SSID.  Built during the
    // SCANNING phase, sorted by RSSI desc, then tried in order.
    struct Candidate {
        uint8_t configSlot;   // index into Config.wifiNetworks[]
        uint8_t bssid[6];     // BSSID from scan result (used as connect hint)
        uint8_t channel;      // 1..14 (used as connect hint)
        int8_t  rssi;         // dBm
        bool    fromHint;     // BSSID/channel came from the NVS cache, not a live scan
    };

    static constexpr int     MAX_CANDIDATES        = 12;
    static constexpr uint8_t CANDIDATE_MAX_RETRIES = 2;
    static constexpr uint32_t SCAN_TIMEOUT_MS      = 12000;

    // beginConnect/pollConnect state. _pendingSsid/Password retained so PMF
    // retry can re-issue the connection without the caller passing them again.
    ConnectPhase _connectPhase;
    uint32_t     _phaseStartMs;
    String       _pendingSsid;
    String       _pendingPassword;
    uint8_t      _consecutiveFailures;

    // Multi-SSID candidate iteration.
    Candidate    _candidates[MAX_CANDIDATES];
    uint8_t      _candidateCount;
    uint8_t      _candidateIndex;
    uint8_t      _candidateRetries;
    const Config* _pendingConfig;
    String       _pendingHostname;
    bool         _pendingPmfDisable;

    // Roaming state (active only when 2+ networks configured).
    uint32_t     _lastRoamCheckMs;
    uint8_t      _lowRssiCount;
    bool         _roamSuspended;

    static constexpr uint32_t ROAM_CHECK_INTERVAL_MS = 60000;
    static constexpr int8_t   ROAM_RSSI_THRESHOLD    = -73;
    static constexpr uint8_t  ROAM_LOW_RSSI_COUNT    = 3;
    static constexpr int8_t   ROAM_HYSTERESIS_DB     = 8;

    static constexpr uint32_t CONNECT_PHASE_TIMEOUT_MS = 15000;

    static volatile uint8_t _lastDisconnectReason;  // Captured in event handler for retry logic
    // STA_GOT_IP fires for every (re)association, including ones we did not
    // initiate via beginConnect (e.g. NetworkRecovery::tryCoordinatedWifiCycle,
    // or a supplicant-driven roam).  Pending flag is consumed in pollConnect
    // to refresh the NetworkHints record for the current AP.
    static volatile bool _hintRefreshPending;
    // softAP client count used to gate background STA reconnect attempts
    static volatile uint8_t _apClientCount;
    static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

    // Stable-quiet teardown of the boot-fallback AP after STA recovers
    uint32_t _apTeardownEligibleSinceMs;
    static constexpr uint32_t AP_TEARDOWN_QUIET_MS = 120000;  // 2 minutes

    void enterPhase(ConnectPhase newPhase);
    void enterPmfRetry();
    void terminateConnect(ConnectPhase result);  // CONNECTED or FAILED
    void logConnectFailure();
    void refreshHintForCurrentConnection();

    void prepareSingleCandidate(uint8_t configSlot);
    void kickScan();
    void processScanResults();
    bool startCurrentCandidate();
    void onCurrentCandidateFailed();

    void roamingTick();
    void kickRoamScan();
    void processRoamScanResults();

public:
    WiFiManager();

    void setupEventHandlers();

    // Multi-SSID connect. Reads up to WIFI_MAX_NETWORKS slots from Config.
    // With 1 populated slot: direct WiFi.begin (no scan).
    // With 2+: async scan first, ranks visible APs by RSSI, tries each
    // connectStation() is the synchronous wrapper used at boot
    bool connectStation(const Config& cfg);
    bool beginConnect(const Config& cfg);

    void pollConnect();
    bool isConnectInProgress() const;
    ConnectPhase getConnectPhase() const { return _connectPhase; }

    // Recommended interval before the next reconnect attempt, in ms.
    // Schedule (consecutive FAILED count -> interval): 0->30s, 1->60s,
    // 2->2min, 3+->5min. Reset to 30s after a CONNECTED.
    uint32_t getReconnectIntervalMs() const;
    uint8_t  getConsecutiveFailures() const { return _consecutiveFailures; }

    // Roaming control. Suspend during upload sessions to avoid disconnecting
    // mid-TLS or mid-SMB. Resume after the session ends.
    void suspendRoaming();
    void resumeRoaming();
    bool isRoamingSuspended() const { return _roamSuspended; }
    void startAP();
    void processDNS();
    bool isAPMode() const { return apMode; }

    // softAP client tracking. While > 0, the caller (main loop) should skip
    // background STA reconnect attempts so the user isn't kicked off mid-config.
    uint8_t getApClientCount() const { return _apClientCount; }

    // Returns true once the boot-fallback AP can be safely torn down: STA has been
    // CONNECTED for AP_TEARDOWN_QUIET_MS, no AP clients connected during that window
    bool shouldTearDownAP();
    
    bool isConnected() const;
    void disconnect();
    String getIPAddress() const;
    int getSignalStrength() const;    // Returns RSSI in dBm
    String getSignalQuality() const;  // Returns quality description
    
    // mDNS support
    bool startMDNS(const String& hostname);
    
    // Power management methods
    void setHighPerformanceMode();    // Disable power save for uploads
    void setPowerSaveMode();          // Enable power save for idle periods
    void setMaxPowerSave();           // Maximum power savings
    void applyTxPowerEarly(WifiTxPower txPower);  // Set TX power before WiFi.begin()
    void applyPowerSettings(WifiTxPower txPower, WifiPowerSaving powerSaving);  // Apply config settings
};

#endif // WIFI_MANAGER_H
