// BasicOpAmp.ino — v1.0.0
// by Harold Street Pedals 2025
// High-gain op-amp style drive: soft feedback clipping with blendable hard diodes,
// post tone, momentary kick, and true bypass.
// LEDs are active-HIGH (HaroldPCB v1.3.0+): SetLED(..., true) turns the LED on.

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE (pre-gain into the virtual op-amp), in dB
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// FS1 "kick" (extra pre-gain while held)
static const float EXTRA_DRIVE_DB = 6.0f;

// Soft feedback clip "threshold" (shaping scale). Lower => more saturation.
static const float SOFT_THR_MIN = 0.18f;
static const float SOFT_THR_MAX = 0.55f;

// Hard diode clip threshold (emulates silicon/LED to ground). Lower => dirtier.
static const float HARD_THR_MIN = 0.20f;
static const float HARD_THR_MAX = 0.50f;

// Mix between soft/hard stages (RV3): 0 = all soft (feedback style), 1 = all hard (to-ground).
static const float MIX_MIN = 0.0f;
static const float MIX_MAX = 1.0f;

// Post-tone: one-pole treble cut after clipping
static const float TONE_CUTOFF_MIN_HZ = 700.0f;
static const float TONE_CUTOFF_MAX_HZ = 9500.0f;

// Output trim + safety ceiling
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// Clip LED envelope
static const float CLIP_LED_ATTACK = 1.00f;
static const float CLIP_LED_DECAY = 0.90f;

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (updated in loop, used in audio)
static volatile bool g_bypassed = false;
static volatile bool g_kick = false;
static volatile float g_drive_lin = 1.0f;
static volatile float g_soft_thr = 0.35f;
static volatile float g_hard_thr = 0.30f;
static volatile float g_mix_hard = 0.25f;  // 0..1
static volatile float g_lp_a = 0.0f;
static volatile float g_lp_b = 1.0f;

// Filter memory
static float g_lp_z = 0.0f;

// Clip meter envelope
static float g_clip_env = 0.0f;

// FS2 edge tracking
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db * (1.0f / 20.0f));
}

// One-pole low-pass coeffs: y = b*x + a*y[n-1], a = exp(-2*pi*fc/fs), b = 1-a
static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

// Soft feedback clip: tanh with adjustable "threshold" scale
// Approximates an op-amp with diodes in the feedback path.
static inline float SoftFeedback(float x, float thr, bool &nonlinear) {
  float y = thr * tanhf(x / thr);
  nonlinear |= (fabsf(x) > thr * 0.6f);
  return y;
}

// Hard diode clip to ground with symmetric thresholds
static inline float HardDiode(float x, float thr, bool &nonlinear) {
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
// Audio callback (audio thread). No control reads here.
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (g_bypassed) {
    out = in;
    return;
  }

  // 1) Pre-gain with momentary kick
  float pre = in * g_drive_lin * (g_kick ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);

  // 2) Run both shapers, then crossfade by g_mix_hard
  bool nl_soft = false, nl_hard = false;
  float y_soft = SoftFeedback(pre, g_soft_thr, nl_soft);
  float y_hard = HardDiode(pre, g_hard_thr, nl_hard);

  float mix = g_mix_hard;  // 0..1
  float y = (1.0f - mix) * y_soft + mix * y_hard;

  // 3) Post tone (LPF)
  g_lp_z = g_lp_b * y + g_lp_a * g_lp_z;
  y = g_lp_z;

  // 4) Output trim + safety
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUT_LIMIT, fminf(y, OUT_LIMIT));

  // Clip meter
  if (nl_soft || nl_hard) g_clip_env = fminf(1.0f, CLIP_LED_ATTACK * (g_clip_env + 0.30f));

  out = y;
}

// -----------------------------------------------------------------------------
// Setup (once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Pots at boot
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));
  g_soft_thr = H.ReadPotMapped(RV2, SOFT_THR_MIN, SOFT_THR_MAX, HPCB_Curve::Exp10);  // "character"
  g_hard_thr = H.ReadPotMapped(RV4, HARD_THR_MIN, HARD_THR_MAX, HPCB_Curve::Exp10);  // "diode level"
  g_mix_hard = H.ReadPot(RV3);                                                       // 0..1 mix

  float fc0 = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(fc0);

  // Master (post)
  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // Don't read pot if bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set unity gain.
  }

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (control/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();

  // RV1 — Drive (dB → linear)
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Soft feedback character (threshold)
  g_soft_thr = H.ReadPotMapped(RV2, SOFT_THR_MIN, SOFT_THR_MAX, HPCB_Curve::Exp10);

  // RV3 — Soft↔Hard blend (0..1)
  g_mix_hard = H.ReadPot(RV3);

  // RV4 — Hard diode threshold
  g_hard_thr = H.ReadPotMapped(RV4, HARD_THR_MIN, HARD_THR_MAX, HPCB_Curve::Exp10);

  // RV5 — Post Tone cutoff
  {
    float fc = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc);
  }

  // RV6 — Master (post), lightly smoothed
  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // Don't read pot if bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set unity gain.
  }

  // FS1 — Kick (momentary)
  g_kick = H.FootswitchIsPressed(FS1);

  // FS2 — Bypass (edge detect)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2) g_bypassed = !g_bypassed;
    prev_fs2 = fs2;
  }

  // LEDs (active-HIGH)
  H.SetLED(LED2, !g_bypassed);  // Effect active
  g_clip_env *= CLIP_LED_DECAY;
  H.SetLED(LED1, g_clip_env > 0.12f || g_kick);  // Clip/kick indicator
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// **BasicOpAmp** captures the feel of a high-gain op-amp dirt block. A soft
// feedback clipper (tanh-like, LED-ish) provides smooth saturation, while a
// hard diode-to-ground clipper adds bite. RV3 blends between the two worlds,
// so you can sweep from TS-like softness to RAT/Dist+-style edge.
//
// Controls
// --------
// - RV1 — Drive: pre-gain into the stage (0 → +36 dB).
// - RV2 — Soft Character: soft feedback threshold (lower = more saturation).
// - RV3 — Soft↔Hard Mix: 0 = all soft feedback, 1 = all hard diodes.
// - RV4 — Hard Threshold: diode clip ceiling (lower = dirtier).
// - RV5 — Tone: post treble cut (700 Hz → 9.5 kHz).
// - RV6 — Master: overall output level (post), via library SetLevel().
// - FS1 — Kick: hold for +6 dB extra pre-gain.
// - FS2 — Bypass: true passthrough on/off.
// - LED1 — Clip Meter: shows saturation (decay) and lights during Kick.
// - LED2 — Effect Active: lit when engaged.
//   (LEDs are **active-HIGH** with HaroldPCB v1.3.0+.)
//
// Signal Flow
// -----------
// Input → Drive → [Soft Feedback || Hard Diodes] → Mix → LPF Tone → Master → Out
//
// Customizable Parameters (top of file)
// -------------------------------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: overall gain window.
// - SOFT_THR_MIN/MAX: feedback clip “softness” range.
// - HARD_THR_MIN/MAX: diode ceiling range; retune for “LED color” vibes.
// - MIX_MIN/MAX: change the sweep behavior or quantize into modes.
// - TONE_CUTOFF_MIN/MAX: post-EQ voicing.
// - EXTRA_DRIVE_DB: Kick amount.
// - OUTPUT_TRIM / OUT_LIMIT: global level and safety.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) **Pre-EQ Tight**: High-pass at 120 Hz before Drive for tighter lows.
// 2) **Presence**: Add a tiny post high-shelf to reopen air above the LPF.
// 3) **Symmetry**: Add a bias pot to asymmetrically offset soft clip before tanh.
// 4) **Modes on TS1/TS2**: Toggle diode types: Si vs LED (just change thresholds).
// 5) **Anti-alias**: If you push extreme drive, experiment with lightweight 2× OS.
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Structured as a textbook example for the
// HaroldPCB library (constants up top, prose user guide at the end). LEDs active-HIGH.
//
