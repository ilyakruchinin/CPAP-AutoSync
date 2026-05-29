#include "FileUploader.h"
#include "Logger.h"
#include "WebStatus.h"
#include <SD_MMC.h>
#include <LittleFS.h>
#include <functional>
#include <time.h>

#ifdef ENABLE_WEBSERVER
#include "CpapWebServer.h"
#endif

// Cooperative abort flag — set by web server when config lock is requested during upload
extern volatile bool g_abortUploadFlag;

// Constructor
FileUploader::FileUploader(Config* cfg, WiFiManager* wifiManager) 
    : config(cfg),
      smbStateManager(nullptr),
      cloudStateManager(nullptr),
      scheduleManager(nullptr),
      wifiManager(wifiManager),
      currentPhase(UploadBackend::NONE),
#ifdef ENABLE_WEBSERVER
      webServer(nullptr),
#endif
      cloudImportCreated(false),
      cloudImportFailed(false),
      cloudDatalogFilesUploaded(0)
#ifdef ENABLE_SMB_UPLOAD
      , smbUploader(nullptr)
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
      , sleephqUploader(nullptr)
#endif
{
}

// Destructor
FileUploader::~FileUploader() {
    if (smbStateManager)   delete smbStateManager;
    if (cloudStateManager) delete cloudStateManager;
    if (scheduleManager)   delete scheduleManager;
#ifdef ENABLE_SMB_UPLOAD
    if (smbUploader) delete smbUploader;
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (sleephqUploader) delete sleephqUploader;
#endif
}

// ============================================================================
// Minimal work probe — streaming, no vectors, no String heap churn
// ============================================================================
// Single pass over /DATALOG that, for every folder in the MAX_DAYS window,
// asks both state managers whether that folder is fully synced, needs work,
// or is irrelevant.  Used before creating the upload task so the no-work
// path avoids TLS allocation entirely.
//
// In addition to the boolean `hasCloudWork` / `hasSmbWork` decisions, the
// probe publishes a per-backend (universe, synced) snapshot into each state
// manager so the dashboard can render a stable "X / Y folders synced" ratio:
//
//   universe = folders in /DATALOG within MAX_DAYS (same across backends).
//   synced   = of those, how many are completed AND (for recent folders)
//              have no .edf that has changed since last upload — i.e. the
//              RECENT_FOLDER_DAYS re-discovery pass would be a no-op.
//
// No short-circuit: we enumerate every in-window folder.  In the common
// "no work" case this is identical to pre-existing behaviour (the old probe
// already walked everything).  In the "work found" case the enumeration
// continues rather than returning early; the extra SD hold time is tens to
// hundreds of ms per session and is trivially small relative to the upload
// itself, but the payoff is an always-accurate progress denominator.
//
// One enumeration covers both backends — previously we ran two passes.

FileUploader::WorkProbeResult FileUploader::hasWorkToUpload(fs::FS &sd) {
    WorkProbeResult result = {false, false, -1, -1, -1};

    // Active backends for this probe run.
    UploadStateManager* cloudSm = nullptr;
    UploadStateManager* smbSm   = nullptr;
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (config->hasCloudEndpoint()) cloudSm = cloudStateManager;
#endif
#ifdef ENABLE_SMB_UPLOAD
    if (config->hasSmbEndpoint())   smbSm   = smbStateManager;
#endif
    if (!cloudSm && !smbSm) {
        LOG("[WorkProbe] No backends configured — skipping probe");
        return result;
    }

    const bool canUploadOld = !scheduleManager || scheduleManager->canProcessOldData();

    // Calculate MAX_DAYS cutoff — same logic as scanDatalogFolders().
    // Supported MAX_DAYS range is 1-365 so we always expect a non-empty cutoff
    // once NTP is synced; if the clock is not yet set we fall back to "no
    // cutoff" and count every DATALOG folder on the card (self-heals on next
    // probe after NTP succeeds).
    String maxDaysCutoff = "";
    {
        int maxDays = config->getMaxDays();
        if (maxDays > 0) {
            time_t now = time(nullptr);
            if (now > 24 * 3600) {
                time_t cutoff = now - (maxDays * 86400L);
                struct tm cutoffTm;
                localtime_r(&cutoff, &cutoffTm);
                char cutoffStr[9];
                snprintf(cutoffStr, sizeof(cutoffStr), "%04d%02d%02d",
                         cutoffTm.tm_year + 1900, cutoffTm.tm_mon + 1, cutoffTm.tm_mday);
                maxDaysCutoff = String(cutoffStr);
            }
        }
    }

    // Lambda: check if a folder has any .edf file (streaming, no vector)
    auto folderHasEdf = [&](const String& folderPath) -> bool {
        File folder = sd.open(folderPath);
        if (!folder || !folder.isDirectory()) return false;
        File f = folder.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                const char* name = f.name();
                size_t len = strlen(name);
                if (len >= 4 && strcasecmp(name + len - 4, ".edf") == 0) {
                    f.close();
                    folder.close();
                    return true;
                }
            }
            f.close();
            f = folder.openNextFile();
        }
        folder.close();
        return false;
    };

    // Lambda: does any .edf in this folder report "changed" for the given state manager?
    // Used for the completed+recent case: even though the folder is marked done,
    // RECENT_FOLDER_DAYS re-discovery will re-upload it if a file has grown or
    // changed since last sync.
    auto folderHasChangedEdf = [&](const String& folderPath, UploadStateManager* sm) -> bool {
        if (!sm) return false;
        File folder = sd.open(folderPath);
        if (!folder || !folder.isDirectory()) return false;
        File f = folder.openNextFile();
        bool dirty = false;
        while (f) {
            if (!f.isDirectory()) {
                const char* fname = f.name();
                size_t flen = strlen(fname);
                if (flen >= 4 && strcasecmp(fname + flen - 4, ".edf") == 0) {
                    const char* base = strrchr(fname, '/');
                    String fullPath = folderPath + "/" + (base ? base + 1 : fname);
                    if (sm->hasFileChanged(sd, fullPath)) {
                        dirty = true;
                        f.close();
                        break;
                    }
                }
            }
            f.close();
            f = folder.openNextFile();
        }
        folder.close();
        return dirty;
    };

    // ── Single-pass enumeration of /DATALOG ────────────────────────────────
    // Per folder in the MAX_DAYS window, consult BOTH state managers so we
    // only pay one directory walk regardless of how many backends are on.

    int universe     = 0;
    int cloudSynced  = 0;
    int smbSynced    = 0;

    File root = sd.open("/DATALOG");
    if (!root || !root.isDirectory()) {
        LOG_WARN("[WorkProbe] /DATALOG not accessible");
        return result;
    }

    for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
        if (!entry.isDirectory()) { entry.close(); continue; }

        const char* rawName = entry.name();
        const char* slash = strrchr(rawName, '/');
        String folderName(slash ? slash + 1 : rawName);

        // MAX_DAYS filter — folders outside the window don't count toward the
        // universe and aren't considered for either backend.
        if (!maxDaysCutoff.isEmpty() && folderName < maxDaysCutoff) {
            entry.close();
            continue;
        }

        char path[64];
        snprintf(path, sizeof(path), "/DATALOG/%s", folderName.c_str());
        const String folderPath(path);
        const bool recent = isRecentFolder(folderName);

        // Look up completion state once per backend (memory-only lookup, cheap).
        const bool cloudCompleted = cloudSm && cloudSm->isFolderCompleted(folderName);
        const bool smbCompleted   = smbSm   && smbSm->isFolderCompleted(folderName);
        const bool anyCompleted   = cloudCompleted || smbCompleted;

        // Lazy `.edf` presence check — one SD read at most per folder, skipped
        // entirely when a backend already knows this folder is completed.
        bool edfChecked = false;
        bool hasEdf = false;
        auto checkEdf = [&]() -> bool {
            if (!edfChecked) { hasEdf = folderHasEdf(folderPath); edfChecked = true; }
            return hasEdf;
        };

        // ── Universe filter ─────────────────────────────────────────────────
        // A folder counts toward the shared denominator only when it is
        // visible to the user as "real data": either it has .edf content now,
        // or at least one backend has successfully uploaded it before (even
        // if the files have since been pruned).  Empty, never-uploaded stubs
        // — e.g. a DATALOG/YYYYMMDD directory the CPAP has pre-created for
        // tonight's session but not yet written to — are skipped so they
        // don't produce a phantom "1 left" that can't be acted on.
        if (!anyCompleted && !checkEdf()) {
            entry.close();
            continue;
        }

        universe++;

        // Evaluate sync status for one backend.  `hasWork` is an output
        // variable that gets OR'd; `syncedOut` is incremented when fully
        // in sync for this backend.
        auto evaluateBackend = [&](UploadStateManager* sm, bool completed,
                                    bool& hasWork, int& syncedOut) {
            if (!sm) return;

            if (completed) {
                if (recent) {
                    // Verify no appended/changed file since last upload.
                    if (folderHasChangedEdf(folderPath, sm)) {
                        hasWork = true;
                    } else {
                        syncedOut++;
                    }
                } else {
                    // Completed and outside recent window — permanently done.
                    syncedOut++;
                }
                return;
            }

            // Incomplete for this backend.  Work only counts if this backend
            // is allowed to upload the folder right now (old folders are
            // skipped outside the upload window).  In that window-closed
            // case the folder still legitimately belongs in the universe —
            // the other backend may have completed it, or it has content
            // waiting for the next window — so a "1 left" display is
            // correct even though this probe run reports no work.
            if (!recent && !canUploadOld) return;
            if (checkEdf()) {
                hasWork = true;
            }
            // Empty incomplete folders are neither work nor synced here;
            // they were filtered out of the universe above unless the other
            // backend had completed them, which is a meaningful divergence
            // worth surfacing to the user.
        };

        evaluateBackend(cloudSm, cloudCompleted, result.hasCloudWork, cloudSynced);
        evaluateBackend(smbSm,   smbCompleted,   result.hasSmbWork,   smbSynced);

        entry.close();
    }
    root.close();

    // ── Publish snapshot ───────────────────────────────────────────────────
    result.universe = universe;
    if (cloudSm) {
        result.cloudSynced = cloudSynced;
        cloudSm->setProbeSnapshot(universe, cloudSynced);
    }
    if (smbSm) {
        result.smbSynced = smbSynced;
        smbSm->setProbeSnapshot(universe, smbSynced);
    }

    LOGF("[WorkProbe] Result: cloud=%d smb=%d | universe=%d cloudSynced=%d smbSynced=%d (fh=%u ma=%u)",
         result.hasCloudWork, result.hasSmbWork,
         universe, cloudSynced, smbSynced,
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return result;
}

// Initialize all components and load upload state
bool FileUploader::begin() {
    LOG("[FileUploader] Initializing components...");

    fs::FS &stateFs = LittleFS;

    String endpointType = config->getEndpointType();
    LOGF("[FileUploader] Endpoint type: %s", endpointType.c_str());

    bool anyBackendCreated = false;

    // ── SMB uploader + state ─────────────────────────────────────────────────
#ifdef ENABLE_SMB_UPLOAD
    if (config->hasSmbEndpoint()) {
        smbUploader = new SMBUploader(
            config->getEndpoint(),
            config->getEndpointUser(),
            config->getEndpointPassword()
        );
        LOG("[FileUploader] SMBUploader created (will connect during upload)");

        smbStateManager = new UploadStateManager();
        smbStateManager->setPaths("/.upload_state.v2.smb", "/.upload_state.v2.smb.log");
        if (!smbStateManager->begin(stateFs)) {
            LOG("[FileUploader] WARNING: SMB state load failed, starting fresh");
        }
        anyBackendCreated = true;
    }
#endif

    // ── Cloud uploader + state ───────────────────────────────────────────────
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (config->hasCloudEndpoint()) {
        sleephqUploader = new SleepHQUploader(config);
        LOG("[FileUploader] SleepHQUploader created (will connect during upload)");

        cloudStateManager = new UploadStateManager();
        cloudStateManager->setPaths("/.upload_state.v2.cloud", "/.upload_state.v2.cloud.log");
        if (!cloudStateManager->begin(stateFs)) {
            LOG("[FileUploader] WARNING: Cloud state load failed, starting fresh");
        }
        anyBackendCreated = true;
    }
#endif

    if (!anyBackendCreated) {
        LOGF("[FileUploader] ERROR: No uploader created for endpoint type: %s", endpointType.c_str());
        return false;
    }

    // Populate GUI backend status — legacy single-name label.
    // The new per-backend UI no longer uses a "DUAL" sentinel; each backend
    // renders its own row from the `backends` block in /api/status.  This
    // name is kept for the header (e.g. "CLOUD" during phase 1, "SMB" during
    // phase 2) and for any external consumers of the legacy field.
    const char* mode = hasCloudBackend() ? "CLOUD" :
                       hasSmbBackend()   ? "SMB"   : "NONE";
    strncpy(g_activeBackendStatus.name, mode, sizeof(g_activeBackendStatus.name) - 1);
    g_activeBackendStatus.valid = anyBackendCreated;
    strncpy(g_inactiveBackendStatus.name, "NONE", sizeof(g_inactiveBackendStatus.name) - 1);
    g_inactiveBackendStatus.valid = false;

    // ── Schedule manager ─────────────────────────────────────────────────────
    scheduleManager = new ScheduleManager();
    scheduleManager->setNtpServer(config->getNtpServer());
    if (!scheduleManager->begin(
            config->getUploadMode(),
            config->getUploadStartHour(),
            config->getUploadEndHour(),
            config->getSmartStartHour(),
            config->getGmtOffsetHours(),
            config->getTzString(),
            config->getNtpServer())) {
        LOG("[FileUploader] ERROR: Failed to initialize ScheduleManager");
        return false;
    }
    UploadStateManager* sm = primaryStateManager();
    if (sm) scheduleManager->setLastUploadTimestamp(sm->getLastUploadTimestamp());

    LOG("[FileUploader] Initialization complete");
    return true;
}

// ============================================================================
// Phased dual-backend upload orchestrator
// ============================================================================
//
// Runs both backends sequentially in a single session:
//   Phase 1: CLOUD (TLS connects on-demand in begin() — TLS Arena ensures
//            mbedTLS buffers come from static .bss, not the general heap)
//   Phase 2: SMB   (TLS torn down, clean sockets, more heap for libsmb2)
//
// The minimal work probe in uploadTaskFunction() already confirmed work exists
// before this function is called. The pre-flight here still runs to determine
// per-backend work split and handle pending-folder promotion.
// Safety resetConnection() before Phase 2 prevents TLS/SMB socket conflicts.
//
// This eliminates backend cycling, prevents TLS/SMB socket conflicts, and
// ensures both backends make progress every session without reboots.
// ============================================================================

UploadResult FileUploader::runFullSession(SDCardManager* sdManager, int maxMinutes, DataFilter filter, bool reducedRetries) {
    fs::FS &sd = sdManager->getFS();
    fs::FS &stateFs = LittleFS;
    unsigned long sessionStart = millis();
    unsigned long maxMs = (unsigned long)maxMinutes * 60UL * 1000UL;

    // When both backends are configured, auto-scale time budget so each
    // backend gets at least the configured minutes.
    bool dual = hasBothBackends();
    if (dual) {
        maxMs *= 2;
        LOGF("[FileUploader] Session start: DUAL mode, maxMinutes=%d×2=%d filter=%d reducedRetries=%d",
             maxMinutes, maxMinutes * 2, (int)filter, reducedRetries);
    } else {
        const char* mode = hasCloudBackend() ? "CLOUD" : hasSmbBackend() ? "SMB" : "NONE";
        LOGF("[FileUploader] Session start: %s mode, maxMinutes=%d filter=%d reducedRetries=%d",
             mode, maxMinutes, (int)filter, reducedRetries);
    }

    if (!wifiManager || !wifiManager->isConnected()) {
        LOG_ERROR("[FileUploader] WiFi not connected - cannot upload");
        return UploadResult::ERROR;
    }
    if (!hasSmbBackend() && !hasCloudBackend()) {
        LOG_ERROR("[FileUploader] No backend configured");
        return UploadResult::ERROR;
    }

    // ── Pre-flight: check every configured backend for pending work ──────────
    bool smbWork   = false;
    bool cloudWork = false;
    {
        // Compute once: can we process old (non-recent) folders this session?
        // When false (smart mode outside upload window), skip old folders entirely
        // to avoid unnecessary SD card I/O scanning hundreds of DATALOG folders.
        bool canUploadOld = !scheduleManager || scheduleManager->canProcessOldData();

        // Calculate MAX_DAYS cutoff once for pre-flight
        String maxDaysCutoff = "";
        {
            int maxDays = config->getMaxDays();
            if (maxDays > 0) {
                time_t now = time(nullptr);
                if (now > 24 * 3600) {
                    time_t cutoff = now - (maxDays * 86400L);
                    struct tm cutoffTm;
                    localtime_r(&cutoff, &cutoffTm);
                    char cutoffStr[9];
                    snprintf(cutoffStr, sizeof(cutoffStr), "%04d%02d%02d",
                             cutoffTm.tm_year + 1900, cutoffTm.tm_mon + 1, cutoffTm.tm_mday);
                    maxDaysCutoff = String(cutoffStr);
                }
            }
        }

        auto preflightFolderHasWork = [&](UploadStateManager* sm) -> bool {
            File root = sd.open("/DATALOG");
            if (!root || !root.isDirectory()) return false;
            File entry = root.openNextFile();
            while (entry) {
                if (entry.isDirectory()) {
                    String name = String(entry.name());
                    int sl = name.lastIndexOf('/');
                    if (sl >= 0) name = name.substring(sl + 1);

                    // Apply MAX_DAYS filter (folder names are YYYYMMDD)
                    if (!maxDaysCutoff.isEmpty() && name < maxDaysCutoff) {
                        entry.close();
                        entry = root.openNextFile();
                        continue;
                    }

                    bool completed = sm->isFolderCompleted(name);
                    bool pending   = sm->isPendingFolder(name);
                    bool recent    = isRecentFolder(name);

                    // Fast path: skip old folders entirely when outside upload window.
                    // Only recent folders can produce work in this mode.  Pending-folder
                    // promotion (7-day timeout) is pure state management — still allowed.
                    if (!recent && !canUploadOld) {
                        // Still promote timed-out pending folders (no SD I/O needed)
                        if (!completed && pending) {
                            unsigned long currentTime = time(NULL);
                            if (currentTime >= 1000000000 &&
                                    sm->shouldPromotePendingToCompleted(name, currentTime)) {
                                sm->promotePendingToCompleted(name);
                                sm->save(stateFs);
                                LOGF("[FileUploader] Pre-flight: empty folder %s pending 7+ days — promoted to completed",
                                     name.c_str());
                            }
                        }
                        entry.close();
                        entry = root.openNextFile();
                        continue;
                    }

                    if (g_debugMode) {
                        LOGF("[FileUploader] Pre-flight scan: folder=%s completed=%d pending=%d recent=%d",
                             name.c_str(), completed, pending, recent);
                    }

                    if (!completed && !pending) {
                        String folderPath = "/DATALOG/" + name;
                        auto files = scanFolderFiles(sd, folderPath);
                        if (!files.empty()) {
                            LOGF("[FileUploader] Pre-flight: WORK — folder %s has %d file(s)",
                                 name.c_str(), (int)files.size());
                            entry.close(); root.close(); return true;
                        } else {
                            unsigned long currentTime = time(NULL);
                            if (currentTime >= 1000000000) {
                                sm->markFolderPending(name, currentTime);
                                sm->save(stateFs);
                                LOGF("[FileUploader] Pre-flight: empty folder %s — marked pending",
                                     name.c_str());
                            }
                        }
                    }
                    if (!completed && pending) {
                        String folderPath = "/DATALOG/" + name;
                        auto pendingFiles = scanFolderFiles(sd, folderPath);
                        if (!pendingFiles.empty()) {
                            LOGF("[FileUploader] Pre-flight: WORK — pending folder %s now has files",
                                     name.c_str());
                            entry.close(); root.close(); return true;
                        } else {
                            unsigned long currentTime = time(NULL);
                            if (currentTime >= 1000000000 &&
                                    sm->shouldPromotePendingToCompleted(name, currentTime)) {
                                sm->promotePendingToCompleted(name);
                                sm->save(stateFs);
                                LOGF("[FileUploader] Pre-flight: empty folder %s pending 7+ days — promoted to completed",
                                     name.c_str());
                            }
                        }
                    }
                    if (completed && recent) {
                        String folderPath = "/DATALOG/" + name;
                        auto files = scanFolderFiles(sd, folderPath);
                        for (const String& fp : files) {
                            String fullPath = folderPath + "/" + fp;
                            if (sm->hasFileChanged(sd, fullPath)) {
                                LOGF("[FileUploader] Pre-flight: WORK — file changed: %s",
                                     fullPath.c_str());
                                entry.close(); root.close(); return true;
                            }
                        }
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
            return false;
        };

        auto checkHasWork = [&](UploadStateManager* sm) -> bool {
            if (!sm) return false;
            return preflightFolderHasWork(sm);
        };

#ifdef ENABLE_SMB_UPLOAD
        if (config->hasSmbEndpoint())   smbWork   = checkHasWork(smbStateManager);
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
        if (config->hasCloudEndpoint()) cloudWork = checkHasWork(cloudStateManager);
#endif
    }

    if (!smbWork && !cloudWork) {
        LOG("[FileUploader] Pre-flight: no work for any backend — skipping session");
        return UploadResult::NOTHING_TO_DO;
    }

    LOGF("[FileUploader] Pre-flight: smb_work=%d cloud_work=%d", smbWork, cloudWork);

    cloudImportCreated = false;
    cloudImportFailed  = false;

    bool timerExpired = false;
    bool sessionHadFailure = false;  // Track if any folder upload failed this session
    auto isTimerExpired = [&]() -> bool {
        return (millis() - sessionStart) >= maxMs;
    };

    bool needFresh = (filter == DataFilter::FRESH_ONLY || filter == DataFilter::ALL_DATA);
    bool needOld   = (filter == DataFilter::OLD_ONLY   || filter == DataFilter::ALL_DATA);

    // Time budget: when both backends have work, split time evenly.
    // cloudDeadline marks when cloud phase must yield to SMB phase.
    unsigned long cloudDeadline = 0;
    if (cloudWork && smbWork) {
        cloudDeadline = sessionStart + (maxMs / 2);
    } else if (cloudWork) {
        cloudDeadline = sessionStart + maxMs;
    }
    auto isCloudTimeBudgetExpired = [&]() -> bool {
        return cloudDeadline > 0 && millis() >= cloudDeadline;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // PHASE 1: CLOUD (TLS connects on-demand in begin() — no pre-warm)
    // ═══════════════════════════════════════════════════════════════════════
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (cloudWork && sleephqUploader && cloudStateManager) {
        currentPhase = UploadBackend::CLOUD;
        strncpy(g_activeBackendStatus.name, "CLOUD", sizeof(g_activeBackendStatus.name) - 1);
        LOG("[FileUploader] === Phase 1: Cloud Session ===");
        cloudDatalogFilesUploaded = 0;

        std::vector<String> freshFolders, oldFolders;
        if (needFresh || needOld) {
            std::vector<String> all = scanDatalogFolders(sd, cloudStateManager);
            for (const String& f : all) {
                if (isRecentFolder(f)) freshFolders.push_back(f);
                else                   oldFolders.push_back(f);
            }
            LOGF("[FileUploader] Cloud scan: %d fresh, %d old folders",
                 (int)freshFolders.size(), (int)oldFolders.size());
        }

        bool cloudHasWork = !freshFolders.empty() ||
                       (!oldFolders.empty() && scheduleManager && scheduleManager->canProcessOldData());

        if (!cloudHasWork) {
            LOG("[FileUploader] Cloud: nothing to upload — skipping auth + import");
        } else {
            LOGF("[FileUploader] Heap before cloud begin: fh=%u ma=%u",
                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            if (!sleephqUploader->isConnected()) {
                if (!sleephqUploader->begin()) {
                    LOG_ERROR("[FileUploader] Cloud init failed — skipping cloud phase");
                    cloudImportFailed = true;
                    sessionHadFailure = true;
                } else {
                    cloudImportCreated = true;
                    LOGF("[FileUploader] Cloud session ready — heap: fh=%u ma=%u",
                         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
                }
            } else {
                if (!cloudImportCreated && !cloudImportFailed) {
                    if (!sleephqUploader->createImport()) {
                        cloudImportFailed = true;
                        sessionHadFailure = true;
                    }
                    else                                  cloudImportCreated = true;
                }
            }

            if (!cloudImportFailed) {
                auto runCloudFolder = [&](const String& folder) -> bool {
                    if (isTimerExpired() || isCloudTimeBudgetExpired()) { timerExpired = true; return false; }
                    if (!uploadDatalogFolderCloud(sdManager, folder)) sessionHadFailure = true;
#ifdef ENABLE_WEBSERVER
                    if (webServer) webServer->handleClient();
#endif
                    return true;
                };
                if (!timerExpired && needFresh) {
                    LOG("[FileUploader] Cloud: Fresh DATALOG folders");
                    for (const String& folder : freshFolders) {
                        if (!runCloudFolder(folder)) break;
                    }
                }
                if (!timerExpired && needOld && scheduleManager && scheduleManager->canProcessOldData()) {
                    LOG("[FileUploader] Cloud: Old DATALOG folders");
                    for (const String& folder : oldFolders) {
                        if (!runCloudFolder(folder)) break;
                    }
                }
                if (cloudImportCreated && cloudDatalogFilesUploaded > 0) {
                    LOGF("[FileUploader] Finalizing import: %d DATALOG files", cloudDatalogFilesUploaded);
                    finalizeCloudImport(sdManager, sd);
                } else if (cloudImportCreated && cloudDatalogFilesUploaded == 0) {
                    LOG("[FileUploader] No new DATALOG files — skipping import finalize");
                }
            }
        }
        cloudStateManager->save(stateFs);

        // Release TLS resources — frees ~40KB heap for SMB phase and clears
        // the lwIP socket table to prevent errno:9 on libsmb2 connects.
        sleephqUploader->resetConnection();
        LOGF("[FileUploader] Released TLS resources after cloud phase (fh=%u ma=%u)",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        delay(100);  // lwIP socket cleanup

        g_cloudSessionStatus.uploadActive     = false;
        g_cloudSessionStatus.filesUploaded    = 0;
        g_cloudSessionStatus.filesTotal       = 0;
        g_cloudSessionStatus.currentFolder[0] = '\0';

        // Mid-session snapshot refresh — publish fresh per-backend counts
        // now that the Cloud phase is done, so the dashboard Cloud row
        // updates immediately instead of waiting for the (potentially
        // multi-minute, possibly failing) SMB phase to finish.  SD card
        // is still mounted; the probe adds ~100–200 ms and reuses the
        // same logic that produced the pre-session snapshot, so partial
        // Cloud success (some folders synced, others left for retry) is
        // reported correctly.  The SMB row's snapshot also gets
        // refreshed, which is desirable — any folder Cloud completed is
        // still "not synced" for SMB until SMB uploads it too.
        if (smbWork) {
            LOG("[FileUploader] Post-cloud snapshot refresh (pre-SMB)");
            hasWorkToUpload(sd);
        }

        // Reset timer flag for SMB phase — cloud budget expiry shouldn't block SMB
        timerExpired = false;
    }
#endif

    // ═══════════════════════════════════════════════════════════════════════
    // PHASE 2: SMB (TLS torn down, clean sockets, more heap for libsmb2)
    // ═══════════════════════════════════════════════════════════════════════
    // Safety: ensure TLS is released before SMB starts.  When TLS is
    // pre-warmed (before SD mount for clean heap) but cloud pre-flight
    // finds no work, the normal cloud-phase cleanup is skipped.  The
    // lingering lwIP socket conflicts with libsmb2's TCP socket (errno:9).
    // resetConnection() is safe to call when already disconnected.
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (smbWork && sleephqUploader) {
        // Always release TLS resources, even if the connection died silently
        // during pre-flight scanning.  When the server closes an idle
        // pre-warmed connection, isConnected() returns false but mbedTLS
        // internal buffers (~32 KB) may still be allocated — fragmenting the
        // heap and starving lwIP during the SMB phase.
        // resetConnection() is safe to call when already disconnected.
        LOG("[FileUploader] Releasing TLS resources before SMB phase");
        sleephqUploader->resetConnection();
        delay(100);  // lwIP socket cleanup
    }
#endif
#ifdef ENABLE_SMB_UPLOAD
    if (smbWork && smbUploader && smbStateManager) {
        currentPhase = UploadBackend::SMB;
        strncpy(g_activeBackendStatus.name, "SMB", sizeof(g_activeBackendStatus.name) - 1);
        LOG("[FileUploader] === Phase 2: SMB Session ===");
        
        // Reduce retries outside scheduled hours to minimize SD card hold time
        if (reducedRetries && smbUploader) {
            smbUploader->setMaxUploadAttempts(1);
        }

        // ── Persistent SMB session for the entire Phase 2 ────────────────────
        // Open the SMB session once here and keep it open until all folders
        // and mandatory files are processed. This eliminates the per-folder
        // NEGPROT + SESSION_SETUP + TREE_CONNECT round-trip (~1–2 s each on
        // a typical LAN, 45–90 s across a 43-folder backlog) and, equally
        // importantly, avoids cycling the ~4–5 KB smb2_context allocation
        // which was a significant source of heap fragmentation.
        //
        // Opening the session before allocating the upload buffer also lets
        // us query the server's negotiated max_write_size and right-size the
        // buffer, so each smb2_write carries the largest payload the server
        // will accept (typically 1 MB on Samba/Windows, 64 KB on legacy).
        if (!smbUploader->begin()) {
            LOG_ERROR("[FileUploader] Failed to open persistent SMB session — skipping SMB phase");
            smbStateManager->save(stateFs);
            g_smbSessionStatus.uploadActive     = false;
            g_smbSessionStatus.filesUploaded    = 0;
            g_smbSessionStatus.filesTotal       = 0;
            g_smbSessionStatus.currentFolder[0] = '\0';
            currentPhase = UploadBackend::NONE;
            sessionHadFailure = true;
            goto smb_phase_done;
        }

        // Adaptive buffer sizing + SMB2 write pipelining (Phase B+C):
        //   policyMax  — 32 KB upper bound per write. Bigger writes rarely add
        //                throughput because a single 32 KB write already exceeds
        //                a full 64 KB TCP window round-trip on a LAN, and
        //                larger allocations strain the ESP32 small-block heap.
        //   serverMax  — server's negotiated max_write_size (0 if not reported).
        //   heapBand   — derived from ESP.getMaxAllocHeap(); top band extended
        //                from the old 8 KB ceiling to 32 KB when headroom allows.
        //   depth      — number of SMB2 writes to keep in flight simultaneously.
        //                1 = stop-and-wait (legacy). 2 = pipelined (credit-based
        //                overlap on a healthy LAN, ~1.7–2× throughput at the
        //                cost of a second slot in the upload buffer).
        // Final per-slot size is min(policyMax, serverMax, heapBand).
        // Total buffer = depth * slotSize; pipeline is enabled only when the
        // heap has headroom for the doubled allocation.
        uint32_t currentMa     = ESP.getMaxAllocHeap();
        uint32_t serverMax     = smbUploader->getNegotiatedMaxWriteSize();
        constexpr uint32_t POLICY_MAX = 32 * 1024;
        size_t heapBand = (currentMa > 80000) ? 32768 :
                          (currentMa > 60000) ? 16384 :
                          (currentMa > 40000) ? 12288 :
                          (currentMa > 30000) ? 8192  :
                          (currentMa > 20000) ? 4096  :
                          (currentMa > 15000) ? 2048  : 1024;
        size_t smbBufSize = heapBand;
        if (smbBufSize > POLICY_MAX) smbBufSize = POLICY_MAX;
        if (serverMax > 0 && smbBufSize > serverMax) smbBufSize = serverMax;

        // Pipelining requires (a) ≥ 8 KB per slot to amortise the per-write
        // PDU overhead, (b) ≥ ~80 KB contiguous heap so the 2 × slotSize
        // allocation succeeds with breathing room for libsmb2's internal PDU
        // buffers, and (c) the server to speak a modern dialect with
        // multi-credit support (anything that reports max_write_size > 64 KB).
        int desiredDepth = 1;
        if (smbBufSize >= 8192 && currentMa > 80000 &&
            (serverMax == 0 || serverMax >= 65536)) {
            desiredDepth = 2;
        }

        LOGF("[FileUploader] SMB phase heap: fh=%u ma=%u, serverMaxWrite=%u, slot=%u, depth=%d",
             (unsigned)ESP.getFreeHeap(),
             (unsigned)currentMa,
             (unsigned)serverMax,
             (unsigned)smbBufSize,
             desiredDepth);

        if (!smbUploader->allocateBuffer(smbBufSize, desiredDepth)) {
            // Pipelined allocation failed — retry single-slot once before giving up.
            if (desiredDepth > 1) {
                LOG_WARN("[FileUploader] Pipelined SMB buffer alloc failed, falling back to serial");
                desiredDepth = 1;
            }
            if (desiredDepth == 1 ? !smbUploader->allocateBuffer(smbBufSize, 1) : true) {
                LOG_ERROR("[FileUploader] Failed to allocate SMB buffer — skipping SMB phase");
                smbStateManager->save(stateFs);
                smbUploader->end();
                g_smbSessionStatus.uploadActive     = false;
                g_smbSessionStatus.filesUploaded    = 0;
                g_smbSessionStatus.filesTotal       = 0;
                g_smbSessionStatus.currentFolder[0] = '\0';
                currentPhase = UploadBackend::NONE;
                goto smb_phase_done;
            }
        }
        {

            std::vector<String> freshFolders, oldFolders;
            if (needFresh || needOld) {
                std::vector<String> all = scanDatalogFolders(sd, smbStateManager);
                for (const String& f : all) {
                    if (isRecentFolder(f)) freshFolders.push_back(f);
                    else                   oldFolders.push_back(f);
                }
                LOGF("[FileUploader] SMB scan: %d fresh, %d old folders",
                     (int)freshFolders.size(), (int)oldFolders.size());
            }

            bool mandatoryChanged = false;
            {
                static const char* rootPaths[] = {
                    "/Identification.json", "/Identification.crc",
                    "/Identification.tgt",  "/STR.edf"
                };
                for (const char* p : rootPaths) {
                    if (sd.exists(p) && smbStateManager->hasFileChanged(sd, String(p))) {
                        mandatoryChanged = true; break;
                    }
                }
                if (!mandatoryChanged) {
                    for (const String& fp : scanSettingsFiles(sd)) {
                        if (smbStateManager->hasFileChanged(sd, fp)) {
                            mandatoryChanged = true; break;
                        }
                    }
                }
            }

            bool smbHasWork = !freshFolders.empty() ||
                           (!oldFolders.empty() && scheduleManager && scheduleManager->canProcessOldData()) ||
                           mandatoryChanged;

            if (!smbHasWork) {
                LOG("[FileUploader] SMB: nothing to upload — skipping");
            } else {
                if (!isTimerExpired()) uploadMandatoryFilesSmb(sdManager, sd);

                if (!timerExpired && needFresh) {
                    LOG("[FileUploader] SMB: Fresh DATALOG folders");
                    for (const String& folder : freshFolders) {
                        if (isTimerExpired()) { timerExpired = true; break; }
                        if (!uploadDatalogFolderSmb(sdManager, folder)) {
                            sessionHadFailure = true;
                            if (reducedRetries) {
                                LOG_WARN("[FileUploader] SMB folder failed — aborting session (reduced retries)");
                                break;
                            }
                        }
#ifdef ENABLE_WEBSERVER
                        if (webServer) webServer->handleClient();
#endif
                    }
                }
                if (!timerExpired && !sessionHadFailure && needOld && scheduleManager && scheduleManager->canProcessOldData()) {
                    LOG("[FileUploader] SMB: Old DATALOG folders");
                    for (const String& folder : oldFolders) {
                        if (isTimerExpired()) { timerExpired = true; break; }
                        if (!uploadDatalogFolderSmb(sdManager, folder)) {
                            sessionHadFailure = true;
                            if (reducedRetries) {
                                LOG_WARN("[FileUploader] SMB folder failed — aborting session (reduced retries)");
                                break;
                            }
                        }
#ifdef ENABLE_WEBSERVER
                        if (webServer) webServer->handleClient();
#endif
                    }
                }
            }
            // ── End persistent SMB session (single teardown for whole phase) ──
            if (smbUploader->isConnected()) smbUploader->end();
            smbStateManager->save(stateFs);

            // Free SMB buffer to recover heap for next session
            smbUploader->freeBuffer();
        }

        g_smbSessionStatus.uploadActive     = false;
        g_smbSessionStatus.filesUploaded    = 0;
        g_smbSessionStatus.filesTotal       = 0;
        g_smbSessionStatus.currentFolder[0] = '\0';
    }
smb_phase_done:
    ;
#endif

    currentPhase = UploadBackend::NONE;
    // Restore GUI status to show configured primary backend (legacy label).
    // The per-backend UI reads the `backends` block directly; this name is
    // only consumed by the legacy header and external clients.
    const char* mode = hasCloudBackend() ? "CLOUD" : hasSmbBackend() ? "SMB" : "NONE";
    strncpy(g_activeBackendStatus.name, mode, sizeof(g_activeBackendStatus.name) - 1);

    // ── Determine result ──────────────────────────────────────────────────────
    unsigned long elapsed = millis() - sessionStart;

    // Log stack high-water mark for tuning task stack size
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    LOGF("[FileUploader] Session ended: %lu seconds, stack HWM=%u bytes free",
         elapsed / 1000, (unsigned)(hwm * sizeof(StackType_t)));

    if (timerExpired && hasIncompleteFolders()) {
        LOG("[FileUploader] Timer expired with incomplete folders (TIMEOUT)");
        return UploadResult::TIMEOUT;
    }

    if (!hasIncompleteFolders() && !sessionHadFailure) {
        time_t endNow; time(&endNow);
        // Update the last-upload timestamp on every configured backend.
        // hasIncompleteFolders() checks both backends, so reaching here means
        // both have no pending work — each backend legitimately finished
        // this session.  Previously only the primary (cloud) got stamped,
        // which left the NAS (SMB) row showing a stale "Last upload: N days
        // ago" in the dashboard even after a successful phased session.
        if (cloudStateManager) {
            cloudStateManager->setLastUploadTimestamp((unsigned long)endNow);
            cloudStateManager->save(stateFs);
        }
        if (smbStateManager) {
            smbStateManager->setLastUploadTimestamp((unsigned long)endNow);
            smbStateManager->save(stateFs);
        }
        if (scheduleManager)   scheduleManager->setLastUploadTimestamp((unsigned long)endNow);
        if (scheduleManager)   scheduleManager->markDayCompleted();
        LOG("[FileUploader] All folders complete — session done");
        return UploadResult::COMPLETE;
    }

    if (sessionHadFailure) {
        LOG("[FileUploader] Session had folder upload failure(s) — not marking day complete");
    }

    return UploadResult::TIMEOUT;
}


// Scan DATALOG folders and sort by date (newest first)
std::vector<String> FileUploader::scanDatalogFolders(fs::FS &sd, UploadStateManager* sm,
                                                      bool includeCompleted) {
    std::vector<String> folders;
    int eligibleFolderCount = 0;
    
    File root = sd.open("/DATALOG");
    if (!root) {
        LOG_ERROR("[FileUploader] Cannot open /DATALOG folder");
        LOG_ERROR("[FileUploader] SD card may be in use by CPAP or not properly mounted");
        LOG_ERROR("[FileUploader] If DATALOG exists, this scan will be retried");
        return folders;  // Return empty - indicates scan failure
    }
    
    if (!root.isDirectory()) {
        LOG_ERROR("[FileUploader] /DATALOG exists but is not a directory");
        root.close();
        return folders;
    }
    
    // Calculate MAX_DAYS cutoff date if configured
    String maxDaysCutoff = "";
    int maxDays = config->getMaxDays();
    if (maxDays > 0) {
        time_t now = time(nullptr);
        if (now > 24 * 3600) {  // Valid NTP time
            time_t cutoff = now - (maxDays * 86400L);
            struct tm cutoffTm;
            localtime_r(&cutoff, &cutoffTm);
            char cutoffStr[9];
            snprintf(cutoffStr, sizeof(cutoffStr), "%04d%02d%02d",
                     cutoffTm.tm_year + 1900, cutoffTm.tm_mon + 1, cutoffTm.tm_mday);
            maxDaysCutoff = String(cutoffStr);
            LOGF("[FileUploader] MAX_DAYS=%d: only processing folders >= %s", maxDays, cutoffStr);
        } else {
            LOG_WARN("[FileUploader] MAX_DAYS configured but NTP time not available, processing all folders");
        }
    }
    
    // Scan for folders
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            String folderName = String(file.name());
            
            // Extract just the folder name (remove path prefix if present)
            int lastSlash = folderName.lastIndexOf('/');
            if (lastSlash >= 0) {
                folderName = folderName.substring(lastSlash + 1);
            }
            
            // Apply MAX_DAYS filter (folder names are in YYYYMMDD format)
            if (!maxDaysCutoff.isEmpty() && folderName < maxDaysCutoff) {
                LOG_DEBUGF("[FileUploader] Skipping old folder (MAX_DAYS): %s", folderName.c_str());
                file.close();
                file = root.openNextFile();
                continue;
            }

            // Count all eligible DATALOG folders (completed, incomplete, and empty-pending)
            // so progress can report remaining data folders across cooldown cycles.
            eligibleFolderCount++;
            
            // Check if folder is already completed
            if (sm->isFolderCompleted(folderName)) {
                if (includeCompleted) {
                    // For delta/deep scans, include completed folders
                    folders.push_back(folderName);
                    LOG_INFOF("[FileUploader] Found completed DATALOG folder: %s", folderName.c_str());
                } else if (isRecentFolder(folderName)) {
                    // Recent completed folders are always rescanned — CPAP may have added
                    // or extended files. Per-file size tracking (hasFileChanged) skips
                    // unchanged files so only new/modified data is re-uploaded.
                    folders.push_back(folderName);
                    LOG_DEBUGF("[FileUploader] Recent completed folder — rescanning: %s", folderName.c_str());
                } else {
                    LOG_DEBUGF("[FileUploader] Skipping completed folder: %s", folderName.c_str());
                }
            } else if (sm->isPendingFolder(folderName)) {
                // Check if pending folder now has files (was empty but now has content)
                String folderPath = "/DATALOG/" + folderName;
                std::vector<String> folderFiles = scanFolderFiles(sd, folderPath);
                
                if (!folderFiles.empty()) {
                    // Folder now has files - remove from pending state immediately and process normally
                    LOG_DEBUGF("[FileUploader] Pending folder now has files, removing from pending: %s", folderName.c_str());
                    sm->removeFolderFromPending(folderName);
                    folders.push_back(folderName);
                } else {
                    // Still empty - check if pending folder has timed out
                    unsigned long currentTime = time(NULL);
                    if (currentTime >= 1000000000 && sm->shouldPromotePendingToCompleted(folderName, currentTime)) {
                        // Timed out pending folder - include in scan for promotion
                        folders.push_back(folderName);
                        LOG_DEBUGF("[FileUploader] Found timed-out pending folder: %s", folderName.c_str());
                    } else {
                        // Still pending, skip for now
                        LOG_DEBUGF("[FileUploader] Skipping pending folder (within 7-day window): %s", folderName.c_str());
                    }
                }
            } else {
                // Regular incomplete folder
                folders.push_back(folderName);
                LOG_DEBUGF("[FileUploader] Found incomplete DATALOG folder: %s", folderName.c_str());
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    
    // Sort folders by date (newest first) - folders are in YYYYMMDD format
    std::sort(folders.begin(), folders.end(), [](const String& a, const String& b) {
        return a > b;  // Descending order (newest first)
    });
    
    if (folders.empty()) {
        LOG("[FileUploader] No incomplete DATALOG folders found");
        LOG_DEBUG("[FileUploader] Either all folders are uploaded or DATALOG is empty");
    } else {
        LOG_DEBUGF("[FileUploader] Found %d incomplete DATALOG folders", folders.size());
    }

    if (sm) sm->setTotalFoldersCount(eligibleFolderCount);
    
    return folders;
}

// Scan files in a specific folder
// Returns empty vector on error - caller must check if scan was successful
std::vector<String> FileUploader::scanFolderFiles(fs::FS &sd, const String& folderPath) {
    std::vector<String> files;
    
    File folder = sd.open(folderPath);
    if (!folder) {
        LOG_ERRORF("[FileUploader] Failed to open folder: %s", folderPath.c_str());
        LOG_ERROR("[FileUploader] SD card may be in use by CPAP or experiencing read errors");
        LOG_ERROR("[FileUploader] This folder will be retried in the next upload session");
        return files;  // Return empty - caller should treat as error
    }
    
    if (!folder.isDirectory()) {
        LOG_ERRORF("[FileUploader] Path exists but is not a directory: %s", folderPath.c_str());
        folder.close();
        return files;
    }
    
    // Scan for .edf files
    File file = folder.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String fileName = String(file.name());
            
            // Extract just the file name (remove path prefix if present)
            int lastSlash = fileName.lastIndexOf('/');
            if (lastSlash >= 0) {
                fileName = fileName.substring(lastSlash + 1);
            }
            
            // Check if it's an .edf file
            if (fileName.endsWith(".edf") || fileName.endsWith(".EDF")) {
                files.push_back(fileName);
            }
        }
        file.close();
        file = folder.openNextFile();
    }
    folder.close();
    
    LOG_DEBUGF("[FileUploader] Found %d .edf files in %s", files.size(), folderPath.c_str());
    
    return files;
}

// Scan all SETTINGS files (change-checking is left to the upload method)
std::vector<String> FileUploader::scanSettingsFiles(fs::FS &sd) {
    std::vector<String> files;
    File settingsDir = sd.open("/SETTINGS");
    if (settingsDir && settingsDir.isDirectory()) {
        File settingsFile = settingsDir.openNextFile();
        while (settingsFile) {
            if (!settingsFile.isDirectory()) {
                String name = String(settingsFile.name());
                int lastSlash = name.lastIndexOf('/');
                if (lastSlash >= 0) name = name.substring(lastSlash + 1);
                files.push_back("/SETTINGS/" + name);
            }
            settingsFile.close();
            settingsFile = settingsDir.openNextFile();
        }
        settingsDir.close();
    }
    return files;
}

// Check if a DATALOG folder name (YYYYMMDD) is within the recent window
bool FileUploader::isRecentFolder(const String& folderName) const {
    int recentDays = config->getRecentFolderDays();
    if (recentDays <= 0) return false;
    
    time_t now = time(nullptr);
    if (now < 24 * 3600) return false;  // NTP not synced
    
    time_t cutoff = now - ((long)recentDays * 86400L);
    struct tm cutoffTm;
    localtime_r(&cutoff, &cutoffTm);
    char cutoffStr[9];
    snprintf(cutoffStr, sizeof(cutoffStr), "%04d%02d%02d",
             cutoffTm.tm_year + 1900, cutoffTm.tm_mon + 1, cutoffTm.tm_mday);
    
    return folderName >= String(cutoffStr);
}

// Lazily create a cloud import session on first actual upload
// Returns true if import is ready (already created or just created)
bool FileUploader::ensureCloudImport() {
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (cloudImportCreated) return true;
    if (cloudImportFailed) return false;  // Already failed this session, don't retry
    if (!sleephqUploader || !config->hasCloudEndpoint()) return true;  // No cloud = OK
    
    if (!sleephqUploader->isConnected()) {
        LOG("[FileUploader] Connecting cloud uploader for import session...");
        if (!sleephqUploader->begin()) {
            LOG_ERROR("[FileUploader] Failed to initialize cloud uploader");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            cloudImportFailed = true;
            return false;
        }
    }
    if (sleephqUploader->isConnected()) {
        if (!sleephqUploader->createImport()) {
            LOG_ERROR("[FileUploader] Failed to create cloud import");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            cloudImportFailed = true;
            return false;
        }
        cloudImportCreated = true;
    }
    return cloudImportCreated;
#else
    return true;
#endif
}


// Finalize current cloud import: upload mandatory files, process, reset for next folder
void FileUploader::finalizeCloudImport(SDCardManager* sdManager, fs::FS &sd) {
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (!cloudImportCreated || !sleephqUploader || !config->hasCloudEndpoint()) return;

    LOG("[FileUploader] Finalizing cloud import with mandatory files...");

    const char* rootPaths[] = {
        "/Identification.json", "/Identification.crc", "/Identification.tgt", "/STR.edf"
    };
    for (const char* path : rootPaths) {
        if (sd.exists(path)) uploadSingleFileCloud(sdManager, String(path), true);
    }
    std::vector<String> settingsFiles = scanSettingsFiles(sd);
    for (const String& filePath : settingsFiles) {
        uploadSingleFileCloud(sdManager, filePath, true);
    }

    if (!sleephqUploader->getCurrentImportId().isEmpty()) {
        if (!sleephqUploader->processImport()) {
            LOG_WARN("[FileUploader] Failed to process cloud import for this folder");
        }
    }

    cloudImportCreated = false;
    cloudImportFailed  = false;

    if (!sleephqUploader->isTlsAlive()) {
        sleephqUploader->resetConnection();
        LOG("[FileUploader] Connection lost, TLS memory freed for next folder");
    } else {
        LOG("[FileUploader] Import cycle complete, connection kept alive for next folder");
    }
#endif
}


// ============================================================================
// Shared helper: handle empty folder state (pending/promote). Uses provided sm.
// Returns true if caller should return true (no files but handled).
// Returns false if caller should return false (error).
// Sets filesOut to the file list on success.
// ============================================================================
static bool handleFolderScan(fs::FS &sd, fs::FS &stateFs, const String& folderName, const String& folderPath,
                              UploadStateManager* sm,
                              std::vector<String>& filesOut,
                              std::function<std::vector<String>(fs::FS&, const String&)> scanFn) {
    File folderCheck = sd.open(folderPath);
    if (!folderCheck) {
        LOG_ERRORF("[FileUploader] Cannot access folder: %s", folderPath.c_str());
        return false;
    }
    if (!folderCheck.isDirectory()) {
        LOG_ERRORF("[FileUploader] Path is not a directory: %s", folderPath.c_str());
        folderCheck.close();
        return false;
    }
    folderCheck.close();

    filesOut = scanFn(sd, folderPath);

    if (sm->isPendingFolder(folderName) && !filesOut.empty()) {
        sm->removeFolderFromPending(folderName);
    }

    if (filesOut.empty()) {
        File vf = sd.open(folderPath);
        if (!vf) return false;
        vf.close();
        LOG_WARN("[FileUploader] No .edf files found in folder (folder is empty)");
        unsigned long currentTime = time(NULL);
        if (currentTime < 1000000000) { return false; }
        if (sm->isPendingFolder(folderName)) {
            if (sm->shouldPromotePendingToCompleted(folderName, currentTime)) {
                sm->promotePendingToCompleted(folderName);
                sm->save(stateFs);
            }
        } else {
            sm->markFolderPending(folderName, currentTime);
            sm->save(stateFs);
        }
        return true;  // "done" for this folder (empty)
    }
    return true;  // filesOut populated
}

// ============================================================================
// SMB PASS: upload all DATALOG files for one folder
// ============================================================================
bool FileUploader::uploadDatalogFolderSmb(SDCardManager* sdManager, const String& folderName) {
#ifndef ENABLE_SMB_UPLOAD
    return true;
#else
    if (!smbUploader || !smbStateManager) return false;
    fs::FS &sd = sdManager->getFS();
    fs::FS &stateFs = LittleFS;

    LOGF("[FileUploader] [SMB] Uploading DATALOG folder: %s", folderName.c_str());
    String folderPath = "/DATALOG/" + folderName;

    std::vector<String> files;
    if (!handleFolderScan(sd, stateFs, folderName, folderPath, smbStateManager, files,
            [this](fs::FS& sd2, const String& fp) { return scanFolderFiles(sd2, fp); })) {
        return false;
    }
    if (files.empty()) return true;  // empty folder handled

    bool isRecent     = isRecentFolder(folderName);
    bool isRescan     = smbStateManager->isFolderCompleted(folderName) && isRecent;

    g_smbSessionStatus.uploadActive = true;
    strncpy((char*)g_smbSessionStatus.currentFolder, folderName.c_str(),
            sizeof(g_smbSessionStatus.currentFolder) - 1);
    ((char*)g_smbSessionStatus.currentFolder)[sizeof(g_smbSessionStatus.currentFolder) - 1] = '\0';
    g_smbSessionStatus.filesTotal    = (int)files.size();
    g_smbSessionStatus.filesUploaded = 0;

    int uploadedCount    = 0;
    int skippedUnchanged = 0;
    int skippedEmpty     = 0;

    for (const String& fileName : files) {
        String localPath  = folderPath + "/" + fileName;
        if (isRescan) {
            if (!smbStateManager->hasFileChanged(sd, localPath)) { skippedUnchanged++; continue; }
            LOG_DEBUGF("[FileUploader] [SMB] File changed: %s", fileName.c_str());
        }
        File f = sd.open(localPath);
        if (!f) { LOG_ERRORF("[FileUploader] [SMB] Cannot open: %s", localPath.c_str()); continue; }
        unsigned long fileSize = f.size();
        if (fileSize == 0) {
            f.close();
            smbStateManager->markFileUploaded(localPath, "empty_file", 0);
            skippedEmpty++;
            continue;
        }
        f.close();

        LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);

        if (!smbUploader->isConnected()) {
            if (!smbUploader->begin()) {
                LOG_ERROR("[FileUploader] [SMB] Failed to connect");
                smbStateManager->save(stateFs);
                return false;
            }
        }
        unsigned long smbBytes = 0;
        if (!smbUploader->upload(localPath, localPath, sd, smbBytes)) {
            LOG_ERRORF("[FileUploader] [SMB] Upload failed: %s", localPath.c_str());
            LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
            smbStateManager->save(stateFs);
            return false;
        }
        if (isRecent) smbStateManager->markFileUploaded(localPath, "", fileSize);
        uploadedCount++;
        g_smbSessionStatus.filesUploaded = uploadedCount;
        if (g_debugMode) LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), smbBytes);
#ifdef ENABLE_WEBSERVER
        if (webServer) webServer->handleClient();
#endif
        if (g_abortUploadFlag) {
            LOG_WARN("[FileUploader] [SMB] Abort requested — stopping upload cleanly");
            smbStateManager->save(stateFs);
            return false;
        }
    }

    if (isRescan) {
        LOGF("[FileUploader] [SMB] Re-scan complete: %d uploaded, %d unchanged", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] [SMB] Folder complete: %d files", uploadedCount);
    }

    // NOTE: connection is kept open — Phase 2 caller (`FileUploader::loop()`)
    // owns the persistent session lifecycle and tears it down once at the end.
    // Previously we disconnected here "to avoid socket exhaustion", but that
    // cost a full NEGPROT + SESSION_SETUP + TREE_CONNECT (~1–2 s) and a
    // ~4–5 KB smb2_context alloc/free cycle per folder.

    bool uploadSuccess = (uploadedCount == (int)files.size() - skippedUnchanged - skippedEmpty);
    LOGF("[FileUploader] [SMB] Folder %s: %d/%d files, %d unchanged, %d empty — success=%s",
         folderName.c_str(), uploadedCount, (int)files.size(), skippedUnchanged, skippedEmpty,
         uploadSuccess ? "yes" : "no");

    // Mark-complete strategy:
    //   Recent folders — always mark complete so the next scan uses the isRescan path
    //   (per-file size entries track which files need re-upload).
    //   Old folders — only mark complete when every file was uploaded (enables full retry
    //   of partially-uploaded old folders on the next session).
    if (isRecent) {
        smbStateManager->markFolderCompleted(folderName);
    } else if (uploadSuccess) {
        smbStateManager->markFolderCompleted(folderName);
    }

    smbStateManager->save(stateFs);
    return uploadSuccess;
#endif
}

// ── SMB: upload a single root/SETTINGS file ──────────────────────────────────
bool FileUploader::uploadSingleFileSmb(SDCardManager* sdManager, const String& filePath, bool force) {
#ifndef ENABLE_SMB_UPLOAD
    return true;
#else
    if (!smbUploader || !smbStateManager) return false;
    fs::FS &sd = sdManager->getFS();

    if (!sd.exists(filePath)) return true;  // file absent — not an error

    File f = sd.open(filePath);
    if (!f) { LOG_ERRORF("[FileUploader] [SMB] Cannot open: %s", filePath.c_str()); return false; }
    unsigned long fileSize = f.size();
    f.close();

    if (fileSize == 0) return true;

    if (!force && !smbStateManager->hasFileChanged(sd, filePath)) {
        LOG_DEBUGF("[FileUploader] [SMB] Unchanged, skipping: %s", filePath.c_str());
        return true;
    }

    LOGF("[FileUploader] Uploading single file: %s", filePath.c_str());

    if (!smbUploader->isConnected() && !smbUploader->begin()) {
        LOG_ERROR("[FileUploader] [SMB] Connection failed");
        return false;
    }
    unsigned long smbBytes = 0;
    if (!smbUploader->upload(filePath, filePath, sd, smbBytes)) {
        LOG_ERRORF("[FileUploader] [SMB] Upload failed: %s", filePath.c_str());
        return false;
    }
    String checksum = smbStateManager->calculateChecksum(sd, filePath);
    if (!checksum.isEmpty()) smbStateManager->markFileUploaded(filePath, checksum, fileSize);

    LOGF("[FileUploader] Successfully uploaded: %s (%lu bytes)", filePath.c_str(), smbBytes);
    return true;
#endif
}

// ── SMB: upload all mandatory root + SETTINGS files ──────────────────────────
bool FileUploader::uploadMandatoryFilesSmb(SDCardManager* sdManager, fs::FS &sd) {
#ifndef ENABLE_SMB_UPLOAD
    return true;
#else
    fs::FS &stateFs = LittleFS;

    LOG("[FileUploader] [SMB] Uploading mandatory root files...");
    const char* rootPaths[] = {
        "/Identification.json", "/Identification.crc", "/Identification.tgt", "/STR.edf"
    };
    for (const char* path : rootPaths) {
        if (sd.exists(path)) uploadSingleFileSmb(sdManager, String(path), false);
    }
    std::vector<String> settingsFiles = scanSettingsFiles(sd);
    for (const String& fp : settingsFiles) {
        uploadSingleFileSmb(sdManager, fp, false);
    }
    if (smbStateManager) smbStateManager->save(stateFs);
    return true;
#endif
}

// ============================================================================
// Cloud PASS: upload all DATALOG files for one folder (SleepHQ)
// ============================================================================
bool FileUploader::uploadDatalogFolderCloud(SDCardManager* sdManager, const String& folderName) {
#ifndef ENABLE_SLEEPHQ_UPLOAD
    return true;
#else
    if (!sleephqUploader || !cloudStateManager) return false;
    fs::FS &sd = sdManager->getFS();
    fs::FS &stateFs = LittleFS;

    LOGF("[FileUploader] [Cloud] Uploading DATALOG folder: %s", folderName.c_str());
    String folderPath = "/DATALOG/" + folderName;

    std::vector<String> files;
    if (!handleFolderScan(sd, stateFs, folderName, folderPath, cloudStateManager, files,
            [this](fs::FS& sd2, const String& fp) { return scanFolderFiles(sd2, fp); })) {
        return false;
    }
    if (files.empty()) return true;

    bool isRecent = isRecentFolder(folderName);
    bool isRescan = cloudStateManager->isFolderCompleted(folderName) && isRecent;

    g_cloudSessionStatus.uploadActive = true;
    strncpy((char*)g_cloudSessionStatus.currentFolder, folderName.c_str(),
            sizeof(g_cloudSessionStatus.currentFolder) - 1);
    ((char*)g_cloudSessionStatus.currentFolder)[sizeof(g_cloudSessionStatus.currentFolder) - 1] = '\0';
    g_cloudSessionStatus.filesTotal    = (int)files.size();
    g_cloudSessionStatus.filesUploaded = 0;

    int uploadedCount    = 0;
    int skippedUnchanged = 0;
    int skippedEmpty     = 0;

    // Import was created eagerly in begin() before this folder loop starts
    if (cloudImportFailed || sleephqUploader->getCurrentImportId().isEmpty()) {
        LOG_WARN("[FileUploader] [Cloud] No active import — skipping folder");
        return true;
    }

    for (const String& fileName : files) {
        String localPath = folderPath + "/" + fileName;
        if (isRescan) {
            if (!cloudStateManager->hasFileChanged(sd, localPath)) { skippedUnchanged++; continue; }
            LOG_DEBUGF("[FileUploader] [Cloud] File changed: %s", fileName.c_str());
        }
        File f = sd.open(localPath);
        if (!f) { LOG_ERRORF("[FileUploader] [Cloud] Cannot open: %s", localPath.c_str()); continue; }
        unsigned long fileSize = f.size();
        f.close();
        if (fileSize == 0) {
            cloudStateManager->markFileUploaded(localPath, "empty_file", 0);
            skippedEmpty++;
            continue;
        }

        LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);

        if (!sleephqUploader->isConnected() && !sleephqUploader->begin()) {
            LOG_ERROR("[FileUploader] [Cloud] Connection failed");
            cloudStateManager->save(stateFs);
            return false;
        }
        unsigned long cloudBytes = 0;
        String cloudChecksum = "";
        if (!sleephqUploader->upload(localPath, localPath, sd, cloudBytes, cloudChecksum)) {
            LOG_ERRORF("[FileUploader] [Cloud] Upload failed: %s", localPath.c_str());
            LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
            cloudStateManager->save(stateFs);
            return false;
        }
        if (isRecent) cloudStateManager->markFileUploaded(localPath, "", fileSize);
        uploadedCount++;
        cloudDatalogFilesUploaded++;
        g_cloudSessionStatus.filesUploaded = uploadedCount;
        if (g_debugMode) LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), cloudBytes);
#ifdef ENABLE_WEBSERVER
        if (webServer) webServer->handleClient();
#endif
        if (g_abortUploadFlag) {
            LOG_WARN("[FileUploader] [Cloud] Abort requested — stopping upload cleanly");
            cloudStateManager->save(stateFs);
            return false;
        }
    }

    if (isRescan) {
        LOGF("[FileUploader] [Cloud] Re-scan complete: %d uploaded, %d unchanged", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] [Cloud] Folder complete: %d files", uploadedCount);
    }

    bool uploadSuccess = (uploadedCount == (int)files.size() - skippedUnchanged - skippedEmpty);
    LOGF("[FileUploader] [Cloud] Folder %s: %d/%d files, %d unchanged, %d empty — success=%s",
         folderName.c_str(), uploadedCount, (int)files.size(), skippedUnchanged, skippedEmpty,
         uploadSuccess ? "yes" : "no");

    // Mark-complete strategy: same as SMB — recent always, old only on full success.
    if (isRecent) {
        cloudStateManager->markFolderCompleted(folderName);
    } else if (uploadSuccess) {
        cloudStateManager->markFolderCompleted(folderName);
    }

    cloudStateManager->save(stateFs);
    return uploadSuccess;
#endif
}

// ── Cloud: upload a single root/SETTINGS file ────────────────────────────────
bool FileUploader::uploadSingleFileCloud(SDCardManager* sdManager, const String& filePath, bool force) {
#ifndef ENABLE_SLEEPHQ_UPLOAD
    return true;
#else
    if (!sleephqUploader || !cloudStateManager) return false;
    fs::FS &sd = sdManager->getFS();

    if (!sd.exists(filePath)) return true;

    File f = sd.open(filePath);
    if (!f) return false;
    unsigned long fileSize = f.size();
    f.close();
    if (fileSize == 0) return true;

    if (!force && !cloudStateManager->hasFileChanged(sd, filePath)) {
        LOG_DEBUGF("[FileUploader] [Cloud] Unchanged, skipping: %s", filePath.c_str());
        return true;
    }

    LOGF("[FileUploader] Uploading single file: %s", filePath.c_str());

    if (!sleephqUploader->isConnected() && !sleephqUploader->begin()) {
        LOG_ERROR("[FileUploader] [Cloud] Connection failed");
        return false;
    }
    unsigned long cloudBytes = 0;
    String cloudChecksum = "";
    if (!sleephqUploader->upload(filePath, filePath, sd, cloudBytes, cloudChecksum)) {
        LOG_ERRORF("[FileUploader] [Cloud] Upload failed: %s", filePath.c_str());
        return false;
    }
    String checksum = cloudChecksum.isEmpty()
        ? cloudStateManager->calculateChecksum(sd, filePath)
        : cloudChecksum;
    if (!checksum.isEmpty()) cloudStateManager->markFileUploaded(filePath, checksum, fileSize);

    LOGF("[FileUploader] Successfully uploaded: %s (%lu bytes)", filePath.c_str(), cloudBytes);
    return true;
#endif
}

