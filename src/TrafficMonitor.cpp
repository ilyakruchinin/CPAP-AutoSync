#include "TrafficMonitor.h"
#include "Logger.h"

TrafficMonitor::TrafficMonitor()
    : _pin(-1)
    , _initialized(false)
    , _pcntUnit(nullptr)
    , _pcntChannel(nullptr)
    , _lastSampleTime(0)
    , _lastSampleActive(false)
    , _lastPulseCount(0)
    , _consecutiveIdleMs(0)
    , _lastSecondTime(0)
    , _secondPulseAccumulator(0)
    , _sampleBuffer(nullptr)
    , _sampleHead(0)
    , _sampleCount(0)
    , _bufferEnabled(false)
    , _longestIdleMs(0)
    , _totalActiveSamples(0)
    , _totalIdleSamples(0)
    , _suspended(false)
    , _activityLatch(false)
{
}

void TrafficMonitor::begin(int pin) {
    _pin = pin;
    
    // Configure GPIO as input (floating - rely on external pull-ups on SD bus)
    pinMode(_pin, INPUT);
    
    // Configure PCNT unit (new IDF 5.x driver)
    pcnt_unit_config_t unit_config = {};
    unit_config.high_limit = 32767;       // Max 16-bit signed
    unit_config.low_limit  = -1;          // Must be negative; we only count up
    
    esp_err_t err = pcnt_new_unit(&unit_config, &_pcntUnit);
    if (err != ESP_OK) {
        LOG_ERRORF("PCNT unit creation failed: %d", err);
        return;
    }
    
    // Set glitch filter to reject EMI noise while passing valid SD edges.
    // AS11 runs the SD clock at 20 MHz → 50 ns per bit.  The filter must be
    // shorter than the minimum valid pulse to avoid rejecting real activity.
    // 40 ns ≈ 3 APB cycles at 80 MHz — comfortably below the 50 ns bit period.
    pcnt_glitch_filter_config_t filter_config = {};
    filter_config.max_glitch_ns = 40;
    err = pcnt_unit_set_glitch_filter(_pcntUnit, &filter_config);
    if (err != ESP_OK) {
        LOG_WARNF("PCNT filter config failed: %d", err);
    }
    
    // Configure PCNT channel on the unit
    pcnt_chan_config_t chan_config = {};
    chan_config.edge_gpio_num  = _pin;
    chan_config.level_gpio_num = -1;       // No control/level GPIO
    
    err = pcnt_new_channel(_pcntUnit, &chan_config, &_pcntChannel);
    if (err != ESP_OK) {
        LOG_ERRORF("PCNT channel creation failed: %d", err);
        pcnt_del_unit(_pcntUnit);
        _pcntUnit = nullptr;
        return;
    }
    
    // Count on both rising and falling edges (same as legacy pos_mode/neg_mode = INC)
    pcnt_channel_set_edge_action(_pcntChannel,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,   // Rising edge
        PCNT_CHANNEL_EDGE_ACTION_INCREASE);  // Falling edge
    // No control GPIO — keep counting regardless of level
    pcnt_channel_set_level_action(_pcntChannel,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,      // High level
        PCNT_CHANNEL_LEVEL_ACTION_KEEP);     // Low level
    
    // Enable and start counter
    pcnt_unit_enable(_pcntUnit);
    pcnt_unit_clear_count(_pcntUnit);
    pcnt_unit_start(_pcntUnit);
    
    _lastSampleTime = millis();
    _lastSecondTime = millis();
    _initialized = true;
    
    LOGF("TrafficMonitor initialized on GPIO %d", _pin);
}

void TrafficMonitor::adoptUnit(PcntHandles h, int pin) {
    _pin = pin;
    _pcntUnit = h.unit;
    _pcntChannel = h.channel;
    
    if (!_pcntUnit || !_pcntChannel) {
        LOG_ERROR("TrafficMonitor::adoptUnit — null handles, falling back to begin()");
        begin(pin);
        return;
    }
    
    _lastSampleTime = millis();
    _lastSecondTime = millis();
    _initialized = true;
    
    LOGF("TrafficMonitor adopted pre-existing PCNT unit on GPIO %d", _pin);
}

void TrafficMonitor::update() {
    if (!_initialized || _suspended) return;
    
    unsigned long now = millis();
    
    // Sample every ~100ms
    if (now - _lastSampleTime < SAMPLE_INTERVAL_MS) return;
    
    uint32_t elapsed = now - _lastSampleTime;
    _lastSampleTime = now;
    
    // Read and clear PCNT counter
    int count = 0;
    pcnt_unit_get_count(_pcntUnit, &count);
    pcnt_unit_clear_count(_pcntUnit);
    
    _lastPulseCount = (count > 0) ? (uint16_t)count : 0;
    _lastSampleActive = (_lastPulseCount > 0);
    
    // DEBUG: Log every 5 seconds to verify PCNT is actually counting
    static unsigned long lastDiagMs = 0;
    if (now - lastDiagMs >= 5000) {
        lastDiagMs = now;
        LOG_DEBUGF("[PCNT] count=%d active=%d idle=%lums buf=%s",
                   count, _lastSampleActive ? 1 : 0,
                   (unsigned long)_consecutiveIdleMs,
                   _sampleBuffer ? "alloc" : "null");
    }
    
    // Update idle tracking
    if (_lastSampleActive) {
        _consecutiveIdleMs = 0;
        _activityLatch = true;
    } else {
        _consecutiveIdleMs += elapsed;
        if (_consecutiveIdleMs > _longestIdleMs) {
            _longestIdleMs = _consecutiveIdleMs;
        }
    }
    
    // Aggregate into 1-second windows for sample buffer
    _secondPulseAccumulator += _lastPulseCount;
    
    if (now - _lastSecondTime >= 1000) {
        uint32_t ts = now / 1000;
        pushSample(ts, (uint16_t)min(_secondPulseAccumulator, (uint32_t)65535));
        
        // Update per-second statistics
        if (_secondPulseAccumulator > 0) {
            _totalActiveSamples++;
        } else {
            _totalIdleSamples++;
        }
        
        _secondPulseAccumulator = 0;
        _lastSecondTime = now;
    }
}

bool TrafficMonitor::isBusy() {
    return _lastSampleActive;
}

bool TrafficMonitor::isIdleFor(uint32_t ms) {
    return _consecutiveIdleMs >= ms;
}

uint32_t TrafficMonitor::getConsecutiveIdleMs() {
    return _consecutiveIdleMs;
}

bool TrafficMonitor::hasActivityLatch() const {
    return _activityLatch;
}

void TrafficMonitor::clearActivityLatch() {
    _activityLatch = false;
    _lastSampleActive = false;
    if (!_suspended && _initialized && _pcntUnit != nullptr) {
        pcnt_unit_clear_count(_pcntUnit);
    }
}

void TrafficMonitor::suspend() {
    if (!_initialized || _suspended) return;
    
    // Stop counting, then disable the unit.
    // pcnt_unit_disable() releases the ESP_PM_APB_FREQ_MAX lock,
    // allowing DFS to drop to 40 MHz and auto light-sleep to engage.
    pcnt_unit_stop(_pcntUnit);
    pcnt_unit_disable(_pcntUnit);
    _suspended = true;
    LOG_DEBUG("[POWER] PCNT suspended — APB lock released for light-sleep");
}

void TrafficMonitor::resume() {
    if (!_initialized || !_suspended) return;
    
    // Re-enable the unit (reacquires ESP_PM_APB_FREQ_MAX lock),
    // clear stale count accumulated while disabled, then restart.
    pcnt_unit_enable(_pcntUnit);
    pcnt_unit_clear_count(_pcntUnit);
    pcnt_unit_start(_pcntUnit);
    _suspended = false;
    _lastSampleTime = millis();
    _lastSecondTime = millis();
    _secondPulseAccumulator = 0;
    LOG_DEBUG("[POWER] PCNT resumed — APB lock reacquired");
}

bool TrafficMonitor::isSuspended() const {
    return _suspended;
}

void TrafficMonitor::resetIdleTracking() {
    _consecutiveIdleMs = 0;
    _lastSampleTime = millis();          // Prevent stale elapsed after COOLDOWN
    _secondPulseAccumulator = 0;
    _lastSecondTime = millis();
    // Drain any pulses accumulated so the first update() sample starts clean.
    // Translate any drained pulses into the activity latch so we don't drop them.
    // Skip if suspended — resume() already clears the count.
    if (!_suspended) {
        int drain = 0;
        pcnt_unit_get_count(_pcntUnit, &drain);
        if (drain > 0) {
            _activityLatch = true;
        }
        pcnt_unit_clear_count(_pcntUnit);
    }
}

const ActivitySample* TrafficMonitor::getSampleBuffer() const {
    return _sampleBuffer;
}

int TrafficMonitor::getSampleCount() const {
    return _sampleCount;
}

int TrafficMonitor::getSampleHead() const {
    return _sampleHead;
}

uint32_t TrafficMonitor::getLongestIdleMs() const {
    return _longestIdleMs;
}

uint32_t TrafficMonitor::getTotalActiveSamples() const {
    return _totalActiveSamples;
}

uint32_t TrafficMonitor::getTotalIdleSamples() const {
    return _totalIdleSamples;
}

uint32_t TrafficMonitor::getLastPulseCount() const {
    return (uint32_t)_lastPulseCount;
}

void TrafficMonitor::enableSampleBuffer() {
    if (_bufferEnabled && _sampleBuffer) return;
    _sampleBuffer = new (std::nothrow) ActivitySample[MAX_SAMPLES];
    if (_sampleBuffer) {
        memset(_sampleBuffer, 0, sizeof(ActivitySample) * MAX_SAMPLES);
        _bufferEnabled = true;
        _sampleHead = 0;
        _sampleCount = 0;
        LOG_DEBUG("TrafficMonitor sample buffer allocated (~2.4KB)");
    } else {
        LOG_WARN("TrafficMonitor sample buffer allocation failed");
        _bufferEnabled = false;
    }
}

void TrafficMonitor::disableSampleBuffer() {
    if (_sampleBuffer) {
        delete[] _sampleBuffer;
        _sampleBuffer = nullptr;
    }
    _bufferEnabled = false;
    _sampleHead = 0;
    _sampleCount = 0;
    LOG_DEBUG("TrafficMonitor sample buffer freed");
}

bool TrafficMonitor::isSampleBufferEnabled() const {
    return _bufferEnabled && _sampleBuffer != nullptr;
}

void TrafficMonitor::resetStatistics() {
    _longestIdleMs = 0;
    _totalActiveSamples = 0;
    _totalIdleSamples = 0;
    _sampleHead = 0;
    _sampleCount = 0;
    _secondPulseAccumulator = 0;
    _lastSecondTime = millis();
    if (_sampleBuffer) {
        memset(_sampleBuffer, 0, sizeof(ActivitySample) * MAX_SAMPLES);
    }
    LOG("TrafficMonitor statistics reset");
}

void TrafficMonitor::pushSample(uint32_t timestamp, uint16_t pulseCount) {
    if (!_sampleBuffer) return;  // Buffer not allocated (not in MONITORING mode)
    
    _sampleBuffer[_sampleHead].timestamp = timestamp;
    _sampleBuffer[_sampleHead].pulseCount = pulseCount;
    _sampleBuffer[_sampleHead].active = (pulseCount > 0);
    
    _sampleHead = (_sampleHead + 1) % MAX_SAMPLES;
    if (_sampleCount < MAX_SAMPLES) {
        _sampleCount++;
    }
}
