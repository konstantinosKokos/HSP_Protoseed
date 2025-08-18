// BasicLEDSoftClip.ino — v1.0.0
// by Harold Street Pedals 2025
// LED-style soft clipping (op-amp feedback style) with drive, tone, symmetry, and bypass.
//
// Structure:
// - Constants up top for builders
// - Fixed 48 kHz / 8-sample block
// - All controls read in loop() (never inside the audio callback)
// - Mono audio callback handles DSP
// - User Guide at bottom

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE: pre-gain before saturation (in dB)
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// MOMENTARY KICK (FS1): extra drive when held
static const float EXTRA_DRIVE_DB = 6.0f;

// “LED feel” threshold (softer than hard clip)
static const float CLIP_THR_SOFT = 0.40f;  // softness center scaling

// Symmetry skew range (RV3) — ±45% between positive/negative
static const float SYM_RANGE_FRAC = 0.45f;

// TONE: one-pole low-pass (post saturation)
static const float TONE_CUTOFF_MIN_HZ = 600.0f;
static const float TONE_CUTOFF_MAX_HZ = 8200.0f;

// Output trim + safety
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// LED indicator decay
static const float CLIP_LED_DECAY = 0.90f;

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state for audio thread
static volatile bool g_bypassed = false;
static volatile bool g_kick = false;
static volatile float g_drive_lin = 1.0f;
static volatile float g_thr_pos = CLIP_THR_SOFT;
static volatile float g_thr_neg = CLIP_THR_SOFT;
static volatile float g_lp_a = 0.0f;
static volatile float g_lp_b = 1.0f;

// Filter memory
static float g_lp_z = 0.0f;

// Clip indicator
static volatile bool g_clip_hit = false;
static float g_clip_env = 0.0f;

// Edge detect for FS2
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db * (1.0f / 20.0f));
}

static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

// Soft clipper (LED feedback style).
// Uses tanh() with asymmetry scaling to simulate LED forward voltages in feedback.
static inline float SoftClipAsym(float x, float thr_p, float thr_n, bool &clipped) {
  float y = 0.0f;
  if (x >= 0.0f) {
    y = thr_p * tanhf(x / thr_p);
    if (fabsf(x) > thr_p) clipped = true;
  } else {
    y = thr_n * tanhf(x / thr_n);
    if (fabsf(x) > thr_n) clipped = true;
  }
  return y;
}

// -----------------------------------------------------------------------------
// Audio callback (audio thread only)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (g_bypassed) {
    out = in;
    return;
  }

  // Pre-gain
  float drive = g_drive_lin * (g_kick ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);
  float x = in * drive;

  // Soft clip
  bool clipped = false;
  float y = SoftClipAsym(x, g_thr_pos, g_thr_neg, clipped);
  if (clipped) g_clip_hit = true;

  // Post tone LPF
  g_lp_z = g_lp_b * y + g_lp_a * g_lp_z;
  y = g_lp_z;

  // Output trim and limit
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUT_LIMIT, fminf(y, OUT_LIMIT));

  out = y;
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  float fc0 = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(fc0);

  float r = H.ReadPot(RV3) * 2.0f - 1.0f;
  float skew = r * SYM_RANGE_FRAC;
  g_thr_pos = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f + skew));
  g_thr_neg = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f - skew));

  H.SetLevel(H.ReadPot(RV6));
  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (control thread)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();

  // RV1: Drive
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2: Tone cutoff
  {
    float fc = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc);
  }

  // RV3: Symmetry skew
  {
    float r = H.ReadPot(RV3) * 2.0f - 1.0f;
    float skew = r * SYM_RANGE_FRAC;
    g_thr_pos = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f + skew));
    g_thr_neg = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f - skew));
  }

  // RV6: Master level
  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // Don't read pot if bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set unity gain.
  }

  // FS1: Kick
  g_kick = H.FootswitchIsPressed(FS1);

  // FS2: Bypass toggle
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2) g_bypassed = !g_bypassed;
    prev_fs2 = fs2;
  }

  // LEDs
  H.SetLED(LED2, !g_bypassed);
  if (g_clip_hit) {
    g_clip_env = 1.0f;
    g_clip_hit = false;
  }
  g_clip_env *= CLIP_LED_DECAY;
  H.SetLED(LED1, g_clip_env > 0.12f || g_kick);
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// This pedal models **LED soft clipping** as found in op-amp feedback paths.
// Instead of abruptly cutting off, the transfer curve bends smoothly (tanh),
// producing smoother saturation and more dynamic touch than hard clip LEDs.
//
// Controls
// --------
// - RV1 — Drive: 0 to +36 dB pre-gain.
// - RV2 — Tone: 600 Hz – 8.2 kHz treble cut (LPF).
// - RV3 — Symmetry: adjusts bias between + and – sides.
// - RV6 — Master: post-output level.
// - FS1 — Kick: hold for +6 dB extra drive.
// - FS2 — Bypass toggle.
// - LED1 — Clip indicator (lights on clip, decays; also on during Kick).
// - LED2 — Effect active.
//
// Signal Flow
// -----------
// Input → Drive → SoftClip (LED tanh asym) → LPF Tone → Master → Out
//
// Customizable Parameters
// -----------------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: pre-gain window.
// - CLIP_THR_SOFT: scaling for soft clip curve (smaller = more distortion).
// - SYM_RANGE_FRAC: asymmetry range.
// - TONE_CUTOFF_MIN/MAX: tone control voicing.
// - EXTRA_DRIVE_DB: kick boost amount.
//
// Mods for Builders
// -----------------
// 1) Try arctan() instead of tanh() for a different soft curve.
// 2) Add TS1 as a mode switch: soft tanh vs hard clip.
// 3) Pre-EQ before drive for Tube Screamer-like mid bump.
// 4) Replace LPF tone with a tilt EQ for brighter voicing control.
// 5) Experiment with CLIP_THR_SOFT to mimic different LED colors.
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Example in textbook format for HaroldPCB.
//
