// HSP_BasicBoost.ino — v1.0.0 
// by Harold Street Pedals 2025
// Clean, transparent boost with a simple treble‑cut “tone” knob
// and true passthrough bypass.

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// SECTION 1 — CONSTANTS (your “settings on the chalkboard”)
// -----------------------------------------------------------------------------
// We pick a modern sample rate and a tiny block size so latency stays low and
// the pedal feels immediate.
static const uint32_t SAMPLE_RATE_HZ = 48000;  // 48,000 samples per second
static const uint16_t BLOCK_SIZE     = 8;      // process 8 samples at a time

// Boost range in decibels (what musicians expect on a knob)
static const float BOOST_MIN_DB = 0.0f;   // no change
static const float BOOST_MAX_DB = 20.0f;  // ≈ 10× louder

// Tone (simple one‑pole treble cut) cutoff range in Hertz
static const float TONE_CUTOFF_MIN_HZ = 400.0f;   // darker
static const float TONE_CUTOFF_MAX_HZ = 8200.0f;  // brighter

// Optional safety/trim after processing (keep defaults for “transparent” feel)
static const float OUTPUT_TRIM  = 1.0f;  // leave 1.0 unless you want bias
static const float OUTPUT_LIMIT = 1.2f;  // clamp peaks just in case

// -----------------------------------------------------------------------------
// SECTION 2 — GLOBAL STATE (updated in loop(), read in AudioCB())
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Boost: we store the *linear* multiplier the audio engine needs
static volatile float boostLinear = 1.0f;  // from dB knob → multiplier

// Tone filter coefficients and memory (one‑pole low‑pass)
// y[n] = b*x[n] + a*y[n-1]   where a = toneBlendOld,  b = toneBlendNew
static volatile float toneBlendOld = 0.0f;  // “how much of the recent past to keep”
static volatile float toneBlendNew = 1.0f;  // “how much of the fresh input to take”
static float toneMemory = 0.0f;             // last output (y[n-1])

// Bypass and footswitch edge tracking
static volatile bool bypassed = false;
static bool prevFS2 = false;

// -----------------------------------------------------------------------------
// SECTION 3 — MATH HELPERS (with plain‑English notes)
// -----------------------------------------------------------------------------

// dB → linear multiplier
// Ears like dB (log), but the computer multiplies samples (linear).
// Formula you’ll reuse everywhere: linear = 10^(dB/20)
// Benchmarks: +6 dB ≈ 2×, +12 dB ≈ 4×, +20 dB ≈ 10×
static inline float dBtoLinear(float dB) {
  return powf(10.0f, dB / 20.0f);
}

// Set the low‑pass filter from a cutoff frequency (Hz).
// a = exp(-2π*fc/fs),  b = 1 - a
// Intuition: a near 1 keeps more “yesterday” (darker); small a takes more “today” (brighter).
static void SetToneFromCutoff(float cutoffHz) {
  cutoffHz = fmaxf(10.0f, fminf(cutoffHz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * cutoffHz / (float)SAMPLE_RATE_HZ);
  toneBlendOld = a;
  toneBlendNew = 1.0f - a;
}

// -----------------------------------------------------------------------------
// SECTION 4 — AUDIO CALLBACK (runs at audio speed: keep it lean)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (bypassed) { out = in; return; }

  // 1) Boost: multiply the input by our linear gain
  float boosted = in * boostLinear;

  // 2) Tone: mix “now” (boosted) with a little “recent past” (toneMemory)
  //    y = b*x + a*y_prev
  toneMemory = toneBlendNew * boosted + toneBlendOld * toneMemory;
  float toned = toneMemory;

  // 3) Optional safety/trim
  toned *= OUTPUT_TRIM;
  toned  = fmaxf(-OUTPUT_LIMIT, fminf(toned, OUTPUT_LIMIT));

  out = toned;
}

// -----------------------------------------------------------------------------
// SECTION 5 — SETUP (runs once, align code with real‑world knob positions)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Seed parameters from current knobs so it “boots sounding like it looks”
  float boostDbAtBoot = H.ReadPotMapped(RV1, BOOST_MIN_DB, BOOST_MAX_DB, HPCB_Curve::Exp10);
  boostLinear = dBtoLinear(boostDbAtBoot);

  float toneCutoffAtBoot = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  SetToneFromCutoff(toneCutoffAtBoot);

  // Master volume (post effect, handled by the library)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// SECTION 6 — LOOP (textbook walkthrough with tiny ASCII signal maps)
// -----------------------------------------------------------------------------
// Philosophy: read controls here (slow lane), *never* in the audio callback.
// We translate knobs into clean numbers the audio thread can use.
//
// Legend for the mini‑diagrams:
//   [KNOB] --> (mapping) --> [PARAM USED BY AUDIO]
// -----------------------------------------------------------------------------
void loop() {
  // Keep hardware scanning fresh (pots, toggles, debounced switches)
  H.Idle();

  // --- RV1 (Boost amount) ----------------------------------------------------
  // [RV1 0..1] --> (map to dB window) --> [dB] --> (10^(dB/20)) --> [boostLinear]
  //
  // Steps:
  //  1) Map 0..1 into BOOST_MIN_DB..BOOST_MAX_DB (Exp curve feels musical).
  //  2) Convert that dB number into a multiplier the audio callback can use.
  //
  // Quick mental math:
  //   halfway-ish (~10 dB) → ~3.16×
  float boostDb = H.ReadPotMapped(RV1, BOOST_MIN_DB, BOOST_MAX_DB, HPCB_Curve::Exp10);
  boostLinear = dBtoLinear(boostDb);

  // --- RV2 (Tone cutoff) -----------------------------------------------------
  // [RV2 0..1] --> (map to Hz range) --> [cutoffHz] --> (exp coeffs) --> [toneBlendOld/New]
  //
  // Steps:
  //  1) Map 0..1 into TONE_CUTOFF_MIN_HZ..TONE_CUTOFF_MAX_HZ (Exp curve again).
  //  2) Recompute the filter’s a/b coefficients for that cutoff.
  //
  // Ear guide:
  //   lower cutoff → darker; higher cutoff → brighter.
  float cutoffHz = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  SetToneFromCutoff(cutoffHz);

  // --- RV6 (Master volume, post effect) --------------------------------------
  // If the effect is active, follow RV6 (with light smoothing to avoid zippering).
  // If bypassed, force unity so the pedal is truly transparent.
  //
  // [RV6 0..1] --> (optional smoothing) --> [master level]
  if (!bypassed) {
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // ~15 ms smoothing feels steady
  } else {
    H.SetLevel(1.0f);                           // unity gain in bypass
  }

  // --- FS2 (Bypass toggle) ---------------------------------------------------
  // Edge‑detect so we only toggle once per press (no repeat while held).
  //
  // [FS2] --> (rising edge?) --> [flip bypassed]
  bool fs2 = H.FootswitchIsPressed(FS2);
  if (fs2 && !prevFS2) { bypassed = !bypassed; }
  prevFS2 = fs2;

  // --- LED2 (Effect active indicator) ----------------------------------------
  // Lights when the effect path is engaged.
  H.SetLED(LED2, !bypassed);

  // (LED1 is free—use it for your own mods if you like!)
}

// -----------------------------------------------------------------------------
// SECTION 7 — USER GUIDE (friendly & practical)
// -----------------------------------------------------------------------------
//
// What the knobs do (in real words)
// • RV1 “Boost”: turns the whole signal up. +6 dB ≈ 2×, +12 dB ≈ 4×, +20 dB ≈ 10×.
// • RV2 “Tone”: after the boost, trims highs. Left = smoother/darker, right = brighter.
// • RV6 “Master”: overall level after the effect (good for matching bypass volume).
// • FS2 toggles a *true* passthrough bypass. LED2 lights when the effect is ON.
//
// How the math maps to sound
// • Boost is just multiplication. dB is for humans; the code converts it to a number to multiply.
// • The tone filter “remembers” a bit of the last sound (that’s toneMemory). More memory → darker.
//
// Quick start
// 1) Set RV1 low, RV2 around noon, RV6 so bypass and effect sound equally loud.
// 2) Click FS2 to A/B the tone. Raise RV1 until your amp wakes up.
// 3) Sweep RV2 to taste: shave fizz or add sparkle.
//
// Tinker ideas
// • Cap BOOST_MAX_DB at +12 dB for a subtler always‑on sweetener.
// • Narrow tone range to 800–4000 Hz if you want a tighter “bright/dark” feel.
// • Use LED1 as a “solo” light and have FS1 add a momentary +6 dB in your own mod.
//
// Version
// v1.0.0 — Harold Street Pedals 2025
