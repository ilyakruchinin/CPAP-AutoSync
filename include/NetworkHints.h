#ifndef NETWORK_HINTS_H
#define NETWORK_HINTS_H

#include <Arduino.h>

// Per-AP connection hints - derived knowledge cached across reboots.
// Keyed by (SSID, BSSID) so a mesh / multi-AP-per-SSID setup can have one
// entry per physical AP.  Used by WiFiManager to short-circuit scans
// (apply BSSID + channel directly) and to remember PMF/802.11w workarounds.
//
// Storage: NVS namespace "wifi_hints", single blob "blob" containing
// [uint16 version][uint16 count][record x count]. 52 bytes per record,
// capped at MAX_HINTS = 8 records, ~420 bytes total.
//
// last_used_secs is epoch seconds (time(nullptr)) Used later for LRU eviction
// at cap and for stale-entry cleanup.
//
// pmf_set_secs is when pmf_disable last transitioned (or was last revalidated
// after TTL expiry). WiFiManager re-tests PMF posture if older than PMF_TTL_SECS
// to detect router upgrades that re-enable 802.11w.

struct SavedNetworkHint {
    char     ssid[33];          // null-terminated, max 32 chars + null
    uint8_t  bssid[6];          // BSSID (raw bytes)
    uint8_t  channel;           // 1..14 for 2.4 GHz
    uint8_t  pmf_disable;       // 1 = router needs PMF disabled (reason 208)
    uint32_t last_used_secs;    // epoch seconds (time()) at last successful use
    uint32_t pmf_set_secs;      // epoch seconds when pmf_disable was last set/revalidated
};
static_assert(sizeof(SavedNetworkHint) == 52, "SavedNetworkHint must pack to 52 bytes for stable on-disk layout");

class NetworkHints {
public:
    static constexpr int      MAX_HINTS = 8;
    static constexpr uint16_t BLOB_VERSION = 2;
    static constexpr uint32_t PMF_TTL_SECS = 30u * 24u * 3600u;  // 30 days

    NetworkHints();

    bool begin();
    bool save();
    void clear();

    // Lookup an exact (ssid, bssid) match. Returns nullptr if not found.
    const SavedNetworkHint* find(const char* ssid, const uint8_t* bssid) const;

    // Insert or update a hint. Refreshes last_used_secs to time(nullptr).
    // If the array is full and the entry is new, the LRU entry is evicted.
    // Returns false on argument error (null/empty ssid, null bssid).
    bool upsert(const char* ssid, const uint8_t* bssid, uint8_t channel, bool pmf_disable);

    // Remove an exact (ssid, bssid) hint. Returns false if not found.
    bool remove(const char* ssid, const uint8_t* bssid);

    // Drop entries with last_used_secs older than (nowSecs - maxAgeSecs).
    // No-op if nowSecs <= maxAgeSecs (clock not synced).
    int evictStale(uint32_t nowSecs, uint32_t maxAgeSecs);

    int count() const { return _count; }
    const SavedNetworkHint* at(int idx) const;

private:
    SavedNetworkHint _hints[MAX_HINTS];
    int _count;

    static const char* PREFS_NAMESPACE;
    static const char* PREFS_KEY;

    int findIndex(const char* ssid, const uint8_t* bssid) const;
    void evictLRUSlot();
};

#endif  // NETWORK_HINTS_H
