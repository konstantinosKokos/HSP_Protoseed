// BasicCMOS.ino — v1.0.1
// by Harold Street Pedals 2025
// CMOS inverter–style fuzz (CD4049/4069 vibe) using odd-symmetric polynomial shaping,
// optional supply "sag," post tone, momentary kick, and true bypass.
// LEDs are active-HIGH (HaroldPCB v1.3.0+): SetLED(..., true) turns the LED on.

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders / tinkerers / nerds)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed project-wide
static const uint16_t BLOCK_SIZE      = 8;     // fixed project-wide

// DRIVE (pre-gain into the CMOS shaper), in dB
static const float DRIVE_MIN_DB = 0.0f;   // unity
static const float DRIVE_MAX_DB = 36.0f;  // ~63x linear

// FS1 momentary "kick" (extra pre-gain while held)
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
static const float SOFT_LIMIT_THRESHOLD = 0.95f;  // lower = softer

// Optional "sag" (envelope-controlled gain drop), RV5 in [0..1] maps to range below
static const float SAG_MIN         = 0.00f;   // 0 = no sag
static const float SAG_MAX         = 0.60f;   // strong droop at peaks
static const float SAG_ATTACK_MS   = 3.0f;    // fast-ish grab on peaks
static const float SAG_RELEASE_MS  = 120.0f;  // lazy recovery

// Post-TONE: one-pole treble cut after the shaper
static const float TONE_CUTOFF_MIN_HZ = 700.0f;
static const float TONE_CUTOFF_MAX_HZ = 9500.0f;

// Output trim + safety ceiling
static const float OUTPUT_TRIM = 1.0f;
static const float OUT_LIMIT   = 1.2f;

// Clip LED envelope timing
static const float CLIP_LED_ATTACK = 1.00f;
static const float CLIP_LED_DECAY  = 0.90f;

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (updated in loop, consumed in audio)
static volatile bool  isBypassed        = false;
static volatile bool  isKickHeld        = false;
static volatile float driveLinear       = 1.0f;  // dB → linear
static volatile float preGainScaler     = 1.2f;  // PRE_GAIN_MIN..MAX
static volatile float cubicAmount       = 1.0f;  // "a" (3rd-order)
static volatile float fifthAmount       = 0.3f;  // "b" (5th-order)
static volatile float sagAmount01       = 0.0f;  // 0..1 mapped to SAG_MIN..MAX
static volatile float lowpassCoefA      = 0.0f;  // tone LPF 'a'
static volatile float lowpassCoefB      = 1.0f;  // tone LPF 'b'

// Filter memory (tone)
static float lowpassState = 0.0f;

// Sag detector state (envelope follower)
static float envelopeLevel   = 0.0f;
static float envCoefAttack   = 0.0f;
static float envCoefRelease  = 0.0f;

// Clip meter envelope (for LED1)
static float clipLedEnvelope = 0.0f;

// FS2 edge tracking
static bool prevBypassSwitch = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline float dB_to_lin(float db) {
  return powf(10.0f, db * (1.0f / 20.0f));
}

static void UpdateLowpassFromCutoff(float fc_hz) {
  fc_hz = fmaxf(10.0f, fminf(fc_hz, SAMPLE_RATE_HZ * 0.45f));
  float a = expf(-2.0f * (float)M_PI * fc_hz / (float)SAMPLE_RATE_HZ);
  lowpassCoefA = a;
  lowpassCoefB = 1.0f - a;
}

static void UpdateSagTimeConstants() {
  envCoefAttack  = 1.0f - expf(-1.0f / ((SAG_ATTACK_MS  / 1000.0f) * SAMPLE_RATE_HZ));
  envCoefRelease = 1.0f - expf(-1.0f / ((SAG_RELEASE_MS / 1000.0f) * SAMPLE_RATE_HZ));
}

// Soft limiter around ±SOFT_LIMIT_THRESHOLD using a tanh "skirt"
// Goal: keep the polynomial from instantly becoming a hard square,
// but still allow it to approach a saturated shape smoothly.
static inline float HSP_SoftLimit(float x) {
  const float t = SOFT_LIMIT_THRESHOLD;
  if (fabsf(x) <= t) return x;             // inside the linear window
  float sign = (x >= 0.0f) ? 1.0f : -1.0f;
  float amountPast = fabsf(x) - t;         // how far beyond the window
  // tanhf() smoothly compresses the overshoot; result approaches ±(t + 1)
  return sign * (t + tanhf(amountPast));
}

// ----------------------
// CMOS shaper (textbook)
// ----------------------
// We build an **odd-symmetric polynomial** that echoes an inverter’s transfer curve:
//
//   1) First we apply a controllable **linear pre-gain** so different (a,b) settings
//      still sweep musically (you can think of this as “how hard we hit the chip”).
//
//   2) Then we compute an odd polynomial:
//
//        y_poly = v - a3*v^3 + b5*v^5
//
//      - The v^3 term (scaled by 'a3') delivers the **classic CMOS growl** and
//        most of the odd-harmonic content.
//      - The v^5 term (scaled by 'b5') adds **upper odd harmonics** (“glass/fizz”),
//        letting you push towards more spitty, synthy textures if desired.
//
//   3) Finally we run the polynomial through **HSP_SoftLimit()** to avoid an
//      immediate transition to a near-square when levels get high. This gives
//      you a wider “usable edge” where touch dynamics remain interesting.
//
// IMPORTANT: The **heuristic line** below drives the clip LED. Per your request,
// we keep it *exactly* as-is so your thresholds remain intact.
//
static inline float CMOS_Poly(float x, float pre, float a3, float b5, bool &clipped) {
  // Stage the input so the polynomial lands in a good operating region.
  float v  = x * pre;

  // Powers used by the polynomial
  float v2 = v * v;
  float v3 = v2 * v;
  float v5 = v3 * v2;

  // Odd-symmetric polynomial: fundamental minus cubic plus fifth.
  float y_poly = v - a3 * v3 + b5 * v5;

  // Gentle limiter that keeps peaks musical rather than brick-walled.
  float y_limited = HSP_SoftLimit(y_poly);

  // --- Clip LED heuristic (DO NOT CHANGE) ---
  if (fabsf(a3 * v3) > 0.08f || fabsf(b5 * v5) > 0.04f) clipped = true;

  return y_limited;
}

// -----------------------------------------------------------------------------
// Audio callback (audio thread). No control reads here.
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  if (isBypassed) {
    out = in;
    return;
  }

  // 1) Pre-gain with optional "kick" boost
  float preFromDrive = driveLinear * (isKickHeld ? dB_to_lin(EXTRA_DRIVE_DB) : 1.0f);

  // 2) Envelope follower for supply "sag"
  // Rectify input and smooth with attack/release; louder input → higher envelope.
  float levelNow = fabsf(in);
  float envCoef  = (levelNow > envelopeLevel) ? envCoefAttack : envCoefRelease;
  envelopeLevel += envCoef * (levelNow - envelopeLevel);

  // Map sagAmount01 (0..1) to an attenuation range, then apply to pre-gain.
  // More envelope → more droop → lower gain (emulates rail collapse).
  float sagDepth   = SAG_MIN + (SAG_MAX - SAG_MIN) * sagAmount01;  // 0..SAG_MAX
  float sagFactor  = 1.0f - (sagDepth * envelopeLevel);
  float stagedGain = preFromDrive * preGainScaler * fmaxf(0.25f, sagFactor);

  // 3) CMOS polynomial stage (with limiter inside)
  bool  clippedNow = false;
  float shaped     = CMOS_Poly(in, stagedGain, cubicAmount, fifthAmount, clippedNow);

  // 4) Post tone (simple LPF)
  lowpassState = lowpassCoefB * shaped + lowpassCoefA * lowpassState;
  float toned = lowpassState;

  // 5) Output trim + safety ceiling
  toned *= OUTPUT_TRIM;
  toned  = fmaxf(-OUT_LIMIT, fminf(toned, OUT_LIMIT));

  // Clip meter envelope (fast add on clip, slow decay in loop())
  if (clippedNow) clipLedEnvelope = fminf(1.0f, CLIP_LED_ATTACK * (clipLedEnvelope + 0.30f));

  out = toned;
}

// -----------------------------------------------------------------------------
// Setup (once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Pots at boot (standard layout):
  // RV1: Drive, RV2: Crunch(a), RV3: Fizz(b), RV4: Tone, RV5: Sag, RV6: Master
  driveLinear   = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));
  cubicAmount   = H.ReadPotMapped(RV2, A_MIN, A_MAX, HPCB_Curve::Exp10);
  fifthAmount   = H.ReadPotMapped(RV3, B_MIN, B_MAX, HPCB_Curve::Linear);
  preGainScaler = H.ReadPotMapped(RV4, PRE_GAIN_MIN, PRE_GAIN_MAX, HPCB_Curve::Exp10); // lets you rebalance stage hit
  {
    float sag01 = H.ReadPot(RV5);
    sagAmount01 = sag01;
    UpdateSagTimeConstants();
  }
  float fc0 = H.ReadPotMapped(RV4 /* Tone lives on RV4 in this layout */,
                              TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
  UpdateLowpassFromCutoff(fc0);

  // Master level (post)
  H.SetLevel(H.ReadPot(RV6));

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (controls/UI; not in audio time)
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();

  // RV1 — Drive (dB → linear)
  driveLinear = dB_to_lin(H.ReadPotMapped(RV1, DRIVE_MIN_DB, DRIVE_MAX_DB, HPCB_Curve::Exp10));

  // RV2 — Crunch (cubic 'a')
  cubicAmount = H.ReadPotMapped(RV2, A_MIN, A_MAX, HPCB_Curve::Exp10);

  // RV3 — Fizz (fifth 'b')
  fifthAmount = H.ReadPotMapped(RV3, B_MIN, B_MAX, HPCB_Curve::Linear);

  // RV4 — Pre-gain scaler AND Tone cutoff
  preGainScaler = H.ReadPotMapped(RV4, PRE_GAIN_MIN, PRE_GAIN_MAX, HPCB_Curve::Exp10);
  {
    float fc = H.ReadPotMapped(RV4, TONE_CUTOFF_MIN_HZ, TONE_CUTOFF_MAX_HZ, HPCB_Curve::Exp10);
    UpdateLowpassFromCutoff(fc);
  }

  // RV5 — Sag amount (0..1 → SAG_MIN..MAX internally)
  sagAmount01 = H.ReadPot(RV5);

  // RV6 — Master (post), lightly smoothed when engaged; unity when bypassed
  H.SetLevel(isBypassed ? 1.0f : H.ReadPotSmoothed(RV6, 15.0f));

  // FS1 — Kick (momentary)
  isKickHeld = H.FootswitchIsPressed(FS1);

  // FS2 — Bypass (edge detect)
  bool bypassSwitch = H.FootswitchIsPressed(FS2);
  if (bypassSwitch && !prevBypassSwitch) {
    isBypassed = !isBypassed;
  }
  prevBypassSwitch = bypassSwitch;

  // LEDs (active-HIGH)
  H.SetLED(LED2, !isBypassed);                         // effect active
  clipLedEnvelope *= CLIP_LED_DECAY;                   // decay the clip glow
  H.SetLED(LED1, (clipLedEnvelope > 0.12f) || isKickHeld);  // clip/kick indicator
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview
// --------
// **BasicCMOS** captures the crunchy, lo‑fi character of **CD4049/4069 inverter**
// fuzz. We approximate the rail‑to‑rail inverter transfer with an **odd‑symmetric
// polynomial** (3rd + 5th) to pile on odd harmonics without instantly flattening
// into a square. A **soft limiter** after the polynomial widens the sweet spot,
// and an optional **sag** control ducks the gain on peaks to mimic a starving 9 V rail.
//
// Controls
// --------
// - RV1 — Drive: pre-gain into the inverter-style stage (0 → +36 dB).
// - RV2 — Crunch (Cubic): strength of the 3rd-order term (main CMOS growl).
// - RV3 — Fizz (Fifth): adds upper odd harmonics (“glass/spit”).
// - RV4 — Pre-Gain Trim + Tone: balances stage hit and sets treble roll-off.
// - RV5 — Sag: increases supply droop on peaks (0 = none, 1 = strong ducking).
// - RV6 — Master: overall output level (post).
// - FS1 — Kick: hold for +6 dB extra pre-gain.
// - FS2 — Bypass: true passthrough on/off.
// - LED1 — Clip Meter: lights on nonlinear action (decays) and while Kick is held.
// - LED2 — Effect Active: lit when engaged. (LEDs are **active-HIGH**.)
//
// Signal Flow
// -----------
// Input → Drive → CMOS Polynomial (x − a·x^3 + b·x^5) → HSP_SoftLimit → LPF Tone → Master → Out
//             ↘—— optional SAG (envelope-controlled pre-attenuation) ——↗
//
// Customizable Parameters (top of file)
// -------------------------------------
// - A_MIN/A_MAX: cubic range (main flavor).
// - B_MIN/B_MAX: fifth range (adds spitty top).
// - PRE_GAIN_MIN/MAX: staging before polynomial; sets sweep feel.
// - SOFT_LIMIT_THRESHOLD: lower for softer edges.
// - SAG_*: MIN/MAX amount and attack/release times; longer release feels “saggier.”
// - TONE_CUTOFF_MIN/MAX: voice the treble window.
// - EXTRA_DRIVE_DB: Kick amount. OUTPUT_TRIM/OUT_LIMIT: global level and safety.
//
