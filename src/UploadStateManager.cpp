#include "UploadStateManager.h"
#include "Logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef UNIT_TEST
#include "MockMD5.h"
#else
#include <esp_rom_md5.h>
#endif

namespace {
static inline bool readLine(File& file, char* buffer, size_t bufferLen) {
    if (bufferLen == 0) {
        return false;
    }

    size_t idx = 0;
    while (file.available()) {
        int ch = file.read();
        if (ch < 0) {
            break;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        if (idx + 1 < bufferLen) {
            buffer[idx++] = (char)ch;
        }
    }

    if (idx == 0 && !file.available()) {
        buffer[0] = '\0';
        return false;
    }

    buffer[idx] = '\0';
    return true;
}

static inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool parseDayToken(const char* token, uint32_t& day) {
    if (!token) {
        return false;
    }

    if (strcmp(token, "0") == 0) {
        day = 0;
        return true;
    }

    if (strlen(token) != 8) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        char c = token[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (uint32_t)(c - '0');
    }

    day = value;
    return true;
}
}

void UploadStateManager::setPaths(const String& snapshotPath, const String& journalPath) {
    stateSnapshotPath = snapshotPath;
    stateJournalPath  = journalPath;
}

UploadStateManager::UploadStateManager() 
    : stateSnapshotPath("/littlefs/.upload_state.v2"),
      stateJournalPath("/littlefs/.upload_state.v2.log"),
      lastUploadTimestamp(0),
      completedCount(0),
      pendingCount(0),
      fileEntryCount(0),
      currentRetryFolderDay(0),
      currentRetryCount(0),
      journalEventCount(0),
      journalLineCount(0),
      forceCompaction(false),
      totalFoldersCount(0),
      probeUniverse(-1),
      probeSynced(-1),
      probeSnapshotCompletedCount(-1),
      hasProbed(false) {
    clearState();
}

bool UploadStateManager::begin(fs::FS &sd) {
    LOG("[UploadStateManager] Initializing...");
    
    // Try to load existing state
    bool loadedState = loadState(sd);
    if (!loadedState) {
        LOG("[UploadStateManager] WARNING: No existing state file or failed to load");
        LOG("[UploadStateManager] Starting with empty state - all files will be considered new");
        clearState();
    }

    if (loadedState && shouldCompact(sd)) {
        compactState(sd);
    }
    
    return true;  // Always return true - we can operate with empty state
}

void UploadStateManager::clearState() {
    lastUploadTimestamp = 0;
    completedCount = 0;
    pendingCount = 0;
    fileEntryCount = 0;
    currentRetryFolderDay = 0;
    currentRetryCount = 0;
    journalEventCount = 0;
    journalLineCount = 0;
    forceCompaction = false;
    totalFoldersCount = 0;
    probeUniverse = -1;
    probeSynced = -1;
    probeSnapshotCompletedCount = -1;
    hasProbed = false;

    memset(completedFolders, 0, sizeof(completedFolders));
    memset(pendingFolders, 0, sizeof(pendingFolders));
    memset(fileEntries, 0, sizeof(fileEntries));
    memset(journalEvents, 0, sizeof(journalEvents));
}

bool UploadStateManager::isDatalogPath(const String& path) {
    return path.startsWith("/DATALOG/");
}

bool UploadStateManager::parseDayKey(const String& text, DayKey& outDay) {
    if (text.length() != 8) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (uint32_t)(c - '0');
    }

    outDay = value;
    return true;
}

void UploadStateManager::dayKeyToChars(DayKey day, char* out, size_t outLen) {
    if (outLen == 0) {
        return;
    }
    if (day == 0) {
        snprintf(out, outLen, "0");
        return;
    }
    snprintf(out, outLen, "%08lu", (unsigned long)day);
}

UploadStateManager::PathHash UploadStateManager::hashPath(const String& path) {
    const uint64_t fnvOffset = 1469598103934665603ULL;
    const uint64_t fnvPrime = 1099511628211ULL;
    uint64_t hash = fnvOffset;

    const char* p = path.c_str();
    while (*p) {
        hash ^= (uint8_t)(*p);
        hash *= fnvPrime;
        ++p;
    }

    return hash;
}

bool UploadStateManager::parseHexMd5(const char* hex, uint8_t out[16]) {
    if (!hex || strlen(hex) != 32) {
        return false;
    }

    for (int i = 0; i < 16; ++i) {
        int hi = hexNibble(hex[i * 2]);
        int lo = hexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return true;
}

void UploadStateManager::md5ToHex(const uint8_t md5[16], char out[33]) {
    for (int i = 0; i < 16; ++i) {
        snprintf(out + (i * 2), 3, "%02x", md5[i]);
    }
    out[32] = '\0';
}

int UploadStateManager::findCompletedIndex(DayKey day) const {
    for (uint16_t i = 0; i < completedCount; ++i) {
        if (completedFolders[i].day == day) {
            return (int)i;
        }
    }
    return -1;
}

int UploadStateManager::findPendingIndex(DayKey day) const {
    for (uint16_t i = 0; i < pendingCount; ++i) {
        if (pendingFolders[i].day == day) {
            return (int)i;
        }
    }
    return -1;
}

int UploadStateManager::findFileIndex(PathHash pathHash) const {
    for (uint16_t i = 0; i < fileEntryCount; ++i) {
        if ((fileEntries[i].flags & FILE_FLAG_ACTIVE) && fileEntries[i].pathHash == pathHash) {
            return (int)i;
        }
    }
    return -1;
}

void UploadStateManager::queueEvent(const JournalEvent& event) {
    if (journalEventCount >= MAX_JOURNAL_EVENTS) {
        forceCompaction = true;
        return;
    }
    journalEvents[journalEventCount++] = event;
}

bool UploadStateManager::addCompletedInternal(DayKey day, bool queue) {
    if (findCompletedIndex(day) >= 0) {
        return false;  // Already exists
    }

    if (completedCount >= MAX_COMPLETED_FOLDERS) {
        // Remove oldest entry (at index 0)
        if (completedCount > 1) {
            memmove(&completedFolders[0], &completedFolders[1], 
                    sizeof(CompletedFolderEntry) * (completedCount - 1));
        }
        completedCount--;
        forceCompaction = true;
    }

    completedFolders[completedCount].day = day;
    completedCount++;

    if (queue) {
        JournalEvent event = {};
        event.type = JournalEventType::AddCompleted;
        event.day = day;
        queueEvent(event);
    }

    return true;
}

bool UploadStateManager::removeCompletedInternal(DayKey day, bool queue) {
    int idx = findCompletedIndex(day);
    if (idx < 0) {
        return false;
    }

    if ((uint16_t)idx < (completedCount - 1)) {
        memmove(&completedFolders[idx], &completedFolders[idx + 1], sizeof(CompletedFolderEntry) * (completedCount - idx - 1));
    }
    completedCount--;

    if (queue) {
        JournalEvent ev = {};
        ev.type = JournalEventType::RemoveCompleted;
        ev.day = day;
        queueEvent(ev);
    }
    return true;
}

bool UploadStateManager::addPendingInternal(DayKey day, UnixTs ts, bool queue) {
    if (day == 0) {
        return false;
    }

    int idx = findPendingIndex(day);
    if (idx >= 0) {
        pendingFolders[idx].firstSeenTs = ts;
    } else {
        if (pendingCount >= MAX_PENDING_FOLDERS) {
            memmove(&pendingFolders[0], &pendingFolders[1], sizeof(PendingFolderEntry) * (MAX_PENDING_FOLDERS - 1));
            pendingCount = MAX_PENDING_FOLDERS - 1;
            forceCompaction = true;
        }
        pendingFolders[pendingCount].day = day;
        pendingFolders[pendingCount].firstSeenTs = ts;
        pendingCount++;
    }

    if (queue) {
        JournalEvent ev = {};
        ev.type = JournalEventType::AddPending;
        ev.day = day;
        ev.timestamp = ts;
        queueEvent(ev);
    }
    return true;
}

bool UploadStateManager::removePendingInternal(DayKey day, bool queue) {
    int idx = findPendingIndex(day);
    if (idx < 0) {
        return false;
    }

    if ((uint16_t)idx < (pendingCount - 1)) {
        memmove(&pendingFolders[idx], &pendingFolders[idx + 1], sizeof(PendingFolderEntry) * (pendingCount - idx - 1));
    }
    pendingCount--;

    if (queue) {
        JournalEvent ev = {};
        ev.type = JournalEventType::RemovePending;
        ev.day = day;
        queueEvent(ev);
    }
    return true;
}

void UploadStateManager::setRetryInternal(DayKey day, int retryCount, bool queue) {
    currentRetryFolderDay = day;
    currentRetryCount = retryCount < 0 ? 0 : retryCount;

    if (queue) {
        JournalEvent ev = {};
        ev.type = JournalEventType::SetRetry;
        ev.day = day;
        ev.retryCount = (uint16_t)currentRetryCount;
        queueEvent(ev);
    }
}

void UploadStateManager::setTimestampInternal(UnixTs ts, bool queue) {
    lastUploadTimestamp = ts;

    if (queue) {
        JournalEvent ev = {};
        ev.type = JournalEventType::SetTimestamp;
        ev.timestamp = ts;
        queueEvent(ev);
    }
}

String UploadStateManager::calculateChecksum(fs::FS &sd, const String& filePath) {
    File file = sd.open(filePath, FILE_READ);
    if (!file) {
        LOGF("[UploadStateManager] ERROR: Failed to open file for checksum: %s", filePath.c_str());
        return "";
    }
    
    // Check if file is readable
    if (!file.available() && file.size() > 0) {
        LOGF("[UploadStateManager] ERROR: File exists but cannot be read: %s", filePath.c_str());
        file.close();
        return "";
    }
    
    md5_context_t md5_ctx;
    esp_rom_md5_init(&md5_ctx);
    
    const size_t bufferSize = 4096;
    uint8_t buffer[bufferSize];
    size_t totalBytesRead = 0;
    size_t expectedSize = file.size();
    
    while (file.available()) {
        size_t bytesRead = file.read(buffer, bufferSize);
        if (bytesRead == 0) {
            // Read error
            LOGF("[UploadStateManager] ERROR: Read error while calculating checksum for: %s", filePath.c_str());
            file.close();
            return "";
        }
        
        esp_rom_md5_update(&md5_ctx, buffer, bytesRead);
        totalBytesRead += bytesRead;
        
        // Yield periodically to prevent watchdog timeout on large files
        if (totalBytesRead % (10 * bufferSize) == 0) {
            yield();
        }
    }
    
    // Verify we read the expected amount
    if (totalBytesRead != expectedSize) {
        LOG_DEBUGF("[UploadStateManager] WARNING: Checksum size mismatch for %s (read %u bytes, expected %u bytes)", 
             filePath.c_str(), totalBytesRead, expectedSize);
    }
    
    uint8_t hash[16];
    esp_rom_md5_final(hash, &md5_ctx);
    
    file.close();
    
    // Convert hash to hex string
    String checksumStr = "";
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", hash[i]);
        checksumStr += hex;
    }
    
    return checksumStr;
}

bool UploadStateManager::hasFileChanged(fs::FS &sd, const String& filePath) {
    PathHash pathHash = hashPath(filePath);
    int idx = findFileIndex(pathHash);
    if (idx < 0) {
        File file = sd.open(filePath, FILE_READ);
        if (!file) {
            return false;
        }
        file.close();
        return true;
    }

    const FileFingerprintEntry& entry = fileEntries[idx];

    if (entry.fileSize > 0) {
        File file = sd.open(filePath, FILE_READ);
        if (!file) {
            return false;
        }

        unsigned long currentSize = file.size();
        file.close();

        if (currentSize != entry.fileSize) {
            LOG_DEBUGF("[UploadStateManager] Size changed: %s (%lu -> %lu)",
                       filePath.c_str(),
                       entry.fileSize,
                       currentSize);
            return true;
        }

        if ((entry.flags & FILE_FLAG_HAS_MD5) == 0) {
            return false;
        }
    }

    if ((entry.flags & FILE_FLAG_HAS_MD5) == 0) {
        return false;
    }

    String currentChecksum = calculateChecksum(sd, filePath);
    if (currentChecksum.isEmpty()) {
        return false;
    }

    uint8_t currentMd5[16] = {0};
    if (!parseHexMd5(currentChecksum.c_str(), currentMd5)) {
        return true;
    }

    return memcmp(currentMd5, entry.md5, sizeof(currentMd5)) != 0;
}

void UploadStateManager::markFileUploaded(const String& filePath, const String& checksum, unsigned long fileSize) {
    PathHash pathHash = hashPath(filePath);

    if (isDatalogPath(filePath)) {
        if (fileSize > 0) {
            // Enable persistence for DATALOG files (persistent=true, queue=true)
            // This allows skipping already uploaded files even after a reboot
            upsertFileEntry(pathHash, (uint32_t)fileSize, nullptr, false, true, true);
        }
        return;
    }

    if (checksum.isEmpty()) {
        removeFileEntry(pathHash, true);
        return;
    }

    uint8_t md5[16] = {0};
    bool hasMd5 = parseHexMd5(checksum.c_str(), md5);
    upsertFileEntry(pathHash,
                    (uint32_t)fileSize,
                    hasMd5 ? md5 : nullptr,
                    hasMd5,
                    true,
                    true);
}

bool UploadStateManager::isFolderCompleted(const String& folderName) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        return false;
    }
    return findCompletedIndex(day) >= 0;
}

void UploadStateManager::markFolderCompleted(const String& folderName) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        LOG_WARNF("[UploadStateManager] Invalid folder name for completion: %s", folderName.c_str());
        return;
    }

    addCompletedInternal(day, true);

    if (removePendingInternal(day, true)) {
        LOG_DEBUGF("[UploadStateManager] Removed folder from pending state: %s", folderName.c_str());
    }

    if (currentRetryFolderDay == day) {
        clearCurrentRetry();
    }
}

void UploadStateManager::removeFileEntriesForPaths(const std::vector<String>& filePaths) {
    for (const String& path : filePaths) {
        PathHash h = hashPath(path);
        removeFileEntry(h, true);
    }
}

void UploadStateManager::removeFolderFromCompleted(const String& folderName) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        return;
    }

    if (removeCompletedInternal(day, true)) {
        LOG_DEBUGF("[UploadStateManager] Removed folder from completed state: %s", folderName.c_str());
    }
}

int UploadStateManager::getCurrentRetryCount() {
    return currentRetryCount;
}

void UploadStateManager::setCurrentRetryFolder(const String& folderName) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        LOG_WARNF("[UploadStateManager] Invalid retry folder: %s", folderName.c_str());
        return;
    }

    if (currentRetryFolderDay != day) {
        setRetryInternal(day, 0, true);
    }
}

void UploadStateManager::incrementCurrentRetryCount() {
    setRetryInternal(currentRetryFolderDay, currentRetryCount + 1, true);
}

void UploadStateManager::clearCurrentRetry() {
    setRetryInternal(0, 0, true);
}

int UploadStateManager::getCompletedFoldersCount() const {
    return completedCount;
}

int UploadStateManager::getIncompleteFoldersCount() const {
    if (hasProbed) {
        int incomplete = probeUniverse - probeSynced;
        return incomplete > 0 ? incomplete : 0;
    }
    if (totalFoldersCount == 0) {
        return 0;  // Not yet scanned
    }
    int incomplete = totalFoldersCount - completedCount - pendingCount;
    return incomplete > 0 ? incomplete : 0;
}

void UploadStateManager::setTotalFoldersCount(int count) {
    totalFoldersCount = count;
}

void UploadStateManager::setProbeSnapshot(int universe, int synced) {
    probeUniverse = universe;
    probeSynced   = synced;
    probeSnapshotCompletedCount = completedCount;
    hasProbed     = true;
}

int UploadStateManager::getProbeUniverse() const {
    return probeUniverse;
}

int UploadStateManager::getProbeSynced() const {
    return probeSynced;
}

int UploadStateManager::getProbeSnapshotCompletedCount() const {
    return probeSnapshotCompletedCount;
}

bool UploadStateManager::hasBeenProbed() const {
    return hasProbed;
}

bool UploadStateManager::isPendingFolder(const String& folderName) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        return false;
    }
    return findPendingIndex(day) >= 0;
}

void UploadStateManager::markFolderPending(const String& folderName, unsigned long timestamp) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        LOG_WARNF("[UploadStateManager] Invalid folder name for pending: %s", folderName.c_str());
        return;
    }

    addPendingInternal(day, (UnixTs)timestamp, true);
    LOG_DEBUGF("[UploadStateManager] Marked folder as pending: %s (timestamp: %lu)", 
         folderName.c_str(), timestamp);
}

void UploadStateManager::removeFolderFromPending(const String& folderName) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        return;
    }

    if (removePendingInternal(day, true)) {
        LOG_DEBUGF("[UploadStateManager] Removed folder from pending state: %s", folderName.c_str());
    }
}

bool UploadStateManager::shouldPromotePendingToCompleted(const String& folderName, unsigned long currentTime) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        return false;
    }

    int idx = findPendingIndex(day);
    if (idx < 0) {
        return false;
    }

    unsigned long firstSeenTime = pendingFolders[idx].firstSeenTs;
    return (currentTime - firstSeenTime) >= PENDING_FOLDER_TIMEOUT_SECONDS;
}

void UploadStateManager::promotePendingToCompleted(const String& folderName) {
    DayKey day = 0;
    if (!parseDayKey(folderName, day)) {
        return;
    }

    if (removePendingInternal(day, true)) {
        addCompletedInternal(day, true);
        LOGF("[UploadStateManager] Promoted pending folder to completed: %s (empty for 7+ days)", 
             folderName.c_str());
    }
}

int UploadStateManager::getPendingFoldersCount() const {
    return pendingCount;
}

String UploadStateManager::getCurrentRetryFolder() const {
    if (currentRetryFolderDay == 0) {
        return "";
    }

    char dayText[16] = {0};
    dayKeyToChars(currentRetryFolderDay, dayText, sizeof(dayText));
    return String(dayText);
}

unsigned long UploadStateManager::getLastUploadTimestamp() {
    return lastUploadTimestamp;
}

void UploadStateManager::setLastUploadTimestamp(unsigned long timestamp) {
    setTimestampInternal((UnixTs)timestamp, true);
}

bool UploadStateManager::save(fs::FS &sd) {
    return saveState(sd);
}

bool UploadStateManager::upsertFileEntry(PathHash pathHash,
                                         uint32_t fileSize,
                                         const uint8_t* md5,
                                         bool hasMd5,
                                         bool persistent,
                                         bool queue) {
    int idx = findFileIndex(pathHash);

    if (idx < 0) {
        if (fileEntryCount >= MAX_FILE_ENTRIES) {
            int evictIdx = -1;
            for (uint16_t i = 0; i < fileEntryCount; ++i) {
                if ((fileEntries[i].flags & FILE_FLAG_PERSISTENT) == 0) {
                    evictIdx = (int)i;
                    break;
                }
            }

            if (evictIdx < 0) {
                evictIdx = 0;
            }

            if ((uint16_t)evictIdx < (fileEntryCount - 1)) {
                memmove(&fileEntries[evictIdx], &fileEntries[evictIdx + 1],
                        sizeof(FileFingerprintEntry) * (fileEntryCount - evictIdx - 1));
            }
            fileEntryCount--;
            forceCompaction = true;
        }

        idx = fileEntryCount++;
    }

    FileFingerprintEntry& entry = fileEntries[idx];
    entry.pathHash = pathHash;
    entry.fileSize = fileSize;
    entry.flags = FILE_FLAG_ACTIVE | (persistent ? FILE_FLAG_PERSISTENT : 0) | (hasMd5 ? FILE_FLAG_HAS_MD5 : 0);

    if (hasMd5 && md5) {
        memcpy(entry.md5, md5, sizeof(entry.md5));
    } else {
        memset(entry.md5, 0, sizeof(entry.md5));
    }

    if (queue && persistent) {
        JournalEvent ev = {};
        ev.type = JournalEventType::SetFile;
        ev.pathHash = pathHash;
        ev.fileSize = fileSize;
        ev.hasMd5 = hasMd5;
        if (hasMd5 && md5) {
            memcpy(ev.md5, md5, sizeof(ev.md5));
        }
        queueEvent(ev);
    }

    return true;
}

bool UploadStateManager::removeFileEntry(PathHash pathHash, bool queue) {
    int idx = findFileIndex(pathHash);
    if (idx < 0) {
        return false;
    }

    bool persistent = (fileEntries[idx].flags & FILE_FLAG_PERSISTENT) != 0;

    if ((uint16_t)idx < (fileEntryCount - 1)) {
        memmove(&fileEntries[idx], &fileEntries[idx + 1],
                sizeof(FileFingerprintEntry) * (fileEntryCount - idx - 1));
    }
    fileEntryCount--;

    if (queue && persistent) {
        JournalEvent ev = {};
        ev.type = JournalEventType::RemoveFile;
        ev.pathHash = pathHash;
        queueEvent(ev);
    }

    return true;
}

bool UploadStateManager::appendJournalLine(File& file, const JournalEvent& event) {
    char line[192] = {0};
    char dayText[16] = {0};

    switch (event.type) {
        case JournalEventType::SetTimestamp:
            snprintf(line, sizeof(line), "T|%lu", (unsigned long)event.timestamp);
            break;
        case JournalEventType::SetRetry:
            dayKeyToChars(event.day, dayText, sizeof(dayText));
            snprintf(line, sizeof(line), "R|%s|%u", dayText, (unsigned)event.retryCount);
            break;
        case JournalEventType::AddCompleted:
            dayKeyToChars(event.day, dayText, sizeof(dayText));
            snprintf(line, sizeof(line), "C+|%s", dayText);
            break;
        case JournalEventType::RemoveCompleted:
            dayKeyToChars(event.day, dayText, sizeof(dayText));
            snprintf(line, sizeof(line), "C-|%s", dayText);
            break;
        case JournalEventType::AddPending:
            dayKeyToChars(event.day, dayText, sizeof(dayText));
            snprintf(line, sizeof(line), "P+|%s|%lu", dayText, (unsigned long)event.timestamp);
            break;
        case JournalEventType::RemovePending:
            dayKeyToChars(event.day, dayText, sizeof(dayText));
            snprintf(line, sizeof(line), "P-|%s", dayText);
            break;
        case JournalEventType::SetFile: {
            char md5Hex[33] = {0};
            if (event.hasMd5) {
                md5ToHex(event.md5, md5Hex);
            } else {
                snprintf(md5Hex, sizeof(md5Hex), "-");
            }
            snprintf(line,
                     sizeof(line),
                     "F|%016llx|%lu|%s",
                     (unsigned long long)event.pathHash,
                     (unsigned long)event.fileSize,
                     md5Hex);
            break;
        }
        case JournalEventType::RemoveFile:
            snprintf(line, sizeof(line), "F-|%016llx", (unsigned long long)event.pathHash);
            break;
    }

    return file.println(line) > 0;
}

bool UploadStateManager::flushJournal(fs::FS &sd) {
    if (journalEventCount == 0) {
        return true;
    }

    File file = sd.open(stateJournalPath, FILE_APPEND);
    if (!file) {
        LOGF("[UploadStateManager] ERROR: Failed to open journal file for append: %s", stateJournalPath.c_str());
        return false;
    }

    for (uint16_t i = 0; i < journalEventCount; ++i) {
        if (!appendJournalLine(file, journalEvents[i])) {
            file.close();
            LOG("[UploadStateManager] ERROR: Failed to append journal event");
            return false;
        }
    }

    file.close();

    if ((uint32_t)journalLineCount + journalEventCount > 0xFFFFu) {
        journalLineCount = 0xFFFFu;
    } else {
        journalLineCount = (uint16_t)(journalLineCount + journalEventCount);
    }

    journalEventCount = 0;
    return true;
}

bool UploadStateManager::applySnapshotLine(const char* line) {
    if (!line || line[0] == '\0') {
        return true;
    }

    if (strncmp(line, "R|", 2) == 0) {
        char dayToken[16] = {0};
        unsigned long retryCount = 0;
        if (sscanf(line, "R|%15[^|]|%lu", dayToken, &retryCount) == 2) {
            DayKey day = 0;
            if (!parseDayToken(dayToken, day)) {
                return false;
            }

            setRetryInternal(day, (int)retryCount, false);
            return true;
        }
        return false;
    }

    if (strncmp(line, "C|", 2) == 0) {
        // Format: C|day  (extra fields from older snapshot formats are silently ignored)
        char dayToken[16] = {0};
        if (sscanf(line, "C|%15[^|\n]", dayToken) == 1) {
            DayKey day = 0;
            if (parseDayToken(dayToken, day)) {
                return addCompletedInternal(day, false);
            }
        }
        return false;
    }

    if (strncmp(line, "P|", 2) == 0) {
        char dayToken[16] = {0};
        unsigned long firstSeenTs = 0;
        if (sscanf(line, "P|%15[^|]|%lu", dayToken, &firstSeenTs) == 2) {
            DayKey day = 0;
            if (parseDayToken(dayToken, day)) {
                return addPendingInternal(day, (UnixTs)firstSeenTs, false);
            }
        }
        return false;
    }

    if (strncmp(line, "F|", 2) == 0) {
        char pathHashHex[24] = {0};
        unsigned long fileSize = 0;
        char md5Hex[40] = {0};
        if (sscanf(line, "F|%23[^|]|%lu|%39s", pathHashHex, &fileSize, md5Hex) == 3) {
            PathHash pathHash = (PathHash)strtoull(pathHashHex, nullptr, 16);
            uint8_t md5[16] = {0};
            bool hasMd5 = false;
            if (strcmp(md5Hex, "-") != 0) {
                hasMd5 = parseHexMd5(md5Hex, md5);
                if (!hasMd5) {
                    return false;
                }
            }

            return upsertFileEntry(pathHash,
                                   (uint32_t)fileSize,
                                   hasMd5 ? md5 : nullptr,
                                   hasMd5,
                                   true,
                                   false);
        }
        return false;
    }

    return false;
}

bool UploadStateManager::applyJournalLine(const char* line) {
    if (!line || line[0] == '\0') {
        return true;
    }

    if (strncmp(line, "T|", 2) == 0) {
        unsigned long ts = 0;
        if (sscanf(line, "T|%lu", &ts) == 1) {
            setTimestampInternal((UnixTs)ts, false);
            return true;
        }
        return false;
    }

    if (strncmp(line, "R|", 2) == 0) {
        char dayToken[16] = {0};
        unsigned long retryCount = 0;
        if (sscanf(line, "R|%15[^|]|%lu", dayToken, &retryCount) == 2) {
            DayKey day = 0;
            if (!parseDayToken(dayToken, day)) {
                return false;
            }

            setRetryInternal(day, (int)retryCount, false);
            return true;
        }
        return false;
    }

    if (strncmp(line, "C+|", 3) == 0) {
        char dayToken[16] = {0};
        if (sscanf(line, "C+|%15s", dayToken) == 1) {
            DayKey day = 0;
            if (parseDayToken(dayToken, day)) {
                return addCompletedInternal(day, false);
            }
        }
        return false;
    }

    if (strncmp(line, "C-|", 3) == 0) {
        char dayToken[16] = {0};
        if (sscanf(line, "C-|%15s", dayToken) == 1) {
            DayKey day = 0;
            if (parseDayToken(dayToken, day)) {
                return removeCompletedInternal(day, false);
            }
        }
        return false;
    }

    if (strncmp(line, "P+|", 3) == 0) {
        char dayToken[16] = {0};
        unsigned long firstSeenTs = 0;
        if (sscanf(line, "P+|%15[^|]|%lu", dayToken, &firstSeenTs) == 2) {
            DayKey day = 0;
            if (parseDayToken(dayToken, day)) {
                return addPendingInternal(day, (UnixTs)firstSeenTs, false);
            }
        }
        return false;
    }

    if (strncmp(line, "P-|", 3) == 0) {
        char dayToken[16] = {0};
        if (sscanf(line, "P-|%15s", dayToken) == 1) {
            DayKey day = 0;
            if (parseDayToken(dayToken, day)) {
                return removePendingInternal(day, false);
            }
        }
        return false;
    }

    if (strncmp(line, "F-|", 3) == 0) {
        char pathHashHex[24] = {0};
        if (sscanf(line, "F-|%23s", pathHashHex) == 1) {
            PathHash pathHash = (PathHash)strtoull(pathHashHex, nullptr, 16);
            return removeFileEntry(pathHash, false);
        }
        return false;
    }

    if (strncmp(line, "F|", 2) == 0) {
        char pathHashHex[24] = {0};
        unsigned long fileSize = 0;
        char md5Hex[40] = {0};
        if (sscanf(line, "F|%23[^|]|%lu|%39s", pathHashHex, &fileSize, md5Hex) == 3) {
            PathHash pathHash = (PathHash)strtoull(pathHashHex, nullptr, 16);
            uint8_t md5[16] = {0};
            bool hasMd5 = false;
            if (strcmp(md5Hex, "-") != 0) {
                hasMd5 = parseHexMd5(md5Hex, md5);
                if (!hasMd5) {
                    return false;
                }
            }

            return upsertFileEntry(pathHash,
                                   (uint32_t)fileSize,
                                   hasMd5 ? md5 : nullptr,
                                   hasMd5,
                                   true,
                                   false);
        }
        return false;
    }

    return false;
}

bool UploadStateManager::replayJournal(fs::FS &sd) {
    if (!sd.exists(stateJournalPath)) {
        journalLineCount = 0;
        return false;
    }
    File file = sd.open(stateJournalPath, FILE_READ);
    if (!file) {
        journalLineCount = 0;
        return false;
    }

    char line[256] = {0};
    uint16_t lines = 0;

    while (readLine(file, line, sizeof(line))) {
        if (line[0] == '\0') {
            continue;
        }

        if (!applyJournalLine(line)) {
            LOG_WARNF("[UploadStateManager] Ignoring invalid journal line: %s", line);
            continue;
        }

        if (lines < 0xFFFFu) {
            lines++;
        }
    }

    file.close();
    journalLineCount = lines;
    return lines > 0;
}

bool UploadStateManager::shouldCompact(fs::FS &sd) const {
    if (forceCompaction) {
        return true;
    }

    if (!sd.exists(stateSnapshotPath)) {
        return true;
    }

    if (journalLineCount >= COMPACTION_LINE_THRESHOLD) {
        return true;
    }

    if (sd.exists(stateJournalPath)) {
        File file = sd.open(stateJournalPath, FILE_READ);
        if (file) {
            size_t journalSize = file.size();
            file.close();
            if (journalSize >= COMPACTION_SIZE_THRESHOLD_BYTES) {
                return true;
            }
        }
    }

    return false;
}

bool UploadStateManager::compactState(fs::FS &sd) {
    String tempPath = stateSnapshotPath + ".tmp";
    File file = sd.open(tempPath, FILE_WRITE);
    if (!file) {
        LOGF("[UploadStateManager] ERROR: Failed to open temp snapshot file: %s", tempPath.c_str());
        return false;
    }

    char line[256] = {0};
    snprintf(line, sizeof(line), "U2|2|%lu", (unsigned long)lastUploadTimestamp);
    if (file.println(line) == 0) {
        file.close();
        sd.remove(tempPath);
        return false;
    }

    char retryDay[16] = {0};
    dayKeyToChars(currentRetryFolderDay, retryDay, sizeof(retryDay));
    snprintf(line, sizeof(line), "R|%s|%u", retryDay, (unsigned)currentRetryCount);
    if (file.println(line) == 0) {
        file.close();
        sd.remove(tempPath);
        return false;
    }

    for (uint16_t i = 0; i < completedCount; ++i) {
        char dayText[16] = {0};
        dayKeyToChars(completedFolders[i].day, dayText, sizeof(dayText));
        snprintf(line, sizeof(line), "C|%s", dayText);
        if (file.println(line) == 0) {
            file.close();
            sd.remove(tempPath);
            return false;
        }
    }

    for (uint16_t i = 0; i < pendingCount; ++i) {
        char dayText[16] = {0};
        dayKeyToChars(pendingFolders[i].day, dayText, sizeof(dayText));
        snprintf(line, sizeof(line), "P|%s|%lu", dayText, (unsigned long)pendingFolders[i].firstSeenTs);
        if (file.println(line) == 0) {
            file.close();
            sd.remove(tempPath);
            return false;
        }
    }

    for (uint16_t i = 0; i < fileEntryCount; ++i) {
        const FileFingerprintEntry& entry = fileEntries[i];
        if ((entry.flags & FILE_FLAG_ACTIVE) == 0 || (entry.flags & FILE_FLAG_PERSISTENT) == 0) {
            continue;
        }

        char md5Hex[33] = {0};
        if (entry.flags & FILE_FLAG_HAS_MD5) {
            md5ToHex(entry.md5, md5Hex);
        } else {
            snprintf(md5Hex, sizeof(md5Hex), "-");
        }

        snprintf(line,
                 sizeof(line),
                 "F|%016llx|%lu|%s",
                 (unsigned long long)entry.pathHash,
                 (unsigned long)entry.fileSize,
                 md5Hex);
        if (file.println(line) == 0) {
            file.close();
            sd.remove(tempPath);
            return false;
        }
    }

    file.close();

    File verify = sd.open(tempPath, FILE_READ);
    if (!verify) {
        sd.remove(tempPath);
        return false;
    }

    size_t verifySize = verify.size();
    verify.close();
    if (verifySize == 0) {
        sd.remove(tempPath);
        return false;
    }

    if (sd.exists(stateSnapshotPath)) {
        sd.remove(stateSnapshotPath);
    }

    if (!sd.rename(tempPath, stateSnapshotPath)) {
        sd.remove(tempPath);
        return false;
    }

    if (sd.exists(stateJournalPath)) {
        sd.remove(stateJournalPath);
    }

    journalEventCount = 0;
    journalLineCount = 0;
    forceCompaction = false;

    return true;
}

bool UploadStateManager::loadState(fs::FS &sd) {
    clearState();

    bool loadedSnapshot = false;

    if (sd.exists(stateSnapshotPath)) {
        File file = sd.open(stateSnapshotPath, FILE_READ);
        if (!file) {
            LOGF("[UploadStateManager] ERROR: Failed to open snapshot file: %s", stateSnapshotPath.c_str());
            return false;
        }

        char line[256] = {0};
        if (!readLine(file, line, sizeof(line))) {
            file.close();
            LOG("[UploadStateManager] WARNING: Snapshot file is empty");
            return false;
        }

        unsigned long version = 0;
        unsigned long ts = 0;
        if (sscanf(line, "U2|%lu|%lu", &version, &ts) != 2 || version != 2) {
            file.close();
            LOGF("[UploadStateManager] ERROR: Invalid snapshot header: %s", line);
            return false;
        }

        setTimestampInternal((UnixTs)ts, false);

        while (readLine(file, line, sizeof(line))) {
            if (line[0] == '\0') {
                continue;
            }

            if (!applySnapshotLine(line)) {
                LOG_WARNF("[UploadStateManager] Ignoring invalid snapshot line: %s", line);
            }
        }

        file.close();
        loadedSnapshot = true;
    }

    bool replayedJournal = replayJournal(sd);

    if (!loadedSnapshot && !replayedJournal) {
        LOG("[UploadStateManager] State snapshot/journal not found - will create on first save");
        return false;
    }

    LOG("[UploadStateManager] State v2 loaded successfully");
    LOG_DEBUGF("[UploadStateManager]   Completed folders: %u", completedCount);
    LOG_DEBUGF("[UploadStateManager]   Pending folders: %u", pendingCount);
    LOG_DEBUGF("[UploadStateManager]   Tracked files: %u", fileEntryCount);
    if (currentRetryFolderDay != 0) {
        char retryDay[16] = {0};
        dayKeyToChars(currentRetryFolderDay, retryDay, sizeof(retryDay));
        LOG_DEBUGF("[UploadStateManager]   Current retry folder: %s (attempt %d)", retryDay, currentRetryCount);
    }

    return true;
}

bool UploadStateManager::saveState(fs::FS &sd) {
    if (!flushJournal(sd)) {
        return false;
    }

    if (shouldCompact(sd)) {
        if (!compactState(sd)) {
            LOG("[UploadStateManager] ERROR: Failed to compact state snapshot");
            return false;
        }
    }

    return true;
}
