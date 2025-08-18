// ProtoSEED_OCD_Minimal_HPCB128.ino
// HaroldPCB v1.2.8: FS1/LED1 = bypass, RV6 = master volume (ignored in bypass).
// Mono path; right output muted. FIR block ships as delta (no-op).

#include <Arduino.h>
#include "HaroldPCB.h"

// ========= Simple FIR (256 taps) =========
static constexpr size_t kIRLen = 256;

struct SimpleFIR {
  float ir[kIRLen];
  float buf[kIRLen];
  size_t idx = 0;

  void init(const float* taps, size_t n) {
    for (size_t i = 0; i < kIRLen; ++i) {
      ir[i] = (i < n) ? taps[i] : 0.0f;
      buf[i] = 0.0f;
    }
    idx = 0;
  }

  inline float process(float x) {
    buf[idx] = x;
    float acc = 0.0f;
    size_t bi = idx;
#pragma unroll
    for (size_t i = 0; i < kIRLen; ++i) {
      acc += ir[i] * buf[bi];
      bi = (bi == 0) ? (kIRLen - 1) : (bi - 1);
    }
    idx = (idx + 1) % kIRLen;
    return acc;
  }
};

// Delta IR → engaged == bypass until you replace taps
static float kIR_Default[kIRLen];
static SimpleFIR gFIR;

// ========= HaroldPCB + state =========
static HaroldPCB hpcb;
static volatile bool gBypass = true;  // FS1 toggles this
static volatile float gVol = 1.0f;    // RV6 0..1 (used only when engaged)

// ========= Audio (mono) callback per v1.2.8 =========
static void ProcessMono(float in, float& out) {
  if (gBypass) {
    // True passthru: ignore RV6 in bypass
    out = in;
  } else {
    // Engaged → FIR then master volume
    out = gFIR.process(in) * gVol;
  }
}

// ========= Arduino setup/loop =========
void setup() {
  // Build delta IR
  for (size_t i = 0; i < kIRLen; ++i) kIR_Default[i] = 0.0f;
  kIR_Default[0] = 1.0f;
  gFIR.init(kIR_Default, kIRLen);

  // Start board @ 48k / 48-sample blocks
  hpcb.Init(48000, 48);

  // Ensure we don't bind RV6 to internal master (that would affect bypass).
  // We'll do our own volume in ProcessMono.

  // LED off at boot (bypass)
  hpcb.SetLED(HPCB_Led::LED1, false);

  // Start audio with our mono callback
  hpcb.StartAudio(ProcessMono);
}

void loop() {
  // Service pots, switches, debounce, and optional master binding (unused)
  hpcb.Idle();

  // --- FS1: bypass toggle on rising edge ---
  static bool last_fs = false;
  bool fs_now = hpcb.FootswitchIsPressed(HPCB_Footswitch::FS1);
  if (fs_now && !last_fs) {
    gBypass = !gBypass;
    hpcb.SetLED(HPCB_Led::LED1, !gBypass);  // LED on when engaged
  }
  last_fs = fs_now;

  // --- RV6: read as 0..1 (we’ll apply curve if you want log feel) ---
  // Linear feel:
  gVol = hpcb.ReadPot(HPCB_Pot::RV6);
  // If you prefer log volume law, use:
  // gVol = hpcb.ReadPotMapped(HPCB_Pot::RV6, 0.0f, 1.0f, HPCB_Curve::Log10);
}
