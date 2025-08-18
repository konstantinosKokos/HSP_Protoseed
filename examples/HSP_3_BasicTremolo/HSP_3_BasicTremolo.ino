// HSP_BasicTremolo.ino — v1.0.0
// by Harold Street Pedals 2025
// Classic amplitude tremolo with triangle/square shapes, momentary chop, and bypass
//
// This example follows the Harold Street Pedals textbook style:
// - Constants for builders are at the top
// - 48 kHz / 8-sample blocks
// - Controls are read in loop() (never inside the audio callback)
// - Audio callback is mono: in -> out
// - Long, prose User Guide lives at the bottom of the file

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // Fixed: 48 kHz
static const uint16_t BLOCK_SIZE = 8;          // Fixed: 8-sample audio block

// Rate range for RV1 (triangle/square tremolo LFO). Use musical bounds by default.
static const float TREMOLO_RATE_MIN_HZ = 0.20f;  // slow throb
static const float TREMOLO_RATE_MAX_HZ = 12.0f;  // fast shimmer

// Depth taper and limits for RV2.
// Depth maps 0..1 → modulation depth (0 = dry, 1 = full on/off chop)
static const bool DEPTH_LOG_TAPER = true;  // set false for linear feel
static const float DEPTH_MIN = 0.00f;      // floor
static const float DEPTH_MAX = 1.00f;      // ceiling

// Square smoothing (avoids hard-edged click). 0ms = raw square.
// 5–20ms feels musical; increase for softer pulses.
static const float SQUARE_SMOOTH_MS = 12.0f;

// Output trim (post trem) if you want to bias perceived loudness.
// Usually 1.0f is fine; keep for experimentation or preset authoring.
static const float OUTPUT_TRIM = 1.0f;

// Safety clamp for the tremolo gain (just in case of future mods).
static const float GAIN_MIN = 0.0f;
static const float GAIN_MAX = 1.2f;  // a touch above unity for creative headroom

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (updated in loop(), consumed in AudioCB)
static volatile float g_rate_hz = 2.0f;
static volatile float g_depth = 0.7f;
static volatile bool g_shape_sq = false;  // false=triangle, true=square
static volatile bool g_bypassed = false;  // FS2 toggles this

// LFO phase and increments (used only in audio thread)
static float g_lfo_phase = 0.0f;  // 0..1
static float g_lfo_inc = 0.0f;    // per-sample phase step

// Square smoothing state (simple 1st-order low-pass in audio thread)
static float g_sq_smooth = 0.0f;

// FS2 edge tracking done in loop()
static bool prev_fs2 = false;

// -----------------------------------------------------------------------------
// Helpers (mapping and small utilities) — lightweight and self-contained
// -----------------------------------------------------------------------------

// Map pot [0..1] to [min..max] with optional log/exp feel using library curves.
static float MapPotRange(float raw01, float minv, float maxv, bool logfeel) {
  if (!logfeel) return minv + (maxv - minv) * raw01;
  // Use ReadPotMapped’s curve semantics by mimicking Exp10 feel:
  // perceptually nicer for depth; rebuild locally to avoid extra reads here
  float shaped = log10f(1.0f + 9.0f * raw01);  // Exp10 curve from library
  return minv + (maxv - minv) * shaped;
}

// advance and wrap 0..1
static inline float PhaseAdvance(float ph, float inc) {
  ph += inc;
  if (ph >= 1.0f) ph -= 1.0f;
  return ph;
}

// -----------------------------------------------------------------------------
// Audio callback (runs at audio rate). No control reads here — use cached state.
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  // Bypass: pure passthrough, LED2 reflects active state from loop()
  if (g_bypassed) {
    out = in;
    return;
  }

  // Compute one-sample LFO value
  // Triangle: 0..1..0; Square: 0 or 1 (smoothed).
  // We use a bipolar-to-unipolar mapping later for gain.
  float uni = 0.0f;  // unipolar 0..1

  if (!g_shape_sq) {
    // Triangle via folded saw: t in [0..1]
    float t = g_lfo_phase;
    float tri = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);  // 0..1..0
    uni = tri;
  } else {
    // Raw square, then smooth with simple 1-pole LP to avoid clicks
    float raw = (g_lfo_phase < 0.5f) ? 0.0f : 1.0f;
    if (SQUARE_SMOOTH_MS <= 0.0f) {
      uni = raw;
    } else {
      // time-constant from ms → per-sample smoothing factor
      // a = 1 - exp(-1 / (tau * fs)); tau = SQUARE_SMOOTH_MS/1000
      float a = 1.0f - expf(-1.0f / ((SQUARE_SMOOTH_MS / 1000.0f) * SAMPLE_RATE_HZ));
      g_sq_smooth += a * (raw - g_sq_smooth);
      uni = g_sq_smooth;
    }
  }

  // Depth hack: FS1 held = momentary full-chop (performance feature)
  const bool fs1_down = H.FootswitchIsPressed(FS1);
  float depth = fs1_down ? 1.0f : g_depth;

  // Unipolar trem gain: mix between 1.0 (dry) and uni*(depth range)
  // We want classic amplitude trem: gain = (1 - depth) + depth * uni
  float gain = (1.0f - depth) + depth * uni;

  // Safety + post trim
  gain = constrain(gain * OUTPUT_TRIM, GAIN_MIN, GAIN_MAX);

  out = in * gain;

  // Advance LFO
  g_lfo_phase = PhaseAdvance(g_lfo_phase, g_lfo_inc);
}

// -----------------------------------------------------------------------------
// Setup (runs once at power-on)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Set initial master level from RV6 at boot
  float master0 = H.ReadPot(RV6);
  H.SetLevel(master0);

  // Precompute LFO increment from initial rate (will be updated in loop)
  g_lfo_inc = (TREMOLO_RATE_MIN_HZ / (float)SAMPLE_RATE_HZ);

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (runs continuously, not in audio time). Do all control work here.
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();  // services pots, toggles, footswitch debounce, etc.

  // 1) RV1 → Rate (Hz) with musical bounds
  {
    float r = H.ReadPot(RV1);
    float rate_hz = H.ReadPotMapped(RV1, TREMOLO_RATE_MIN_HZ, TREMOLO_RATE_MAX_HZ, HPCB_Curve::Exp10);
    g_rate_hz = rate_hz;
    g_lfo_inc = g_rate_hz / (float)SAMPLE_RATE_HZ;  // phase is 0..1 per cycle
  }

  // 2) RV2 → Depth with optional log feel
  {
    float raw = H.ReadPot(RV2);
    g_depth = MapPotRange(raw, DEPTH_MIN, DEPTH_MAX, DEPTH_LOG_TAPER);
  }

  // 3) TS1 → Shape (triangle / square)
  g_shape_sq = H.ReadToggle(TS1);  // off=triangle, on=square

  // 4) RV6 → Master level (post, handled by library)
  {
    float m = H.ReadPotSmoothed(RV6, 15.0f);  // mild smoothing for fine control
    if (!g_bypassed) {
      H.SetLevel(m);  // Don't give the pot control in bypass,
    } else {
      H.SetLevel(1.0f);  // instead allow unity passthrough
    }
  }

  // 5) FS2 → Bypass toggle (edge detect here; library gives current state)
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prev_fs2)  // rising edge
    {
      g_bypassed = !g_bypassed;
    }
    prev_fs2 = fs2;
  }

  // 6) LEDs: LED2 = effect active, LED1 = momentary chop indicator
  H.SetLED(LED2, !g_bypassed);
  H.SetLED(LED1, H.FootswitchIsPressed(FS1));
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// This is a classic amplitude tremolo voiced for musical ranges. It offers triangle
// and square shapes, a momentary “chop” on FS1 (full-depth while held), and a true
// passthrough bypass mode controlled by FS2. The master level (RV6) is post-effect
// and handled by the HaroldPCB library.
//
// Controls
// --------
// - RV1 — Rate: sets tremolo speed from ~0.2 Hz (slow) up to ~12 Hz (fast).
// - RV2 — Depth: 0 (no modulation) to 1 (full on/off chop). Uses a gentle log feel.
// - RV6 — Master: overall output level (post effect), via library SetLevel().
// - TS1 — Shape: off = triangle (smooth), on = square (smoothed to avoid clicks).
// - FS1 — Momentary Chop: hold to force full-depth chop regardless of RV2.
// - FS2 — Bypass Toggle: toggles clean passthrough on/off.
// - LED1 — Chop Indicator: lights while FS1 is held.
// - LED2 — Effect Active: on when effect is active, off when bypassed.
//
// How It Works
// ------------
// Pot and switch reads are done in loop() to keep the audio callback clean. The
// audio callback uses cached values only. The LFO runs in the audio thread as a
// phase accumulator (0..1); triangle is synthesized via a folded saw, square is
// smoothed with a first-order low-pass (SQUARE_SMOOTH_MS) to avoid clicks.
//
// Customizable Parameters (see constants at the top)
// --------------------------------------------------
// - TREMOLO_RATE_MIN_HZ / TREMOLO_RATE_MAX_HZ: expand or narrow the speed window.
// - DEPTH_LOG_TAPER: set to false for linear depth feel.
// - SQUARE_SMOOTH_MS: increase to soften the square more (5–20ms is typical).
// - OUTPUT_TRIM: adjust perceived loudness if you change depth behavior.
// - GAIN_MIN / GAIN_MAX: safety clamp for experimental mods.
//
// Mods for Builders / Tinkerers
// -----------------------------
// 1) Bias the depth curve: try a quadratic remap for extra sensitivity near 0–0.3.
// 2) Add stereo trem: duplicate this file and mod the library’s right channel path
//    (HaroldPCB currently mutes right; you’d branch the library for true stereo).
// 3) Pan-trem: convert the square/triangle to a bipolar LFO (-1..+1) and use it to
//    crossfade two outputs for ping-pong effects (requires stereo support).
// 4) Envelope tremolo: detect input level (simple rect + RC) in audio and modulate
//    depth by dynamics for “auto-trem” vibes.
// 5) Harmonic trem: split the signal with low/high shelving filters and invert the
//    LFO polarity on one band for a vintage “harmonic tremolo” feel.
// 6) Tap tempo: use FS1 double-press detection (from library timing) to estimate
//    period and set g_rate_hz accordingly; blink LED1 at the tapped rate.
//
// Version & Credits
// -----------------
// v1.0.0 — by Harold Street Pedals 2025. Structured as a textbook example for the
// HaroldPCB library with constants up top and a prose user guide at the end.
//
