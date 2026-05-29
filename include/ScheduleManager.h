#ifndef SCHEDULE_MANAGER_H
#define SCHEDULE_MANAGER_H

#include <Arduino.h>
#include <time.h>

class ScheduleManager {
private:
    // Upload window
    int uploadStartHour;   // 0-23
    int uploadEndHour;     // 0-23
    int smartStartHour;    // 0-23, hour when Smart mode quiet period ends
    String uploadMode;     // "scheduled" or "smart"
    
    // Legacy single-hour support (for backward compat in begin())
    int uploadHour;        // Used by legacy begin() overload
    
    // Day completion tracking
    bool uploadCompletedToday;
    int lastCompletedDay;  // tm_yday of last completion (-1 = none)
    
    // NTP
    unsigned long lastUploadTimestamp;
    String ntpServer;
    String tzString;
    int gmtOffsetHours;

public:
    ScheduleManager();
    
    // New FSM-aware begin
    bool begin(const String& mode, int startHour, int endHour, int smartStart,
               int gmtOffsetHours,
               const String& tzString = "", const String& ntpServer = "pool.ntp.org");
    // Legacy begin (backward compat — creates a 2-hour window from uploadHour)
    bool begin(int uploadHour, int gmtOffsetHours);
    
    void setNtpServer(const String& server);
    
    bool syncTime();
    
    // New window-based methods
    bool isInUploadWindow();                                  // Current hour within [start, end]
    bool isSmartQuietPeriod();                                // Smart mode quiet period active?
    bool canUploadFreshData();                                // Smart: always (except quiet). Scheduled: in window.
    bool canUploadOldData();                                  // Scheduled/Smart: in window only. Manual: always false.
    bool canProcessOldData();                                 // canUploadOldData(), plus manual Force Upload (explicit "upload all").
    bool isUploadEligible(bool hasFreshData, bool hasOldData); // Combines mode + window + data
    
    // Day completion (scheduled mode)
    void markDayCompleted();
    bool isDayCompleted();
    
    // Legacy methods (still used by existing code, delegate to new logic)
    bool isUploadTime();
    void markUploadCompleted();
    unsigned long getSecondsUntilNextUpload();
    
    // Time utilities
    bool isTimeSynced() const;
    unsigned long getLastUploadTimestamp() const;
    void setLastUploadTimestamp(unsigned long timestamp);
    String getCurrentLocalTime() const;
    
    // Getters for web UI
    const String& getUploadMode() const;
    int getUploadStartHour() const;
    int getUploadEndHour() const;
    int getSmartStartHour() const;
    bool isSmartMode() const;
    bool isManualMode() const;
};

#endif // SCHEDULE_MANAGER_H
