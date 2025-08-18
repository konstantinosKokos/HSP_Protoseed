// HSP_OptimodBox_v3_18_96k_bs4_AM_constants_TS3invert_WITH_KLON_BOOST.ino
// Broadcast-style loudness box tuned for ≤8 kHz front-end LPF
// Modes (RV1 Flavor): 0=FM 3-band, 1=FM 5-band, 2=AM Narrowband (≈5 kHz LPF + asym clip)
// Pots:  RV1=Flavor, RV2=Amount, RV3=Tune, RV4=Harmonics(Air/Bias), RV5=Mix, RV6=Master (post, PROCESSED PATH ONLY)
// FS1/LED1: Global BYPASS (soft crossfade; settled bypass is 100% clean). LED1 = effect active.
// FS2/LED2: **Transparent clean BOOST** (~+6 dB). Not toggleable when bypassed. LED2 = boost ON, forced OFF when bypassed.
// TS1: Comp Release (Fast/Slow)
// TS2: Tight Low
// TS3: Presence Tilt — **INVERTED** relative to other toggles per request
// TS4: Mode helper — FM: HF saver ceiling; AM: Narrow/Wide bandwidth.
// NOTE: LEDs are active-LOW; TS1, TS2, TS4 read inverted; **TS3 is NOT inverted (special-case)**.
// NOTE: 96 kHz / 4-sample block. Mono only.

// ----------------------------------------------------------------------
//                       T U N A B L E   C O N S T A N T S
// ----------------------------------------------------------------------

// Engine / housekeeping
static const float kSR_HZ = 96000.0f;  // informational; actual SR is taken from hardware
static const int kBlockSize = 4;       // informational; actual block size is set in Init

// Crossfade speeds (per-sample one-pole alphas). Lower = slower/softer.
static const float kBypassFadeAlpha = 0.0010f;  // engage/disengage fade (kills clicks)
static const float kBoostFadeAlpha = 0.0040f;   // FS2 boost in/out

// Master volume taper (0..1)^exp → natural to true zero
static const float kMasterPowerExp = 2.5f;

// Tight low (TS2) high-pass
static const float kTightHP_On_Hz = 110.0f;  // audible cleanup when TS2 on
static const float kTightHP_Off_Hz = 20.0f;  // essentially flat when TS2 off

// Presence / “air” tilt (TS3) pivoting inside ≤8 kHz ceiling
static const float kPresencePivot_Lo = 1800.0f;  // TS3 off
static const float kPresencePivot_Hi = 3800.0f;  // TS3 on
static const float kPresenceAmt_Max = 3.5f;      // max added HF (linear gain added to HP output)
static const float kPresenceAmt_OffMul = 1.2f;   // base strength when TS3 off
static const float kPresenceAmt_OnMul = 3.2f;    // stronger when TS3 on

// HF saver / ceiling (TS4) for FM modes
static const float kCeiling_Hz_Wide = 7800.0f;
static const float kCeiling_Hz_Tight = 6000.0f;

// ---------- Multiband compressor behavior (TS1 Fast/Slow) -------------
static const float kThr_dB_Base = -3.0f;        // gentler base threshold
static const float kThr_dB_PerAmount = -18.0f;  // threshold slope vs Amount
static const float kRatio_Base = 1.3f;          // softer base ratio
static const float kRatio_PerAmount = 3.2f;     // gentler growth with Amount
static const float kRatio_FastBonus = 0.6f;     // TS1 Fast adds firmness
static const float kRatio_SlowPenalty = -0.2f;  // TS1 Slow softens a bit
static const float kFastAtk_ms = 0.5f;
static const float kFastRel_Min_ms = 15.0f;
static const float kFastRel_Max_ms = 60.0f;
static const float kSlowAtk_ms = 18.0f;
static const float kSlowRel_Min_ms = 380.0f;
static const float kSlowRel_Max_ms = 980.0f;

// LF band extra control when TS2 (Tight) is on
static const float kLF_GainMul_FM3_Tight = 0.5f;  // FM3 lowest band gain multiplier
static const float kLF_GainMul_FM5_Tight = 0.6f;  // FM5 lowest band gain multiplier

// AM mode specifics (mode 2)
static const float kAM_Cut_Base_Hz = 5000.0f;  // center point; swept by Tune
static const float kAM_Cut_Min_Hz = 3800.0f;
static const float kAM_Cut_Max_Hz = 6200.0f;
static const float kAM_Cut_NarrowMul = 0.85f;  // TS4 on (Narrow)
static const float kAM_Cut_WideMul = 1.10f;    // TS4 off (Wide)
static const float kAM_Asym_Min = 0.10f;       // floor asym
static const float kAM_Asym_Max = 0.65f;       // ceiling asym
static const float kAM_Asym_AmountMix = 0.6f;  // Amount weight vs Harmonics in asym calc
static const float kAM_Asym_HarmMix = 0.4f;
static const float kAM_Asym_NarrowBoost = 1.15f;  // TS4 Narrow → slightly more asym
static const float kAM_DrivePerAmount = 6.0f;     // added to 1.0 with Amount for clip drive

// All-pass phase rotator (pre-dynamics)
static const float kAP_LF_Hz_FM = 120.0f;
static const float kAP_LF_Hz_AM = 90.0f;

// Flavor snapping thresholds (RV1)
static const float kFlavorZone01 = 1.0f / 3.0f;
static const float kFlavorZone02 = 2.0f / 3.0f;

// Epsilon for pot change gating
static const float kEpsParam = 0.01f;

// FS2 boost amount (transparent “Klon-ish” push) — tweak if you want more/less
static const float kBoost_dB = 6.0f;  // ~+6 dB
// ----------------------------------------------------------------------

#include <HaroldPCB.h>
#include <math.h>

HaroldPCB hpcb;
static float gSR = kSR_HZ;  // will be updated from hardware

// ========================== Parameters (0..1) ==========================
struct Params {
  float flavor;     // snapped centers
  float amount;     // intensity
  float tune;       // crossover / cutoff shift
  float harmonics;  // air / bias
  float mix;        // wet/dry
} P, Pprev;

// Master gain applied ONLY to processed branch (computed in loop, used in ISR)
static volatile float gMasterGain = 1.0f;

// ========================== Minimal DSP helpers ========================
struct OnePoleLP {
  float a = 0.0f, y = 0.0f;
  void setFC(float fc) {
    if (fc < 5.0f) fc = 5.0f;
    a = 1.0f - expf(-2.0f * 3.14159265359f * fc / gSR);
  }
  inline float process(float x) {
    y += a * (x - y);
    return y;
  }
};

struct OnePoleHP {
  OnePoleLP lp;
  void setFC(float fc) {
    lp.setFC(fc);
  }
  inline float process(float x) {
    return x - lp.process(x);
  }
};

struct AP1 {
  float a = 0.0f, x1 = 0.0f, y1 = 0.0f;
  void setFC(float fc) {
    float w = tanf(3.14159265359f * fc / gSR);
    a = (1.0f - w) / (1.0f + w);
  }
  inline float process(float x) {
    float y = -a * x + x1 + a * y1;
    x1 = x;
    y1 = y;
    return y;
  }
};

struct RMSDet {
  float atk_a = 0.0f, rel_a = 0.0f, e2 = 1e-12f;
  void setTimes(float atk_ms, float rel_ms) {
    float dt = 1000.0f / gSR;
    float atk_tc = atk_ms <= 0 ? dt : atk_ms;
    float rel_tc = rel_ms <= 0 ? dt : rel_ms;
    atk_a = 1.0f - expf(-dt / atk_tc);
    rel_a = 1.0f - expf(-dt / rel_tc);
  }
  inline float process(float x) {
    float s2 = x * x;
    float a = (s2 > e2) ? atk_a : rel_a;
    e2 += a * (s2 - e2);
    return sqrtf(e2 + 1e-20f);
  }
};

struct Comp {
  float thr_lin = 0.5f;  // updated from dB
  float ratio = 2.0f;    // updated per mode/TS1
  inline float gainFrom(float env) {
    if (env <= thr_lin) return 1.0f;
    float p = (ratio - 1.0f) / ratio;
    return powf(thr_lin / (env + 1e-20f), p);
  }
};

static inline float sat_tanh(float x, float drive) {
  float d = fmaxf(0.5f, drive);
  float y = tanhf(x * d);
  float n = tanhf(d);
  return y / (n > 1e-6f ? n : 1.0f);
}

// ========================== FS2 CLEAN BOOST ============================
// Simple transparent pre-processor gain, smoothly blended by gDriveWet.
// This is NOT a drive/clipper — just level push before the processor.
struct CleanBoost {
  float boost_lin = 1.0f;
  void setupFrom_dB(float dB) {
    boost_lin = powf(10.0f, dB * 0.05f);
  }
  inline float process(float x) {
    return x * boost_lin;
  }
} gBoost;

// ========================== Coeffs/state ===============================
enum { MODE_FM3 = 0,
       MODE_FM5 = 1,
       MODE_AM = 2 };

struct Coeffs {
  int mode = MODE_FM3;
  int nbands = 3;

  float fc[4] = { 150.0f, 1600.0f, 5000.0f, 7000.0f };

  AP1 ap_lf;
  OnePoleLP lp[4];
  OnePoleHP hp_preemph;
  OnePoleLP lpf_dolby_ceiling;  // used as FM ceiling and AM LPF1
  OnePoleHP hp_dolby_split;     // (unused in AM)

  OnePoleHP hp_tight;  // global tight low (TS2)

  // AM mode extras
  OnePoleLP am_lpf2;  // second pole for 12 dB/oct ceiling
  float am_cut = kAM_Cut_Base_Hz;
  float am_asym = 0.25f;

  RMSDet det[5];
  Comp comp[5];

  RMSDet dolby_env;  // (unused in AM now, kept for parity)
  float dolby_thr = 0.25f;
  float dolby_boost_lin = 1.0f;
  bool dolby_decode = false;

  bool hf_saver = false;

  float preemph_amt = 0.0f;
  float drive = 1.0f;
  float mix = 1.0f;

  float atk_ms = 5.0f;
  float rel_ms = 150.0f;
} C;

// ========================== Bypass & LEDs ==============================
static volatile float gActiveWet = 1.0f;  // 1=processed weight, 0=dry
static volatile float gActiveTarget = 1.0f;
static bool gHardBypassed = false;

static volatile float gDriveWet = 0.0f;  // 1=boost fully on, 0=off
static volatile float gDriveTarget = 0.0f;

static bool gEffectOn = true;  // FS1
static bool gDriveOn = false;  // FS2 (boost)

// Toggles: TS1, TS2, TS4 are read inverted; **TS3 is read non-inverted**
static bool t_fast = false;      // TS1
static bool t_tight = false;     // TS2
static bool t_presence = false;  // TS3 (special-case inversion)
static bool t_ts4 = false;       // TS4

// LEDs are active-LOW on this hardware helper
static inline void LedWrite(int idx, bool wantOn) {
  hpcb.SetLED(idx, !wantOn);
}

// ========================== Debounce helpers ===========================
struct Debounce {
  bool stable = false, prev = false;
  bool rawPrev = false;
  uint32_t lastChange = 0;
  void update(bool raw, uint32_t nowMs, uint32_t debounceMs) {
    if (raw != rawPrev) {
      rawPrev = raw;
      lastChange = nowMs;
    }
    if ((uint32_t)(nowMs - lastChange) >= debounceMs) {
      prev = stable;
      stable = raw;
    }
  }
  bool rose() const {
    return (!prev && stable);
  }
};

static Debounce dbFS1, dbFS2;

static constexpr int FS_BYP_IDX = 1;    // panel FS1 = BYPASS
static constexpr int FS_BOOST_IDX = 0;  // panel FS2 = BOOST

// ========================== Update coefficients ========================
static void updateCoeffs() {
  // Select mode from snapped RV1
  float f = P.flavor;
  C.mode = (f < kFlavorZone01) ? MODE_FM3 : (f < kFlavorZone02 ? MODE_FM5 : MODE_AM);

  // Tune shifts band edges/cutoffs in octaves
  float tune_oct = (P.tune - 0.5f) * 1.6f;
  float factor = powf(2.0f, tune_oct);

  // Presence tilt (TS3): pivot frequency and strength
  float airPivotBase = t_presence ? kPresencePivot_Hi : kPresencePivot_Lo;
  float air_fc = airPivotBase * powf(2.0f, (P.harmonics - 0.5f) * 0.6f);
  C.hp_preemph.setFC(air_fc);
  C.preemph_amt = P.harmonics * (t_presence ? kPresenceAmt_OnMul : kPresenceAmt_OffMul);
  if (C.preemph_amt > kPresenceAmt_Max) C.preemph_amt = kPresenceAmt_Max;

  // Mix
  C.mix = P.mix;

  // Ratio & envelopes shaped by Amount and TS1
  float ratio = kRatio_Base + kRatio_PerAmount * P.amount;
  if (t_fast) ratio += kRatio_FastBonus;
  else ratio += kRatio_SlowPenalty;
  if (ratio < 1.05f) ratio = 1.05f;

  if (t_fast) {
    C.atk_ms = kFastAtk_ms;
    C.rel_ms = kFastRel_Min_ms + (kFastRel_Max_ms - kFastRel_Min_ms) * P.amount;
  } else {
    C.atk_ms = kSlowAtk_ms;
    C.rel_ms = kSlowRel_Min_ms + (kSlowRel_Max_ms - kSlowRel_Min_ms) * P.amount;
  }

  // Threshold and drive (gentler threshold range)
  float thr_dB = kThr_dB_Base + kThr_dB_PerAmount * P.amount;  // e.g., -3 .. -21 dB
  float thr_lin = powf(10.0f, thr_dB * 0.05f);
  C.drive = 1.0f + 7.0f * P.amount;  // overall post-comp drive

  // HF saver ceiling for FM
  C.lpf_dolby_ceiling.setFC(t_ts4 ? kCeiling_Hz_Tight : kCeiling_Hz_Wide);

  if (C.mode == MODE_FM3) {
    C.nbands = 3;
    float base1 = t_tight ? 450.0f : 150.0f;
    float base2 = 1600.0f;
    C.fc[0] = base1 * factor;
    C.fc[1] = base2 * factor;
    C.lp[0].setFC(C.fc[0]);
    C.lp[1].setFC(C.fc[1]);
    C.ap_lf.setFC(kAP_LF_Hz_FM);
    C.hf_saver = t_ts4;

  } else if (C.mode == MODE_FM5) {
    C.nbands = 5;
    float base[4] = { t_tight ? 320.0f : 120.0f, 500.0f, 1600.0f, 5000.0f };
    for (int i = 0; i < 4; ++i) {
      C.fc[i] = base[i] * factor;
      C.lp[i].setFC(C.fc[i]);
    }
    C.ap_lf.setFC(kAP_LF_Hz_FM);
    C.hf_saver = t_ts4;

  } else {
    // === AM Narrowband flavor ===
    C.nbands = 1;
    float base = kAM_Cut_Base_Hz * powf(2.0f, (P.tune - 0.5f));  // sweep with Tune
    base = t_ts4 ? (base * kAM_Cut_NarrowMul) : (base * kAM_Cut_WideMul);
    if (base < kAM_Cut_Min_Hz) base = kAM_Cut_Min_Hz;
    if (base > kAM_Cut_Max_Hz) base = kAM_Cut_Max_Hz;
    C.am_cut = base;
    C.lpf_dolby_ceiling.setFC(C.am_cut);
    C.am_lpf2.setFC(C.am_cut * 0.95f);  // 2nd pole slightly below

    // Asymmetry from Amount/Harmonics, boosted a touch when Narrow
    float asym = kAM_Asym_Min + (kAM_Asym_Max - kAM_Asym_Min) * (kAM_Asym_AmountMix * P.amount + kAM_Asym_HarmMix * P.harmonics);
    if (t_ts4) asym *= kAM_Asym_NarrowBoost;
    if (asym > kAM_Asym_Max) asym = kAM_Asym_Max;
    C.am_asym = asym;

    C.ap_lf.setFC(kAP_LF_Hz_AM);
  }

  // Global tight-HP for TS2 prominence
  C.hp_tight.setFC(t_tight ? kTightHP_On_Hz : kTightHP_Off_Hz);

  // Bake detector/comp params
  for (int i = 0; i < 5; ++i) {
    C.det[i].setTimes(C.atk_ms, C.rel_ms);
    C.comp[i].thr_lin = thr_lin;
    C.comp[i].ratio = ratio;
  }
}

// ========================== Band split helpers =========================
static inline void split3(float x, float &b0, float &b1, float &b2) {
  float L1 = C.lp[0].process(x);
  float L2 = C.lp[1].process(x);
  b0 = L1;
  b1 = L2 - L1;
  b2 = x - L2;
}
static inline void split5(float x, float &b0, float &b1, float &b2, float &b3, float &b4) {
  float L1 = C.lp[0].process(x), L2 = C.lp[1].process(x), L3 = C.lp[2].process(x), L4 = C.lp[3].process(x);
  b0 = L1;
  b1 = L2 - L1;
  b2 = L3 - L2;
  b3 = L4 - L3;
  b4 = x - L4;
}

// ========================== Shadow warm-up while bypassed ==============
static inline void ShadowWarmStates(float rawIn) {
  float x0e = rawIn + 1e-24f;

  // Follow the same pre path shape used when engaged: a smooth blend to BOOST
  float x_boost = gBoost.process(x0e);
  float x_in = (1.0f - gDriveWet) * x0e + gDriveWet * x_boost;

  x_in = C.hp_tight.process(x_in);
  float x = C.ap_lf.process(x_in);

  if (C.mode == MODE_FM3) {
    float b0, b1, b2;
    split3(x, b0, b1, b2);
    (void)C.det[0].process(b0);
    (void)C.det[1].process(b1);
    (void)C.det[2].process(b2);
    float y = b0 + b1 + b2;
    (void)C.hp_preemph.process(y);
    if (C.hf_saver) { (void)C.lpf_dolby_ceiling.process(y); }
  } else if (C.mode == MODE_FM5) {
    float b0, b1, b2, b3, b4;
    split5(x, b0, b1, b2, b3, b4);
    (void)C.det[0].process(b0);
    (void)C.det[1].process(b1);
    (void)C.det[2].process(b2);
    (void)C.det[3].process(b3);
    (void)C.det[4].process(b4);
    float y = b0 + b1 + b2 + b3 + b4;
    (void)C.hp_preemph.process(y);
    if (C.hf_saver) { (void)C.lpf_dolby_ceiling.process(y); }
  } else {
    float y = C.lpf_dolby_ceiling.process(x);
    (void)C.am_lpf2.process(y);
  }
}

// ========================== Asymmetric clipper (AM) ====================
static inline float asym_clip(float x, float drive, float asym) {
  float gpos = drive * (1.0f + asym);
  float gneg = drive * (1.0f - asym);
  float y;
  if (x >= 0.0f) {
    y = tanhf(x * gpos);
    y /= tanhf(gpos) + 1e-9f;
  } else {
    y = tanhf(x * gneg);
    y /= tanhf(gneg) + 1e-9f;
  }
  return y;
}

// ========================== Audio callback (mono) ======================
static void AudioCallback(float in, float &out) {
  // Smooth crossfades (click-free)
  gActiveWet += kBypassFadeAlpha * (gActiveTarget - gActiveWet);
  gDriveWet += kBoostFadeAlpha * (gDriveTarget - gDriveWet);

  if (gActiveTarget <= 0.0f && gActiveWet < 0.0005f) { gHardBypassed = true; }
  if (gActiveTarget >= 1.0f && gActiveWet > 0.9995f) { gHardBypassed = false; }

  const float rawIn = in;

  // Track internal states while bypassed to avoid re-engage thump
  if (gHardBypassed) {
    ShadowWarmStates(rawIn);
    out = rawIn;  // true bypass audio when fully bypassed
    return;
  }

  // Processed branch input with optional transparent BOOST
  float x0e = rawIn + 1e-24f;
  float x_boost = gBoost.process(x0e);
  // Blend from unity to boosted input as FS2 fades in/out
  float x_in = (1.0f - gDriveWet) * x0e + gDriveWet * x_boost;

  // TS2 Tight
  x_in = C.hp_tight.process(x_in);

  float x = C.ap_lf.process(x_in);
  float y = x_in;

  if (C.mode == MODE_FM3) {
    float b0, b1, b2;
    split3(x, b0, b1, b2);
    float e0 = C.det[0].process(b0), g0 = C.comp[0].gainFrom(e0) * (t_tight ? kLF_GainMul_FM3_Tight : 1.0f);
    float e1 = C.det[1].process(b1), g1 = C.comp[1].gainFrom(e1);
    float e2 = C.det[2].process(b2), g2 = C.comp[2].gainFrom(e2);
    y = b0 * g0 + b1 * g1 + b2 * g2;
    float high = C.hp_preemph.process(y);
    y = y + C.preemph_amt * high;
    if (C.hf_saver) { y = C.lpf_dolby_ceiling.process(y); }
    y = sat_tanh(y, C.drive);

  } else if (C.mode == MODE_FM5) {
    float b0, b1, b2, b3, b4;
    split5(x, b0, b1, b2, b3, b4);
    float e0 = C.det[0].process(b0), g0 = C.comp[0].gainFrom(e0) * (t_tight ? kLF_GainMul_FM5_Tight : 1.0f);
    float e1 = C.det[1].process(b1), g1 = C.comp[1].gainFrom(e1);
    float e2 = C.det[2].process(b2), g2 = C.comp[2].gainFrom(e2);
    float e3 = C.det[3].process(b3), g3 = C.comp[3].gainFrom(e3);
    float e4 = C.det[4].process(b4), g4 = C.comp[4].gainFrom(e4);
    y = b0 * g0 + b1 * g1 + b2 * g2 + b3 * g3 + b4 * g4;
    float high = C.hp_preemph.process(y);
    y = y + C.preemph_amt * high;
    if (C.hf_saver) { y = C.lpf_dolby_ceiling.process(y); }
    y = sat_tanh(y, C.drive);

  } else {
    // AM path: mild presence tilt → asym clip → 12 dB/oct ceiling
    float high = C.hp_preemph.process(x);
    y = x + (0.5f * C.preemph_amt) * high;
    float drive = 1.0f + kAM_DrivePerAmount * P.amount;
    y = asym_clip(y, drive, C.am_asym);
    y = C.lpf_dolby_ceiling.process(y);
    y = C.am_lpf2.process(y);
  }

  // Post-DSP wet/dry, then master on processed branch only
  float wet = (1.0f - C.mix) * x_in + C.mix * y;
  float wetLeveled = gMasterGain * wet;

  // Final crossfade vs clean input (clean path is always unscaled)
  out = gActiveWet * wetLeveled + (1.0f - gActiveWet) * rawIn;
}

// ========================== Setup / Loop ===============================
static inline float snapFlavor(float v01) {
  if (v01 < kFlavorZone01) return 0.5f * kFlavorZone01;                         // center of zone 0
  else if (v01 < kFlavorZone02) return 0.5f * (kFlavorZone01 + kFlavorZone02);  // center of zone 1
  else return 0.5f * (kFlavorZone02 + 1.0f);                                    // center of zone 2
}

// Default inverted reader (active-low hardware)
static inline bool ReadToggleFlipped(int idx) {
  return !hpcb.ReadToggle(idx);
}
// Special-case TS3: **NOT** inverted per request
static inline bool ReadToggleNormal(int idx) {
  return hpcb.ReadToggle(idx);
}

static void QuiesceUnusedPins() {
  for (int i = 2; i < 8; ++i) { hpcb.SetLED(i, /*OFF*/ true); }
}

void setup() {
  // Audio at 96 kHz / 4-sample block
  hpcb.Init(96000, 4);
  gSR = (float)hpcb.SampleRate();

  // Initial states
  gEffectOn = true;
  gDriveOn = false;
  gActiveWet = gActiveTarget = 1.0f;
  gDriveWet = gDriveTarget = 0.0f;
  gHardBypassed = false;

  // LEDs
  LedWrite(0, gEffectOn);
  LedWrite(1, (gEffectOn && gDriveOn));

  // Pot defaults
  P = { snapFlavor(0.0f), 0.55f, 0.5f, 0.55f, 1.0f };
  Pprev = P;

  // FS2 clean boost gain
  gBoost.setupFrom_dB(kBoost_dB);

  // Read and latch toggles:
  // TS1, TS2, TS4 use inverted read; TS3 uses normal read (inverted behavior vs others)
  t_fast = ReadToggleFlipped(0);     // TS1
  t_tight = ReadToggleFlipped(1);    // TS2
  t_presence = ReadToggleNormal(2);  // TS3 **inverted request**
  t_ts4 = ReadToggleFlipped(3);      // TS4

  QuiesceUnusedPins();

  updateCoeffs();
  hpcb.Idle();
  hpcb.StartAudio(AudioCallback);
}

void loop() {
  hpcb.Idle();

  const uint32_t nowMs = millis();
  const uint32_t DEB_MS = 25;

  // Debounce both footswitches
  Debounce &b1 = dbFS1;
  Debounce &b2 = dbFS2;
  b1.update(hpcb.FootswitchIsPressed(FS_BYP_IDX), nowMs, DEB_MS);
  b2.update(hpcb.FootswitchIsPressed(FS_BOOST_IDX), nowMs, DEB_MS);

  // BYPASS logic (always available)
  if (b1.rose()) {
    gEffectOn = !gEffectOn;
    gActiveTarget = gEffectOn ? 1.0f : 0.0f;
    // Force BOOST off when bypassing, and don't allow it to turn on while bypassed
    gDriveTarget = gEffectOn ? (gDriveOn ? 1.0f : 0.0f) : 0.0f;
  }

  // FS2 BOOST: only when engaged
  if (b2.rose() && gEffectOn) {
    gDriveOn = !gDriveOn;
    gDriveTarget = gDriveOn ? 1.0f : 0.0f;
  }

  // LEDs reflect current logical states; LED2 forced off when bypassed
  LedWrite(0, gEffectOn);
  LedWrite(1, (gEffectOn && gDriveOn));

  // Toggles: TS1, TS2, TS4 inverted; **TS3 normal**
  bool t1 = ReadToggleFlipped(0);
  bool t2 = ReadToggleFlipped(1);
  bool t3 = ReadToggleNormal(2);  // **inverted vs others**
  bool t4 = ReadToggleFlipped(3);
  bool togg_dirty = (t1 != t_fast) || (t2 != t_tight) || (t3 != t_presence) || (t4 != t_ts4);
  if (togg_dirty) {
    t_fast = t1;
    t_tight = t2;
    t_presence = t3;
    t_ts4 = t4;
  }

  // Pots with smoothing; update only on meaningful change
  bool dirty = togg_dirty;

  float f_raw = hpcb.ReadPotSmoothed(RV1, 25.0f);
  float a_raw = hpcb.ReadPotSmoothed(RV2, 25.0f);
  float t_raw = hpcb.ReadPotSmoothed(RV3, 25.0f);
  float h_raw = hpcb.ReadPotSmoothed(RV4, 25.0f);
  float m_raw = hpcb.ReadPotSmoothed(RV5, 25.0f);
  float master_raw = hpcb.ReadPotSmoothed(RV6, 25.0f);

  // Smooth master taper (0..1)^exp to true zero
  float x = master_raw;
  if (x < 0.0f) x = 0.0f;
  else if (x > 1.0f) x = 1.0f;
  gMasterGain = powf(x, kMasterPowerExp);

  // Quantize flavor to stable centers
  float f = snapFlavor(f_raw);

  if (fabsf(f - Pprev.flavor) > kEpsParam) {
    P.flavor = f;
    dirty = true;
  }
  if (fabsf(a_raw - Pprev.amount) > kEpsParam) {
    P.amount = a_raw;
    dirty = true;
  }
  if (fabsf(t_raw - Pprev.tune) > kEpsParam) {
    P.tune = t_raw;
    dirty = true;
  }
  if (fabsf(h_raw - Pprev.harmonics) > kEpsParam) {
    P.harmonics = h_raw;
    dirty = true;
  }
  if (fabsf(m_raw - Pprev.mix) > kEpsParam) {
    P.mix = m_raw;
    dirty = true;
  }

  if (dirty) {
    Pprev = P;
    updateCoeffs();
  }
}
