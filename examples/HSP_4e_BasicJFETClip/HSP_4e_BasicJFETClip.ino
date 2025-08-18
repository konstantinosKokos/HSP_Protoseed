// BasicJFETclip.ino — v1.0.0
// by Harold Street Pedals 2025
// Touch-sensitive JFET-style overdrive (square-law saturation) with bias/asymmetry,
// simple tone, momentary kick, and true bypass.
// LEDs are active-HIGH (library v1.3.0+): SetLED(..., true) turns the LED on.

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE (pre-gain before the JFET stage), in dB
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 30.0f;  // ~31.6x linear

// MOMENTARY kick on FS1 (extra drive while held)
static const float EXTRA_DRIVE_DB = 6.0f;

// JFET shaping parameters
// Square-law soft knee: y = x - alpha * x * |x|, then optional asymmetry via bias.
// Larger ALPHA => softer (earlier) saturation; lower => cleaner headroom.
static const float JFET_ALPHA_MIN = 0.10f;  // gentle saturation
static const float JFET_ALPHA_MAX = 0.60f;  // earlier/squishier saturation

// Bias (RV3) introduces asymmetry similar to gate bias / Vgs offset
// Positive bias pushes the curve one way, negative the other.
static const float BIAS_RANGE = 0.30f;  // ±0.30 added pre-shaper (on normalized input)

// Post-TONE: simple treble-cut one-pole LPF after the stage
static const float TONE_CUTOFF_MIN_HZ = 700.0f;
static const float TONE_CUTOFF_MAX_HZ = 9000.0f;

// Output trim + safety ceiling
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// Clip indicator envelope (LED1)
static const float CLIP_LED_ATTACK = 1.00f;
static const float CLIP_LED_DECAY = 0.90f;

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (read in loop, consumed in audio)
static volatile bool g_bypassed = false;
static volatile bool g_kick = false;
static volatile float g_drive_lin = 1.0f;
static volatile float g_alpha = 0.30f;
static volatile float g_bias = 0.0f;
static volatile float g_lp_a = 0.0f;
static volatile float g_lp_b = 1.0f;

// Filter memory
static float g_lp_z = 0.0f;

// Clip meter
static float g_clip_env = 0.0f;

// FS2 edge tracking
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db * (1.0f / 20.0f));
}

// One-pole low-pass coefficient update
// y[n] = b*x[n] + a*y[n-1],  a = exp(-2*pi*fc/fs),  b = 1 - a
static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

// JFET square-law saturator with bias (asymmetry) and gentle soft knee
// Model: y = x' - alpha * x' * |x'|, where x' = x + bias
// Returns output and sets 'clipped' heuristic if nonlinear term engaged strongly.
static inline float JFET_SquareLaw(float x, float alpha, float bias, bool &clipped) {
  float xp = x + bias;
  float nl = alpha * xp * fabsf(xp);
  float y = xp - nl;

  // Simple heuristic for clip LED: when |nl| is notable vs |xp|
  if (fabsf(nl) > 0.15f * (0.001f + fabsf(xp))) clipped = true;
  return y;
}

// -----------------------------------------------------------------------------
// Audio callback (runs at audio rate). No direct control reads here.
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (g_bypassed) {
    out = in;
    return;
  }

  // 1) Pre-gain (with momentary kick)
  float pre = in * g_drive_lin * (g_kick ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);

  // 2) JFET square-law saturation with bias
  bool clipped = false;
  float y = JFET_SquareLaw(pre, g_alpha, g_bias, clipped);

  // 3) Post TONE (LPF)
  g_lp_z = g_lp_b * y + g_lp_a * g_lp_z;
  y = g_lp_z;

  // 4) Output trim + safety
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUT_LIMIT, fminf(y, OUT_LIMIT));

  // Clip meter envelope (audio-thread friendly)
  if (clipped) g_clip_env = fminf(1.0f, CLIP_LED_ATTACK * (g_clip_env + 0.30f));
  out = y;
}

// -----------------------------------------------------------------------------
// Setup (runs once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initialize from pots
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));
  g_alpha = H.ReadPotMapped(RV2, JFET_ALPHA_MIN, JFET_ALPHA_MAX, HPCB_Curve::Exp10);  // use RV2 as "grit"
  {
    float bias01 = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    g_bias = bias01 * BIAS_RANGE;
  }
  float fc0 = H.ReadPotMapped(RV4, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);  // RV4 = tone
  UpdateLowpassFromCutoff(fc0);

  // Master (post)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (control/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();

  // RV1 — Drive (dB → linear)
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Grit (alpha)
  g_alpha = H.ReadPotMapped(RV2, JFET_ALPHA_MIN, JFET_ALPHA_MAX, HPCB_Curve::Exp10);

  // RV3 — Bias (asymmetry)
  {
    float bias01 = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    g_bias = bias01 * BIAS_RANGE;
  }

  // RV4 — Tone (LPF cutoff)
  {
    float fc = H.ReadPotMapped(RV4, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc);
  }

  // RV6 — Master (post), lightly smoothed
  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // Don't read pot if bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set unity gain.
  }

  // FS1 — momentary kick
  g_kick = H.FootswitchIsPressed(FS1);

  // FS2 — bypass toggle (edge detect)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2) g_bypassed = !g_bypassed;
    prev_fs2 = fs2;
  }

  // LEDs (active-HIGH):
  // LED2 = effect active; LED1 = clip meter (decays each loop)
  H.SetLED(LED2, !g_bypassed);
  g_clip_env *= CLIP_LED_DECAY;
  H.SetLED(LED1, g_clip_env > 0.12f || g_kick);
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// This is a touch-sensitive **JFET-style** overdrive built around a square-law
// nonlinearity. The shaper approximates how a JFET compresses as it nears pinch-off,
// with a bias control to introduce asymmetry (similar to shifting Vgs). It feels
// dynamic, cleans up with guitar volume, and takes EQ well.
//
// Controls
// --------
// - RV1 — Drive: pre-gain into the stage (0 to +30 dB).
// - RV2 — Grit (Alpha): sets how early/strong the square-law knee engages.
// - RV3 — Bias: introduces asymmetry (±), shifting the knee for touch/texture.
// - RV4 — Tone: post-EQ treble cut (700 Hz to 9 kHz), smooths fizz.
// - RV6 — Master: overall output level (post), via library SetLevel().
// - FS1 — Kick: hold for +6 dB extra drive into the stage.
// - FS2 — Bypass: true passthrough on/off.
// - LED1 — Clip Meter: lights on saturation (decays over time); also on during Kick.
// - LED2 — Effect Active: lit when the effect is engaged.
//   (LEDs are **active-HIGH** as of HaroldPCB v1.3.0.)
//
// Signal Flow
// -----------
// Input → Drive → JFET Square-Law + Bias → LPF Tone → Master → Out
//
// Customizable Parameters (top of file)
// -------------------------------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: overall gain window.
// - JFET_ALPHA_MIN / MAX: grit range; higher = earlier/squishier saturation.
// - BIAS_RANGE: strength of asymmetry to mimic gate bias.
// - TONE_CUTOFF_MIN/MAX: post-EQ voicing window.
// - EXTRA_DRIVE_DB: momentary kick level.
// - OUTPUT_TRIM / OUT_LIMIT: global gain and safety.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) **Source Degeneration Feel:** Add a small pre-emphasis (HPF 100–150 Hz)
//    before the shaper to “tighten” the lows like a source resistor would.
// 2) **JFET-to-Tube Blend:** Crossfade between square-law (this) and tanh()
//    (tube-ish) on TS1 for two flavors.
// 3) **Dynamic Bias:** Modulate g_bias slightly with input envelope for “sag.”
// 4) **Two-Band Drive:** Split lows/highs, feed different alpha values, then sum.
// 5) **Presence Shelf:** Add a gentle post high-shelf to reopen the top after tone.
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Structured as a textbook example for the
// HaroldPCB library (constants up top, prose user guide at the end). LEDs active-HIGH.
//
