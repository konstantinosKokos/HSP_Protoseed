#pragma once
#include <Arduino.h>
#include <math.h>
#include "ThirdPartyDaisy.h"

// ===== Version =====
#define HPCB_VERSION_MAJOR 1
#define HPCB_VERSION_MINOR 3
#define HPCB_VERSION_PATCH 0
#define HPCB_VERSION_STR   "1.3.0"

// ==== Audio callback types ====
typedef void (*HPCB_AudioCB_Mono)(float in, float &out);

// ==== Mapping curves ====
enum class HPCB_Curve : uint8_t { Linear = 0, Log10 = 1, Exp10 = 2 };

// ==== Hardware enums ====
enum HPCB_Pot       : uint8_t { RV1 = 0, RV2, RV3, RV4, RV5, RV6, HPCB_NUM_POTS };
enum HPCB_Toggle    : uint8_t { TS1 = 0, TS2, TS3, TS4, HPCB_NUM_TOGGLES };
enum HPCB_Footswitch: uint8_t { FS1 = 0, FS2, HPCB_NUM_FOOTSWITCHES };
enum HPCB_Led       : uint8_t { LED1 = 0, LED2, HPCB_NUM_LEDS };

// ==== Input mode ====
enum class HPCB_InputMode : uint8_t { Auto = 0, Left, Right, Sum };

// ==== Footswitch timing ====
// Defaults: quick, stable taps. Use helpers in setup() to tune.
struct HPCB_FootswitchTiming {
    uint16_t debounce_ms       = 12;   // ignore bounces shorter than this
    uint16_t longpress_ms      = 500;  // hold duration to count as "long"
    uint16_t multiclick_gap_ms = 300;  // max gap for a "double" click
};

class HaroldPCB;

// ==== Master level connector ====
struct _ConnToMaster { HaroldPCB& H; const HPCB_Pot src_pot; void level(HPCB_Curve curve); };
struct _ConnStart   { HaroldPCB& H; const HPCB_Pot src_pot; _ConnToMaster to_master(){ return _ConnToMaster{H, src_pot}; } };
inline _ConnStart Connect(HaroldPCB& H, HPCB_Pot pot){ return _ConnStart{H, pot}; }

// ==== HaroldPCB class ====
class HaroldPCB {
public:
    bool Init(uint32_t sample_rate_hz = 48000, uint16_t block_size = 48);
    bool StartAudio(HPCB_AudioCB_Mono cb_mono);
    void StopAudio();
    void Idle();

    uint32_t SampleRate() const { return sr_; }
    uint16_t BlockSize()  const { return block_; }

    void SetLevel(float lvl) { master_level_ = constrain(lvl, 0.0f, 1.0f); }

    // Input routing
    void SetInputMode(HPCB_InputMode m){ in_mode_ = m; }
    HPCB_InputMode GetInputMode() const { return in_mode_; }

    // Pots
    float ReadPot(uint8_t index);
    float ReadPotMapped(uint8_t index, float min, float max, HPCB_Curve curve = HPCB_Curve::Linear);
    float ReadPotSmoothed(uint8_t index, float smooth_ms);

    // Toggles
    bool ReadToggle(uint8_t index) const;

    // Footswitches
    bool FootswitchIsPressed(uint8_t index) const;
    bool FootswitchIsReleased(uint8_t index) const;

    // One-shot events
    bool FootswitchIsLongPressed(uint8_t index);
    bool FootswitchIsDoublePressed(uint8_t index);
    bool FootswitchIsDoubleLongPressed(uint8_t index);

    // Footswitch timing setters
    void SetFootswitchTiming(const HPCB_FootswitchTiming &t);
    void SetDebounce(uint16_t ms);
    void SetLongPress(uint16_t ms);
    void SetMultiClickGap(uint16_t ms);

    // LEDs (ACTIVE-HIGH)
    void SetLED(uint8_t index, bool on);

private:
    friend struct _ConnToMaster;
    friend void _BlockCB(float **in, float **out, size_t sz);

    uint32_t sr_    = 48000;
    uint16_t block_ = 48;

    uint8_t pot_pins_[HPCB_NUM_POTS]                = {A6, A5, A4, A3, A2, A1};
    uint8_t toggle_pins_[HPCB_NUM_TOGGLES]          = {10, 9, 8, 7};
    uint8_t footswitch_pins_[HPCB_NUM_FOOTSWITCHES] = {25, 26};
    uint8_t led_pins_[HPCB_NUM_LEDS]                = {23, 22};

    float    pot_state_[HPCB_NUM_POTS]        = {0};
    uint32_t pot_last_ms_[HPCB_NUM_POTS]      = {0};

    bool        master_bound_ = false;
    HPCB_Pot    master_src_   = RV6;
    HPCB_Curve  master_curve_ = HPCB_Curve::Linear;
    float       master_level_ = 1.0f;

    HPCB_InputMode in_mode_ = HPCB_InputMode::Auto;
    HPCB_AudioCB_Mono cb_   = nullptr;

    // Footswitch debounce/event state
    HPCB_FootswitchTiming fs_timing_;
    bool     fs_pressed_[HPCB_NUM_FOOTSWITCHES]   = {false};
    uint32_t fs_last_change_[HPCB_NUM_FOOTSWITCHES] = {0};
    uint8_t  fs_click_count_[HPCB_NUM_FOOTSWITCHES] = {0};
    uint32_t fs_last_press_time_[HPCB_NUM_FOOTSWITCHES] = {0};

    bool fs_evt_long_[HPCB_NUM_FOOTSWITCHES]       = {false};
    bool fs_evt_double_[HPCB_NUM_FOOTSWITCHES]     = {false};
    bool fs_evt_doublelong_[HPCB_NUM_FOOTSWITCHES] = {false};

    void  _serviceMaster();
    void  _serviceFootswitches();
    static float _applyCurve(float v, HPCB_Curve c);
};

inline void _ConnToMaster::level(HPCB_Curve curve){
    H.master_bound_ = true; H.master_src_ = src_pot; H.master_curve_ = curve;
}