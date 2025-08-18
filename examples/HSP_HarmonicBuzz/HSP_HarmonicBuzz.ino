// HSP_HarmonicBuzz.ino
// Version: 1.0.4  (48 kHz / 48-sample block; mono; right out muted by library)
//
// Hybrid Baldwin Burns Buzzaround × Interfax Harmonic Percolator
// Artifact-like, raw, and interactive — optimized for shoegaze/post-rock.
//
// Hardware (HaroldPCB v1.2):
//   Pots   : RV1=Drive, RV2=Balance, RV3=Timbre, RV4=Sustain/Bias, RV5=Texture, RV6=Level
//   Toggles: TS1=Ge/Si curve, TS2=Bass-cut on/off, TS3=unused, TS4=unused
//   Footsw : FS1=Rage boost (LED1 on when active), FS2=Soft bypass (LED2 shows effect active)
//   Rate   : 48 kHz    Block: 48
//
// Notes for v1.2.8 of HaroldPCB:
// - StartAudio(AudioCB) and Idle() (no args) are the expected calls.
// - LED control uses SetLED(LEDx, bool).
// - Footswitch read uses footswitchIsPressed(FSx).
//
// All control reads & smoothing happen outside the audio callback.

#include <HaroldPCB.h>
#include <math.h>

HaroldPCB hpcb;

static const float SR = 48000.0f;
static const int BLKSIZE = 8;

// ---------- small helpers ----------
inline float clamp01(float x) {
  return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}
inline float fast_tanh(float x) {
  const float x2 = x * x;
  return x * (27.f + x2) / (27.f + 9.f * x2);
}

// ==============================
// ====== Smoothed Params =======
// ==============================
struct Smoothed {
  float drive_t = 0.4f, drive = 0.4f;      // pre-gain
  float balance_t = 0.5f, balance = 0.5f;  // clean/fuzz blend
  float timbre_t = 0.5f, timbre = 0.5f;    // tilt EQ
  float bias_t = 0.5f, bias = 0.5f;        // DC bias shift
  float texture_t = 0.5f, texture = 0.5f;  // Percolator ↔ Buzzaround morph
  float level_t = 0.7f, level = 0.7f;      // master level
  float ge_si_t = 0.0f, ge_si = 0.0f;      // 0=Ge-ish, 1=Si-ish
  float hpf_on_t = 0.0f, hpf_on = 0.0f;    // bass-cut
  float rage_t = 0.0f, rage = 0.0f;        // FS1 latch
  float bypass_t = 0.0f, bypass = 0.0f;    // FS2 latch (1=bypassed)
} P;

static const float SMOOTH_POTS = 1.0f - expf(-BLKSIZE / (SR * 0.020f));    // ~20 ms
static const float SMOOTH_TOGS = 1.0f - expf(-BLKSIZE / (SR * 0.010f));    // ~10 ms
static const float SMOOTH_BYPASS = 1.0f - expf(-BLKSIZE / (SR * 0.007f));  // ~7  ms

// ==============================
// ====== DSP State =============
// ==============================
float z_prev_in = 0.0f;                           // for 2×-over midpoint
float postlp_y = 0.0f, postlp_a = 0.0f;           // post polish LPF
float tilt_lp_y = 0.0f, tilt_a = 0.0f;            // tilt EQ lowpass
float hpf_y = 0.0f, hpf_x1 = 0.0f, hpf_a = 0.0f;  // input HPF (bass-cut)

inline float soft_limit(float x) {
  return fast_tanh(0.85f * x);
}

// Footswitch edge memory
bool fs1_last = false, fs2_last = false;

// ==============================
// ====== Mapping Helpers =======
// ==============================
inline float map_drive(float v) {
  float k = v * v;
  return 1.0f + k * 29.0f;
}  // 1x→30x
inline float map_level(float v) {
  float s = v * v;
  float db = -60.f + s * 72.f;
  return powf(10.f, db / 20.f);
}
inline float map_bias(float v) {
  return (v - 0.5f) * 1.2f;
}
inline float map_texture(float v) {
  float t = (v <= 0.5f) ? (v * v * 2.f) : (1.f - (1.f - v) * (1.f - v) * 2.f);
  return clamp01(t);
}
inline float map_tilt(float v) {
  return v * 2.f - 1.f;
}
inline void ge_si_params(float ge_si, float &knee, float &sag) {
  knee = 0.8f + ge_si * 0.6f;
  sag = 0.06f + ge_si * 0.02f;
}

// ==============================
// ====== Nonlinear cores =======
// ==============================
inline float shaper_percolator(float x, float pre_bias, float knee, float sag_tracker) {
  float v = x + pre_bias - sag_tracker;
  float pos = fast_tanh(knee * (v + 0.15f));
  float neg = fast_tanh(knee * (v - 0.30f));
  float a = 0.65f * pos + 0.35f * neg;
  if (v < 0.f) a -= 0.12f * (v * (1.f - fabsf(v)));  // little concave fold
  return a;
}
inline float shaper_buzzaround(float x, float pre_bias, float knee) {
  float v = x + pre_bias * 0.5f;
  float c = v - (v * v * v) * (0.333f * knee);
  float t = fast_tanh(knee * v);
  return 0.55f * t + 0.45f * c;
}

// ==============================
// ====== Audio Callback ========
// ==============================
void AudioCB(float inL, float &outL) {
  const float is_byp = P.bypass;

  float knee, sag_amt;
  ge_si_params(P.ge_si, knee, sag_amt);

  float x = inL;

  // Bass-cut (~150 Hz), smoothly blendable
  float hpf_raw = x;
  {
    float y = hpf_y + hpf_a * (x - hpf_x1);
    hpf_y = y;
    hpf_x1 = x;
    float hp = x - y;
    hpf_raw = (1.0f - P.hpf_on) * x + P.hpf_on * hp;
  }

  // 2× oversampling on the nonlinear core
  float mid = 0.5f * (z_prev_in + hpf_raw);
  z_prev_in = hpf_raw;

  float pregain = map_drive(P.drive) * (1.f + 0.7f * P.rage);
  float pre_bias = map_bias(P.bias);

  static float sag_z = 0.f;
  float target_sag = sag_amt * 0.25f * fabsf(hpf_raw);
  sag_z += 0.0015f * (target_sag - sag_z);

  float a_in = mid * pregain;
  float a_perc = shaper_percolator(a_in, pre_bias, knee, sag_z);
  float a_buzz = shaper_buzzaround(a_in, pre_bias, knee);

  float b_in = hpf_raw * pregain;
  float b_perc = shaper_percolator(b_in, pre_bias, knee, sag_z);
  float b_buzz = shaper_buzzaround(b_in, pre_bias, knee);

  float t = map_texture(P.texture);
  float a_mix = (1.f - t) * a_perc + t * a_buzz;
  float b_mix = (1.f - t) * b_perc + t * b_buzz;

  float nl = 0.5f * (a_mix + b_mix);

  // Tilt EQ (pivot ~1 kHz)
  {
    tilt_lp_y += tilt_a * (nl - tilt_lp_y);
    float hp = nl - tilt_lp_y;
    float lp = tilt_lp_y;
    float tilt = map_tilt(P.timbre);
    float lp_gain = 0.5f * (1.f - tilt);
    float hp_gain = 0.5f * (1.f + tilt);
    nl = lp_gain * lp + hp_gain * hp;
  }

  // Buzzaround “Balance” (clean pre-drive tap)
  float mixed = (1.f - P.balance) * nl + P.balance * hpf_raw;

  // Post polish (~11 kHz 1-pole)
  postlp_y += postlp_a * (mixed - postlp_y);
  float polished = postlp_y;

  float level = map_level(P.level);
  float active_out = soft_limit(polished * level);

  outL = is_byp * inL + (1.f - is_byp) * active_out;
}

// ==============================
// ====== Controls & LEDs =======
// ==============================
void ReadAndSmoothControls() {
  // Pots
  P.drive_t = clamp01(hpcb.ReadPot(RV1));
  P.balance_t = 1.0f - clamp01(hpcb.ReadPot(RV2));
  P.timbre_t = clamp01(hpcb.ReadPot(RV3));
  P.bias_t = clamp01(hpcb.ReadPot(RV4));
  P.texture_t = clamp01(hpcb.ReadPot(RV5));
  P.level_t = clamp01(hpcb.ReadPot(RV6));

  // Toggles
  P.ge_si_t = hpcb.ReadToggle(TS1) ? 1.f : 0.f;
  P.hpf_on_t = hpcb.ReadToggle(TS2) ? 1.f : 0.f;

  // Footswitches — HaroldPCB v1.2.8
  bool fs1 = hpcb.FootswitchIsPressed(FS1);
  if (fs1 && !fs1_last) { P.rage_t = (P.rage_t < 0.5f) ? 1.f : 0.f; }
  fs1_last = fs1;

  bool fs2 = hpcb.FootswitchIsPressed(FS2);
  if (fs2 && !fs2_last) { P.bypass_t = (P.bypass_t < 0.5f) ? 1.f : 0.f; }
  fs2_last = fs2;

  // Smooth targets → working values
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

  // LEDs
  hpcb.SetLED(LED2, (P.rage > 0.5f));
  hpcb.SetLED(LED1, (P.bypass > 0.5f));
}

// ==============================
// ====== Setup / Loop ==========
// ==============================
void setup() {
  hpcb.Init(SR, BLKSIZE);

  // Post-LPF (~11 kHz)
  {
    float fc = 11000.f;
    float x = expf(-2.f * 3.14159265f * fc / SR);
    postlp_a = 1.f - x;
  }

  // Tilt pivot (~1 kHz)
  {
    float fc = 1000.f;
    float x = expf(-2.f * 3.14159265f * fc / SR);
    tilt_a = 1.f - x;
  }

  // Bass-cut HPF (~150 Hz)
  {
    float fc = 150.f;
    float x = expf(-2.f * 3.14159265f * fc / SR);
    hpf_a = 1.f - x;
  }

  // Start audio
  hpcb.StartAudio(AudioCB);
}

void loop() {
  ReadAndSmoothControls();
  hpcb.Idle();
}

/* =============================  QUICK GUIDE  =============================
RV1 Drive   : gain into fuzz. RV2 Balance : adds clean for articulation.
RV3 Timbre  : tilt EQ (dark ↔ bright). RV4 Sustain/Bias : gate ↔ infinite sustain.
RV5 Texture : Percolator grit ↔ Buzzaround sustain. RV6 Level : master +12 dB max.

TS1 Ge/Si   : softer knee/sag ↔ crisper. TS2 BassCut : ~150 Hz HPF for stacking.
FS1 Rage    : extra pre-gain (LED1). FS2 Bypass : soft bypass (LED2 on when active).
===========================================================================
*/
