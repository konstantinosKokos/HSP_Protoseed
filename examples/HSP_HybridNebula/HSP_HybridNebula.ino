// HSP_HybridNebula.ino
// Version: 1.1.3  (48 kHz / block 48) — FS/LED mapping locked to library: FS1↔LED1 (bypass), FS2↔LED2 (smear/tap)

#include <HaroldPCB.h>
#include <math.h>

static HaroldPCB H;

// — LED/FS mapping: exactly as the library defines —
static const HPCB_Led kBypassLED = HPCB_Led::LED1;              // shows effect active
static const HPCB_Led kAuxLED = HPCB_Led::LED2;                 // tap/smear activity
static const HPCB_Footswitch kBypassFS = HPCB_Footswitch::FS1;  // toggles bypass
static const HPCB_Footswitch kAuxFS = HPCB_Footswitch::FS2;     // smear/tap

// — user-defined types —
struct LP1 {
  float z;
  float a;
};

// — constants —
static const float kSampleRate = 48000.0f;
static const int kDelayBufMax = 48000, kShimmerBufMax = 24000, kNumTaps = 10;
static const float kMinDragMs = 60.0f, kMaxDragMs = 800.0f, kFbMax = 0.88f, kSmearMax = 0.95f;
static const float kDampenMinHz = 400.0f, kDampenMaxHz = 6500.0f;
static const float kModRateMinHz = 0.05f, kModRateMaxHz = 0.35f, kModDepthMax = 0.25f;
static const float kGravityBase = 0.20f;
static const float kTempoMinMs = 150.0f, kTempoMaxMs = 1200.0f, kTapBlinkDuty = 0.10f;
static const float kTiltAmount = 0.12f, kTiltPivotHz = 2500.0f;

// — helpers —
inline float smooth_to(float c, float t, float a) {
  return c + a * (t - c);
}
inline void LP1_setHz(LP1 &f, float hz) {
  float x = (float)M_PI * 2.0f * hz / kSampleRate;
  if (x < 0) x = 0;
  if (x > 1) x = 1;
  f.a = x;
}
inline float LP1_proc(LP1 &f, float in) {
  f.z += f.a * (in - f.z);
  return f.z;
}
inline float read_delay_frac(const float *b, int n, float idx) {
  while (idx < 0) idx += n;
  while (idx >= n) idx -= n;
  int i0 = (int)idx, i1 = i0 + 1;
  if (i1 >= n) i1 = 0;
  float f = idx - (float)i0;
  return b[i0] + (b[i1] - b[i0]) * f;
}

// — buffers & indices —
static float delayBuf[kDelayBufMax];
static int wIdx = 0;
static float shimmerBuf[kShimmerBufMax];
static int wShimmer = 0;
static float shimmerRead = 0.0f;

// — params & targets —
static float drag_time_s = 0.3f, t_drag_time_s = 0.3f;
static float diffusion_amt = 0.4f, t_diffusion_amt = 0.4f;
static float gravity_depth = 0.5f, t_gravity_depth = 0.5f;
static float damp_hz = 6000.0f, t_damp_hz = 6000.0f;
static float mod_depth = 0.1f, t_mod_depth = 0.1f;
static float wet_mix = 0.5f, t_wet_mix = 0.5f;
static float feedback_amt = 0.55f, t_feedback_amt = 0.55f;

// — modes / flags —
static int mode_B = 0, shimmer_on = 0, bright_voice = 0, tap_mode = 0;

// — tap-tempo & LED2 —
static unsigned long lastTapMs = 0, lastBlinkUpdateMs = 0;
static float tappedPeriodS = 0.0f, blinkPhase = 0.0f;

// — LFO —
static float lfoPhase = 0.0f, lfoRateHz = 0.12f;

// — filters —
static LP1 dampLPF = { 0.f, 0.05f }, postTilt = { 0.f, 0.02f };

// — taps —
static const float kTapFrac[kNumTaps] = { 0.09f, 0.16f, 0.21f, 0.28f, 0.34f, 0.43f, 0.55f, 0.66f, 0.78f, 0.92f };

// — bypass with tails —
static bool effectOn = true;
static float bypassBlend = 1.0f, t_bypassBlend = 1.0f;

// — smear momentary —
static int smearHeld = 0;

// — edge latches —
static bool fsBy_prev = false, fsAux_prev = false;

// — shimmer helper —
inline float shimmer_process(float in, float modDepth) {
  shimmerBuf[wShimmer] = in;
  if (++wShimmer >= kShimmerBufMax) wShimmer = 0;
  const float baseInc = 1.0035f, wobble = 1.0f + 0.0020f * sinf(2.0f * (float)M_PI * lfoPhase);
  shimmerRead += baseInc * wobble;
  if (shimmerRead >= kShimmerBufMax) shimmerRead -= kShimmerBufMax;
  float s = read_delay_frac(shimmerBuf, kShimmerBufMax, shimmerRead);
  return s * (0.10f + 0.12f * modDepth);
}

// ==============================================================================
// CONTROL SCAN (NO DSP) — use library enums as-is
// ==============================================================================
void ScanControls() {
  // Toggles (enum → uint8_t; library handles polarity)
  mode_B = H.ReadToggle((uint8_t)HPCB_Toggle::TS1);
  shimmer_on = H.ReadToggle((uint8_t)HPCB_Toggle::TS2);
  bright_voice = H.ReadToggle((uint8_t)HPCB_Toggle::TS3);
  tap_mode = H.ReadToggle((uint8_t)HPCB_Toggle::TS4);

  // Footswitch states (debounced by H.Idle())
  const bool fsBy_now = H.FootswitchIsPressed((uint8_t)kBypassFS);  // FS1 per library
  const bool fsAux_now = H.FootswitchIsPressed((uint8_t)kAuxFS);    // FS2 per library
  const bool fsBy_rise = (fsBy_now && !fsBy_prev);
  const bool fsAux_rise = (fsAux_now && !fsAux_prev);

  // BYPASS latch on FS1
  if (fsBy_rise) effectOn = !effectOn;
  t_bypassBlend = effectOn ? 1.0f : 0.0f;
  H.SetLED((uint8_t)kBypassLED, effectOn ? 1 : 0);  // LED1 follows bypass

  // FS2: smear (momentary) OR tap-tempo with LED2 blink
  const unsigned long nowMs = millis();
  if (tap_mode == 0) {
    smearHeld = fsAux_now ? 1 : 0;
    H.SetLED((uint8_t)kAuxLED, fsAux_now ? 1 : 0);  // LED2 mirrors FS2 while in smear mode
    tappedPeriodS = 0.0f;
    blinkPhase = 0.0f;
    lastBlinkUpdateMs = nowMs;
    lastTapMs = 0;
  } else {
    smearHeld = 0;
    if (fsAux_rise) {
      if (lastTapMs == 0) lastTapMs = nowMs;
      else {
        unsigned long dt = nowMs - lastTapMs;
        lastTapMs = nowMs;
        float ms = (float)dt;
        if (ms > kTempoMinMs && ms < kTempoMaxMs) tappedPeriodS = ms * 0.001f;
      }
    }
    if (tappedPeriodS > 0.0f) {
      if (lastBlinkUpdateMs == 0) lastBlinkUpdateMs = nowMs;
      float step = (float)(nowMs - lastBlinkUpdateMs) / (tappedPeriodS * 1000.0f);
      lastBlinkUpdateMs = nowMs;
      blinkPhase += step;
      if (blinkPhase >= 1.0f) blinkPhase -= 1.0f;
      H.SetLED((uint8_t)kAuxLED, (blinkPhase < kTapBlinkDuty) ? 1 : 0);
    } else {
      H.SetLED((uint8_t)kAuxLED, 0);
    }
  }

  // Pots → targets
  float pTime = H.ReadPot(RV1);
  float pSmear = H.ReadPot(RV2);
  float pGravity = H.ReadPot(RV3);
  float pDampen = H.ReadPot(RV4);
  float pModDepth = H.ReadPot(RV5);
  float pMix = H.ReadPot(RV6);

  float potDragS = (kMinDragMs + pTime * (kMaxDragMs - kMinDragMs)) * 0.001f;
  if (tap_mode == 1 && tappedPeriodS > 0.0f) {
    float half = tappedPeriodS * 0.5f;
    float minS = fmaxf(tappedPeriodS - half, kMinDragMs * 0.001f);
    float maxS = fminf(tappedPeriodS + half, kMaxDragMs * 0.001f);
    t_drag_time_s = minS + pTime * (maxS - minS);
  } else t_drag_time_s = potDragS;

  t_diffusion_amt = pSmear;
  t_gravity_depth = pGravity;
  float dLog = pDampen * pDampen;
  t_damp_hz = kDampenMinHz + dLog * (kDampenMaxHz - kDampenMinHz);
  t_mod_depth = pModDepth;
  lfoRateHz = kModRateMinHz + pModDepth * (kModRateMaxHz - kModRateMinHz);
  t_wet_mix = pMix;

  float fbBase = (mode_B ? 0.62f : 0.54f) + 0.25f * t_gravity_depth;
  if (fbBase > kFbMax) fbBase = kFbMax;
  t_feedback_amt = fbBase;

  // edge latches
  fsBy_prev = fsBy_now;
  fsAux_prev = fsAux_now;
}

// ==============================================================================
// AUDIO CALLBACK (DSP ONLY)
// ==============================================================================
void AudioCallback(float in, float &out) {
  drag_time_s = smooth_to(drag_time_s, t_drag_time_s, 0.0025f);
  diffusion_amt = smooth_to(diffusion_amt, t_diffusion_amt, 0.0030f);
  gravity_depth = smooth_to(gravity_depth, t_gravity_depth, 0.0020f);
  damp_hz = smooth_to(damp_hz, t_damp_hz, 0.0040f);
  mod_depth = smooth_to(mod_depth, t_mod_depth, 0.0030f);
  wet_mix = smooth_to(wet_mix, t_wet_mix, 0.0040f);
  feedback_amt = smooth_to(feedback_amt, t_feedback_amt, 0.0040f);
  bypassBlend = smooth_to(bypassBlend, t_bypassBlend, 0.020f);

  LP1_setHz(dampLPF, damp_hz);
  LP1_setHz(postTilt, kTiltPivotHz);

  lfoPhase += lfoRateHz / kSampleRate;
  if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
  float lfo = sinf(2.0f * (float)M_PI * lfoPhase);

  float dragSamps = drag_time_s * kSampleRate;
  if (dragSamps < 1.0f) dragSamps = 1.0f;
  if (dragSamps > (float)(kDelayBufMax - 4)) dragSamps = (float)(kDelayBufMax - 4);

  float cluster = 0.0f, tapGain = 1.0f / (float)kNumTaps, tapEmph = mode_B ? 0.75f : 1.0f;
  for (int i = 0; i < kNumTaps; ++i) {
    float rd = (float)wIdx - kTapFrac[i] * dragSamps;
    cluster += tapGain * read_delay_frac(delayBuf, kDelayBufMax, rd);
  }
  cluster *= tapEmph;

  static float ap_z1 = 0.0f, ap_z2 = 0.0f;
  float g = kGravityBase + 0.75f * gravity_depth;
  float gMod = g * (1.0f + (mod_depth * kModDepthMax) * lfo);
  if (gMod > 0.98f) gMod = 0.98f;

  float x1 = cluster;
  float y1 = -gMod * x1 + ap_z1;
  ap_z1 = x1 + gMod * y1;
  float x2 = y1;
  float y2 = -gMod * x2 + ap_z2;
  ap_z2 = x2 + gMod * y2;

  float smearNow = (smearHeld ? kSmearMax : diffusion_amt);
  float diffused = (1.0f - smearNow) * cluster + smearNow * y2;

  static float fbState = 0.0f;
  float inPlusFb = in + fbState;
  float toWrite = 0.70f * inPlusFb + 0.30f * diffused;
  float damped = LP1_proc(dampLPF, toWrite);

  if (shimmer_on) {
    float sh = shimmer_process(damped, mod_depth);
    damped += 0.22f * sh;
  }

  delayBuf[wIdx] = damped;

  float wet = diffused, fbNext = wet;
  if (bright_voice) {
    float low = LP1_proc(postTilt, fbNext);
    float air = fbNext - low;
    fbNext = fbNext + kTiltAmount * air;
  } else {
    fbNext = LP1_proc(postTilt, fbNext);
  }
  fbState = feedback_amt * fbNext;

  if (++wIdx >= kDelayBufMax) wIdx = 0;

  float userWet = wet_mix * bypassBlend, userDry = 1.0f - userWet;
  out = userDry * in + userWet * wet;
}

// ==============================================================================
// SETUP / LOOP
// ==============================================================================
void setup() {
  H.Init();
  H.StartAudio(AudioCallback);

  for (int i = 0; i < kDelayBufMax; ++i) delayBuf[i] = 0.0f;
  for (int i = 0; i < kShimmerBufMax; ++i) shimmerBuf[i] = 0.0f;
  wIdx = 0;
  wShimmer = 0;
  shimmerRead = 0.0f;

  LP1_setHz(postTilt, kTiltPivotHz);

  effectOn = true;
  t_bypassBlend = 1.0f;
  bypassBlend = 1.0f;
  H.SetLED((uint8_t)kBypassLED, 1);  // LED1 on at boot
  H.SetLED((uint8_t)kAuxLED, 0);
}

void loop() {
  H.Idle();        // required for debouncing & event updates
  ScanControls();  // consume debounced states and update LEDs/targets
}
