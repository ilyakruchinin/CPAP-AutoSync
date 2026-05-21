#include "EarlyPCNT.h"

// ============================================================================
// EarlyPCNT — standalone lightweight pulse counter
// Extracted from BusWidthDetector.cpp for production use.
// ============================================================================
namespace EarlyPCNT {
    static pcnt_unit_handle_t   s_unit    = nullptr;
    static pcnt_channel_handle_t s_chan   = nullptr;

    void init(int gpio) {
        pcnt_unit_config_t ucfg = {};
        ucfg.high_limit = 32767;
        ucfg.low_limit  = -1;
        if (pcnt_new_unit(&ucfg, &s_unit) != ESP_OK) return;

        pcnt_glitch_filter_config_t fcfg = {};
        fcfg.max_glitch_ns = 40;   // 3 APB cycles at 80 MHz; below 50 ns SD bit @ 20 MHz
        pcnt_unit_set_glitch_filter(s_unit, &fcfg);

        pcnt_chan_config_t ccfg = {};
        ccfg.edge_gpio_num  = gpio;
        ccfg.level_gpio_num = -1;
        if (pcnt_new_channel(s_unit, &ccfg, &s_chan) != ESP_OK) {
            pcnt_del_unit(s_unit);
            s_unit = nullptr;
            return;
        }
        pcnt_channel_set_edge_action(s_chan,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE);
        pcnt_channel_set_level_action(s_chan,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP);
        pcnt_unit_enable(s_unit);
        pcnt_unit_clear_count(s_unit);
        pcnt_unit_start(s_unit);
    }

    int read() {
        if (!s_unit) return -1;
        int val = 0;
        pcnt_unit_get_count(s_unit, &val);
        return val;
    }

    void teardown() {
        if (s_chan)  { pcnt_del_channel(s_chan); s_chan = nullptr; }
        if (s_unit) { pcnt_unit_stop(s_unit); pcnt_unit_disable(s_unit); pcnt_del_unit(s_unit); s_unit = nullptr; }
    }

    PcntHandles detach() {
        PcntHandles h = { s_unit, s_chan };
        s_unit = nullptr;
        s_chan  = nullptr;
        return h;
    }
}
