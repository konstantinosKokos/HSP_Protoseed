// BasicOpAmp.ino — v1.0.1
// by Harold Street Pedals 2025
// High‑gain op‑amp style drive: soft feedback clipping + blendable hard diodes,
// post tone, momentary kick, and true bypass.
// LEDs are active‑HIGH (HaroldPCB v1.3.0+): SetLED(..., true) turns the LED on.
//
// Signal Path
// -----------
// Input → Drive (pre‑gain) → [Soft Feedback || Hard Diodes] → Crossfade Mix → LPF Tone → Master (library) → Output

#include <HaroldPCB.h>
#include <math.h>

// -----------------------------------------------------------------------------
// CONSTANTS (tweakable builder settings)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE (pre‑gain into the virtual op‑amp), in dB
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// FS1 "kick" (extra pre‑gain while held)
static const float EXTRA_DRIVE_DB = 6.0f;

// Soft feedback clip “threshold” (shaping scale). Smaller = more saturation.
static const float SOFT_THR_MIN = 0.18f;
static const float SOFT_THR_MAX = 0.55f;

// Hard diode clip threshold (emulates Si/LED to ground). Smaller = dirtier.
static const float HARD_THR_MIN = 0.20f;
static const float HARD_THR_MAX = 0.50f;

// Mix between soft/hard stages (RV3): 0 = all soft (feedback), 1 = all hard (to‑ground).
static const float MIX_MIN = 0.0f;
static const float MIX_MAX = 1.0f;

// Post‑tone: one‑pole treble cut after clipping
static const float TONE_CUTOFF_MIN_HZ = 700.0f;
static const float TONE_CUTOFF_MAX_HZ = 9500.0f;

// Output trim + safety ceiling
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// Clip LED envelope
static const float CLIP_LED_ATTACK = 1.00f;
static const float CLIP_LED_DECAY = 0.90f;

// -----------------------------------------------------------------------------
// GLOBAL STATE
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (updated in loop, used in audio)
static volatile bool bypassOn = false;
static volatile bool kickOn = false;
static volatile float driveAmount = 1.0f;
static volatile float softThreshold = 0.35f;
static volatile float hardThreshold = 0.30f;
static volatile float mixHardAmount = 0.25f;  // 0..1

// One‑pole low‑pass: y[n] = B*x[n] + A*y[n‑1], A = exp(-2πfc/fs), B = 1‑A
static volatile float lowpassCoef_A = 0.0f;
static volatile float lowpassCoef_B = 1.0f;
static float lowpassMemory = 0.0f;

// Clip meter envelope (for LED1)
static float clipEnvelope = 0.0f;

// FS2 edge tracking for bypass toggle
static bool prevBypassSwitch = false;

// -----------------------------------------------------------------------------
// HELPERS
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db / 20.0f);
}

// Compute one‑pole LPF coefficients for given cutoff
static void UpdateLowpassFromCutoff(float cutoffHz) {
  cutoffHz = fmaxf(10.0f, fminf(cutoffHz, SAMPLE_RATE_HZ * 0.45f));
  float A = expf(-2.0f * (float)M_PI * cutoffHz / (float)SAMPLE_RATE_HZ);
  lowpassCoef_A = A;
  lowpassCoef_B = 1.0f - A;
}

// Soft feedback clip: tanh‑like feedback shaping
// Math picture (plain English):
// - Imagine an op‑amp whose gain is tamed by diodes in its feedback path.
// - The more the signal grows, the more the “feedback diodes” push back,
//   so instead of a sharp cutoff we get a smooth bend (an S‑curve).
// Implementation here:
//   y = thr * tanh(x / thr)
// Where:
//   - x  is the incoming sample (after Drive).
//   - thr (softThreshold) is a scale factor that “spreads” the bend:
//       smaller thr  → bend happens earlier (more saturation).
//       larger  thr  → bend happens later  (cleaner).
// Heuristic flag ‘nonlinear’ is set when x is big enough that the curve is
// clearly bending (used to light the clip LED).
static inline float SoftFeedbackShaper(float x, float thr, bool &nonlinear) {
  float shaped = thr * tanhf(x / thr);
  if (fabsf(x) > 0.6f * thr) nonlinear = true;  // “into the knee” → likely audible bend
  return shaped;
}

// Hard diode clip to ground (symmetric)
// Math picture (plain English):
// - This is the classic “ceiling” at ±threshold, like antiparallel diodes to ground.
// - Below the ceiling: pass straight through.
// - Above it: cut flat at +threshold / -threshold.
// This adds strong edges (more bite) compared to the soft feedback bend.
static inline float HardDiodeClipper(float x, float thr, bool &nonlinear) {
  if (x > thr) {
    nonlinear = true;
    return thr;
  }
  if (x < -thr) {
    nonlinear = true;
    return -thr;
  }
  return x;
}

// -----------------------------------------------------------------------------
// AUDIO CALLBACK (runs at audio rate; use cached values only)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (bypassOn) {
    out = in;
    return;
  }

  // 1) Pre‑gain with momentary kick (FS1)
  //    We multiply the input sample by:
  //      driveAmount (from RV1, dB→linear) and, if kickOn, an extra +6 dB.
  float preGain = driveAmount * (kickOn ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);
  float driven = in * preGain;

  // 2) Run both shapers independently, then crossfade them
  //    SoftFeedbackShaper() → smooth bend (feedback feel)
  //    HardDiodeClipper()   → flat ceiling (to‑ground bite)
  bool bentSoft = false, bentHard = false;
  float y_soft = SoftFeedbackShaper(driven, softThreshold, bentSoft);
  float y_hard = HardDiodeClipper(driven, hardThreshold, bentHard);

  // Crossfade:
  //   mixHardAmount = 0   → all soft
  //   mixHardAmount = 1   → all hard
  float mixed = (1.0f - mixHardAmount) * y_soft + mixHardAmount * y_hard;

  // 3) Post TONE (brief: one‑pole LPF; we’ll cover filters deeper later)
  lowpassMemory = lowpassCoef_B * mixed + lowpassCoef_A * lowpassMemory;
  float toned = lowpassMemory;

  // 4) Output trim + safety limit
  toned *= OUTPUT_TRIM;
  toned = fmaxf(-OUT_LIMIT, fminf(toned, OUT_LIMIT));

  // Clip meter envelope “kick” when either stage is clearly nonlinear
  if (bentSoft || bentHard)
    clipEnvelope = fminf(1.0f, CLIP_LED_ATTACK * (clipEnvelope + 0.30f));

  out = toned;
}

// -----------------------------------------------------------------------------
// SETUP (runs once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Pots at boot
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));
  softThreshold = H.ReadPotMapped(RV2, SOFT_THR_MIN, SOFT_THR_MAX, HPCB_Curve::Exp10);  // “character”
  hardThreshold = H.ReadPotMapped(RV4, HARD_THR_MIN, HARD_THR_MAX, HPCB_Curve::Exp10);  // “diode level”
  mixHardAmount = H.ReadPot(RV3);                                                       // 0..1 mix

  float cutoff0 = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(cutoff0);

  // Master (post)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// LOOP (controls/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  // If you prefer, remove this later once you centralize control servicing elsewhere.
  H.Idle();

  // RV1 — Drive (dB → linear)
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Soft feedback character (threshold)
  softThreshold = H.ReadPotMapped(RV2, SOFT_THR_MIN, SOFT_THR_MAX, HPCB_Curve::Exp10);

  // RV3 — Soft↔Hard blend (0..1)
  mixHardAmount = H.ReadPot(RV3);

  // RV4 — Hard diode threshold
  hardThreshold = H.ReadPotMapped(RV4, HARD_THR_MIN, HARD_THR_MAX, HPCB_Curve::Exp10);

  // RV5 — Post Tone cutoff (LPF)
  {
    float cutoff = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(cutoff);
  }

  // RV6 — Master (post). Smooth when engaged; unity when bypassed.
  H.SetLevel(bypassOn ? 1.0f : H.ReadPotSmoothed(RV6, 15.0f));

  // FS1 — Kick (momentary)
  kickOn = H.FootswitchIsPressed(FS1);

  // FS2 — Bypass toggle (edge detect)
  bool bypassSwitch = H.FootswitchIsPressed(FS2);
  if (bypassSwitch && !prevBypassSwitch)
    bypassOn = !bypassOn;
  prevBypassSwitch = bypassSwitch;

  // LEDs (active‑HIGH)
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
// **BasicOpAmp** blends two classic clipping flavors:
// 1) **Soft feedback bend** (tanh‑like): smooth, LED/feedback vibe.
// 2) **Hard to ground** (flat ceiling): bite and edge.
// RV3 crossfades between them so you can sweep from TS‑like softness to RAT/Dist+‑style bite.
//
// Controls
// --------
// - RV1 — Drive: pre‑gain (0 → +36 dB).
// - RV2 — Soft Character: soft feedback threshold (smaller = more saturation).
// - RV3 — Soft↔Hard Mix: 0 = all soft feedback, 1 = all hard diodes.
// - RV4 — Hard Threshold: diode clip ceiling (smaller = dirtier).
// - RV5 — Tone: post treble cut (700 Hz → 9.5 kHz).
// - RV6 — Master: overall output level (post), via library SetLevel().
// - FS1 — Kick: hold for +6 dB extra pre‑gain.
// - FS2 — Bypass: true passthrough on/off.
// - LED1 — Clip Meter: shows saturation (decay) and lights during Kick.
// - LED2 — Effect Active: lit when engaged.
//
// Signal Flow (for quick reference)
// ---------------------------------
// Input → Drive → [Soft Feedback || Hard Diodes] → Crossfade Mix → LPF Tone → Master → Output
