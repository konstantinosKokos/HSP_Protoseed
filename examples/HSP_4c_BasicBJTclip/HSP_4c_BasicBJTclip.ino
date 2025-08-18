// BasicBJT.ino — v1.0.0
// by Harold Street Pedals 2025
// BJT-style distortion using an exponential (diode-equation) soft clip,
// with drive, bias/asymmetry, tone, momentary kick, and true bypass.
// LEDs are active-HIGH (HaroldPCB v1.3.0+): SetLED(..., true) turns the LED on.

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE (pre-gain in dB) — how hard we push into the BJT nonlinearity
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// FS1 momentary "kick" amount (extra pre-gain while held)
static const float EXTRA_DRIVE_DB = 6.0f;

// BJT / diode-equation shaper parameters
// We approximate IE ≈ Is*(exp(Vd/Vt)-1). Use scaling to keep things stable in -1..+1.
// Vt sets curve steepness; smaller = sharper knee (more aggressive).
static const float Vt_MIN = 0.20f;
static const float Vt_MAX = 0.60f;
// Output scaling after the diode mapping so nominal gain stays in range
static const float SHAPER_GAIN = 1.0f;

// Asymmetry control (RV3) blends different positive/negative "Vt" to mimic BJT odd bias.
// SYM_RANGE_FRAC = how far ± we move Vt for + vs - sides.
static const float SYM_RANGE_FRAC = 0.45f;

// DC bias (pre-shaper) range to push asymmetry, similar to shifting operating point
static const float BIAS_RANGE = 0.25f;  // ±0.25 added to input before shaping

// Post-TONE: one-pole treble-cut after the shaper
static const float TONE_CUTOFF_MIN_HZ = 700.0f;
static const float TONE_CUTOFF_MAX_HZ = 9000.0f;

// Output trim + safety ceiling
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// Clip LED envelope timing
static const float CLIP_LED_ATTACK = 1.00f;
static const float CLIP_LED_DECAY = 0.90f;

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (updated in loop(), consumed in AudioCB)
static volatile bool g_bypassed = false;
static volatile bool g_kick = false;
static volatile float g_drive_lin = 1.0f;
static volatile float g_Vt_pos = 0.35f;  // effective Vt for positive half
static volatile float g_Vt_neg = 0.35f;  // effective Vt for negative half
static volatile float g_bias = 0.0f;
static volatile float g_lp_a = 0.0f;  // tone LPF 'a'
static volatile float g_lp_b = 1.0f;  // (1 - a)

// Filter memory
static float g_lp_z = 0.0f;

// Clip indicator envelope
static float g_clip_env = 0.0f;

// FS2 edge tracking
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db * (1.0f / 20.0f));
}

// One-pole low-pass coefficient update (post-tone)
// y[n] = b*x[n] + a*y[n-1], a = exp(-2*pi*fc/fs), b = 1 - a
static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

// Diode-like soft clip with independent positive/negative steepness (Vt_pos/Vt_neg)
// We use a signed mapping: y = sign(xp) * (1 - exp(-|xp|/Vt_side)), with xp = x + bias
// Then scale with SHAPER_GAIN. 'clipped' flags when the curve is clearly nonlinear.
static inline float BJT_DiodeSoftClip(float x, float Vt_pos, float Vt_neg, float bias, bool &clipped) {
  float xp = x + bias;
  float a = fabsf(xp);
  float Vt = (xp >= 0.0f) ? Vt_pos : Vt_neg;
  Vt = fmaxf(0.05f, Vt);

  // Soft knee: approaches ±1 as input grows
  float y_uni = 1.0f - expf(-a / Vt);
  float y = copysignf(y_uni, xp) * SHAPER_GAIN;

  // simple heuristic to drive LED: if |xp| is big enough vs Vt
  if (a > 0.6f * Vt) clipped = true;
  return y;
}

// -----------------------------------------------------------------------------
// Audio callback (runs at audio rate). No control reads here.
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (g_bypassed) {
    out = in;
    return;
  }

  // 1) Pre-gain (with momentary kick)
  float pre = in * g_drive_lin * (g_kick ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);

  // 2) BJT/diode soft clip with bias + asymmetry
  bool clipped = false;
  float y = BJT_DiodeSoftClip(pre, g_Vt_pos, g_Vt_neg, g_bias, clipped);

  // 3) Post Tone LPF
  g_lp_z = g_lp_b * y + g_lp_a * g_lp_z;
  y = g_lp_z;

  // 4) Output trim + safety
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUT_LIMIT, fminf(y, OUT_LIMIT));

  // Clip LED envelope
  if (clipped) g_clip_env = fminf(1.0f, CLIP_LED_ATTACK * (g_clip_env + 0.30f));

  out = y;
}

// -----------------------------------------------------------------------------
// Setup (runs once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initial params from pots
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 as "character": map to Vt (steepness). Lower Vt = harder clipping.
  {
    float Vt = H.ReadPotMapped(RV2, Vt_MIN, Vt_MAX, HPCB_Curve::Exp10);
    // RV3 adds asymmetry by skewing Vt+ vs Vt-
    float r = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = r * SYM_RANGE_FRAC;
    g_Vt_pos = fmaxf(0.05f, Vt * (1.0f - skew));
    g_Vt_neg = fmaxf(0.05f, Vt * (1.0f + skew));
  }

  // RV4 adds DC bias pre-shaper (±BIAS_RANGE)
  {
    float b01 = H.ReadPot(RV4) * 2.0f - 1.0f;
    g_bias = b01 * BIAS_RANGE;
  }

  // RV5 post Tone LPF cutoff
  {
    float fc0 = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc0);
  }

  // Master (post)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (controls/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();

  // RV1 — Drive (dB → linear)
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Vt (curve steepness) + RV3 asymmetry skew
  {
    float Vt = H.ReadPotMapped(RV2, Vt_MIN, Vt_MAX, HPCB_Curve::Exp10);
    float r = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = r * SYM_RANGE_FRAC;
    g_Vt_pos = fmaxf(0.05f, Vt * (1.0f - skew));
    g_Vt_neg = fmaxf(0.05f, Vt * (1.0f + skew));
  }

  // RV4 — DC bias (±BIAS_RANGE)
  {
    float b01 = H.ReadPot(RV4) * 2.0f - 1.0f;
    g_bias = b01 * BIAS_RANGE;
  }

  // RV5 — Tone cutoff
  {
    float fc = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc);
  }

  // RV6 — Master (post), mild smoothing for fine control
  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // Don't read pot if bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set unity gain.
  }

  // FS1 — Kick (momentary)
  g_kick = H.FootswitchIsPressed(FS1);

  // FS2 — Bypass (edge)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    static bool prev = false;
    if (fs2 && !prev) g_bypassed = !g_bypassed;
    prev = fs2;
  }

  // LEDs (active-HIGH)
  H.SetLED(LED2, !g_bypassed);  // effect active
  g_clip_env *= CLIP_LED_DECAY;
  H.SetLED(LED1, g_clip_env > 0.12f || g_kick);  // clip meter / kick indicator
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// **BasicBJT** captures the feel of transistor-based distortion by using an
// exponential diode-like transfer (akin to a B-E junction). Compared to hard
// clipping, it saturates more gradually, but with stronger odd-harmonic energy
// and richer asymmetry options, especially when you bias the stage off-center.
//
// Controls
// --------
// - RV1 — Drive: overall pre-gain (0 → +36 dB).
// - RV2 — Character (Vt): lower values = sharper knee / more fuzz; higher = smoother.
// - RV3 — Asymmetry: skews the steepness of + vs – sides (mimics mismatched devices).
// - RV4 — Bias: DC offset pre-shaper (±), shifting the operating point for texture.
// - RV5 — Tone: post-EQ treble cut (700 Hz → 9 kHz).
// - RV6 — Master: overall output level (post).
// - FS1 — Kick: hold for +6 dB extra into the shaper.
// - FS2 — Bypass: true passthrough on/off.
// - LED1 — Clip Meter: shows saturation activity (decay) and lights during Kick.
// - LED2 — Effect Active: lit when engaged.
//   (LEDs are **active-HIGH** with HaroldPCB v1.3.0+.)
//
// Signal Flow
// -----------
// Input → Drive → BJT/Diode Soft Clip (+Bias & Asym) → LPF Tone → Master → Out
//
// Customizable Parameters (top of file)
// -------------------------------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: gain window.
// - Vt_MIN / Vt_MAX: curve steepness range (lower = harsher).
// - SYM_RANGE_FRAC: asymmetry strength for Vt+ vs Vt-.
// - BIAS_RANGE: DC offset authority pre-shaper.
// - TONE_CUTOFF_MIN/MAX: post-EQ voicing.
// - EXTRA_DRIVE_DB: Kick amount.
// - SHAPER_GAIN: overall shaper scale if you retune curves.
// - OUTPUT_TRIM / OUT_LIMIT: global gain and safety.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) **Pre-Emphasis Tighten:** Add an input HPF (~120 Hz) before Drive to reduce mud.
// 2) **Asym via Threshold:** Instead of skewing Vt, add small DC after Drive, before shaper.
// 3) **Germanium Mode:** Lower Vt range (e.g., 0.10–0.30) for softer, squashier “Ge” flavor.
// 4) **Presence Shelf:** After the LPF, add a tiny high-shelf to bring back air.
// 5) **Two-Stage Stack:** Duplicate the shaper with different Vt/bias for complex fuzz.
// 6) **Gate-y Mode:** Couple bias to envelope (decrease Vt and pull bias negative on low levels).
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Structured like a textbook page for the
// HaroldPCB library (constants up top, prose user guide at the end). LEDs active-HIGH.
//
