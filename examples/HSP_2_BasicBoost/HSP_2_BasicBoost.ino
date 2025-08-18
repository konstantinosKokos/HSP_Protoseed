// HSP_BasicBoost.ino — v1.0.0
// by Harold Street Pedals 2025
// Transparent clean boost with a simple treble-cut tone control and true passthrough bypass.
//
// Structure notes:
// - Constants for builders are at the top
// - Fixed 48 kHz / 8-sample blocks
// - Controls are read in loop() (never inside the audio callback)
// - Audio callback is mono (in -> out)
// - Detailed User Guide lives at the bottom

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // Fixed: 48 kHz
static const uint16_t BLOCK_SIZE = 8;          // Fixed: 8-sample audio block

// Boost range (dB). 0 dB = unity, 20 dB ≈ 10x. Adjust to taste.
static const float BOOST_MIN_DB = 0.0f;
static const float BOOST_MAX_DB = 20.0f;

// Tone control = simple one-pole low-pass (post-boost).
// Map RV2 to cutoff frequency [TONE_CUTOFF_MIN_HZ .. TONE_CUTOFF_MAX_HZ].
// This is a classic treble-cut “tone” knob: left = darker, right = brighter.
static const float TONE_CUTOFF_MIN_HZ = 400.0f;   // darker
static const float TONE_CUTOFF_MAX_HZ = 8200.0f;  // brighter

// Master level (post effect) is handled by the library via RV6.
// We still include a safety trim after processing for experiments.
static const float OUTPUT_TRIM = 1.0f;  // leave 1.0 for transparent feel

// Safety limit (prevents accidental overs). Raise carefully if you add soft clipping.
static const float OUT_LIMIT = 1.2f;  // linear peak clamp

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state consumed by the audio thread
static volatile bool g_bypassed = false;  // FS2 toggles true bypass
static volatile float g_gain_lin = 1.0f;  // linear boost factor
static volatile float g_lp_a = 0.0f;      // one-pole smoothing coefficient 'a'
static volatile float g_lp_b = 1.0f;      // (1 - a) precomputed for efficiency

// Filter memory (audio thread)
static float g_lp_z = 0.0f;

// Edge tracking for FS2
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

// Map 0..1 RV to dB range then to linear gain
static float PotToLinearGain(float rv01, float db_min, float db_max) {
  float db = db_min + (db_max - db_min) * rv01;
  return powf(10.0f, db * (1.0f / 20.0f));
}

// Compute one-pole low-pass coefficients from cutoff
// y[n] = b*x[n] + a*y[n-1], where a = exp(-2*pi*fc/fs), b = 1 - a
static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));  // sanity
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  g_lp_a = a;
  g_lp_b = 1.0f - a;
}

// -----------------------------------------------------------------------------
// Audio callback (runs at audio rate). No direct control reads here.
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (g_bypassed) {
    out = in;
    return;
  }

  // 1) Apply boost (transparent linear gain)
  float x = in * g_gain_lin;

  // 2) Tone (post-boost treble-cut one-pole LPF)
  //    z = b*x + a*z
  g_lp_z = g_lp_b * x + g_lp_a * g_lp_z;
  float y = g_lp_z;

  // 3) Output trim and safety clamp
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUT_LIMIT, fminf(y, OUT_LIMIT));

  out = y;
}

// -----------------------------------------------------------------------------
// Setup (runs once at power-on)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initialize cached parameters from current pot positions
  g_gain_lin = PotToLinearGain(H.ReadPot(RV1), BOOST_MIN_DB, BOOST_MAX_DB);

  float cutoff0 = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(cutoff0);

  // Master level from RV6 (post-effect trim handled by library)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (control & UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();  // services pots, toggles, LEDs, and footswitch debounce

  // RV1 → Boost amount in dB (0..20 dB), with perceptual mapping to linear
  g_gain_lin = PotToLinearGain(H.ReadPot(RV1), BOOST_MIN_DB, BOOST_MAX_DB);

  // RV2 → Tone cutoff (treble cut). Exp curve for nicer feel.
  {
    float cutoff = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(cutoff);
  }

  if (!g_bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // don't allow the pot to control when bypassed,
  } else {
    H.SetLevel(1.0f);  // instead set the level to unity.
  }


  // FS2 → Bypass toggle (edge detect)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2)
      g_bypassed = !g_bypassed;
    prev_fs2 = fs2;
  }

  // LEDs: LED2 shows effect active; LED1 unused here (free for future mods)
  H.SetLED(LED2, !g_bypassed);
  H.SetLED(LED1, false);
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// A transparent **clean boost** with a classic **tone** control. RV1 sets how much
// level you add (up to ~+20 dB). RV2 acts as a treble-cut (one-pole low-pass) to tame
// fizz or brighten/darken the signal post-boost. FS2 engages true passthrough bypass.
// RV6 is a master level after the effect, handled by the HaroldPCB library.
//
// Controls
// --------
// - RV1 — Boost Amount: 0 dB (unity) to +20 dB (~10×). Transparent linear gain.
// - RV2 — Tone (Treble-Cut): left = darker (≈400 Hz), right = brighter (≈8.2 kHz).
// - RV6 — Master: overall output level (post effect) via library SetLevel().
// - FS2 — Bypass Toggle: on/off for the effect (true passthrough when bypassed).
// - LED2 — Effect Active: on when the effect is active, off when bypassed.
// - FS1 / LED1 — Unused in this basic build (reserved for your mods).
//
// Signal Flow
// -----------
// Input → Boost (linear gain) → Tone (LPF) → Master (library) → Output
//
// Customizable Parameters (see constants at the top)
// --------------------------------------------------
// - BOOST_MIN_DB / BOOST_MAX_DB: set your gain window (e.g., cap at +12 dB).
// - TONE_CUTOFF_MIN_HZ / TONE_CUTOFF_MAX_HZ: widen/narrow the tone range.
// - OUTPUT_TRIM: overall post-processing trim (keep at 1.0 for “transparent”).
// - OUT_LIMIT: safety linear peak clamp. Raise with care if you add soft clip.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) **Tilt EQ Tone**: Replace the low-pass with a tilt (low shelf + high shelf)
//    pivoting at ~800 Hz; map RV2 to “bright ↔ dark” balance instead of cutoff.
// 2) **Soft Clip Safety**: Add a gentle saturator after the boost (e.g., tanh or
//    cubic soft clip) to catch peaks musically rather than hard clamp.
// 3) **Mid Boost Mode**: Put a peaking EQ at ~900 Hz with Q≈0.7; use TS1 to
//    toggle mid emphasis for solos.
// 4) **Momentary Solo**: Make FS1 a momentary +6 dB kick by temporarily raising
//    g_gain_lin while held; LED1 can indicate the solo boost.
// 5) **High-Cut “Speaker” Mode**: Add a fixed 6–8 kHz low-pass on TS2 to simulate
//    small speaker roll-off for direct rigs.
// 6) **Dual-Range Tone**: Use TS1 to switch the tone range between “Guitar”
//    (400 Hz–8.2 kHz) and “Bass” (150 Hz–3.5 kHz).
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Structured as a textbook example for the
// HaroldPCB library with constants up top and a prose user guide at the end.
//
