#include "NetworkHints.h"
#include "Logger.h"
#include <Preferences.h>
#include <time.h>
#include <string.h>

const char* NetworkHints::PREFS_NAMESPACE = "wifi_hints";
const char* NetworkHints::PREFS_KEY       = "blob";

// Blob layout:
//   [uint16_t version][uint16_t count][SavedNetworkHint x count]

NetworkHints::NetworkHints() : _count(0) {
    memset(_hints, 0, sizeof(_hints));
}

bool NetworkHints::begin() {
    _count = 0;

    Preferences prefs;
    if (!prefs.begin(PREFS_NAMESPACE, true /*read-only*/)) {
        LOG_DEBUG("[Hints] NVS namespace empty - starting with no cached hints");
        return false;
    }

    size_t blobSize = prefs.getBytesLength(PREFS_KEY);
    if (blobSize < 4) {
        prefs.end();
        return false;
    }

    constexpr size_t maxBlob = 4 + MAX_HINTS * sizeof(SavedNetworkHint);
    if (blobSize > maxBlob) {
        LOG_WARNF("[Hints] NVS blob too large (%u bytes), discarding", (unsigned)blobSize);
        prefs.end();
        return false;
    }

    uint8_t buf[maxBlob];
    size_t got = prefs.getBytes(PREFS_KEY, buf, blobSize);
    prefs.end();

    if (got != blobSize) return false;

    uint16_t version = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    uint16_t count   = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);

    if (version != BLOB_VERSION) {
        LOG_WARNF("[Hints] Blob version %u != expected %u - discarding", version, BLOB_VERSION);
        return false;
    }
    if (count > MAX_HINTS) {
        LOG_WARNF("[Hints] Blob count %u > MAX_HINTS %d - discarding", count, MAX_HINTS);
        return false;
    }
    if (4 + (size_t)count * sizeof(SavedNetworkHint) != blobSize) {
        LOG_WARN("[Hints] Blob size/count mismatch - discarding");
        return false;
    }

    memcpy(_hints, buf + 4, (size_t)count * sizeof(SavedNetworkHint));
    _count = count;

    for (int i = 0; i < _count; i++) {
        _hints[i].ssid[sizeof(_hints[i].ssid) - 1] = '\0';
    }

    LOG_INFOF("[Hints] Loaded %d network hint(s) from NVS", _count);
    return true;
}

bool NetworkHints::save() {
    Preferences prefs;
    if (!prefs.begin(PREFS_NAMESPACE, false /*read-write*/)) {
        LOG_ERROR("[Hints] Failed to open NVS for write");
        return false;
    }

    constexpr size_t maxBlob = 4 + MAX_HINTS * sizeof(SavedNetworkHint);
    uint8_t buf[maxBlob];
    buf[0] = (uint8_t)(BLOB_VERSION & 0xFF);
    buf[1] = (uint8_t)(BLOB_VERSION >> 8);
    buf[2] = (uint8_t)(_count & 0xFF);
    buf[3] = (uint8_t)((uint16_t)_count >> 8);

    size_t recordsBytes = (size_t)_count * sizeof(SavedNetworkHint);
    memcpy(buf + 4, _hints, recordsBytes);

    size_t written = prefs.putBytes(PREFS_KEY, buf, 4 + recordsBytes);
    prefs.end();

    if (written != 4 + recordsBytes) {
        LOG_ERRORF("[Hints] Short write: %u / %u bytes", (unsigned)written, (unsigned)(4 + recordsBytes));
        return false;
    }
    LOG_DEBUGF("[Hints] Saved %d hint(s), %u bytes", _count, (unsigned)(4 + recordsBytes));
    return true;
}

int NetworkHints::findIndex(const char* ssid, const uint8_t* bssid) const {
    if (!ssid || !bssid) return -1;
    for (int i = 0; i < _count; i++) {
        if (memcmp(_hints[i].bssid, bssid, 6) != 0) continue;
        if (strcmp(_hints[i].ssid, ssid) != 0) continue;
        return i;
    }
    return -1;
}

const SavedNetworkHint* NetworkHints::find(const char* ssid, const uint8_t* bssid) const {
    int idx = findIndex(ssid, bssid);
    return (idx < 0) ? nullptr : &_hints[idx];
}

bool NetworkHints::upsert(const char* ssid, const uint8_t* bssid, uint8_t channel, bool pmf_disable) {
    if (!ssid || ssid[0] == '\0' || !bssid) return false;

    uint32_t nowSecs = (uint32_t)time(nullptr);
    uint8_t  newPmf  = pmf_disable ? 1 : 0;

    int idx = findIndex(ssid, bssid);
    bool wasNew = (idx < 0);
    if (wasNew) {
        if (_count >= MAX_HINTS) evictLRUSlot();
        idx = _count;
        _count++;
        memset(&_hints[idx], 0, sizeof(_hints[idx]));
        strncpy(_hints[idx].ssid, ssid, sizeof(_hints[idx].ssid) - 1);
        memcpy(_hints[idx].bssid, bssid, 6);
    }

    // Refresh pmf_set_secs on fresh entry, transition in the pmf_disable bit,
    // or revalidation after the TTL expired
    bool refreshPmfTs = wasNew
        || (_hints[idx].pmf_disable != newPmf)
        || (nowSecs > 1700000000u  // sanity-check post-NTP
            && nowSecs - _hints[idx].pmf_set_secs > PMF_TTL_SECS);

    _hints[idx].channel        = channel;
    _hints[idx].pmf_disable    = newPmf;
    _hints[idx].last_used_secs = nowSecs;
    if (refreshPmfTs) _hints[idx].pmf_set_secs = nowSecs;
    return true;
}

bool NetworkHints::remove(const char* ssid, const uint8_t* bssid) {
    int idx = findIndex(ssid, bssid);
    if (idx < 0) return false;
    for (int i = idx; i < _count - 1; i++) {
        _hints[i] = _hints[i + 1];
    }
    _count--;
    memset(&_hints[_count], 0, sizeof(_hints[_count]));
    return true;
}

int NetworkHints::evictStale(uint32_t nowSecs, uint32_t maxAgeSecs) {
    if (nowSecs <= maxAgeSecs) return 0;  // clock unsynced or unreasonable
    uint32_t cutoff = nowSecs - maxAgeSecs;
    int dropped = 0;
    int dst = 0;
    for (int src = 0; src < _count; src++) {
        if (_hints[src].last_used_secs < cutoff) {
            dropped++;
            continue;
        }
        if (dst != src) _hints[dst] = _hints[src];
        dst++;
    }
    for (int i = dst; i < _count; i++) {
        memset(&_hints[i], 0, sizeof(_hints[i]));
    }
    _count = dst;
    if (dropped > 0) LOG_INFOF("[Hints] Evicted %d stale hint(s)", dropped);
    return dropped;
}

void NetworkHints::clear() {
    memset(_hints, 0, sizeof(_hints));
    _count = 0;
}

const SavedNetworkHint* NetworkHints::at(int idx) const {
    if (idx < 0 || idx >= _count) return nullptr;
    return &_hints[idx];
}

void NetworkHints::evictLRUSlot() {
    if (_count == 0) return;
    int victim = 0;
    for (int i = 1; i < _count; i++) {
        if (_hints[i].last_used_secs < _hints[victim].last_used_secs) victim = i;
    }

    for (int i = victim; i < _count - 1; i++) {
        _hints[i] = _hints[i + 1];
    }
    _count--;
    memset(&_hints[_count], 0, sizeof(_hints[_count]));
}
