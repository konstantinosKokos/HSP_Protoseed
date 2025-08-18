#include "HaroldPCB.h"
#include "ThirdPartyDaisy.h"

// Static instance of the hardware
HaroldPCB* HaroldPCB::_haroldpcb_instance = nullptr;

HaroldPCB::HaroldPCB() {
    _haroldpcb_instance = this;
}

void HaroldPCB::Init(uint32_t sample_rate_hz, uint16_t block_size) {
    sample_rate_hz_ = sample_rate_hz;
    block_size_ = block_size;

    // initialize hardware
    daisy_.Init(sample_rate_hz_, block_size_);

    // Initialize footswitch state tracking
    for (int i = 0; i < NUM_FOOTSWITCHES; i++) {
        footswitch_pressed[i] = false;                  // was fs_pressed_
        footswitch_last_change_ms[i] = 0;               // was fs_time_last_change_
        footswitch_click_count[i] = 0;                  // was fs_click_count_
        footswitch_last_press_time_ms[i] = 0;           // was fs_last_press_time_
        footswitch_event_long[i] = false;               // was fs_evt_long_
        footswitch_event_double[i] = false;             // was fs_evt_dbl_
        footswitch_event_doublelong[i] = false;         // was fs_evt_dbl_long_
    }

    // Initialize pots
    for (int i = 0; i < NUM_POTS; i++) {
        pot_smoothed_values[i] = 0.0f;                  // was pot_smoothed_
        pot_last_update_ms[i] = 0;                      // was pot_time_last_update_
    }

    master_volume_is_bound = false;
    master_volume_source_pot = RV6;
    master_volume_curve = HPCB_Curve::Linear;
    master_volume_level = 1.0f;
}

void HaroldPCB::StartAudio(AudioCallback cb) {
    audio_callback = cb;                               // was cb_
    daisy_.StartAudio(_AudioThunk, sample_rate_hz_, block_size_);
}

void HaroldPCB::_AudioThunk(float** in, float** out, size_t size) {
    if (_haroldpcb_instance && _haroldpcb_instance->audio_callback) {
        for (size_t i = 0; i < size; i++) {
            float left = in[0][i];
            float right = in[1][i];

            float mono = 0.5f * (left + right);
            float processed = 0.0f;

            _haroldpcb_instance->audio_callback(mono, processed);

            out[0][i] = processed * _haroldpcb_instance->master_volume_level;
            out[1][i] = processed * _haroldpcb_instance->master_volume_level;
        }
    }
}

void HaroldPCB::Idle() {
    _updatePots();
    _updateFootswitches();
}

void HaroldPCB::_updatePots() {
    uint32_t now = daisy_.GetNow();

    for (int i = 0; i < NUM_POTS; i++) {
        if (now - pot_last_update_ms[i] > 2) {
            pot_last_update_ms[i] = now;
            pot_smoothed_values[i] = daisy_.ReadAnalog(pot_pins[i]);
        }
    }
}

float HaroldPCB::ReadPot(int idx) {
    return pot_smoothed_values[idx];
}

float HaroldPCB::ReadPotMapped(int idx, float min_val, float max_val, HPCB_Curve curve) {
    float raw = ReadPot(idx);
    return _applyMappingCurve(raw, min_val, max_val, curve);
}

float HaroldPCB::_applyMappingCurve(float raw, float min_val, float max_val, HPCB_Curve curve) {
    switch (curve) {
        case HPCB_Curve::Linear:
            return min_val + (max_val - min_val) * raw;
        case HPCB_Curve::Exp10:
            return min_val + (max_val - min_val) * powf(raw, 10.0f);
        default:
            return min_val + (max_val - min_val) * raw;
    }
}

void HaroldPCB::_updateFootswitches() {
    uint32_t now = daisy_.GetNow();

    for (int i = 0; i < NUM_FOOTSWITCHES; i++) {
        bool pressed = daisy_.ReadDigital(footswitch_pins[i]);
        if (pressed != footswitch_pressed[i]) {
            footswitch_pressed[i] = pressed;
            footswitch_last_change_ms[i] = now;

            if (pressed) {
                footswitch_click_count[i]++;
                footswitch_last_press_time_ms[i] = now;
            }
        }

        // Long press detection
        if (footswitch_pressed[i] && (now - footswitch_last_change_ms[i]) > 1000) {
            footswitch_event_long[i] = true;
        } else {
            footswitch_event_long[i] = false;
        }

        // Double click detection
        if (footswitch_click_count[i] == 2 && (now - footswitch_last_press_time_ms[i]) < 400) {
            footswitch_event_double[i] = true;
            footswitch_click_count[i] = 0;
        } else {
            footswitch_event_double[i] = false;
        }

        // Double + long press detection
        if (footswitch_click_count[i] == 2 && (now - footswitch_last_press_time_ms[i]) > 1000) {
            footswitch_event_doublelong[i] = true;
            footswitch_click_count[i] = 0;
        } else {
            footswitch_event_doublelong[i] = false;
        }
    }
}

bool HaroldPCB::FootswitchIsPressed(int idx) {
    return footswitch_pressed[idx];
}

bool HaroldPCB::FootswitchEventLong(int idx) {
    return footswitch_event_long[idx];
}

bool HaroldPCB::FootswitchEventDouble(int idx) {
    return footswitch_event_double[idx];
}

bool HaroldPCB::FootswitchEventDoubleLong(int idx) {
    return footswitch_event_doublelong[idx];
}
