// HSP_LEDHardClip.ino — v1.0.0
// by Harold Street Pedals 2025
// LED-style hard clipping with symmetry control, simple tone, and bypass.
//
// Structure:
// - Constants for builders at the top
// - Fixed 48 kHz / 8-sample block
// - Controls read in loop() (never inside the audio callback)
// - Mono audio callback does the DSP
// - Detailed User Guide at the bottom

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // Fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // Fixed project-wide

// DRIVE: pre-gain before clipping (in dB)
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// MOMENTARY: extra drive while FS1 is held (performance "kick")
static const float EXTRA_DRIVE_DB = 6.0f;  // +6 dB ≈ 2x

// CLIP THRESHOLDS (LED-like). We operate on normalized audio (≈ -1..+1).
// "LED" feel is emulated by a relatively high clip threshold vs silicon diodes.
static const float CLIP_THR_BASE = 0.40f;   // base symmetric threshold
static const float SYM_RANGE_FRAC = 0.45f;  // RV3 can skew ±45% between +/-

// TONE: one-pole low-pass (post-clipping) for simple treble cut
static const float TONE_CUTOFF_MIN_HZ = 600.0f;
static const float TONE_CUTOFF_MAX_HZ = 8200.0f;

// OUTPUT: safety and trim
static const float OUTPUT_TRIM = 1.0f;  // keep 1.0 for transparent level
static const float OUT_LIMIT = 1.2f;    // hard ceiling to catch extremes

// CLIP LED decay (visual indicator), 0..1 envelope
static const float CLIP_LED_DECAY = 0.90f;  // per-loop decay; smaller = faster

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state consumed by audio thread
static volatile bool g_bypassed = false;   // FS2 toggles
static volatile bool g_kick = false;       // FS1 momentary extra drive
static volatile float g_drive_lin = 1.0f;  // linear gain factor
static volatile float g_thr_pos = CLIP_THR_BASE;
static volatile float g_thr_neg = CLIP_THR_BASE;
static volatile float g_lp_a = 0.0f;  // LPF a
static volatile float g_lp_b = 1.0f;  // LPF (1-a)

// Audio-thread filter memory
static float g_lp_z = 0.0f;

// Clip indicator between threads: set in audio, decayed in loop
static volatile bool g_clip_hit = false;
static float g_clip_env = 0.0f;

// Edge tracking for FS2
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db * (1.0f / 20.0f));
}

// Map RV ∈ [0..1] to cutoff and compute LPF coeffs:
// y[n] = b*x[n] + a*y[n-1], with a = exp(-2*pi*fc/fs), b = 1-a
static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

// Hard-clip with asymmetry (separate + / - thresholds)
static inline float HardClipAsym(float x, float th_p, float th_n, bool &clipped) {
  if (x > th_p) {
    clipped = true;
    return th_p;
  }
  if (x < -th_n) {
    clipped = true;
    return -th_n;
  }
  return x;
}

// -----------------------------------------------------------------------------
// Audio callback (runs at audio rate). No control reads here.
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (g_bypassed) {
    out = in;
    return;
  }

  // 1) Pre-gain (with momentary "kick" if held)
  float drive = g_drive_lin * (g_kick ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);
  float x = in * drive;

  // 2) LED-like hard clip with asym thresholds
  bool clipped = false;
  float y = HardClipAsym(x, g_thr_pos, g_thr_neg, clipped);
  if (clipped) g_clip_hit = true;

  // 3) Post low-pass "tone"
  g_lp_z = g_lp_b * y + g_lp_a * g_lp_z;
  y = g_lp_z;

  // 4) Output trim + safety limit
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUT_LIMIT, fminf(y, OUT_LIMIT));

  out = y;
}

// -----------------------------------------------------------------------------
// Setup (once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initialize params from current pots
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));
  float tone0 = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(tone0);

  // Initial symmetry (RV3 center = symmetric)
  {
    float r = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = r * SYM_RANGE_FRAC;
    g_thr_pos = fmaxf(0.05f, CLIP_THR_BASE * (1.0f + skew));
    g_thr_neg = fmaxf(0.05f, CLIP_THR_BASE * (1.0f - skew));
  }

  // Master level (post)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (controls, UI, LEDs)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();  // services pots, toggles, and footswitch debounce

  // RV1 → Drive (dB → linear), perceptual curve
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 → Tone (LPF cutoff)
  {
    float fc = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc);
  }

  // RV3 → Symmetry: skew positive vs negative threshold
  {
    float r = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = r * SYM_RANGE_FRAC;
    g_thr_pos = fmaxf(0.05f, CLIP_THR_BASE * (1.0f + skew));
    g_thr_neg = fmaxf(0.05f, CLIP_THR_BASE * (1.0f - skew));
  }

  // RV6 → Master (post), with light smoothing for fine moves
  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f)); // Don't read pot if bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set unity gain.
  }


  // FS1 → momentary extra drive (no reads in audio thread)
  g_kick = H.FootswitchIsPressed(FS1);

  // FS2 → bypass toggle (edge detect)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2) g_bypassed = !g_bypassed;
    prev_fs2 = fs2;
  }

  // LEDs:
  // - LED2 = effect active
  H.SetLED(LED2, !g_bypassed);

  // - LED1 = clip indicator (envelope decays each loop)
  if (g_clip_hit) {
    g_clip_env = 1.0f;
    g_clip_hit = false;
  }
  g_clip_env *= CLIP_LED_DECAY;
  H.SetLED(LED1, g_clip_env > 0.12f || g_kick);  // also show while kick is held
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// This pedal models **LED hard clipping** in the simplest classic way: a strong
// pre-gain pushes the signal into a fixed ceiling (separate + / – thresholds for
// asymmetry), then a one-pole treble-cut "tone" shapes fizz. It’s punchy, loud,
// and harmonically rich — the vibe of LED clippers in dirt boxes.
//
// Controls
// --------
// - RV1 — Drive: 0 to +36 dB pre-gain (Exp curve for musical sweep).
// - RV2 — Tone (Treble-Cut): 600 Hz to 8.2 kHz one-pole LPF after the clipper.
// - RV3 — Symmetry: skews positive vs negative clip thresholds (center = symmetric).
// - RV6 — Master: overall output level (post effect), via library SetLevel().
// - FS1 — Momentary Extra Drive: hold for +6 dB extra push into the clipper.
// - FS2 — Bypass Toggle: true passthrough on/off.
// - LED1 — Clip Indicator: lights on clipping (decays visually); also on during FS1.
// - LED2 — Effect Active: lit when the effect is engaged.
//
// Signal Flow
// -----------
// Input → Drive (dB → linear) → Hard Clip (LED-like, asym capable) → LPF Tone → Master → Out
//
// What Makes It “LED” Here?
// -------------------------
// Real LEDs clip at higher forward voltages than small-signal diodes, so they
// feel **louder and less squashed**. We emulate that by using a **higher clip
// threshold (CLIP_THR_BASE)** and by offering **asymmetry** (RV3) to mimic LED
// mismatch and op-amp bias quirks.
//
// Customizable Parameters (top of file)
// -------------------------------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: set your gain window (e.g., +18 dB for cleaner).
// - EXTRA_DRIVE_DB: performance kick amount on FS1.
// - CLIP_THR_BASE: overall clip ceiling (lower = more distortion at the same drive).
// - SYM_RANGE_FRAC: how far RV3 can skew asymmetry.
// - TONE_CUTOFF_MIN_HZ / MAX: voice the treble-cut range.
// - OUTPUT_TRIM / OUT_LIMIT: global calibration and safety.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) **Pre-EQ Tighten:** High-pass before the clipper (e.g., 80–120 Hz) to cut mud.
// 2) **OP-AMP “Feel”:** Replace hard clip with tanh() for a softer feedback-style edge.
// 3) **LED Color Modes:** Use TS1 to switch thresholds to “red/amber/blue” profiles.
// 4) **Post Presence:** Add a small high-shelf after the LPF to restore sparkle.
// 5) **Asym Bias Control:** Map RV3 to add a DC bias pre-clip instead of threshold skew.
// 6) **Anti-Alias:** If you push extreme drive, consider 2× oversampling in a future
//    advanced example to reduce fold-back on very bright tones.
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Structured as a textbook example for the
// HaroldPCB library with constants up top and a prose user guide at the end.
//
