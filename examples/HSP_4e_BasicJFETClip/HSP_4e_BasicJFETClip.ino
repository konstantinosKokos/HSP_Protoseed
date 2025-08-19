// BasicJFETclip.ino — v1.0.1
// by Harold Street Pedals 2025
// Touch-sensitive JFET-style overdrive (square‑law saturation) with bias/asymmetry,
// simple tone, momentary kick, and true bypass.
//
// Signal path: Input → Drive → JFET Square‑Law (+DC Bias) → LPF Tone → Master → Output

#include <HaroldPCB.h>
#include <math.h>

// -----------------------------------------------------------------------------
// CONSTANTS (tweakable builder settings)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project‑wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project‑wide

// DRIVE (pre‑gain before the JFET stage), in dB
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 30.0f;  // ~31.6x linear

// MOMENTARY kick on FS1 (extra drive while held)
static const float EXTRA_DRIVE_DB = 6.0f;

// JFET shaping parameters
// Square‑law soft knee model (see math in JFET_SquareLaw()):
//   y = x' − softness * x' * |x'|,  where  x' = x + dcBias
// Larger "softness" => earlier/softer saturation; smaller => cleaner headroom.
static const float SOFTNESS_MIN = 0.10f;  // gentle saturation
static const float SOFTNESS_MAX = 0.60f;  // earlier/squishier saturation

// Bias (RV3) introduces asymmetry similar to gate bias / Vgs offset.
// Positive bias pushes the curve one way; negative the other (changes texture).
static const float BIAS_RANGE = 0.30f;  // ±0.30 added pre‑shaper (on normalized input)

// Post‑TONE: simple treble‑cut one‑pole LPF after the stage
static const float TONE_CUTOFF_MIN_HZ = 700.0f;
static const float TONE_CUTOFF_MAX_HZ = 9000.0f;

// Output trim + safety ceiling
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// Clip indicator envelope (LED1)
static const float CLIP_LED_ATTACK = 1.00f;
static const float CLIP_LED_DECAY = 0.90f;

// -----------------------------------------------------------------------------
// GLOBAL STATE
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (read in loop, used in AudioCB)
static volatile bool bypassOn = false;
static volatile bool kickOn = false;
static volatile float driveAmount = 1.0f;      // linear pre‑gain
static volatile float softnessAmount = 0.30f;  // was "alpha"
static volatile float dcBias = 0.0f;

// One‑pole low‑pass tone coefficients (post tone):
//   y[n] = B*x[n] + A*y[n−1],  where  A = exp(−2π fc / fs),  B = 1 − A
static volatile float lowpassCoef_A = 0.0f;
static volatile float lowpassCoef_B = 1.0f;

// Low‑pass memory (previous output)
static float lowpassMemory = 0.0f;

// Clip meter envelope
static float clipEnvelope = 0.0f;

// FS2 edge tracking for bypass toggle
static bool prevBypassSwitch = false;

// -----------------------------------------------------------------------------
// HELPERS
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db / 20.0f);
}

// Compute one‑pole LPF coefficients from a cutoff in Hz
static void UpdateLowpassFromCutoff(float cutoffHz) {
  cutoffHz = fmaxf(10.0f, fminf(cutoffHz, SAMPLE_RATE_HZ * 0.45f));
  float A = expf(-2.0f * (float)M_PI * cutoffHz / (float)SAMPLE_RATE_HZ);
  lowpassCoef_A = A;
  lowpassCoef_B = 1.0f - A;
}

// JFET square‑law saturator with DC bias (for asymmetry/texture)
// ----------------------------------------------------------------
// Intuition first:
// - Imagine the waveform as a line that gets gently "dragged down" as it grows.
// - The nonlinear term (softnessAmount * x' * |x'|) increases faster than linearly,
//   so the tops/valleys flatten smoothly (a soft knee), like a JFET approaching pinch‑off.
// - Adding a tiny dcBias shifts where each half starts bending → asymmetry.
//
// Math (per sample):
//   x' = input + dcBias                   // shift operating point
//   nonlin = softnessAmount * x' * |x'|   // square‑law "drag"
//   y = x' − nonlin                       // output after soft knee
//
// We also raise a "clipped" flag when the nonlinear part is a noticeable fraction
// of the total, to drive the clip‑indicator LED.
static inline float JFET_SquareLaw(float input,
                                   float softness,
                                   float bias,
                                   bool &clipped) {
  // 1) Shift the waveform left/right to change symmetry feel
  float shifted = input + bias;

  // 2) Compute the square‑law term (grows ~quadratically with |shifted|)
  float nonlinear = softness * shifted * fabsf(shifted);

  // 3) Apply soft knee by subtracting the nonlinear "drag"
  float shaped = shifted - nonlinear;

  // 4) Heuristic for clip LED: if nonlinear part is significant vs the signal
  //    (the tiny +0.001f avoids divide-by-0 when shifted≈0)
  if (fabsf(nonlinear) > 0.15f * (0.001f + fabsf(shifted)))
    clipped = true;

  return shaped;
}

// -----------------------------------------------------------------------------
// AUDIO CALLBACK (runs at audio rate; keep it tiny and allocation‑free)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (bypassOn) {
    out = in;
    return;
  }

  // 1) Pre‑gain (with momentary kick)
  //    Turn the input up before distortion:
  //    pre = input * driveAmount * (kick ? +6 dB : 0 dB)
  float pre = in * driveAmount * (kickOn ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);

  // 2) JFET square‑law saturation with dcBias for asymmetry
  bool clippedNow = false;
  float shaped = JFET_SquareLaw(pre, softnessAmount, dcBias, clippedNow);

  // 3) Post TONE (one‑pole low‑pass). This reduces harsh treble after clipping.
  //    y[n] = B * x[n] + A * y[n−1]
  lowpassMemory = lowpassCoef_B * shaped + lowpassCoef_A * lowpassMemory;
  float filtered = lowpassMemory;

  // 4) Output trim + a gentle safety limiter
  filtered *= OUTPUT_TRIM;
  filtered = fmaxf(-OUT_LIMIT, fminf(filtered, OUT_LIMIT));

  // Update clip‑LED envelope (attack in audio, decay in loop)
  if (clippedNow)
    clipEnvelope = fminf(1.0f, CLIP_LED_ATTACK * (clipEnvelope + 0.30f));

  out = filtered;
}

// -----------------------------------------------------------------------------
// SETUP (runs once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initial pot reads
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));
  softnessAmount = H.ReadPotMapped(RV2, SOFTNESS_MIN, SOFTNESS_MAX, HPCB_Curve::Exp10);  // "grit"

  // RV3 → dcBias in ±BIAS_RANGE
  {
    float bias01 = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    dcBias = bias01 * BIAS_RANGE;
  }

  // RV4 → Tone cutoff
  {
    float cutoff0 = H.ReadPotMapped(RV4, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(cutoff0);
  }

  // Master (post)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// LOOP (control/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();  // keep UI stable (pots/switches debounced, etc.)

  // RV1 — Drive (dB → linear)
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Grit (square‑law softness)
  softnessAmount = H.ReadPotMapped(RV2, SOFTNESS_MIN, SOFTNESS_MAX, HPCB_Curve::Exp10);

  // RV3 — Bias (asymmetry)
  {
    float bias01 = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    dcBias = bias01 * BIAS_RANGE;
  }

  // RV4 — Tone (LPF cutoff)
  {
    float cutoff = H.ReadPotMapped(RV4, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(cutoff);
  }

  // RV6 — Master (post). Smooth when engaged; unity when bypassed.
  H.SetLevel(bypassOn ? 1.0f : H.ReadPotSmoothed(RV6, 15.0f));

  // FS1 — momentary kick
  kickOn = H.FootswitchIsPressed(FS1);

  // FS2 — bypass toggle (edge detect)
  {
    bool bypassSwitch = H.FootswitchIsPressed(FS2);
    if (bypassSwitch && !prevBypassSwitch) { bypassOn = !bypassOn; }
    prevBypassSwitch = bypassSwitch;
  }

  // LEDs (active‑HIGH): LED2 = effect active; LED1 = clip meter (decays each loop)
  H.SetLED(LED2, !bypassOn);
  clipEnvelope *= CLIP_LED_DECAY;
  H.SetLED(LED1, clipEnvelope > 0.12f || kickOn);
}

// -----------------------------------------------------------------------------
// USER GUIDE
// -----------------------------------------------------------------------------
//
// Overview
// --------
// This is a touch‑sensitive **JFET‑style** overdrive built around a square‑law
// nonlinearity. The shaper approximates how a JFET compresses as it nears pinch‑off,
// with a bias control to introduce asymmetry (similar to shifting Vgs). It feels
// dynamic, cleans up with guitar volume, and takes EQ well.
//
// Controls
// --------
// - RV1 — Drive: pre‑gain into the stage (0 to +30 dB).
// - RV2 — Grit (Softness): sets how early/strong the square‑law knee engages.
// - RV3 — Bias: introduces asymmetry (±), shifting the knee for touch/texture.
// - RV4 — Tone: post‑EQ treble cut (700 Hz to 9 kHz), smooths fizz.
// - RV6 — Master: overall output level (post), via library SetLevel().
// - FS1 — Kick: hold for +6 dB extra drive into the stage.
// - FS2 — Bypass: true passthrough on/off.
// - LED1 — Clip Meter: lights on saturation (decays over time); also on during Kick.
// - LED2 — Effect Active: lit when the effect is engaged. (LEDs are active‑HIGH.)
//
// Signal Flow
// -----------
// Input → Drive → JFET Square‑Law (+DC Bias) → LPF Tone → Master → Output
//
// Customizable Parameters (top of file)
// -------------------------------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: overall gain window.
// - SOFTNESS_MIN / MAX: grit range; higher = earlier/squishier saturation.
// - BIAS_RANGE: strength of asymmetry to mimic gate bias.
// - TONE_CUTOFF_MIN/MAX: post‑EQ voicing window.
// - EXTRA_DRIVE_DB: momentary kick level.
// - OUTPUT_TRIM / OUT_LIMIT: global gain and safety.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) **Source Degeneration Feel:** Add a small pre‑emphasis (HPF 100–150 Hz)
//    before the shaper to “tighten” the lows like a source resistor would.
// 2) **JFET‑to‑Tube Blend:** Crossfade between square‑law (this) and tanh()
//    (tube‑ish) on TS1 for two flavors.
// 3) **Dynamic Bias:** Modulate dcBias slightly with input envelope for “sag.”
// 4) **Two‑Band Drive:** Split lows/highs, feed different softness values, then sum.
// 5) **Presence Shelf:** Add a gentle post high‑shelf to reopen the top after tone.
