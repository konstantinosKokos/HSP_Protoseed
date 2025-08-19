// HSP_BasicBoost.ino — v1.0.0 (textbook-style, friendly guide)
// by Harold Street Pedals 2025
//
// Clean, transparent boost with a simple treble‑cut “tone” knob
// and true passthrough bypass.
//
// Signal path: Input → Boost → Tone (low‑pass) → Master (library) → Output

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// SECTION 1 — FIXED SETTINGS (Why these matter)
// -----------------------------------------------------------------------------
// • 48 kHz sample rate: modern “hi‑fi” sound, low latency.
// • Block size 8: snappy feel without stressing the CPU.
static const uint32_t SAMPLE_RATE_HZ = 48000;
static const uint16_t BLOCK_SIZE = 8;

// Boost range you’ll feel on the volume knob.
// 0 dB = same level; +20 dB ≈ 10× louder (useful for driving amps)
static const float BOOST_MIN_DB = 0.0f;
static const float BOOST_MAX_DB = 20.0f;

// Tone = post‑boost treble cut. Left = darker, Right = brighter.
// These are musical, guitar‑friendly limits.
static const float TONE_CUTOFF_MIN_HZ = 400.0f;
static const float TONE_CUTOFF_MAX_HZ = 8200.0f;

// Final touch and safety:
// • OUTPUT_TRIM usually 1.0 for “invisible” processing.
// • OUTPUT_LIMIT stops runaway peaks (keep conservative unless you add soft‑clip).
static const float OUTPUT_TRIM = 1.0f;
static const float OUTPUT_LIMIT = 1.2f;

// -----------------------------------------------------------------------------
// SECTION 2 — SHARED STATE (updated in loop(), read in AudioCB())
// -----------------------------------------------------------------------------
// We keep names literal so the code reads like a story.
static HaroldPCB H;

static volatile bool effectBypassed = false;  // FS2 flips this
static volatile float boostFactor = 1.0f;     // linear multiplier (converted from dB)

// One‑pole tone coefficients: y = toneBlendNew*x + toneBlendOld*y_prev
static volatile float toneBlendOld = 0.0f;  // “how much past to keep”
static volatile float toneBlendNew = 1.0f;  // “how much fresh input to take”

// ⭐ toneMemory: the filter’s “last output” (y_prev).
// Each new sample blends “now” (sparkle) with a little “recent past” (smoothness).
static float toneMemory = 0.0f;

// Used to detect a fresh press on FS2 (so we toggle only once per click).
static bool prevFS2 = false;

// -----------------------------------------------------------------------------
// SECTION 3 — SMALL HELPERS (clear math, plain English)
// -----------------------------------------------------------------------------

// 3.1  Knob 0..1 → dB window → linear gain
// Why: ears think in dB (log), the computer multiplies (linear).
// Formula you’ll use a lot: linear = 10^(dB/20).
// Benchmarks: +6 dB ≈ 2×, +12 dB ≈ 4×, +20 dB ≈ 10×
static float KnobToLinearGain(float knob01, float dbMin, float dbMax) {
  float dB = dbMin + (dbMax - dbMin) * knob01;
  return powf(10.0f, dB / 20.0f);
  // This takes a knob value (0..1) and maps it into a dB range, then converts dB to linear gain.
  // Step 1: Scale knob position into dBMin..dBMax (our chosen “window”).
  // Step 2: Convert that dB value into a linear multiplier, since audio math is linear.
  // Formula reminder: linear = 10^(dB/20). Example: +6dB ≈ 2×, +12dB ≈ 4×, +20dB ≈ 10×.
}

// 3.2  SetToneFromCutoff: pick how bright/dark the post‑boost sound is
// One‑pole low‑pass: y = b*x + a*y_prev
// with a = e^(−2π*fc/fs), b = 1 − a.
// Intuition: a near 1 = more “memory” = darker.  a small = less memory = brighter.
static void SetToneFromCutoff(float cutoffHz) {
  cutoffHz = fmaxf(10.0f, fminf(cutoffHz, SAMPLE_RATE_HZ * 0.45f));  // sensible bounds
  float a = expf(-2.0f * (float)M_PI * cutoffHz / (float)SAMPLE_RATE_HZ);
  toneBlendOld = a;
  toneBlendNew = 1.0f - a;
}

// -----------------------------------------------------------------------------
// SECTION 4 — AUDIO ENGINE (runs for every sample, keep it light)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (effectBypassed) {
    out = in;
    return;
  }

  // STEP 1 — Boost (just a multiply; transparent if OUTPUT_TRIM stays 1.0)
  float boosted = in * boostFactor;

  // STEP 2 — Tone (post‑boost treble trim via one‑pole low‑pass)
  // Blend “now” (boosted) with a touch of “recent past” (toneMemory).
  toneMemory = toneBlendNew * boosted + toneBlendOld * toneMemory;
  float y = toneMemory;

  // STEP 3 — Final trim + safety
  y *= OUTPUT_TRIM;
  y = fmaxf(-OUTPUT_LIMIT, fminf(y, OUTPUT_LIMIT));

  out = y;
}

// -----------------------------------------------------------------------------
// SECTION 5 — SETUP (runs once; line up code with the hardware in your hands)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Seed parameters from current knob positions so it sounds “as set” at boot.
  boostFactor = KnobToLinearGain(H.ReadPot(RV1), BOOST_MIN_DB, BOOST_MAX_DB);

  float startCutoff = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  SetToneFromCutoff(startCutoff);

  // Post‑effect master level (RV6) is handled by the library.
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// SECTION 6 — LOOP (human‑speed control; no heavy work here)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();  // keep pots/toggles/footswitches steady and debounced

  // RV1 → Boost (convert dB feel to linear multiply)
  boostFactor = KnobToLinearGain(H.ReadPot(RV1), BOOST_MIN_DB, BOOST_MAX_DB);

  // RV2 → Tone cutoff (exp curve = nicer feel for frequency knobs)
  float cutoff = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  SetToneFromCutoff(cutoff);

  // RV6 → Master only when active. In bypass we force unity so out = in.
  if (!effectBypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));
  } else {
    H.SetLevel(1.0f);
  }

  // FS2 → Bypass toggle: flip only on a fresh press
  bool fs2 = H.FootswitchIsPressed(FS2);
  if (fs2 && !prevFS2) effectBypassed = !effectBypassed;
  prevFS2 = fs2;

  // LEDs: LED2 follows effect active; LED1 is yours for mods
  H.SetLED(LED2, !effectBypassed);
  H.SetLED(LED1, false);
}

// -----------------------------------------------------------------------------
// SECTION 7 — USER GUIDE (friendly + practical)
// -----------------------------------------------------------------------------
//
// What you’ll feel under your fingers
// • RV1 “Boost”: brings the whole guitar up. Past noon it’ll push most amps.
// • RV2 “Tone”: left tames fizz (darker), right adds sparkle (brighter).
// • FS2 “Bypass”: true passthrough; LED2 tells you when Boost+Tone are active.
// • RV6 “Master”: final volume after the effect (handy for matching levels).
//
// Quick‑start checklist
// 1. Start with RV1 at minimum (0 dB) and RV6 at noon.
// 2. Strum a chord. Toggle FS2: active vs bypass should sound nearly identical.
// 3. Turn RV1 clockwise until the amp wakes up; trim RV6 to keep stage volume sane.
// 4. Sweep RV2: find the sweet spot where brightness helps without harshness.
//
// Dial‑in recipes
// • Always‑on sweetener: RV1 at +3–6 dB, RV2 around 3 kHz, RV6 to taste.
// • Solo lift: RV1 near +8–12 dB, RV2 a touch brighter, assign LED1 as “Solo”.
// • Tame bright amps: RV1 small (≤+6 dB), RV2 lower (1–2 kHz), OUTPUT_TRIM = 0.9f.
// • Bass‑friendly: narrow the tone range (e.g., 150–3500 Hz in the constants).
//
// Friendly reminders (important, not fussy)
// • AudioCB is for quick math only; keep all knob reads in loop().
// • OUTPUT_LIMIT prevents nasty spikes if you crank things—leave it conservative.
// • If parameter jumps click, smooth them (we already do for RV6).
//
// Try these mods next
// • Momentary Solo: make FS1 add +6 dB while held and light LED1.
// • Softer top: add a fixed 6–8 kHz low‑pass on a toggle for direct rigs.
// • Tilt EQ: replace the low‑pass with a “bright↔dark” tilt for a single‑knob tone.
//
// End of file.
