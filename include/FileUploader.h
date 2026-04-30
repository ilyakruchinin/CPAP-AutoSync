#ifndef FILE_UPLOADER_H
#define FILE_UPLOADER_H

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include "Config.h"
#include "UploadStateManager.h"
#include "ScheduleManager.h"
#include "WiFiManager.h"
#include "SDCardManager.h"

// Forward declaration to avoid circular dependency
#ifdef ENABLE_WEBSERVER
class CpapWebServer;
#endif

// Include uploader implementations based on feature flags
#ifdef ENABLE_SMB_UPLOAD
#include "SMBUploader.h"
#endif

#ifdef ENABLE_WEBDAV_UPLOAD
#include "WebDAVUploader.h"
#endif

#ifdef ENABLE_SLEEPHQ_UPLOAD
#include "SleepHQUploader.h"
#endif

// Which upload backend is active this session
enum class UploadBackend { NONE, SMB, CLOUD, DUAL };

// Result of an exclusive-access upload session
enum class UploadResult {
    COMPLETE,        // All eligible files uploaded
    TIMEOUT,         // X-minute timer expired (partial upload, not an error)
    ERROR,           // Upload failure
    NOTHING_TO_DO    // Pre-flight scan found no work for any backend — skip reboot, go to cooldown
};

// Filter for which data categories to upload
enum class DataFilter {
    FRESH_ONLY,  // Only fresh DATALOG + root/SETTINGS (mandatory)
    OLD_ONLY,    // Only old DATALOG folders + root/SETTINGS (mandatory)
    ALL_DATA     // Everything
};

class FileUploader {
private:
    Config* config;
    UploadStateManager* smbStateManager;    // tracks SMB-only uploads
    UploadStateManager* cloudStateManager;  // tracks Cloud-only uploads
    ScheduleManager* scheduleManager;
    WiFiManager* wifiManager;
    // Tracks which phase is currently running (for GUI status)
    UploadBackend currentPhase;

#ifdef ENABLE_WEBSERVER
    CpapWebServer* webServer;
#endif

    // Uploader instances
#ifdef ENABLE_SMB_UPLOAD
    SMBUploader* smbUploader;
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    SleepHQUploader* sleephqUploader;
#endif

    // File scanning (sm = state manager used for completed/pending checks)
    std::vector<String> scanDatalogFolders(fs::FS &sd, UploadStateManager* sm,
                                           bool includeCompleted = false);
    std::vector<String> scanFolderFiles(fs::FS &sd, const String& folderPath);
    std::vector<String> scanSettingsFiles(fs::FS &sd);

    // ── SMB pass helpers ────────────────────────────────────────────────────
    bool uploadMandatoryFilesSmb(class SDCardManager* sdManager, fs::FS &sd);
    bool uploadSingleFileSmb(class SDCardManager* sdManager, const String& filePath,
                             bool force = false);
    bool uploadDatalogFolderSmb(class SDCardManager* sdManager, const String& folderName);

    // ── Cloud pass helpers ───────────────────────────────────────────────────
    bool uploadDatalogFolderCloud(class SDCardManager* sdManager, const String& folderName);
    bool uploadSingleFileCloud(class SDCardManager* sdManager, const String& filePath,
                               bool force = false);

    // Helper: check if a DATALOG folder name (YYYYMMDD) is within the recent window
    bool isRecentFolder(const String& folderName) const;

    // Cloud import session management
    bool ensureCloudImport();
    void finalizeCloudImport(class SDCardManager* sdManager, fs::FS &sd);
    bool cloudImportCreated;
    bool cloudImportFailed;
    int  cloudDatalogFilesUploaded;  // DATALOG files uploaded this cloud pass; 0 = skip finalize

    // Return the "primary" state manager for web UI (prefers cloud if both exist)
    UploadStateManager* primaryStateManager() const {
        if (cloudStateManager) return cloudStateManager;
        return smbStateManager;
    }

public:
    // Allow main FSM to trigger pre-connections before SD acquisition
    SMBUploader* getSmbUploader() { return smbUploader; }
    SleepHQUploader* getCloudUploader() { return sleephqUploader; }

    FileUploader(Config* cfg, WiFiManager* wifi);
    ~FileUploader();

    bool begin();

    // Lightweight work probe — streaming directory check with no vector/String heap churn.
    // Used before creating the upload task so the no-work path avoids TLS allocation.
    // Single pass over /DATALOG consults both backends per folder, then publishes
    // a per-backend snapshot (universe, synced) into each state manager so the
    // dashboard can render a stable, meaningful "X / Y folders synced" ratio.
    //
    //   universe = folders within MAX_DAYS on the card (same value for both backends).
    //   synced   = of those, how many are fully in sync for THIS backend
    //              (completed AND, for recent folders, no .edf has changed).
    //   hasWork  = true when any in-window folder still needs work for this backend.
    struct WorkProbeResult {
        bool hasCloudWork;
        bool hasSmbWork;
        int  universe;        // Folders on card within MAX_DAYS window (-1 if no probe ran)
        int  cloudSynced;     // Fully-synced folders from `universe` for cloud (-1 if N/A)
        int  smbSynced;       // Fully-synced folders from `universe` for SMB   (-1 if N/A)
    };
    WorkProbeResult hasWorkToUpload(fs::FS &sd);

    // Full session: phased upload (CLOUD → SMB) with SD card mounted.
    // TLS connects on-demand in cloud phase — no pre-warm needed (arena protects heap).
    // Safety resetConnection() before SMB phase handles any lingering TLS.
    UploadResult runFullSession(class SDCardManager* sdManager, int maxMinutes,
                                DataFilter filter, bool reducedRetries = false);

    // Getters for internal components (for web interface access)
    UploadStateManager* getStateManager()    { return primaryStateManager(); }
    UploadStateManager* getSmbStateManager() { return smbStateManager; }
    UploadStateManager* getCloudStateManager() { return cloudStateManager; }
    ScheduleManager* getScheduleManager() { return scheduleManager; }
    UploadBackend getCurrentPhase() const { return currentPhase; }
    bool hasCloudBackend() const { return cloudStateManager != nullptr; }
    bool hasSmbBackend()   const { return smbStateManager   != nullptr; }
    bool hasBothBackends() const { return hasCloudBackend() && hasSmbBackend(); }
    bool hasIncompleteFolders() {
        // Use the work probe snapshot (universe vs synced) rather than
        // getIncompleteFoldersCount(), which relies on totalFoldersCount
        // being set during the upload phase's scanDatalogFolders() call.
        // When a backend fails to connect (e.g. SMB socket error),
        // scanDatalogFolders() never runs and totalFoldersCount stays 0,
        // making getIncompleteFoldersCount() falsely return 0.
        // The work probe always runs (pre- and post-upload) and has
        // accurate universe/synced counts.
        bool smbInc   = smbStateManager   && smbStateManager->hasBeenProbed()
                        && smbStateManager->getProbeSynced() < smbStateManager->getProbeUniverse();
        bool cloudInc = cloudStateManager && cloudStateManager->hasBeenProbed()
                        && cloudStateManager->getProbeSynced() < cloudStateManager->getProbeUniverse();
        return smbInc || cloudInc;
    }

#ifdef ENABLE_WEBSERVER
    void setWebServer(CpapWebServer* server) { webServer = server; }
#endif
};

#endif // FILE_UPLOADER_H
