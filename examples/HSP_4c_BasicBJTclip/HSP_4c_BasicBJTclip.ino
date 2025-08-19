// BasicBJT.ino — v1.0.2
// by Harold Street Pedals 2025
// BJT-style distortion using an exponential (diode-equation) soft clip,
// with drive, bias/asymmetry, tone, momentary kick, and true bypass.
//
// Signal Path: Input → Drive → BJT/Diode Soft Clip (+Bias & Asym) → Low-Pass Tone → Master → Out
//

#include <HaroldPCB.h>
#include <math.h>

// -----------------------------------------------------------------------------
// CONSTANTS (tweakable builder settings)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE (pre-gain in dB) — how hard we push into the BJT-like nonlinearity
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// FS1 momentary "kick" amount (extra pre-gain while held)
static const float EXTRA_DRIVE_DB = 6.0f;

// BJT / diode-equation shaper parameters
// Approx: IE ≈ Is * (exp(Vd/Vt) - 1).
// Vt sets curve steepness; smaller Vt => sharper knee (more aggressive).
static const float Vt_MIN = 0.20f;
static const float Vt_MAX = 0.60f;

// Output scaling after the diode mapping so nominal gain stays in range
static const float SHAPER_GAIN = 1.0f;

// Asymmetry (RV3) blends different positive/negative "Vt" to mimic BJT bias mismatch.
// SYM_RANGE_FRAC = how far ± we move Vt for + vs - sides.
static const float SYM_RANGE_FRAC = 0.45f;

// DC bias (pre-shaper) range to shift the operating point for asymmetry textures
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
// GLOBAL STATE
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (updated in loop(), used in AudioCB)
static volatile bool bypassOn = false;
static volatile bool kickOn = false;
static volatile float driveAmount = 1.0f;
static volatile float curveVtPositive = 0.35f;  // effective Vt for + half
static volatile float curveVtNegative = 0.35f;  // effective Vt for - half
static volatile float dcBias = 0.0f;

// One-pole low-pass tone coefficients: y[n] = B*x[n] + A*y[n-1]
// A = exp(-2*pi*fc/fs), B = 1 - A
static volatile float lowpassCoef_A = 0.0f;
static volatile float lowpassCoef_B = 1.0f;

// Low-pass memory (previous output y[n-1])
static float lowpassMemory = 0.0f;

// Clip indicator envelope
static float clipEnvelope = 0.0f;

// FS2 edge tracking for bypass toggle
static bool prevBypassSwitch = false;

// -----------------------------------------------------------------------------
// HELPERS
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db / 20.0f);
}

// One-pole low-pass coefficient update (post-tone)
static void UpdateLowpassFromCutoff(float cutoffHz) {
  cutoffHz = fmaxf(10.0f, fminf(cutoffHz, SAMPLE_RATE_HZ * 0.45f));
  float A = expf(-2.0f * (float)M_PI * cutoffHz / (float)SAMPLE_RATE_HZ);
  lowpassCoef_A = A;
  lowpassCoef_B = 1.0f - A;
}

// Diode-like soft clip with independent positive/negative steepness (Vt+ / Vt-).
// Mapping (signed): y = sign(xp) * (1 - exp(-|xp| / Vt_side)), with xp = input + dcBias.
// Then scaled by SHAPER_GAIN. We set 'clipped' when the curve is clearly bending.
static inline float BJT_DiodeSoftClip(float input,
                                      float Vt_pos,
                                      float Vt_neg,
                                      float bias,
                                      bool &clipped) {
  float shiftedInput = input + bias;  // move the waveform left/right (DC bias)
  float magnitude = fabsf(shiftedInput);
  float VtSide = (shiftedInput >= 0.0f) ? Vt_pos : Vt_neg;
  VtSide = fmaxf(0.05f, VtSide);  // safety floor

  // Soft knee: approaches ±1 as input grows, but bends smoothly (exponential)
  float uni = 1.0f - expf(-magnitude / VtSide);
  float shaped = copysignf(uni, shiftedInput) * SHAPER_GAIN;

  // Heuristic for the clip LED: “far into the bend region”
  if (magnitude > 0.6f * VtSide) clipped = true;

  return shaped;
}

// -----------------------------------------------------------------------------
// AUDIO CALLBACK (runs very fast, one tiny slice of sound at a time)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (bypassOn) {
    out = in;  // true bypass: just pass the sound through
    return;
  }

  // ------------------------------
  // 1) DRIVE STAGE (Pre‑Gain)
  // ------------------------------
  // Turn the guitar signal up before we distort it.
  // driveAmount from RV1; EXTRA_DRIVE_DB kicks in while FS1 held.
  float preGain = driveAmount * (kickOn ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);
  float drivenSignal = in * preGain;

  // ------------------------------
  // 2) CLIPPING STAGE (BJT‑style, diode equation flavor)
  // ------------------------------
  // What: bend the waveform with an exponential curve.
  // How:  y = sign(x+bias) * (1 - exp(-|x+bias| / Vt_side)) * SHAPER_GAIN
  //       Vt_side = Vt+ (for +), Vt- (for -)
  // Why:  exponential rise mimics a transistor junction; two Vt’s give asymmetry.
  bool clippedNow = false;
  float shapedSignal = BJT_DiodeSoftClip(drivenSignal,
                                         curveVtPositive,
                                         curveVtNegative,
                                         dcBias,
                                         clippedNow);
  if (clippedNow) {
    // brief “pop” into an envelope, decay handled in loop()
    clipEnvelope = fminf(1.0f, CLIP_LED_ATTACK * (clipEnvelope + 0.30f));
  }

  // ------------------------------
  // 3) TONE STAGE (Low‑Pass Filter)
  // ------------------------------
  // What: gently reduce treble after distortion.
  // How:  1‑pole LPF: y[n] = B*x[n] + A*y[n-1], A=exp(-2πfc/fs), B=1−A
  lowpassMemory = lowpassCoef_B * shapedSignal + lowpassCoef_A * lowpassMemory;
  float filtered = lowpassMemory;

  // ------------------------------
  // 4) OUTPUT STAGE
  // ------------------------------
  // Trim and clamp to a safe ceiling.
  filtered *= OUTPUT_TRIM;
  filtered = fmaxf(-OUT_LIMIT, fminf(filtered, OUT_LIMIT));

  out = filtered;
}

// -----------------------------------------------------------------------------
// SETUP (runs once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // RV1 — Drive (dB → linear)
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Character (Vt), RV3 — Asymmetry (skews Vt+ vs Vt-)
  {
    float vtBase = H.ReadPotMapped(RV2, Vt_MIN, Vt_MAX, HPCB_Curve::Exp10);
    float rawAsym = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = rawAsym * SYM_RANGE_FRAC;
    curveVtPositive = fmaxf(0.05f, vtBase * (1.0f - skew));
    curveVtNegative = fmaxf(0.05f, vtBase * (1.0f + skew));
  }

  // RV4 — DC Bias (±BIAS_RANGE) pre-shaper
  {
    float rawBias = H.ReadPot(RV4) * 2.0f - 1.0f;  // -1..+1
    dcBias = rawBias * BIAS_RANGE;
  }

  // RV5 — Tone cutoff (LPF)
  {
    float cutoff0 = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(cutoff0);
  }

  // RV6 — Master (post)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// LOOP (controls/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  // Library housekeeping (debounce, footswitch/toggle service, etc.)
  H.Idle();

  // RV1 — Drive (dB → linear)
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Vt (curve steepness) + RV3 — Asymmetry skew
  {
    float vtBase = H.ReadPotMapped(RV2, Vt_MIN, Vt_MAX, HPCB_Curve::Exp10);
    float rawAsym = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = rawAsym * SYM_RANGE_FRAC;
    curveVtPositive = fmaxf(0.05f, vtBase * (1.0f - skew));
    curveVtNegative = fmaxf(0.05f, vtBase * (1.0f + skew));
  }

  // RV4 — DC bias (±BIAS_RANGE)
  {
    float rawBias = H.ReadPot(RV4) * 2.0f - 1.0f;
    dcBias = rawBias * BIAS_RANGE;
  }

  // RV5 — Tone cutoff (update one-pole LPF)
  {
    float cutoff = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(cutoff);
  }

  // RV6 — Master (post). Smooth when engaged; unity when bypassed.
  H.SetLevel(bypassOn ? 1.0f : H.ReadPotSmoothed(RV6, 15.0f));

  // FS1 — Kick (momentary)
  kickOn = H.FootswitchIsPressed(FS1);

  // FS2 — Bypass (edge-detected)
  bool bypassSwitch = H.FootswitchIsPressed(FS2);
  if (bypassSwitch && !prevBypassSwitch) {
    bypassOn = !bypassOn;
  }
  prevBypassSwitch = bypassSwitch;

  // LEDs (active‑HIGH on v1.3.0)
  H.SetLED(LED2, !bypassOn);                       // effect active
  clipEnvelope *= CLIP_LED_DECAY;                  // decay the clip glow
  H.SetLED(LED1, clipEnvelope > 0.12f || kickOn);  // clip meter / kick indicator
}

// -----------------------------------------------------------------------------
// USER GUIDE
// -----------------------------------------------------------------------------
//
// Overview
// --------
// **BasicBJT** uses an exponential, diode-like transfer (like a transistor’s base‑emitter)
// to bend the waveform smoothly into distortion. Compared to hard clipping, the knee is
// rounder, with rich odd harmonics and a wide range of asymmetry/bias textures.
//
// Controls
// --------
// - RV1 — Drive: pre‑gain (0 → +36 dB).
// - RV2 — Character (Vt): lower = sharper knee / more fuzz; higher = smoother.
// - RV3 — Asymmetry: skews the steepness of + vs – sides (mismatched devices).
// - RV4 — Bias: DC offset pre-shaper (±), shifting the operating point.
// - RV5 — Tone: post‑EQ treble cut (700 Hz → 9 kHz). (More on filters later.)
// - RV6 — Master: overall output level (post).
// - FS1 — Kick: hold for +6 dB into the shaper.
// - FS2 — Bypass: true passthrough toggle.
// - LED1 — Clip Meter: brief glow when clipping; also lights during Kick.
// - LED2 — Effect Active.
//
// Signal Flow
// -----------
// Input → Drive → BJT/Diode Soft Clip (+Bias & Asym) → Low‑Pass Tone → Master → Out
//
// Tweak Points
// ------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: gain window.
// - Vt_MIN / Vt_MAX: curve steepness range.
// - SYM_RANGE_FRAC: asymmetry strength for Vt+ vs Vt-.
// - BIAS_RANGE: DC offset range pre‑shaper.
// - TONE_CUTOFF_MIN/MAX: post‑EQ voicing.
// - EXTRA_DRIVE_DB: Kick amount.
// - SHAPER_GAIN: overall shaper scale.
// - OUTPUT_TRIM / OUT_LIMIT: final gain and safety.
