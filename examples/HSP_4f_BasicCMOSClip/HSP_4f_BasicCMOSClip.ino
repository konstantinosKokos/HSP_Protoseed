// BasicCMOS.ino — v1.0.0
// by Harold Street Pedals 2025
// CMOS inverter–style fuzz (CD4049/4069 vibe) using odd-symmetric polynomial shaping,
// optional supply "sag," post tone, momentary kick, and true bypass.
// LEDs are active-HIGH (HaroldPCB v1.3.0+): SetLED(..., true) turns the LED on.

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE = 8;          // fixed project-wide

// DRIVE (pre-gain into the CMOS shaper), in dB
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// FS1 momentary "kick" extra drive while held
static const float EXTRA_DRIVE_DB = 6.0f;

// CMOS polynomial shape: y = g1*x - a*x^3 + b*x^5, then soft-limit
// - a: cubic "crunch" (dominant odd harmonic; Red Llama-ish growl)
// - b: fifth "fizz" (adds spitty top, classic CMOS glass)
static const float A_MIN = 0.40f;  // cubic minimum
static const float A_MAX = 1.60f;  // cubic maximum
static const float B_MIN = 0.00f;  // fifth minimum (off)
static const float B_MAX = 0.80f;  // fifth maximum

// Pre-scaler (linear) before the polynomial so sweep stays musical
static const float PRE_GAIN_MIN = 0.8f;
static const float PRE_GAIN_MAX = 2.2f;

// Soft limiter after polynomial (keeps it from going square immediately)
static const float SOFT_LIM_THR = 0.95f;  // lower = softer

// Optional "sag" (envelope-controlled gain drop), RV5 in [0..1] maps to range below
static const float SAG_MIN = 0.00f;          // 0 = no sag
static const float SAG_MAX = 0.60f;          // strong droop at peaks
static const float SAG_ATTACK_MS = 3.0f;     // fast-ish
static const float SAG_RELEASE_MS = 120.0f;  // lazy recovery

// Post-TONE: one-pole treble cut after the shaper
static const float TONE_CUTOFF_MIN_HZ = 700.0f;
static const float TONE_CUTOFF_MAX_HZ = 9500.0f;

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

// Cached control state (updated in loop, consumed in audio)
static volatile bool g_bypassed = false;
static volatile bool g_kick = false;
static volatile float g_drive_lin = 1.0f;  // dB→linear
static volatile float g_pre_gain = 1.2f;   // PRE_GAIN_MIN..MAX
static volatile float g_a3 = 1.0f;         // cubic coeff
static volatile float g_b5 = 0.3f;         // fifth coeff
static volatile float g_sag_amt = 0.0f;    // 0..1 mapped to SAG_MIN..MAX
static volatile float g_lp_a = 0.0f;       // tone LPF a
static volatile float g_lp_b = 1.0f;       // tone LPF b

// Filter memories
static float g_lp_z = 0.0f;

// Sag detector state
static float g_env = 0.0f;
static float g_env_a_atk = 0.0f;
static float g_env_a_rel = 0.0f;

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

static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

static void UpdateSagTimes() {
  g_env_a_atk = 1.0f - expf(-1.0f / ((SAG_ATTACK_MS / 1000.0f) * SAMPLE_RATE_HZ));
  g_env_a_rel = 1.0f - expf(-1.0f / ((SAG_RELEASE_MS / 1000.0f) * SAMPLE_RATE_HZ));
}

// Soft limiter around ±SOFT_LIM_THR using tanh skirt
static inline float SoftLimit(float x) {
  const float t = SOFT_LIM_THR;
  if (fabsf(x) <= t) return x;
  float sign = (x >= 0.0f) ? 1.0f : -1.0f;
  float over = fabsf(x) - t;
  return sign * (t + tanhf(over));  // gentle approach to ±(t+1)
}

// CMOS polynomial shaper with odd symmetry + soft limit; sets 'clipped' when nonlinearity is active.
static inline float CMOS_Poly(float x, float pre, float a3, float b5, bool &clipped) {
  float v = x * pre;
  float v2 = v * v;
  float v3 = v2 * v;
  float v5 = v3 * v2;

  float y = v - a3 * v3 + b5 * v5;  // odd polynomial
  float yl = SoftLimit(y);

  // heuristic for clip LED: cubic term magnitude vs base
  if (fabsf(a3 * v3) > 0.08f || fabsf(b5 * v5) > 0.04f) clipped = true;
  return yl;
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
  float pre = g_drive_lin * (g_kick ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);

  // 2) Envelope follower for sag (rectified signal, AR smoothing)
  float a = fabsf(in);
  float coef = (a > g_env) ? g_env_a_atk : g_env_a_rel;
  g_env += coef * (a - g_env);

  // Compute sag factor in [1 - sag .. 1]
  float sag = 1.0f - (g_sag_amt * g_env);  // more level → more droop
  float pre_total = pre * g_pre_gain * fmaxf(0.25f, sag);

  // 3) CMOS polynomial stage
  bool clipped = false;
  float y = CMOS_Poly(in, pre_total, g_a3, g_b5, clipped);

  // 4) Post tone (LPF)
  g_lp_z = g_lp_b * y + g_lp_a * g_lp_z;
  y = g_lp_z;

  // 5) Output trim + safety
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUT_LIMIT, fminf(y, OUT_LIMIT));

  if (clipped) g_clip_env = fminf(1.0f, CLIP_LED_ATTACK * (g_clip_env + 0.30f));
  out = y;
}

// -----------------------------------------------------------------------------
// Setup (once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initial pots → params
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));
  g_a3 = H.ReadPotMapped(RV2, A_MIN, A_MAX, HPCB_Curve::Exp10);   // "Crunch"
  g_b5 = H.ReadPotMapped(RV3, B_MIN, B_MAX, HPCB_Curve::Linear);  // "Fizz"
  g_pre_gain = H.ReadPotMapped(RV4, PRE_GAIN_MIN, PRE_GAIN_MAX, HPCB_Curve::Exp10);
  {
    float sag01 = H.ReadPot(RV5);
    g_sag_amt = SAG_MIN + (SAG_MAX - SAG_MIN) * sag01;
    UpdateSagTimes();
  }
  float fc0 = H.ReadPotMapped(RV6 /* temp read for tone? if you prefer RV6 as Master, move tone to RV5 */,
                              TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(fc0);

  // If you want RV6 as Master (recommended), uncomment the next two lines
  H.SetLevel(H.ReadPot(RV6));
  // and set tone on RV5 in loop() below (already done).

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (controls/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();

  // RV1 — Drive (dB → linear)
  g_drive_lin = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Crunch (cubic)
  g_a3 = H.ReadPotMapped(RV2, A_MIN, A_MAX, HPCB_Curve::Exp10);

  // RV3 — Fizz (fifth)
  g_b5 = H.ReadPotMapped(RV3, B_MIN, B_MAX, HPCB_Curve::Linear);

  // RV4 — Pre-gain scaler (keeps sweep musical with different a/b)
  g_pre_gain = H.ReadPotMapped(RV4, PRE_GAIN_MIN, PRE_GAIN_MAX, HPCB_Curve::Exp10);

  // RV5 — Sag amount (0..1 mapped to SAG_MIN..MAX)
  {
    float sag01 = H.ReadPot(RV5);
    g_sag_amt = SAG_MIN + (SAG_MAX - SAG_MIN) * sag01;
  }

  // RV6 — Master (post) and also Tone cutoff on RV5 if you prefer that layout:
  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // Don't read pot if bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set unity gain.
  }
  // If you want tone on RV5 instead (common layout: Drive/Crunch/Fizz/Tone/Master),
  // swap assignments: put Tone here and move Sag to RV4 above.
  {
    float fc = H.ReadPotMapped(RV5, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc);
  }

  // FS1 — Kick (momentary)
  g_kick = H.FootswitchIsPressed(FS1);

  // FS2 — Bypass (edge)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2) g_bypassed = !g_bypassed;
    prev_fs2 = fs2;
  }

  // LEDs (active-HIGH)
  H.SetLED(LED2, !g_bypassed);  // effect active
  g_clip_env *= CLIP_LED_DECAY;
  H.SetLED(LED1, (g_clip_env > 0.12f) || g_kick);  // clip/kick indicator
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// **BasicCMOS** captures the crunchy, lo-fi character of **CD4049/4069 inverter**
// drives (think Red Llama family). We emulate the rail-to-rail inverter transfer
// with an **odd-symmetric polynomial** (3rd+5th) that piles on odd harmonics
// without just slamming into a square. A **sag** control ducks the stage with
// a simple envelope follower to mimic a starving 9 V rail.
//
// Controls
// --------
// - RV1 — Drive: pre-gain into the inverter stage (0 → +36 dB).
// - RV2 — Crunch (Cubic): strength of the 3rd-order term (core CMOS growl).
// - RV3 — Fizz (Fifth): adds upper odd harmonics and “glass.”
// - RV4 — Pre-Gain Trim: balances how hard the polynomial is hit as RV2/RV3 move.
// - RV5 — Sag: increases supply droop on peaks (0 = none, 1 = strong ducking).
// - RV5 (alt) — Tone: if you prefer a classic control map, assign Tone here and
//   move Sag to RV4; both mappings are shown in code.
// - RV6 — Master: overall output level (post effect).
// - FS1 — Kick: hold for +6 dB extra pre-gain (solo boost into dirt).
// - FS2 — Bypass: true passthrough on/off.
// - LED1 — Clip Meter: lights on nonlinear action (decays) and while Kick is held.
// - LED2 — Effect Active: lit when engaged.
//   (LEDs are **active-HIGH** with HaroldPCB v1.3.0+.)
//
// Signal Flow
// -----------
// Input → Drive → CMOS Polynomial (x − a·x^3 + b·x^5) → Soft Limit → LPF Tone → Master → Out
//             ↘—— optional SAG (envelope-controlled pre-attenuation) ——↗
//
// Customizable Parameters (top of file)
// -------------------------------------
// - A_MIN/A_MAX: cubic range (main flavor). Higher = more crunch.
// - B_MIN/B_MAX: fifth range (adds spitty top). Start 0.2–0.4 for classic CMOS glass.
// - PRE_GAIN_MIN/MAX: staging before polynomial; sets how touchy the sweep feels.
// - SOFT_LIM_THR: lower for softer edges (less immediate square).
// - SAG_*: MIN/MAX amount and attack/release times; longer release feels “saggier.”
// - TONE_CUTOFF_MIN/MAX: voice the treble roll-off window.
// - EXTRA_DRIVE_DB: Kick amount. OUTPUT_TRIM/OUT_LIMIT: global level and safety.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) **LED Rail Emu:** Replace SoftLimit with an LED-ish soft clip (tanh scaled) to
//    mimic rails that “glow” instead of brick-wall.
// 2) **Gatey Mode:** Couple sag to a tiny negative DC bias pre-shaper so low-level
//    sustain crackles (lo-fi synthy gate).
// 3) **Presence Shelf:** Add a gentle post high-shelf so high fizz is controllable
//    without removing it entirely.
// 4) **Dual-Stage CMOS:** Cascade two polynomial blocks with different (a,b) for
//    chewy stacking. Keep SOFT_LIM_THR slightly higher on the first stage.
// 5) **Diode Blend:** Borrow the `BasicOpAmp` hard/soft mix idea: parallel a hard
//    diode clamp and crossfade with the CMOS output on TS1.
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Structured like a textbook page for the
// HaroldPCB library (constants up top, prose user guide at the end). LEDs active-HIGH.
//
