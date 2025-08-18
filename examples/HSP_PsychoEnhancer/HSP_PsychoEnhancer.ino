// HSP_OptimodBox_v3_10_96k_bs4.ino
// Broadcast-style loudness box tuned for ≤8 kHz front-end LPF
// Modes (RV1 Flavor): 0=FM 3-band, 1=FM 5-band, 2=Dolby-ish Sparkle (dynamic high-shelf)
// Pots:  RV1=Flavor, RV2=Amount, RV3=Tune, RV4=Harmonics(Air/Bias), RV5=Mix, RV6=Master (post, PROCESSED PATH ONLY)
// FS1/LED1: Global SOFT BYPASS (clean dry when bypassed). LED1 = effect active.
// FS2/LED2: OCD-style PRE-DRIVE toggle before the processor. LED2 = pre-drive ON, but forced OFF when bypassed.
// TS1: Comp Release (Fast/Slow) — pronounced
// TS2: Tight Low (higher low split + stronger LF control) — pronounced
// TS3: Presence Tilt (higher “air” pivot + stronger pre-emphasis) — pronounced
// TS4: Dolby helper (Dolby=Encode/Decode, FM=HF Saver ceiling)
// NOTE: Toggles are read inverted (hardware flipped). LEDs are active-LOW.
// NOTE: Running audio at 96 kHz, 4-sample block.
// CHANGE: Master (RV6) now scales ONLY the processed branch; clean-bypass level is untouched.

#include <HaroldPCB.h>
#include <math.h>

HaroldPCB hpcb;
static float gSR = 96000.0f;

// ========================== Parameters (0..1) ==========================
struct Params {
  float flavor;     // snapped centers
  float amount;     // intensity
  float tune;       // crossover shift
  float harmonics;  // air / bias
  float mix;        // wet/dry
} P, Pprev;

// Track RV6 master position (0..1) for processed-branch gain
static volatile float gMasterPos = 1.0f;

// ========================== Minimal DSP helpers ========================
// One-pole LP: y += a*(x - y)
struct OnePoleLP {
  float a=0.0f, y=0.0f;
  void setFC(float fc){ if(fc<5.0f) fc=5.0f; a = 1.0f - expf(-2.0f*3.14159265359f*fc/gSR); }
  inline float process(float x){ y += a*(x - y); return y; }
};

// HP via LP complement
struct OnePoleHP {
  OnePoleLP lp;
  void setFC(float fc){ lp.setFC(fc); }
  inline float process(float x){ return x - lp.process(x); }
};

// First-order all-pass (phase rotator)
struct AP1 {
  float a=0.0f, x1=0.0f, y1=0.0f;
  void setFC(float fc){ float w = tanf(3.14159265359f*fc/gSR); a = (1.0f - w)/(1.0f + w); }
  inline float process(float x){ float y = -a*x + x1 + a*y1; x1 = x; y1 = y; return y; }
};

// RMS detector (feed-forward), attack/release in ms
struct RMSDet {
  float atk_a=0.0f, rel_a=0.0f, e2=1e-12f;
  void setTimes(float atk_ms, float rel_ms){
    float dt = 1000.0f/gSR;
    float atk_tc = atk_ms<=0? dt : atk_ms;
    float rel_tc = rel_ms<=0? dt : rel_ms;
    atk_a = 1.0f - expf(-dt/atk_tc);
    rel_a = 1.0f - expf(-dt/rel_tc);
  }
  inline float process(float x){
    float s2 = x*x;
    float a  = (s2 > e2) ? atk_a : rel_a;
    e2 += a*(s2 - e2);
    return sqrtf(e2 + 1e-20f);
  }
};

// Simple feed-forward compressor gain
struct Comp {
  float thr_lin=0.5f; // ~ -6 dB
  float ratio=2.0f;
  inline float gainFrom(float env){
    if(env <= thr_lin) return 1.0f;
    float p = (ratio - 1.0f)/ratio;
    return powf(thr_lin/(env + 1e-20f), p);
  }
};

// Soft clip normalizer
static inline float sat_tanh(float x, float drive){
  float d = fmaxf(0.5f, drive);
  float y = tanhf(x*d);
  float n = tanhf(d);
  return y / (n>1e-6f?n:1.0f);
}

// ========================== Pre-drive (OCD-ish) ========================
struct PreDrive {
  OnePoleHP hpf_in;       // tighten lows before clip
  OnePoleHP hp_tilt;      // mild presence tilt pre clip
  OnePoleLP lpf_out;      // smooth top end after clip
  float gain = 5.0f;      // ~14 dB
  float tilt_amt = 0.25f; // 0..1 weight of presence tilt
  void setupModerate(){
    hpf_in.setFC(100.0f);
    hp_tilt.setFC(720.0f);
    lpf_out.setFC(6000.0f); // respect ≤8k front-end
    gain = 5.0f;
    tilt_amt = 0.25f;
  }
  inline float process(float x){
    float t = x;
    t = hpf_in.process(t);
    t = t + tilt_amt * hp_tilt.process(t);
    t *= gain;
    t = tanhf(t * 1.8f);
    if(t > 0.97f) t = 0.97f;
    if(t < -0.97f) t = -0.97f;
    t = lpf_out.process(t);
    return t;
  }
} gPre;

// ========================== Coeffs/state ===============================
enum { MODE_FM3=0, MODE_FM5=1, MODE_DOLBY=2 };

struct Coeffs {
  int   mode = MODE_FM3;
  int   nbands = 3;

  // Crossovers kept ≤8 kHz
  float fc[4] = {150.0f, 1600.0f, 5000.0f, 7000.0f};

  // Filters
  AP1       ap_lf;              // LF phase rotator
  OnePoleLP lp[4];              // LP taps
  OnePoleHP hp_preemph;         // “Air” core (~1.8–3.5 kHz when TS3 engaged)
  OnePoleLP lpf_dolby_ceiling;  // Dolby/FM ceiling helper
  OnePoleHP hp_dolby_split;     // Dolby split HP

  // Per-band dynamics
  RMSDet det[5];
  Comp   comp[5];

  // Dolby dynamics
  RMSDet dolby_env;
  float  dolby_thr = 0.25f;
  float  dolby_boost_lin = 1.0f;
  bool   dolby_decode = false;  // TS4 in Dolby flavor

  // FM helper
  bool   hf_saver = false;      // TS4 in FM flavors

  // Tone / clip
  float preemph_amt = 0.0f; // 0..2.0
  float drive = 1.0f;       // into clip
  float mix = 1.0f;         // wet/dry

  // Dynamics timing
  float atk_ms = 5.0f;
  float rel_ms = 150.0f;
} C;

// ========================== Bypass & LEDs ==============================
// Global soft-bypass crossfade (FS1)
static volatile float gActiveWet    = 1.0f;  // 1=processed path, 0=dry
static volatile float gActiveTarget = 1.0f;
// Pre-drive crossfade (FS2)
static volatile float gDriveWet     = 0.0f;  // 1=with pre-drive mixed in
static volatile float gDriveTarget  = 0.0f;
// Logical states
static bool gEffectOn = true;  // FS1 state (true=active)
static bool gDriveOn  = false; // FS2 state

// Toggles (read inverted)
static bool t_fast = false;     // TS1
static bool t_tight = false;    // TS2
static bool t_presence = false; // TS3
static bool t_ts4 = false;      // TS4

// LEDs are active-LOW on this build. Library SetLED(x, state) expects 'true' to be LED OFF here.
static inline void LedWrite(int idx, bool wantOn){ hpcb.SetLED(idx, !wantOn); }

// ========================== Debounce helpers ===========================
struct Debounce {
  bool stable=false, prev=false;
  bool rawPrev=false;
  uint32_t lastChange=0;
  void update(bool raw, uint32_t nowMs, uint32_t debounceMs){
    if(raw != rawPrev){ rawPrev = raw; lastChange = nowMs; }
    if((uint32_t)(nowMs - lastChange) >= debounceMs){
      prev = stable;
      stable = raw;
    }
  }
  bool rose() const { return (!prev && stable); }
};

static Debounce dbFS1, dbFS2;

// Footswitch index mapping (swap to match panel labels)
static constexpr int FS_BYP_IDX  = 1; // panel FS1 = BYPASS
static constexpr int FS_OCD_IDX  = 0; // panel FS2 = OCD

// ========================== Update coefficients ========================
static void updateCoeffs(){
  float f = P.flavor;
  C.mode = (f<1.0f/3.0f) ? MODE_FM3 : (f<2.0f/3.0f ? MODE_FM5 : MODE_DOLBY);

  float tune_oct = (P.tune - 0.5f) * 1.6f; // -0.8..+0.8
  float factor   = powf(2.0f, tune_oct);

  float airPivotBase = t_presence ? 2800.0f : 1800.0f;
  float air_fc = airPivotBase * powf(2.0f, (P.harmonics-0.5f)*0.6f);
  C.hp_preemph.setFC(air_fc);
  C.preemph_amt = P.harmonics * (t_presence ? 2.0f : 1.0f);
  if(C.preemph_amt > 2.0f) C.preemph_amt = 2.0f;

  C.mix = P.mix;
  if(t_fast){ C.atk_ms = 2.0f;  C.rel_ms = 30.0f + 90.0f*P.amount; }
  else      { C.atk_ms = 10.0f; C.rel_ms = 250.0f + 450.0f*P.amount; }

  float thr_dB   = -6.0f  - 24.0f*P.amount;
  float thr_lin  = powf(10.0f, thr_dB*0.05f);
  float ratio    = 1.3f   + 5.0f*P.amount;
  C.drive        = 1.0f   + 7.0f*P.amount;

  C.lpf_dolby_ceiling.setFC(7400.0f);

  if(C.mode==MODE_FM3){
    C.nbands = 3;
    float base1 = t_tight ? 300.0f : 150.0f;
    float base2 = 1600.0f;
    C.fc[0]=base1*factor; C.fc[1]=base2*factor;
    C.lp[0].setFC(C.fc[0]); C.lp[1].setFC(C.fc[1]);
    C.ap_lf.setFC(120.0f);
    C.hf_saver = t_ts4;

  } else if(C.mode==MODE_FM5){
    C.nbands = 5;
    float base[4] = { t_tight?240.0f:120.0f, 500.0f, 1600.0f, 5000.0f };
    for(int i=0;i<4;++i){ C.fc[i]=base[i]*factor; C.lp[i].setFC(C.fc[i]); }
    C.ap_lf.setFC(120.0f);
    C.hf_saver = t_ts4;

  } else {
    C.nbands = 1;
    float split = 1800.0f * powf(2.0f, (P.tune-0.5f));
    C.hp_dolby_split.setFC(split);
    C.ap_lf.setFC(90.0f);
    C.dolby_env.setTimes(3.0f, 80.0f);
    float thr_dB_d = -12.0f + (-18.0f * P.amount);
    C.dolby_thr = powf(10.0f, thr_dB_d*0.05f);
    float boost_dB = 0.0f + 12.0f * P.harmonics;
    C.dolby_boost_lin = powf(10.0f, boost_dB*0.05f);
    C.dolby_decode = t_ts4;
  }

  for(int i=0;i<5;++i){
    C.det[i].setTimes(C.atk_ms, C.rel_ms);
    C.comp[i].thr_lin = thr_lin;
    C.comp[i].ratio   = ratio;
  }
}

// ========================== Band split helpers =========================
static inline void split3(float x, float &b0, float &b1, float &b2){
  float L1 = C.lp[0].process(x);
  float L2 = C.lp[1].process(x);
  b0 = L1; b1 = L2 - L1; b2 = x - L2;
}
static inline void split5(float x, float &b0, float &b1, float &b2, float &b3, float &b4){
  float L1=C.lp[0].process(x), L2=C.lp[1].process(x), L3=C.lp[2].process(x), L4=C.lp[3].process(x);
  b0=L1; b1=L2-L1; b2=L3-L2; b3=L4-L3; b4=x-L4;
}

// ========================== Audio callback (mono) ======================
static void AudioCallback(float in, float &out){
  const float a_bypass = 0.0025f;
  const float a_drive  = 0.0040f;
  gActiveWet += a_bypass * (gActiveTarget - gActiveWet);
  gDriveWet  += a_drive  * (gDriveTarget  - gDriveWet);

  float x0    = in + 1e-24f;
  float x_pre = gPre.process(x0);
  float x_in  = (1.0f - gDriveWet)*x0 + gDriveWet*x_pre;

  float x = C.ap_lf.process(x_in);
  float y = x_in;

  if(C.mode==MODE_FM3){
    float b0,b1,b2; split3(x, b0,b1,b2);
    float e0=C.det[0].process(b0), g0=C.comp[0].gainFrom(e0) * (t_tight?0.7f:1.0f);
    float e1=C.det[1].process(b1), g1=C.comp[1].gainFrom(e1);
    float e2=C.det[2].process(b2), g2=C.comp[2].gainFrom(e2);
    y = b0*g0 + b1*g1 + b2*g2;
    float high = C.hp_preemph.process(y);
    y = y + C.preemph_amt * high;
    if(C.hf_saver){ y = C.lpf_dolby_ceiling.process(y); }
    y = sat_tanh(y, C.drive);

  } else if(C.mode==MODE_FM5){
    float b0,b1,b2,b3,b4; split5(x, b0,b1,b2,b3,b4);
    float e0=C.det[0].process(b0), g0=C.comp[0].gainFrom(e0) * (t_tight?0.75f:1.0f);
    float e1=C.det[1].process(b1), g1=C.comp[1].gainFrom(e1);
    float e2=C.det[2].process(b2), g2=C.comp[2].gainFrom(e2);
    float e3=C.det[3].process(b3), g3=C.comp[3].gainFrom(e3);
    float e4=C.det[4].process(b4), g4=C.comp[4].gainFrom(e4);
    y = b0*g0 + b1*g1 + b2*g2 + b3*g3 + b4*g4;
    float high = C.hp_preemph.process(y);
    y = y + C.preemph_amt * high;
    if(C.hf_saver){ y = C.lpf_dolby_ceiling.process(y); }
    y = sat_tanh(y, C.drive);

  } else {
    float hi   = C.hp_dolby_split.process(x);
    hi         = C.lpf_dolby_ceiling.process(hi);
    float env  = C.dolby_env.process(fabsf(hi));
    float t    = 0.0f;
    if(!C.dolby_decode){
      if(env < C.dolby_thr){ t = 1.0f - (env / (C.dolby_thr + 1e-9f)); t = t*t*(3.0f - 2.0f*t); }
      float ghs  = 1.0f + (C.dolby_boost_lin - 1.0f)*t;
      float lo   = x - hi;
      y = lo + hi * ghs;
    }else{
      if(env > C.dolby_thr){ t = (env - C.dolby_thr) / (fmaxf(env, C.dolby_thr) + 1e-9f); t = t*t*(3.0f - 2.0f*t); }
      float ghs  = 1.0f - (1.0f - 1.0f/C.dolby_boost_lin)*t;
      float lo   = x - hi;
      y = lo + hi * ghs;
    }
    y = sat_tanh(y, 1.0f + 5.0f*P.amount);
  }

  // Post-DSP wet/dry (pre "master")
  float wet = (1.0f - C.mix)*x_in + C.mix*y;

  // Master (RV6) applies ONLY to processed branch; logarithmic-ish feel:
  // 0..1 -> ~(-40 dB .. 0 dB). Hard mute near floor.
  float procGain = (gMasterPos < 0.01f) ? 0.0f : powf(10.0f, 2.0f*(gMasterPos - 1.0f)); // 0.01..1, or 0 when <1%
  float wetLeveled = procGain * wet;

  // FS1 clean-bypass output (unaffected by RV6)
  out = gActiveWet * wetLeveled + (1.0f - gActiveWet) * x0;

  // Safety
  out = tanhf(out * 1.2f);
}

// ========================== Setup / Loop ===============================
static inline float snapFlavor(float v01){
  if(v01 < 1.0f/3.0f) return 1.0f/6.0f;
  else if(v01 < 2.0f/3.0f) return 0.5f;
  else return 5.0f/6.0f;
}

static inline bool ReadToggleFlipped(int idx){ return !hpcb.ReadToggle(idx); }

// Best-effort quiesce: ensure any extra LED lines driven to OFF (active-low => drive high).
static void QuiesceUnusedPins(){
  for(int i=2;i<8;++i){ hpcb.SetLED(i, /*OFF*/ true); } // keep any unused LEDs off
}

void setup(){
  hpcb.Init(96000, 4);                 // 96 kHz / 4-sample block
  gSR = (float)hpcb.SampleRate();

  // NOTE: We no longer bind RV6 to the library "master" so bypass stays true-clean.
  // RV6 is read manually and applied only to the processed branch (see loop/ISR).

  gEffectOn = true; gDriveOn = false;
  gActiveWet = gActiveTarget = 1.0f;
  gDriveWet  = gDriveTarget  = 0.0f;

  LedWrite(0, gEffectOn);
  LedWrite(1, (gEffectOn && gDriveOn));

  P = { snapFlavor(0.0f), 0.5f, 0.5f, 0.5f, 1.0f };
  Pprev = P;

  gPre.setupModerate();

  t_fast     = ReadToggleFlipped(0);
  t_tight    = ReadToggleFlipped(1);
  t_presence = ReadToggleFlipped(2);
  t_ts4      = ReadToggleFlipped(3);

  // Drive any known unused outputs to a benign (high/off) state.
  QuiesceUnusedPins();

  updateCoeffs();
  hpcb.Idle();
  hpcb.StartAudio(AudioCallback);
}

void loop(){
  hpcb.Idle();

  const uint32_t nowMs = millis();
  const uint32_t DEB_MS = 25;

  // Panel mapping preserved: FS1=bypass, FS2=OCD
  dbFS1.update(hpcb.FootswitchIsPressed(FS_BYP_IDX), nowMs, DEB_MS);
  dbFS2.update(hpcb.FootswitchIsPressed(FS_OCD_IDX), nowMs, DEB_MS);

  if(dbFS1.rose()){
    gEffectOn = !gEffectOn;
    gActiveTarget = gEffectOn ? 1.0f : 0.0f;
  }
  if(dbFS2.rose()){
    gDriveOn = !gDriveOn;
    gDriveTarget = gDriveOn ? 1.0f : 0.0f;
  }

  // LEDs
  LedWrite(0, gEffectOn);
  LedWrite(1, (gEffectOn && gDriveOn));

  // Toggles (inverted)
  bool t1 = ReadToggleFlipped(0);
  bool t2 = ReadToggleFlipped(1);
  bool t3 = ReadToggleFlipped(2);
  bool t4 = ReadToggleFlipped(3);
  bool togg_dirty = (t1!=t_fast) || (t2!=t_tight) || (t3!=t_presence) || (t4!=t_ts4);
  if(togg_dirty){ t_fast=t1; t_tight=t2; t_presence=t3; t_ts4=t4; }

  // Pots with smoothing; update only on meaningful change
  bool dirty = togg_dirty;

  float f_raw = hpcb.ReadPotSmoothed(RV1, 25.0f);
  float a_raw = hpcb.ReadPotSmoothed(RV2, 25.0f);
  float t_raw = hpcb.ReadPotSmoothed(RV3, 25.0f);
  float h_raw = hpcb.ReadPotSmoothed(RV4, 25.0f);
  float m_raw = hpcb.ReadPotSmoothed(RV5, 25.0f);
  float master_raw = hpcb.ReadPotSmoothed(RV6, 25.0f);

  // Track master knob for processed-branch gain (0..1)
  gMasterPos = master_raw;

  float f = snapFlavor(f_raw);

  if(fabsf(f       - Pprev.flavor)    > 0.01f){ P.flavor    = f;       dirty = true; }
  if(fabsf(a_raw   - Pprev.amount)    > 0.01f){ P.amount    = a_raw;   dirty = true; }
  if(fabsf(t_raw   - Pprev.tune)      > 0.01f){ P.tune      = t_raw;   dirty = true; }
  if(fabsf(h_raw   - Pprev.harmonics) > 0.01f){ P.harmonics = h_raw;   dirty = true; }
  if(fabsf(m_raw   - Pprev.mix)       > 0.01f){ P.mix       = m_raw;   dirty = true; }

  if(dirty){
    Pprev = P;
    updateCoeffs();
  }
}
