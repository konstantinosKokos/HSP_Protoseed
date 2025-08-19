// HSP_BasicLEDSoftClip.ino — v1.0.1 (textbook-style, friendly guide)
// by Harold Street Pedals 2025
//
// LED‑style SOFT clipping (op‑amp feedback flavor) with Drive, Tone, Symmetry,
// momentary Kick, and true bypass. Post‑effect Master handled by the library.
//
// Signal path: Input → Drive → SoftClip (tanh, asym) → Tone LPF → Master → Output

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE: pre‑gain before saturation (in dB)
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// MOMENTARY KICK (FS1): extra drive while held
static const float EXTRA_DRIVE_DB = 6.0f;

// “LED feel” threshold (softer than hard clip)
// Think of this as a "bendiness" scale for positive/negative sides.
static const float CLIP_THR_SOFT = 0.40f;  // mid softness

// Symmetry skew range (RV3) — ±45% between positive/negative
static const float SYM_RANGE_FRAC = 0.45f;

// TONE: one‑pole low‑pass (post saturation)
static const float TONE_CUTOFF_MIN_HZ = 600.0f;
static const float TONE_CUTOFF_MAX_HZ = 8200.0f;

// Output trim + safety limit
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// LED indicator decay (simple envelope for "clipping happened" light)
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
static volatile float g_lp_a = 0.0f;  // tone LPF 'a'
static volatile float g_lp_b = 1.0f;  // (1 - a)

// Filter memory
static float lpMemory = 0.0f;

// Clip indicator (control thread owns LED fade)
static volatile bool g_clip_flag = false;
static float clipEnvelope = 0.0f;

// FS2 edge detect
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db * (1.0f / 20.0f));
}

// One‑pole low‑pass coefficient update (post‑tone)
// y[n] = b*x[n] + a*y[n-1], a = exp(-2*pi*fc/fs), b = 1 - a
static void UpdateLowpassFromCutoff(float cutoffHz) {
  float safeCutoff = fmaxf(10.0f, fminf(cutoffHz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * safeCutoff / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

// -----------------------------------------------------------------------------
// SOFT CLIPPER (LED feedback style) — heavily commented
// -----------------------------------------------------------------------------
// What it does (plain English):
// • We want that "LED in the feedback path" feel: instead of slicing the top off
//   flat (hard clip), we BEND toward a ceiling smoothly.
// • We use tanh() because it bends gently as the input gets bigger.
// • Real pedals can clip differently on + and – (diode forward drops, bias, etc).
//   We model that by giving the positive and negative sides their own "softness"
//   numbers (thr_pos, thr_neg). Smaller threshold = bends sooner = more grit.
//
// How it works (step by step):
// 1) Look at the sign of the input sample.
// 2) Choose the threshold for that side (thr_pos for +, thr_neg for –).
// 3) Divide by that threshold so tanh() "feels" where to bend.
// 4) Multiply back by the threshold so the overall output level makes sense.
// 5) If the input magnitude is bigger than the threshold, we mark "clipped" so
//    the LED can show activity.
//
// Inputs:
//   x          — the pre‑gained audio sample
//   thr_pos    — softness scale for positive side
//   thr_neg    — softness scale for negative side
//   clipped    — output flag we set to true if bending is happening clearly
//
// Output:
//   y — the softly bent version of x
static inline float SoftClipAsym(float x, float thr_pos, float thr_neg, bool &clipped) {
  float y = 0.0f;

  if (x >= 0.0f) {
    float bendAmount = x / thr_pos;   // how close we are to the bend zone on +
    y = thr_pos * tanhf(bendAmount);  // bend smoothly, then scale back up
    if (fabsf(x) > thr_pos) clipped = true;
  } else {
    float bendAmount = x / thr_neg;  // how close we are to the bend zone on –
    y = thr_neg * tanhf(bendAmount);
    if (fabsf(x) > thr_neg) clipped = true;
  }

  return y;
}

// -----------------------------------------------------------------------------
// Audio callback (audio thread only — NO direct reads of knobs/switches)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (g_bypassed) {
    out = in;  // true bypass
    return;
  }

  // 1) Pre‑gain (Drive) + optional Kick
  float driveBoost = g_drive_lin * (g_kick ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);
  float driven = in * driveBoost;

  // 2) Soft clip (LED feedback style) with asymmetry
  bool clippedNow = false;
  float bent = SoftClipAsym(driven, g_thr_pos, g_thr_neg, clippedNow);
  if (clippedNow) g_clip_flag = true;

  // 3) Post‑TONE LPF (brief here; full filter lessons later)
  //    One‑pole treble cut controlled by RV2 (via cutoff → g_lp_a/g_lp_b)
  lpMemory = g_lp_b * bent + g_lp_a * lpMemory;
  float toned = lpMemory;

  // 4) Output trim + safety limit
  float limited = fmaxf(-OUT_LIMIT, fminf(toned * OUTPUT_TRIM, OUT_LIMIT));

  out = limited;
}

// -----------------------------------------------------------------------------
// Setup (runs once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initial control state from hardware
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  float initialCutoff = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(initialCutoff);

  // RV3 — Symmetry skew (±) maps to different thresholds for + and –
  {
    float knobPosition = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = knobPosition * SYM_RANGE_FRAC;
    g_thr_pos = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f + skew));
    g_thr_neg = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f - skew));
  }

  // Master (post) from RV6
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (control/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  // IMPORTANT: services pots/toggles/footswitch debounce in v1.3.0
  H.Idle();

  // RV1 — Drive (dB → linear)
  {
    float knobPosition = H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10);
    g_drive_lin = dB_to_lin(knobPosition);
  }

  // RV2 — Tone cutoff (maps to LPF coefficients)
  {
    float cutoffHz = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(cutoffHz);
  }

  // RV3 — Symmetry (skews positive/negative softness)
  {
    float knobPosition = H.ReadPot(RV3) * 2.0f - 1.0f;  // -1..+1
    float skew = knobPosition * SYM_RANGE_FRAC;
    g_thr_pos = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f + skew));
    g_thr_neg = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f - skew));
  }

  // RV6 — Master level (post)
  {
    float masterKnob = H.ReadPotSmoothed(RV6, 15.0f);  // mild smoothing for fine control
    if (!g_bypassed) {
      H.SetLevel(masterKnob);  // when engaged, knob controls master
    } else {
      H.SetLevel(1.0f);  // when bypassed, force unity passthrough
    }
  }

  // FS1 — Kick (momentary extra drive)
  g_kick = H.FootswitchIsPressed(FS1);

  // FS2 — Bypass toggle (edge detect)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2) g_bypassed = !g_bypassed;
    prev_fs2 = fs2;
  }

  // LEDs
  H.SetLED(LED2, !g_bypassed);  // Effect active indicator

  // Clip LED envelope (lights when bendy stuff happened recently)
  if (g_clip_flag) {
    clipEnvelope = 1.0f;
    g_clip_flag = false;
  }
  clipEnvelope *= CLIP_LED_DECAY;
  H.SetLED(LED1, clipEnvelope > 0.12f || g_kick);  // show clipping or Kick
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// This pedal models **LED soft clipping** (as in an op‑amp feedback path). Instead
// of chopping flat, the curve bends smoothly (tanh). You can bias symmetry so the
// + and – halves bend differently, which changes the feel and harmonics. A simple
// post‑LPF “Tone” trims treble. FS1 adds a momentary drive “Kick”. FS2 toggles a
// true bypass path. RV6 sets post‑effect level via the library.
//
// Controls
// --------
// - RV1 — Drive: 0 to +36 dB pre‑gain (use HPCB_Curve::Exp10 mapping).
// - RV2 — Tone: low‑pass cutoff (600 Hz → 8.2 kHz).
// - RV3 — Symmetry: skews positive/negative bend amount.
// - RV6 — Master: overall output level (post).
// - FS1 — Kick: hold for +6 dB extra pre‑gain.
// - FS2 — Bypass: toggles clean passthrough.
// - LED1 — Clip/Kick indicator (envelope decay).
// - LED2 — Effect active.
//
// Notes
// -----
// • Clipping section is thoroughly commented above.
// • Tone is a 1‑pole LPF here for clarity; we’ll dive deeper into filter designs
//   in the dedicated “Filters” chapter.
// • Adjust CLIP_THR_SOFT for softer/harder bending; tweak SYM_RANGE_FRAC for
//   stronger/weaker asymmetry; push DRIVE_MAX_DB for wilder saturation.
//
