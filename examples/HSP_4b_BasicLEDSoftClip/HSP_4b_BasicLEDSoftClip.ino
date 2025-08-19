// BasicLEDSoftClip.ino — v1.0.1 (beginner‑friendly names + detailed clip comments)
// by Harold Street Pedals 2025
// LED-style soft clipping (op-amp feedback style) with drive, tone, symmetry, and bypass.

#include <HaroldPCB.h>
#include <math.h>

// -----------------------------------------------------------------------------
// CONSTANTS (tweakable defaults for builders)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;
static const uint16_t BLOCK_SIZE = 8;

// DRIVE: pre-gain before saturation (in dB)
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// MOMENTARY KICK (FS1): extra drive when held
static const float EXTRA_DRIVE_DB = 6.0f;

// “LED feel” threshold (softer than hard clip)
static const float CLIP_THR_SOFT = 0.40f;  // softness center scaling

// Symmetry skew range (RV3) — ±45% between positive/negative
static const float SYM_RANGE_FRAC = 0.45f;

// TONE: one-pole low-pass (post saturation)
static const float TONE_CUTOFF_MIN_HZ = 600.0f;
static const float TONE_CUTOFF_MAX_HZ = 8200.0f;

// Output trim + safety
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT = 1.2f;

// LED indicator decay
static const float CLIP_LED_DECAY = 0.90f;

// -----------------------------------------------------------------------------
// GLOBAL STATE
// -----------------------------------------------------------------------------
static HaroldPCB H;

// cached control state for the audio thread
static volatile bool bypassOn = false;
static volatile bool kickOn = false;
static volatile float driveAmount = 1.0f;
static volatile float clipLimitPositive = CLIP_THR_SOFT;
static volatile float clipLimitNegative = CLIP_THR_SOFT;

// one-pole low-pass coefficients (tone)
static volatile float lowpassCoef_A = 0.0f;
static volatile float lowpassCoef_B = 1.0f;

// low-pass memory (state)
static float lowpassMemory = 0.0f;

// clip indicator envelope
static volatile bool clipFlag = false;
static float clipEnvelope = 0.0f;

// FS2 edge detect
static bool prevBypassSwitch = false;

// -----------------------------------------------------------------------------
// HELPERS
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  // Convert decibels to linear gain: 20 dB → 10x, 6 dB → ~2x, etc.
  return powf(10.0f, db / 20.0f);
}

static void UpdateLowpassFromCutoff(float cutoffHz) {
  // Protect from extremes and set a simple 1-pole low-pass (post-clip tone).
  cutoffHz = fmaxf(10.0f, fminf(cutoffHz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * M_PI * cutoffHz / SAMPLE_RATE_HZ);
  lowpassCoef_A = a;
  lowpassCoef_B = 1.0f - a;
}

// Soft clipper (LED feedback style).
// Uses tanh() with separate positive/negative “limits” to mimic LED forward voltages.
// If input exceeds either limit, we mark that a clip happened (for the LED).
static inline float SoftClipAsym(float input, float limitPos, float limitNeg, bool &clipped) {
  float result = 0.0f;
  if (input >= 0.0f) {
    result = limitPos * tanhf(input / limitPos);
    if (fabsf(input) > limitPos) clipped = true;
  } else {
    result = limitNeg * tanhf(input / limitNeg);
    if (fabsf(input) > limitNeg) clipped = true;
  }
  return result;
}

// -----------------------------------------------------------------------------
// AUDIO CALLBACK (runs very fast, one tiny slice of sound at a time)
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (bypassOn) {
    out = in;  // If bypass is ON, skip processing entirely.
    return;
  }

  // ------------------------------
  // DRIVE STAGE (Pre-Gain)
  // ------------------------------
  // Turn the guitar signal up before clipping.
  // - driveAmount comes from the Drive knob (RV1).
  // - kickOn adds extra gain while FS1 is held for a momentary "boost".
  float preGain = driveAmount * (kickOn ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);
  float drivenSignal = in * preGain;  // louder input ready to distort

  // ------------------------------
  // CLIPPING STAGE (LED-style soft clip)
  // ------------------------------
  // What is clipping?
  // - Imagine the waveform is a wiggly line. If we push it too high, it “hits the ceiling”.
  //   Hard clipping chops the top flat (like scissors). That sounds aggressive and fizzy.
  // - Soft clipping bends the top smoothly (like pressing clay). That sounds smoother and more amp-like.
  //
  // How do we soft-clip here?
  // - We use the tanh() math curve, which naturally bends as values grow.
  // - We give tanh() two “limits”: one for positive half-waves and one for negative.
  //   This lets us skew the shape (asymmetry) so the + and – sides can behave a bit differently,
  //   similar to how two different LEDs would conduct at slightly different forward voltages.
  //
  // What lights the LED?
  // - If the incoming signal goes beyond either limit, we set a flag.
  //   The control thread (loop) turns that into a short “glow” on LED1,
  //   so you can see when the clipper is working.
  bool clipped = false;
  float clippedSignal = SoftClipAsym(drivenSignal,
                                     clipLimitPositive,
                                     clipLimitNegative,
                                     clipped);
  if (clipped) {
    clipFlag = true;  // Tell the LED system that a clip just happened.
  }

  // ------------------------------
  // TONE STAGE (Low-Pass Filter)
  // ------------------------------
  // After clipping, high treble can get harsh.
  // A simple low-pass filter gently rolls off highs for a smoother tone.
  // (We’ll go deeper on filters later in the textbook.)
  lowpassMemory = lowpassCoef_B * clippedSignal + lowpassCoef_A * lowpassMemory;
  float filteredSignal = lowpassMemory;

  // ------------------------------
  // OUTPUT STAGE
  // ------------------------------
  // Final trim and safety limiter so we don’t exceed a chosen ceiling.
  filteredSignal *= OUTPUT_TRIM;
  filteredSignal = fmaxf(-OUT_LIMIT, fminf(filteredSignal, OUT_LIMIT));

  out = filteredSignal;
}

// -----------------------------------------------------------------------------
// SETUP (runs once at power-up)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // RV1: Drive → convert dB range to linear gain for the audio thread
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2: Tone cutoff → set initial low-pass coefficients
  float toneStart = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(toneStart);

  // RV3: Symmetry skew → shift positive/negative limits
  float rawSym = H.ReadPot(RV3) * 2.0f - 1.0f;  // 0..1 to -1..+1
  float skew = rawSym * SYM_RANGE_FRAC;         // shrink to our chosen range
  clipLimitPositive = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f + skew));
  clipLimitNegative = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f - skew));

  // RV6: initial master level (library also supports smoothing in loop)
  H.SetLevel(H.ReadPot(RV6));

  // Start audio with our processing function
  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// LOOP (runs repeatedly, human-speed control work)
// -----------------------------------------------------------------------------
void loop() {
  // RV1: Drive (read knob, map dB → linear)
  driveAmount = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2: Tone cutoff (update one-pole low-pass)
  float cutoff = H.ReadPotMapped(RV2, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(cutoff);

  // RV3: Symmetry skew (recompute clip limits)
  float rawSym = H.ReadPot(RV3) * 2.0f - 1.0f;
  float skew = rawSym * SYM_RANGE_FRAC;
  clipLimitPositive = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f + skew));
  clipLimitNegative = fmaxf(0.05f, CLIP_THR_SOFT * (1.0f - skew));

  // RV6: Master level (smooth when engaged, unity when bypassed)
  H.SetLevel(bypassOn ? 1.0f : H.ReadPotSmoothed(RV6, 15.0f));

  // FS1: Kick (momentary extra drive)
  kickOn = H.FootswitchIsPressed(FS1);

  // FS2: Bypass toggle (edge-detected)
  bool bypassSwitch = H.FootswitchIsPressed(FS2);
  if (bypassSwitch && !prevBypassSwitch) {
    bypassOn = !bypassOn;
  }
  prevBypassSwitch = bypassSwitch;

  // LEDs
  H.SetLED(LED2, !bypassOn);  // LED2 = effect active
  if (clipFlag) {
    clipEnvelope = 1.0f;  // pop the envelope to full when we detect a clip
    clipFlag = false;
  }
  clipEnvelope *= CLIP_LED_DECAY;
  H.SetLED(LED1, clipEnvelope > 0.12f || kickOn);  // LED1 = clip (or kick) indicator
}

// -----------------------------------------------------------------------------
// USER GUIDE
// -----------------------------------------------------------------------------
//
// Overview
// --------
// LED-style soft clipping with Drive, Tone, Symmetry, and Bypass.
// Smooth “bend” (tanh) instead of a hard cutoff, for a more musical feel.
//
// Controls
// --------
// - RV1 — Drive: 0 to +36 dB pre-gain.
// - RV2 — Tone: 600 Hz – 8.2 kHz treble roll-off (simple low-pass).
// - RV3 — Symmetry: skews positive vs negative clipping thresholds.
// - RV6 — Master: overall level after tone.
// - FS1 — Kick: hold for +6 dB extra drive.
// - FS2 — Bypass toggle.
// - LED1 — Clip indicator (brief glow on clip; also on while Kick is held).
// - LED2 — Effect active.
//
// Signal Flow
// -----------
// Input → Drive → SoftClip (tanh, asym) → Low-Pass Tone → Master → Output
//
// Tweak Points
// ------------
// - DRIVE_MIN_DB / DRIVE_MAX_DB: set the pre-gain window.
// - CLIP_THR_SOFT: smaller = more distortion.
// - SYM_RANGE_FRAC: how far symmetry can skew.
// - TONE_CUTOFF_MIN/MAX: tone control’s range.
// - EXTRA_DRIVE_DB: Kick boost amount.
//
// Notes
// -----
// We kept math names readable (English words) for beginners,
// and we’ll dig deeper into filter design in the later “Filters” chapter.
//
