// HSP_PsychoEnhancer.ino — v1.3.1 (RV6 master on; true unity bypass via library)
// ------------------------------------------------------------------
// Modes (Flavor):
//   0 = Exciter        (HF isolate → rectified even + soft odd → air shelf)
//   1 = BBE Align      (SVF split → short per-band delays (L>M>H) → presence shelf)
//   2 = LF Rotator     (three 1st‑order all‑passes in LF → Amount is wet)
//
// Pots: RV1=Flavor, RV2=Amount, RV3=Tune, RV4=Harmonics, RV5=Mix, RV6=Master
// Rate: 48 kHz, Block: 48. Mono in/out; Right output is muted in the library.
// FS2 = bypass toggle, LED2 = effect active. (Active‑LOW handled in library.)
// ------------------------------------------------------------------

#include <HaroldPCB.h>
#include <math.h>

static HaroldPCB hpcb;
static const float kSR = 48000.0f;

// -------------------- User params (0..1 normalized) --------------------
struct Params {
  float Flavor = 0.00f;     // 0..1 → [0:Exciter, 1:BBE, 2:Rotator] (quantized)
  float Amount = 0.65f;     // overall intensity within each mode
  float Tune = 0.55f;       // per‑mode frequency focus
  float Harmonics = 0.50f;  // even↔odd / presence shelf
  float Mix = 0.50f;        // wet/dry
} P;

// -------------------- Small helpers --------------------
static inline float clamp01(float x) {
  return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}
static inline float lerp(float a, float b, float t) {
  return a + t * (b - a);
}
static inline float tanhish(float x) {
  const float x2 = x * x;
  return x * (27.f + x2) / (27.f + 9.f * x2);
}

// -------------------- Lightweight filters ----------------------------
struct OnePoleLP {
  float a = 0.f, z = 0.f;
  void setFC(float fc) {
    if (fc < 1.f) fc = 1.f;
    if (fc > 0.45f * kSR) fc = 0.45f * kSR;
    a = expf(-2.f * M_PI * fc / kSR);
  }
  float process(float x) {
    z = (1.f - a) * x + a * z;
    return z;
  }
};
struct OnePoleHP {
  OnePoleLP lp;
  void setFC(float fc) {
    lp.setFC(fc);
  }
  float process(float x) {
    return x - lp.process(x);
  }
};

struct HPCB_Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void setHighShelf(float fc, float dBgain) {
    if (fc < 200.f) fc = 200.f;
    if (fc > 0.45f * kSR) fc = 0.45f * kSR;
    const float A = powf(10.f, dBgain / 40.f), w0 = 2.f * M_PI * fc / kSR, cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw * 0.5f * sqrtf(2.f), Ap1 = A + 1.f, Am1 = A - 1.f, Ap1cw = Ap1 * cw;
    const float b0n = A * (Ap1 + Am1 * cw + 2.f * sqrtf(A) * alpha);
    const float b1n = -2.f * A * (Am1 + Ap1cw);
    const float b2n = A * (Ap1 + Am1 * cw - 2.f * sqrtf(A) * alpha);
    const float a0 = (Ap1 - Am1 * cw + 2.f * sqrtf(A) * alpha);
    const float a1n = 2.f * (Am1 - Ap1cw);
    const float a2n = (Ap1 - Am1 * cw - 2.f * sqrtf(A) * alpha);
    b0 = b0n / a0;
    b1 = b1n / a0;
    b2 = b2n / a0;
    a1 = a1n / a0;
    a2 = a2n / a0;
  }
};

struct AP1 {
  float a = 0, x1 = 0, y1 = 0;
  void setFc(float fc) {
    if (fc < 10.f) fc = 10.f;
    if (fc > 0.45f * kSR) fc = 0.45f * kSR;
    const float t = tanf(M_PI * fc / kSR);
    a = (1.f - t) / (1.f + t);
  }
  float process(float x) {
    const float y = -a * x + x1 + a * y1;
    x1 = x;
    y1 = y;
    return y;
  }
};

struct SVF {
  float g = 0, R = 1.f, ic1 = 0, ic2 = 0;
  void set(float fc, float Q) {
    if (fc < 20.f) fc = 20.f;
    if (fc > 0.45f * kSR) fc = 0.45f * kSR;
    if (Q < 0.25f) Q = 0.25f;
    g = tanf(M_PI * fc / kSR);
    R = 1.f / Q;
  }
  void process(float x, float &lp, float &bp, float &hp) {
    const float v1 = (x - ic2 - R * ic1) / (1.f + R * g + g * g);
    const float v2 = g * v1 + ic1;
    const float v3 = g * v2 + ic2;
    ic1 = v2 + g * v1;
    ic2 = v3 + g * v2;
    hp = v1;
    bp = v2;
    lp = v3;
  }
};

template<int MAX_SAMPS>
struct FracDelay {
  float buf[MAX_SAMPS];
  int w = 0;
  float d_samps = 0.f;
  FracDelay() {
    for (int i = 0; i < MAX_SAMPS; ++i) buf[i] = 0.f;
  }
  void setDelaySamples(float d) {
    if (d < 0) d = 0;
    if (d > (float)(MAX_SAMPS - 2)) d = (float)(MAX_SAMPS - 2);
    d_samps = d;
  }
  float process(float x) {
    buf[w] = x;
    float read = (float)w - d_samps;
    while (read < 0) read += (float)MAX_SAMPS;
    int i0 = (int)read, i1 = i0 + 1;
    if (i1 >= MAX_SAMPS) i1 = 0;
    const float t = read - (float)i0;
    const float y = buf[i0] + t * (buf[i1] - buf[i0]);
    if (++w >= MAX_SAMPS) w = 0;
    return y;
  }
};

// -------------------- Processor state ----------------------------------
OnePoleHP excHP;
OnePoleLP dcKill;
HPCB_Biquad hshelf;

AP1 ap1, ap2, ap3;

SVF bands;
FracDelay<2048> dLow, dMid, dHigh;

// -------------------- Params polling -----------------------------------
static void ReadControls() {
  P.Flavor = hpcb.ReadPot(RV1);
  P.Amount = hpcb.ReadPotSmoothed(RV2, 40.f);
  P.Tune = hpcb.ReadPotSmoothed(RV3, 40.f);
  P.Harmonics = hpcb.ReadPotSmoothed(RV4, 40.f);
  P.Mix = hpcb.ReadPotSmoothed(RV5, 40.f);
}

static void BindMaster() {
  Connect(hpcb, RV6).to_master().level(HPCB_Curve::Log10);
}

// -------------------- Modes --------------------------------------------
static inline float ProcessExciter(float x) {
  const float iso_fc = lerp(2000.f, 6000.f, clamp01(P.Tune));
  excHP.setFC(iso_fc);
  const float hf = excHP.process(x);
  const float even = fabsf(hf);
  const float odd = tanhish(2.0f * hf);
  const float harm = clamp01(P.Harmonics);
  float shaped = (1.f - harm) * even + harm * odd;
  shaped *= (0.5f + 1.5f * clamp01(P.Amount));  // 0.5x..2x
  const float shelf_fc = lerp(8000.f, 12000.f, clamp01(P.Tune));
  const float shelf_db = lerp(-3.f, 6.f, harm);
  hshelf.setHighShelf(shelf_fc, shelf_db);
  shaped = hshelf.process(shaped);
  dcKill.setFC(12.f);
  shaped = shaped - dcKill.process(shaped);
  return shaped;
}

static inline float ProcessBBE(float x) {
  const float split = lerp(250.f, 1200.f, clamp01(P.Tune));
  bands.set(split, 0.707f);
  float lp, bp, hp;
  bands.process(x, lp, bp, hp);

  const float amt = clamp01(P.Amount);
  dLow.setDelaySamples(lerp(16.f, 64.f, amt));  // ≈0.33–1.33 ms
  dMid.setDelaySamples(lerp(8.f, 24.f, amt));   // ≈0.17–0.50 ms
  dHigh.setDelaySamples(lerp(2.f, 8.f, amt));   // ≈0.04–0.17 ms

  const float L = dLow.process(lp);
  const float M = dMid.process(bp);
  const float H = dHigh.process(hp);

  const float shelf_fc = lerp(5000.f, 9000.f, clamp01(P.Tune));
  const float shelf_db = lerp(-2.f, 4.f, clamp01(P.Harmonics));
  hshelf.setHighShelf(shelf_fc, shelf_db);

  // Small make‑up to keep Mode 1 from sounding quieter than dry
  const float sum = (L + M + H) * 1.05f;
  return hshelf.process(sum);
}

static inline float ProcessRotator(float x) {
  const float fc = lerp(80.f, 250.f, clamp01(P.Tune));
  ap1.setFc(fc);
  ap2.setFc(fc);
  ap3.setFc(fc);
  float y = ap1.process(x);
  y = ap2.process(y);
  y = ap3.process(y);
  return lerp(x, y, clamp01(P.Amount));  // Amount = wet
}

// -------------------- Audio callback -----------------------------------
static void AudioCB(float in, float &out) {
  // Library handles true bypass now; we just produce the processed path.
  int mode = (P.Flavor < 0.3333f) ? 0 : (P.Flavor < 0.6667f ? 1 : 2);

  float wet = 0.f;
  if (mode == 0) wet = ProcessExciter(in);
  else if (mode == 1) wet = ProcessBBE(in);
  else wet = ProcessRotator(in);

  const float mix = clamp01(P.Mix);
  out = (1.f - mix) * in + mix * wet;
}

// -------------------- Arduino sketch -----------------------------------
void setup() {
  hpcb.Init(48000, 48);
  BindMaster();
  hpcb.StartAudio(AudioCB);

  // Start active; LED2 lit
  hpcb.SetBypassed(false);
  hpcb.SetLED(LED2, false);
  hpcb.SetLED(LED1, true);

  // Optional: slightly longer debounce if your switches chatter
  // HPCB_FootswitchTiming t; t.debounce_ms=12; t.longpress_ms=500; t.multiclick_gap_ms=300; hpcb.SetFootswitchTiming(t);
}

void loop() {
  hpcb.Idle();
  ReadControls();

  // FS2 edge → toggle bypass; LED2 mirrors “effect active”
  static bool fs2_prev = false;
  const bool fs2_now = hpcb.FootswitchIsPressed(FS2);
  if (fs2_now && !fs2_prev) {
    hpcb.SetBypassed(!hpcb.IsBypassed());
    hpcb.SetLED(LED2, hpcb.IsBypassed());
  }
  fs2_prev = fs2_now;
}
