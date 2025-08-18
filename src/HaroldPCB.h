#pragma once
#include <Arduino.h>
#include <math.h>

// -----------------------------------------------------------------------------
// Public “board shape”: how many controls your sketches can count on.
// Adjust if your hardware differs, but most of your examples expect these.
// -----------------------------------------------------------------------------
static constexpr uint8_t kNumPots    = 6;  // RV1..RV6
static constexpr uint8_t kNumToggles = 4;  // TS1..TS4
static constexpr uint8_t kNumFS      = 2;  // FS1..FS2
static constexpr uint8_t kNumLEDs    = 2;  // LED1..LED2

// Human-friendly names for indices used in your sketches
enum : uint8_t { RV1=0, RV2, RV3, RV4, RV5, RV6 };
enum : uint8_t { TS1=0, TS2, TS3, TS4 };
enum : uint8_t { FS1=0, FS2 };
enum : uint8_t { LED1=0, LED2 };

// Optional mapping curves for pots (same semantics your sketches use)
enum class HPCB_Curve : uint8_t { Linear=0, Log10, Exp10 };

// Version string
#define HPCB_VERSION_STR "1.3.1"

// Daisy-style audio callback type you’ve been using in examples:
// mono: (input, output_ref)
using HPCB_AudioCB_Mono = void (*)(float, float&);

// -----------------------------------------------------------------------------
// HaroldPCB — minimal “pedal board” facade your examples rely on
// -----------------------------------------------------------------------------
class HaroldPCB {
public:
  // Initialize hardware: sample rate and audio block size
  bool Init(uint32_t sample_rate_hz, uint16_t block_size);

  // Start/stop audio with your mono callback
  bool StartAudio(HPCB_AudioCB_Mono cb_mono);
  void StopAudio();

  // Library version helper
  static const char* Version();

  // -------- Controls (read in loop(), not in audio callback) --------
  float ReadPot(uint8_t index);                                           // 0..1
  float ReadPotMapped(uint8_t index, float minv, float maxv,
                      HPCB_Curve curve = HPCB_Curve::Linear);             // min..max
  float ReadPotSmoothed(uint8_t index, float smooth_ms);                  // 0..1 (RC)

  bool  ReadToggle(uint8_t index) const;                                  // true if ON
  bool  FootswitchIsPressed(uint8_t index) const;                         // debounced
  bool  FootswitchIsReleased(uint8_t index) const;                        // debounced

  // -------- Outputs --------
  void  SetLED(uint8_t index, bool on);
  void  SetLevel(float level01) { master_level_ = constrain(level01, 0.0f, 1.0f); }

  // Background service (pot reading, debounce, etc.)
  void  Idle();

  // Daisy audio bridge thunk (Left in/out only; Right forced silent).
  // Must be static and match DaisyDuino's callback signature.
  static void _MonoThunk(float **in, float **out, size_t size);

private:
  // -------- Audio plumbing --------
  static HPCB_AudioCB_Mono user_mono_cb_;   // set by StartAudio()
  uint32_t sample_rate_hz_ = 48000;
  uint16_t block_size_     = 8;
  float    master_level_   = 1.0f;

  // -------- Control hardware (pins and simple state) --------
  // NOTE: Replace these pin assignments with your real wiring if needed.
  // They are placeholders that compile; your board-specific core should map them.
  uint8_t pot_pins_[kNumPots]       = { A0, A1, A2, A3, A4, A5 };
  uint8_t toggle_pins_[kNumToggles] = { 2, 3, 4, 5 };
  uint8_t fs_pins_[kNumFS]          = { 6, 7 };
  uint8_t led_pins_[kNumLEDs]       = { 8, 9 };

  // Simple one-pole smoothing memory for each pot
  float   pot_smooth_[kNumPots] = {0};

};