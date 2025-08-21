// ============================================================================
// HSP_HarmonicBuzz.ino — v1.0.6  (48 kHz / 8‑sample block; mono; right out muted)
// Hybrid Baldwin Burns Buzzaround × Interfax Harmonic Percolator
// Artifact‑leaning, raw, and interactive.
//
// Hardware (HaroldPCB v1.2 PCB; works with HaroldPCB lib v1.3.x):
//   Pots   : RV1=Drive, RV2=Balance, RV3=Timbre, RV4=Sustain/Bias, RV5=Texture, RV6=Level
//   Toggles: TS1=Ge/Si curve, TS2=Bass‑cut on/off, TS3=unused, TS4=unused
//   Footsw : FS1=Rage boost (LED2 on when active), FS2=Soft bypass (LED1 = effect active)
//   Audio  : MONO. Library hard‑mutes the Right output.
//
// Notes:
// - All control reads & smoothing occur outside the audio callback (see loop()).
// - Debounced footswitch/toggle states are produced by hpcb.Idle(); loop() calls it first.
//
// --------------------------- ASCII SIGNAL FLOW -------------------------------
//
//   [Guitar In]
//        │
//        ├─► Bass‑cut HPF (~150 Hz), blendable with dry (TS2 toggles amount)
//        │         (smooth crossfade of HPF vs full‑band input)
//        │
//        ├─► (2× oversample midpoint tap for cleaner nonlinear input)
//        │                │
//        │                ├─► Percolator‑style shaper ─┐
//        │                │                            ├─► A‑path mix ─┐
//        │                └─► Buzzaround‑style shaper ─┘                │
//        │                                                             (avg) ─► Nonlinear core
//        ├───────────────────────────────────────┐                       │
//        │                                       │                       │
/*      │                                       ├─► Percolator shaper ──┤
        │                                       │                       │
        │                                       └─► Buzzaround shaper ──┘
        │
        └─► Texture morph (Percolator ↔ Buzzaround), then average
            │
            └─► Tilt EQ (pivot ~1 kHz): dark ↔ bright (RV3 Timbre)
                 │
                 ├─► Balance (mix in pre‑drive clean for articulation; RV2)
                 │
                 └─► Post polish LPF (~11 kHz, soft 1‑pole)
                        │
                        └─► Output level & soft limiter → BYPASS crossfade (FS2)
                                   │
                                   └─► [Left Out]   (Right is muted by library)

   FS1 “Rage” increases pre‑gain before the shapers.
   LED1 = effect active (bypass OFF).  LED2 = Rage ON.                               */
// ============================================================================

#include <HaroldPCB.h>
#include <math.h>

HaroldPCB hpcb;

// ============================================================================
// TUNABLE PARAMETERS (lifted from existing literals; values unchanged)
// ============================================================================

// Engine (project default)
static const float SR = 48000.0f;  // sample rate (Hz)
static const int BLKSIZE = 8;      // audio block size (frames)

// Smoothing time constants (seconds) → converted to alphas per block
static const float POT_SMOOTH_SEC = 0.020f;     // ~20 ms
static const float TOG_SMOOTH_SEC = 0.010f;     // ~10 ms
static const float BYPASS_SMOOTH_SEC = 0.007f;  // ~7 ms

// Drive/Level mapping
static const float DRIVE_MAX_GAIN_UP = 29.0f;  // 1x→30x via 1 + k^2 * 29
static const float LEVEL_MIN_DB = -60.0f;      // map pot^2 → dB
static const float LEVEL_SPAN_DB = 72.0f;      // -60..+12 dB total span

// Bias / Texture / Tilt mapping
static const float BIAS_RANGE = 1.2f;          // (v - 0.5) * 1.2
static const float TEXTURE_SHAPE_GAIN = 2.0f;  // morph curve factor
// map_tilt(v) = 2v - 1  (unit tilt)

// Ge/Si flavor mapping
static const float GE_SI_KNEE_MIN = 0.8f;
static const float GE_SI_KNEE_SPAN = 0.6f;
static const float GE_SI_SAG_MIN = 0.06f;
static const float GE_SI_SAG_SPAN = 0.02f;

// Nonlinear cores
static const float SHAPER_PERC_POS_BIAS = 0.15f;
static const float SHAPER_PERC_NEG_BIAS = -0.30f;
static const float SHAPER_PERC_POS_WT = 0.65f;
static const float SHAPER_PERC_NEG_WT = 0.35f;
static const float SHAPER_PERC_EXTRA = 0.12f;  // extra asym tweak

static const float SHAPER_BUZZ_PBIAS = 0.5f;    // pre_bias scaler
static const float SHAPER_BUZZ_CUBIC = 0.333f;  // cubic term factor
static const float SHAPER_BUZZ_MIX_T = 0.55f;   // tanh vs cubic mix
static const float SHAPER_BUZZ_MIX_C = 0.45f;

// Soft limiter
static const float FAST_TANH_A = 27.0f;     // fast_tanh() numerator const
static const float FAST_TANH_B = 9.0f;      // fast_tanh() denom multiplier
static const float SOFT_LIMIT_PRE = 0.85f;  // soft_limit() pre‑gain

// Rage / sag behaviour
static const float RAGE_PREG_GAIN = 0.7f;     // extra pre‑gain when Rage
static const float SAG_TARGET_SCALE = 0.25f;  // |in| → target sag amount
static const float SAG_LERP_ALPHA = 0.0015f;  // very slow tracker

// Post/tilt/HPF frequencies
static const float POST_LP_FC_HZ = 11000.0f;    // post polish LPF
static const float TILT_PIVOT_FC_HZ = 1000.0f;  // tilt EQ pivot
static const float BASSCUT_HP_FC_HZ = 150.0f;   // input HPF

// ============================================================================
// Small helpers — plain‑English math bricks
// ============================================================================
inline float clamp01(float x) {
  return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}

// Fast tanh approximation used as a musically smooth soft clip.
inline float fast_tanh(float x) {
  const float x2 = x * x;
  return x * (FAST_TANH_A + x2) / (FAST_TANH_A + FAST_TANH_B * x2);
}

// Gentle saturator at output stage
inline float soft_limit(float x) {
  return fast_tanh(SOFT_LIMIT_PRE * x);
}

// ============================================================================
// Smoothed (latched) parameters — targets (…_t) low‑pass into working values
// ============================================================================
struct Smoothed {
  float drive_t = 0.4f, drive = 0.4f;      // pre‑gain
  float balance_t = 0.5f, balance = 0.5f;  // clean/fuzz blend
  float timbre_t = 0.5f, timbre = 0.5f;    // tilt EQ
  float bias_t = 0.5f, bias = 0.5f;        // DC bias shift
  float texture_t = 0.5f, texture = 0.5f;  // Percolator ↔ Buzzaround morph
  float level_t = 0.7f, level = 0.7f;      // master level
  float ge_si_t = 0.0f, ge_si = 0.0f;      // 0=Ge‑ish, 1=Si‑ish
  float hpf_on_t = 0.0f, hpf_on = 0.0f;    // bass‑cut
  float rage_t = 0.0f, rage = 0.0f;        // FS1 latch
  float bypass_t = 0.0f, bypass = 0.0f;    // FS2 latch (1=bypassed)
} P;

// Convert time constants → block‑rate one‑pole smoothing factors
static const float SMOOTH_POTS = 1.0f - expf(-BLKSIZE / (SR * POT_SMOOTH_SEC));
static const float SMOOTH_TOGS = 1.0f - expf(-BLKSIZE / (SR * TOG_SMOOTH_SEC));
static const float SMOOTH_BYPASS = 1.0f - expf(-BLKSIZE / (SR * BYPASS_SMOOTH_SEC));

// ============================================================================
// DSP State (one‑poles and memory for filters / simple oversampling tap)
// ============================================================================
float z_prev_in = 0.0f;                           // previous input (for midpoint)
float postlp_y = 0.0f, postlp_a = 0.0f;           // post polish LPF state/alpha
float tilt_lp_y = 0.0f, tilt_a = 0.0f;            // tilt EQ low‑pass state/alpha
float hpf_y = 0.0f, hpf_x1 = 0.0f, hpf_a = 0.0f;  // input HPF state/alpha

// Footswitch edge memory (simple rising‑edge latch logic)
bool fs1_last = false, fs2_last = false;

// ============================================================================
// Mapping helpers — translate 0..1 controls into meaningful ranges
// ============================================================================
inline float map_drive(float v) {
  float k = v * v;                      // more resolution at low end
  return 1.0f + k * DRIVE_MAX_GAIN_UP;  // 1..30×
}
inline float map_level(float v) {
  float s = v * v;                              // gentle taper
  float db = LEVEL_MIN_DB + s * LEVEL_SPAN_DB;  // -60..+12 dB
  return powf(10.f, db / 20.f);
}
inline float map_bias(float v) {
  return (v - 0.5f) * BIAS_RANGE;
}
inline float map_texture(float v) {
  // S‑curve that favors extremes a bit for character
  float t = (v <= 0.5f) ? (v * v * TEXTURE_SHAPE_GAIN)
                        : (1.f - (1.f - v) * (1.f - v) * TEXTURE_SHAPE_GAIN);
  return clamp01(t);
}
inline float map_tilt(float v) {
  return v * 2.f - 1.f;  // -1..+1
}
inline void ge_si_params(float ge_si, float &knee, float &sag) {
  knee = GE_SI_KNEE_MIN + ge_si * GE_SI_KNEE_SPAN;
  sag = GE_SI_SAG_MIN + ge_si * GE_SI_SAG_SPAN;
}

// ============================================================================
// Nonlinear cores — two “flavors” that we morph between
// ============================================================================
inline float shaper_percolator(float x, float pre_bias, float knee, float sag_tracker) {
  float v = x + pre_bias - sag_tracker;
  float pos = fast_tanh(knee * (v + SHAPER_PERC_POS_BIAS));
  float neg = fast_tanh(knee * (v + SHAPER_PERC_NEG_BIAS));
  float a = SHAPER_PERC_POS_WT * pos + SHAPER_PERC_NEG_WT * neg;
  if (v < 0.f) a -= SHAPER_PERC_EXTRA * (v * (1.f - fabsf(v)));  // extra asym for “bite”
  return a;
}
inline float shaper_buzzaround(float x, float pre_bias, float knee) {
  float v = x + pre_bias * SHAPER_BUZZ_PBIAS;
  float c = v - (v * v * v) * (SHAPER_BUZZ_CUBIC * knee);  // cubic soft fold
  float t = fast_tanh(knee * v);                           // smooth saturator
  return SHAPER_BUZZ_MIX_T * t + SHAPER_BUZZ_MIX_C * c;
}

// ============================================================================
// Audio Callback — one sample in, one sample out (MONO). Right is muted in lib.
// ============================================================================
void AudioCB(float inL, float &outL) {
  const float is_byp = P.bypass;

  float knee, sag_amt;
  ge_si_params(P.ge_si, knee, sag_amt);

  float x = inL;

  // Bass‑cut (~150 Hz), smoothly blendable with fullband input
  float hpf_raw = x;
  {
    float y = hpf_y + hpf_a * (x - hpf_x1);  // one‑pole low‑pass state
    hpf_y = y;
    hpf_x1 = x;
    float hp = x - y;  // high‑pass output
    hpf_raw = (1.0f - P.hpf_on) * x + P.hpf_on * hp;
  }

  // 2× oversampling midpoint for the nonlinear core (simple zero‑order)
  float mid = 0.5f * (z_prev_in + hpf_raw);
  z_prev_in = hpf_raw;

  float pregain = map_drive(P.drive) * (1.f + RAGE_PREG_GAIN * P.rage);
  float pre_bias = map_bias(P.bias);

  static float sag_z = 0.f;  // slow envelope used for bias “sag”
  float target_sag = sag_amt * SAG_TARGET_SCALE * fabsf(hpf_raw);
  sag_z += SAG_LERP_ALPHA * (target_sag - sag_z);

  // A‑path uses midpoint (slightly cleaner); B‑path uses current sample
  float a_in = mid * pregain;
  float a_perc = shaper_percolator(a_in, pre_bias, knee, sag_z);
  float a_buzz = shaper_buzzaround(a_in, pre_bias, knee);

  float b_in = hpf_raw * pregain;
  float b_perc = shaper_percolator(b_in, pre_bias, knee, sag_z);
  float b_buzz = shaper_buzzaround(b_in, pre_bias, knee);

  // Texture morph between flavors on both paths, then average
  float t = map_texture(P.texture);
  float a_mix = (1.f - t) * a_perc + t * a_buzz;
  float b_mix = (1.f - t) * b_perc + t * b_buzz;
  float nl = 0.5f * (a_mix + b_mix);

  // Tilt EQ around ~1 kHz pivot (split into HP/LP and rebalance)
  {
    tilt_lp_y += tilt_a * (nl - tilt_lp_y);
    float hp = nl - tilt_lp_y;
    float lp = tilt_lp_y;
    float tilt = map_tilt(P.timbre);      // -1..+1
    float lp_gain = 0.5f * (1.f - tilt);  // darker when negative
    float hp_gain = 0.5f * (1.f + tilt);  // brighter when positive
    nl = lp_gain * lp + hp_gain * hp;
  }

  // Buzzaround‑style “Balance” — add pre‑drive clean for bite/articulation
  float mixed = (1.f - P.balance) * nl + P.balance * hpf_raw;

  // Post polish (~11 kHz soft low‑pass)
  postlp_y += postlp_a * (mixed - postlp_y);
  float polished = postlp_y;

  // Output level and safety soft‑limit
  float level = map_level(P.level);
  float active_out = soft_limit(polished * level);

  // Soft bypass crossfade (P.bypass smoothed separately)
  outL = is_byp * inL + (1.f - is_byp) * active_out;
}

// ============================================================================
// Controls & LEDs — read hardware, update smoothed params, set indicators
// ============================================================================
void ReadAndSmoothControls() {
  // Pots (0..1). Balance uses “more clean to the right” convention (inverted).
  P.drive_t = clamp01(hpcb.ReadPot(RV1));
  P.balance_t = 1.0f - clamp01(hpcb.ReadPot(RV2));
  P.timbre_t = clamp01(hpcb.ReadPot(RV3));
  P.bias_t = clamp01(hpcb.ReadPot(RV4));
  P.texture_t = clamp01(hpcb.ReadPot(RV5));
  P.level_t = clamp01(hpcb.ReadPot(RV6));

  // Toggles → 0/1
  P.ge_si_t = hpcb.ReadToggle(TS1) ? 1.f : 0.f;
  P.hpf_on_t = hpcb.ReadToggle(TS2) ? 1.f : 0.f;

  // Footswitch latches (rising‑edge toggles)
  bool fs1 = hpcb.FootswitchIsPressed(FS1);
  if (fs1 && !fs1_last) { P.rage_t = (P.rage_t < 0.5f) ? 1.f : 0.f; }
  fs1_last = fs1;

  bool fs2 = hpcb.FootswitchIsPressed(FS2);
  if (fs2 && !fs2_last) { P.bypass_t = (P.bypass_t < 0.5f) ? 1.f : 0.f; }
  fs2_last = fs2;

  // Smooth targets → working values (one‑pole at block rate)
  P.drive += SMOOTH_POTS * (P.drive_t - P.drive);
  P.balance += SMOOTH_POTS * (P.balance_t - P.balance);
  P.timbre += SMOOTH_POTS * (P.timbre_t - P.timbre);
  P.bias += SMOOTH_POTS * (P.bias_t - P.bias);
  P.texture += SMOOTH_POTS * (P.texture_t - P.texture);
  P.level += SMOOTH_POTS * (P.level_t - P.level);

  P.ge_si += SMOOTH_TOGS * (P.ge_si_t - P.ge_si);
  P.hpf_on += SMOOTH_TOGS * (P.hpf_on_t - P.hpf_on);
  P.rage += SMOOTH_TOGS * (P.rage_t - P.rage);
  P.bypass += SMOOTH_BYPASS * (P.bypass_t - P.bypass);

  // LEDs (ACTIVE‑HIGH):
  //   LED1 = Effect Active (FS2 latch) → ON when not bypassed
  //   LED2 = Rage (FS1 latch)
  hpcb.SetLED(LED1, (P.bypass < 0.5f));
  hpcb.SetLED(LED2, (P.rage > 0.5f));
}

// ============================================================================
// Setup / Loop — init filters, start audio
// ============================================================================
void setup() {
  hpcb.Init(SR, BLKSIZE);

  // Post‑LPF (~11 kHz) alpha
  {
    float x = expf(-2.f * 3.14159265f * POST_LP_FC_HZ / SR);
    postlp_a = 1.f - x;
  }
  // Tilt pivot (~1 kHz) alpha
  {
    float x = expf(-2.f * 3.14159265f * TILT_PIVOT_FC_HZ / SR);
    tilt_a = 1.f - x;
  }
  // Bass‑cut HPF (~150 Hz) implemented via LP state (hp = x - y)
  {
    float x = expf(-2.f * 3.14159265f * BASSCUT_HP_FC_HZ / SR);
    hpf_a = 1.f - x;
  }

  hpcb.StartAudio(AudioCB);
}

void loop() {
  // FIX: Ensure debounced states/events are fresh before we read them.
  hpcb.Idle();
  ReadAndSmoothControls();
}

/* ==============================  USER GUIDE  ================================
HarmonicBuzz = two classic fuzz “personalities” under one roof, with a tilt EQ,
a clean “balance” blend for bite, and a post polish filter to keep the top end
pleasant. It’s rowdy by design but controllable.

CONTROLS (Pots)
- RV1 Drive: Gain into the fuzz engines. Low = texture, high = sustain/edge.
- RV2 Balance: Adds clean (pre‑drive) on the right side of the knob for
  articulation. Left = all fuzz; right = more clean bite cutting through.
- RV3 Timbre: Tilt EQ around ~1 kHz. Left = darker, Right = brighter.
- RV4 Sustain/Bias: Shifts the operating point for more gate (left) or smoother
  sustain (right). Think of it as sliding the fuzz “sweet spot.”
- RV5 Texture: Morphs between Percolator grit and Buzzaround sustain. Left =
  Percolator‑ish bite; Right = Buzzaround‑ish body.
- RV6 Level: Master output level (internal soft limiter keeps things in bounds).

TOGGLES
- TS1 Ge/Si: Ge‑ish (softer knee, a little more sag) ↔ Si‑ish (crisper).
- TS2 BassCut: ~150 Hz high‑pass blended in for stacking or tightening.

FOOTSWITCHES & LEDs
- FS1 Rage: Extra pre‑gain for more aggression. LED2 lights when Rage is ON.
- FS2 Bypass: Soft bypass crossfade. LED1 lights when the effect is ACTIVE.

TRY THIS
1) Classic slice: TS2 off, Texture noon, Drive at 10 o’clock, Balance 9 o’clock,
   Bias noon, Timbre 1 o’clock. Big rhythm that still cuts.
2) Spitty lead: TS1 to Ge, Drive 2 o’clock, Bias 9–10 o’clock (gated feel),
   Texture toward Percolator, BassCut ON, Level to taste.
3) Stacked clarity: Put HarmonicBuzz first, set Balance 1–2 o’clock, BassCut ON,
   low Drive, then hit a mid‑gain amp model—chime with hair.

TIPS
- Balance is a “clean underlay.” Turning it up keeps pick attack intelligible.
- Texture is not a tone control—it changes the distortion personality itself.
- Rage (FS1) is a quick solo boost without touching the knobs.
============================================================================= */
