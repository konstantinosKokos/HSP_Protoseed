#include "HaroldPCB.h"

// If you’re using DaisyDuino, include its header and use its globals here.
// We keep it header-light so your code compiles even without DaisyDuino present.
// If DAISY isn’t present, we just do “software passthrough” in _MonoThunk.

// Static storage for the user’s mono callback
HPCB_AudioCB_Mono HaroldPCB::user_mono_cb_ = nullptr;

// -----------------------------------------------------------------------------
// Init — set sample rate, block size, and configure pins
// -----------------------------------------------------------------------------
bool HaroldPCB::Init(uint32_t sample_rate_hz, uint16_t block_size) {
  sample_rate_hz_ = sample_rate_hz ? sample_rate_hz : 48000;
  block_size_     = block_size     ? block_size     : 48;

  // Configure control I/O
  for (uint8_t i = 0; i < kNumPots;    ++i) { pinMode(pot_pins_[i],    INPUT); }
  for (uint8_t i = 0; i < kNumToggles; ++i) { pinMode(toggle_pins_[i], INPUT_PULLUP); }
  for (uint8_t i = 0; i < kNumFS;      ++i) { pinMode(fs_pins_[i],     INPUT_PULLUP); }
  for (uint8_t i = 0; i < kNumLEDs;    ++i) { pinMode(led_pins_[i],    OUTPUT);
                                              digitalWrite(led_pins_[i], LOW); } // LED off (active-HIGH)

  // If you have real Daisy audio setup, initialize it here.
  // (Left as a no-op so the library compiles on vanilla cores.)
  return true;
}

// -----------------------------------------------------------------------------
// Start/Stop audio — store the user callback; hook it to your audio driver
// -----------------------------------------------------------------------------
bool HaroldPCB::StartAudio(HPCB_AudioCB_Mono cb_mono) {
  user_mono_cb_ = cb_mono;
  // If you have a hardware driver, start it with _MonoThunk here.
  // DAISY.begin(_MonoThunk);
  return user_mono_cb_ != nullptr;
}

void HaroldPCB::StopAudio() {
  // DAISY.end();
  user_mono_cb_ = nullptr;
}

const char* HaroldPCB::Version() { return HPCB_VERSION_STR; }

// -----------------------------------------------------------------------------
// Audio bridge: pass Left through user callback; keep Right silent for mono
// -----------------------------------------------------------------------------
void HaroldPCB::_MonoThunk(float **in, float **out, size_t size) {
  HPCB_AudioCB_Mono cb = user_mono_cb_;
  if (!cb) {
    // No user callback: just copy L→L and mute R
    for (size_t i = 0; i < size; ++i) {
      out[0][i] = in[0][i];
      out[1][i] = 0.0f;
    }
    return;
  }
  for (size_t i = 0; i < size; ++i) {
    float o = 0.0f;
    cb(in[0][i], o);
    out[0][i] = o;
    out[1][i] = 0.0f;
  }
}

// -----------------------------------------------------------------------------
// Controls
// -----------------------------------------------------------------------------
float HaroldPCB::ReadPot(uint8_t index) {
  if (index >= kNumPots) return 0.0f;
  // Default Arduino analogRead is 10-bit on many cores (0..1023).
  // If your core is different, adjust the divisor.
  return analogRead(pot_pins_[index]) / 1023.0f;
}

float HaroldPCB::ReadPotMapped(uint8_t index, float minv, float maxv, HPCB_Curve curve) {
  float v = ReadPot(index); // 0..1
  switch (curve) {
    case HPCB_Curve::Log10: v = log10f(1.0f + 9.0f * v); break;            // more resolution near 0
    case HPCB_Curve::Exp10: v = (powf(10.0f, v) - 1.0f) / 9.0f; break;     // more near 1
    default: break; // Linear
  }
  return minv + (maxv - minv) * v;
}

float HaroldPCB::ReadPotSmoothed(uint8_t index, float smooth_ms) {
  if (index >= kNumPots) return 0.0f;

  float raw      = ReadPot(index);
  float dt_ms    = (1000.0f * block_size_) / float(sample_rate_hz_);
  float alpha    = (smooth_ms <= 0.0f) ? 1.0f : dt_ms / (smooth_ms + dt_ms);
  pot_smooth_[index] += alpha * (raw - pot_smooth_[index]);
  return pot_smooth_[index];
}

bool HaroldPCB::ReadToggle(uint8_t index) const {
  if (index >= kNumToggles) return false;
  return digitalRead(toggle_pins_[index]) == LOW; // active-low wiring typical
}

bool HaroldPCB::FootswitchIsPressed(uint8_t index) const {
  if (index >= kNumFS) return false;
  return digitalRead(fs_pins_[index]) == LOW;     // pressed = LOW (pull-up)
}

bool HaroldPCB::FootswitchIsReleased(uint8_t index) const {
  if (index >= kNumFS) return true;
  return digitalRead(fs_pins_[index]) == HIGH;
}

// -----------------------------------------------------------------------------
// Outputs / Idle
// -----------------------------------------------------------------------------
void HaroldPCB::SetLED(uint8_t index, bool on) {
  if (index >= kNumLEDs) return;
  digitalWrite(led_pins_[index], on ? HIGH : LOW);
}

void HaroldPCB::Idle() {
  // Hook for future debouncing/timing/housekeeping FSM.
  // Keeping this here lets your sketches stay the same as the library grows.

}