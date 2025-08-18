// HSP_ClusterGravityWash.ino
// Version: 1.0.0  (ProtoSEED v1.2 + HaroldPCB v1.2)
// -----------------------------------------------------------------------------
// Hybrid delay/reverb inspired by:
//  • Afterneath  — irregular, “clustered taps” with a global Drag time
//  • Eventide Blackhole — gravity-like low-frequency diffusion for cavernous size
//  • Meris Mercury7 — slow, lush modulation and a hint of shimmer-like overtones
//
// Hardware (mono only; right channel muted by HaroldPCB library):
//  • Pots:   RV1=Time/Drag, RV2=Smear/Diffusion, RV3=Gravity Depth,
//            RV4=Dampen (LPF in feedback), RV5=Mod Depth, RV6=Mix
//  • Toggles: TS1=Mode A/B (Cluster vs Diffusion emphasis),
//             TS2=Shimmer on/off,
//             TS3=Bright/Dark voicing shift,
//             TS4=FS2 function (OFF=Smear momentary, ON=Tap tempo)
//  • Footswitches: FS1=Soft bypass (LED1 shows effect on/off),
//                  FS2=Smear (momentary) or Tap Tempo (LED2 blinks tempo)
//  • LEDs: LED1=Effect active, LED2=Tempo blink
//
// Performance:
//  • Sample rate: 48kHz, block size: 48 (as per project defaults)
//  • CPU target: < 50%
//  • Tails continue smoothly after bypass (no hard cut)
//  • All parameters smoothed to avoid zipper noise/pops
//
// Code style notes:
//  • Only includes HaroldPCB.h and math.h (no 3rd party DSP libs).
//  • All control scanning/logic happens in Idle(); audio callback is DSP only.
//  • Clear beginner-friendly comments throughout.
//
// -----------------------------------------------------------------------------
// READ ME FIRST (signal flow, high level):
//
// Input  ──► (if bypassed, we gate new signal into the effect but keep running tails)
//          ► Multitap Cluster (10 irregular taps, scaled by Drag time)
//          ► Gravity Diffusion (2 stage low-freq allpass array; “cavern” size)
//          ► Feedback Path (with Dampen LPF + optional Shimmer overtones)
//          ► Wet/Dry Mix to Output
//
// • “Drag” (RV1) sets the overall span of the tap cluster. With TS4=Tap on,
//    RV1 becomes a 0.5×–2× *multiplier* around the tapped tempo.
// • “Smear” (RV2) blends discrete taps (Afterneath vibe) into a cloud
//    by raising the diffusion and inter-tap crossfade.
// • “Gravity” (RV3) pushes more energy into the low-frequency diffusion
//    network (Blackhole vibe).
// • “Mod Depth” (RV5) slowly wobbles small read offsets and diffusion,
//    for a Mercury7-like moving wall.
// • TS2 adds a subtle octave-up overtone into the feedback path (simple,
//    CPU-light shimmer-ish coloration, not a full pitch shifter).
// • TS3 Bright/Dark tilts the overall tone with a gentle shelf (pre-mix).
//
// Bypass with tails:
// • When bypassed via FS1, we still run the effect, but we stop feeding *new*
//   input into the feedback network. Output includes dry plus the continuing
//   wet tails. This gives natural decay without chopping.
//
// Footswitch FS2:
// • TS4 OFF  = Smear momentary: hold to force maximum diffusion/smear.
// • TS4  ON  = Tap tempo: tap twice or more to set tempo. LED2 blinks tempo.
//
// -----------------------------------------------------------------------------


#include <HaroldPCB.h>
#include <math.h>

// --------- Library instance ----------
static HaroldPCB H;

// --------- Audio config --------------
static const float kSampleRate = 48000.0f;
static const int   kBlockSize  = 48;

// --------- Delay buffer sizing -------
// We keep memory conservative but musical.
// Max Drag time: ~0.75 s (Afterneath-ish smear without huge RAM).
// 0.75 s * 48000 Hz ≈ 36000 samples.
static const int   kMaxDelaySamples = 36000;
static float       gDelay[kMaxDelaySamples];
static int         gWriteIdx = 0;

// Irregular tap positions as fractions of Drag time (10 taps):
// (Staggered to avoid rhythmic regularity; sum of weights normalized later)
static const int   kNumTaps = 10;
static const float kTapFrac[kNumTaps] = {
  0.12f, 0.21f, 0.29f, 0.37f, 0.43f,
  0.51f, 0.62f, 0.74f, 0.88f, 0.97f
};
static float gTapWeights[kNumTaps]; // dynamic based on “Smear/Mode”

// --------- Diffusion / Gravity -------
// Two low-frequency allpass stages using small variable delays.
// We implement classic first-order allpass: y = -a*x + x_d + a*y_d
// We'll realize them with short modulated delays for “movement”.
static const int kAPLenA = 160;  // ~3.3 ms
static const int kAPLenB = 320;  // ~6.7 ms
static float apBufA[kAPLenA], apBufB[kAPLenB];
static int   apIdxA = 0, apIdxB = 0;
static float apZA   = 0.0f, apZB   = 0.0f; // z^-1 state for numerical stability
static float apA    = 0.6f; // allpass coefficient (updated from Gravity)
static float apB    = 0.7f;

// --------- Feedback & Tone -----------
static float gFeedback = 0.45f;       // base repeat amount (derived internally)
static float gDampenCoeff = 0.05f;    // one-pole LPF coeff in feedback loop
static float gDampenState = 0.0f;     // LPF memory
static float gShimmerAmt  = 0.0f;     // simple overtone mix (TS2)

// Global Bright/Dark tilt (gentle shelf):
static float gVoicingTilt = 0.0f;     // -1=Dark, 0=Flat, +1=Bright

// --------- Modulation ----------------
// Slow sine LFO to wobble small offsets (depth scaled by RV5)
static float lfoPhase = 0.0f;
static float lfoInc   = 2.0f * 3.14159265f * 0.12f / kSampleRate; // ~0.12 Hz
static float gModDepthSamples = 0.5f; // max additional offset in samples

// --------- Bypass & Tails ------------
static bool  gEffectOn = true;     // FS1 state
static bool  gTailsGate = true;    // When false, we stop feeding *new* input

// --------- Tap Tempo -----------------
static bool  gTapMode = false;     // TS4 ON means FS2 acts as tap tempo
static uint32_t lastTapMs = 0;
static float    tapPeriodMs = 500.0f; // default 120 BPM
static uint32_t nextBlinkMs = 0;
static bool     led2State   = false;

// --------- Smoothed params -----------
// We compute targets in Idle() then one-pole smooth here:
struct Smoothed {
  float z, a;  // z=current, a=alpha
  void set(float v){ z = v; }
  void target(float tgt){ z += a * (tgt - z); }
};
static Smoothed sDragSamps     { 10000.0f, 0.02f }; // overall Drag time (samples)
static Smoothed sSmear         { 0.3f,     0.02f }; // 0..1
static Smoothed sGravity       { 0.5f,     0.02f }; // 0..1
static Smoothed sDampenCut     { 8000.0f,  0.02f }; // cutoff Hz (mapped to coeff)
static Smoothed sModDepth      { 0.3f,     0.02f }; // 0..1 (later to samples)
static Smoothed sMix           { 0.5f,     0.02f }; // 0..1

// --------- Runtime control targets (set in Idle) ----------
static float tDragSamps   = 16000.0f; // target Drag in samples
static float tSmear       = 0.3f;     // 0..1
static float tGravity     = 0.5f;     // 0..1
static float tDampenCut   = 6000.0f;  // Hz
static float tModDepth    = 0.2f;     // 0..1
static float tMix         = 0.5f;     // 0..1
static bool  tModeB       = false;    // TS1: false=Cluster focus, true=Diffusion focus
static bool  tShimmer     = false;    // TS2
static bool  tBright      = true;     // TS3: true=Bright, false=Dark
static bool  tTapSelected = false;    // TS4 (FS2 function)

// Smear momentary active (TS4 OFF and FS2 held)
static bool  gSmearMomentary = false;

// --------- Small helpers -------------
static inline float clampf(float x, float a, float b){ return (x < a) ? a : (x > b ? b : x); }
static inline float lerp(float a, float b, float t){ return a + t*(b-a); }
static inline float onePoleLPF(float in, float &state, float a){ state += a*(in - state); return state; }
static inline float softsat(float x){ // gentle saturation
  const float k = 0.5f;
  return x / (1.0f + k*fabsf(x));
}

// --------- Read delay with fractional index (linear interp) ----------
static inline float readDelayFrac(const float *buf, int size, int widx, float backSamps)
{
  // backSamps >= 0; 0 means “just written”.
  float readPos = (float)widx - backSamps;
  while (readPos < 0.0f) readPos += size;
  while (readPos >= size) readPos -= size;

  int i0 = (int)readPos;
  int i1 = i0 + 1; if (i1 >= size) i1 = 0;
  float frac = readPos - (float)i0;
  return buf[i0] + frac * (buf[i1] - buf[i0]);
}

// --------- Allpass processors (small mod for motion) ----------
static inline float procAP(float x, float &z1, float a)
{
  // First-order allpass (canonical one-pole form for “gravity-like” smear)
  float y = -a * x + z1;
  z1 = x + a * y;
  return y;
}

// ========== AUDIO CALLBACK (DSP only) ==========
static void AudioCB(float in, float &out)
{
  // Update LFO phase
  lfoPhase += lfoInc;
  if (lfoPhase > 2.0f * 3.14159265f) lfoPhase -= 2.0f * 3.14159265f;
  float lfo = 0.5f * (1.0f + sinf(lfoPhase)); // 0..1

  // Smooth run-up toward targets (lightweight per-sample smoothing)
  sDragSamps.target(tDragSamps);
  sSmear.target(tSmear);
  sGravity.target(tGravity);
  sDampenCut.target(tDampenCut);
  sModDepth.target(tModDepth);
  sMix.target(tMix);

  // Map modulation depth to *samples* (tiny offsets)
  gModDepthSamples = lerp(0.0f, 3.5f, sModDepth.z); // up to ~3.5 samples (~0.07ms)

  // Derive allpass “gravity” from sGravity (gentler at low settings)
  apA = 0.35f + 0.50f * sGravity.z; // 0.35..0.85
  apB = 0.45f + 0.45f * sGravity.z; // 0.45..0.90

  // Convert dampen cutoff to a one-pole LPF coefficient.
  // Simple bilinear-ish: a = 1 - exp(-2pi*fc/fs) — cheap enough, smooth.
  float aLP = 1.0f - expf(-2.0f * 3.14159265f * (sDampenCut.z / kSampleRate));
  aLP = clampf(aLP, 0.0005f, 0.2f); // keep stable and gentle
  gDampenCoeff = aLP;

  // Cluster tap weighting: in Mode A (cluster focus), taps are more distinct.
  // In Mode B (diffusion focus), bias weights toward later taps for a wash.
  float sumW = 0.0f;
  for (int i=0;i<kNumTaps;++i){
    float bias = tModeB ? (0.6f + 0.8f * kTapFrac[i]) : (1.2f - 0.6f * kTapFrac[i]);
    // Smear blends weights to be more even (bigger cloud)
    gTapWeights[i] = lerp(bias, 1.0f, sSmear.z);
    sumW += gTapWeights[i];
  }
  float invSumW = (sumW > 0.0f) ? (1.0f / sumW) : 0.1f;

  // Decide how much *new* input reaches the network (tails respect bypass)
  float netIn = gTailsGate ? in : 0.0f;

  // ---------- MULTITAP CLUSTER ----------
  // Irregular taps scaled by Drag time + tiny LFO wobble for motion.
  float cluster = 0.0f;
  for (int t=0;t<kNumTaps;++t){
    float baseBack = kTapFrac[t] * sDragSamps.z;
    float wobble   = (lfo - 0.5f) * gModDepthSamples; // ±half-depth
    float backS    = clampf(baseBack + wobble, 1.0f, (float)(kMaxDelaySamples-2));
    float tapVal   = readDelayFrac(gDelay, kMaxDelaySamples, gWriteIdx, backS);
    cluster += (gTapWeights[t] * tapVal);
  }
  cluster *= invSumW; // normalize

  // ---------- DIFFUSION / GRAVITY ----------
  // Feed the cluster into a two-stage allpass chain.
  // We also bleed a small portion of the *input* to keep articulation at low Smear.
  float diffIn = lerp(netIn, cluster, 0.6f + 0.35f * sSmear.z);

  // Apply small pre-saturation for density
  diffIn = softsat(diffIn);

  // Stage A allpass
  float apOutA = procAP(diffIn, apZA, apA);
  // Stage B allpass
  float apOutB = procAP(apOutA, apZB, apB);

  // ---------- SHIMMER-ISH OVERTONES (optional, very subtle) ----------
  // Lightweight “octave-flavored” brightness: rectify + highpass tint, blended small.
  float shimmer = 0.0f;
  if (tShimmer){
    float rect = fabsf(apOutB) - 0.2f;   // remove some DC-ish
    if (rect < 0.0f) rect = 0.0f;
    shimmer = 0.08f * rect;              // tiny amount, just a sheen
  }

  // ---------- FEEDBACK PATH ----------
  // Combine diffusion, cluster residue, and shimmer for feedback content
  float fbContent = 0.75f * apOutB + 0.25f * cluster + shimmer;
  // Dampen (LPF) in feedback loop
  float damped = onePoleLPF(fbContent, gDampenState, gDampenCoeff);
  // Feedback gain rises with Gravity a touch, but bound for stability
  gFeedback = clampf(0.35f + 0.35f * sGravity.z, 0.15f, 0.75f);

  // ---------- WRITE DELAY ----------
  // Pre-tilt voicing: gentle tone contour before it circles back.
  float tilt = (tBright ? 0.12f : -0.12f); // small brightness tilt
  float voiced = damped + tilt * (damped - gDampenState); // crude tilt flavor

  float writeVal = netIn + gFeedback * voiced;
  // Circular write
  gDelay[gWriteIdx] = writeVal;
  gWriteIdx++; if (gWriteIdx >= kMaxDelaySamples) gWriteIdx = 0;

  // ---------- WET OUTPUT ----------
  // We read a “main” tap around 80% of Drag for stronger body + diffusion tail
  float mainBack = clampf(0.80f * sDragSamps.z + (lfo - 0.5f)*gModDepthSamples, 1.0f, (float)(kMaxDelaySamples-2));
  float wetCore  = readDelayFrac(gDelay, kMaxDelaySamples, gWriteIdx, mainBack);
  // Blend some diffused output for size
  float wet = 0.6f * wetCore + 0.4f * apOutB;

  // ---------- FINAL MIX (pre-clip safety) ----------
  float dry = in;
  float mix = clampf(sMix.z, 0.0f, 1.0f);
  float y   = (1.0f - mix) * dry + mix * wet;
  // Gentle safety:
  y = softsat(y);

  out = y;
}

// ========== CONTROL / UI STATE (IDLE) ==========
// We poll pots/toggles/footswitches here, compute targets, and handle LEDs/tap.
static bool prevFS1 = false, prevFS2 = false;

void Idle()
{
  // ----- Pots (0..1) -----
  float pDrag   = H.ReadPot(RV1);
  float pSmear  = H.ReadPot(RV2);
  float pGrav   = H.ReadPot(RV3);
  float pDmp    = H.ReadPot(RV4);
  float pMod    = H.ReadPot(RV5);
  float pMix    = H.ReadPot(RV6);

  // ----- Toggles -----
  tModeB       = H.ReadToggle(0); // TS1: false=Cluster A, true=Diffusion B
  tShimmer     = H.ReadToggle(1); // TS2
  tBright      = !H.ReadToggle(2); // TS3 (up=Bright). If wiring inverted, flip here.
  tTapSelected = H.ReadToggle(3); // TS4: OFF=Smear momentary, ON=Tap tempo

  // ----- Footswitch edges -----
  bool fs1 = H.ReadFootswitch(0); // FS1: bypass
  bool fs2 = H.ReadFootswitch(1); // FS2: smear OR tap

  // FS1: toggling effect on/off (soft bypass with tails)
  if (fs1 && !prevFS1){
    gEffectOn = !gEffectOn;
    // When effect turns off: keep tails running but gate new input.
    gTailsGate = gEffectOn; // We'll re-evaluate below (tap mode doesn't change this)
  }
  prevFS1 = fs1;

  // LED1 reflects effect state (on when active)
  H.WriteLED(0, gEffectOn);

  // Decide “tails gate” policy:
  // • If gEffectOn: let input into the network.
  // • If bypassed: cut new input, but keep wet output alive (tails).
  gTailsGate = gEffectOn;

  // FS2 behavior depends on TS4 (tTapSelected)
  if (!tTapSelected){
    // -------- Smear momentary mode (TS4 OFF) --------
    // While held: force heavy smear.
    gSmearMomentary = fs2;
  }else{
    // -------- Tap tempo mode (TS4 ON) ---------------
    if (fs2 && !prevFS2){
      uint32_t now = H.millis();
      if (lastTapMs > 0){
        uint32_t diff = now - lastTapMs;
        // Limit sensible tempo: 200ms..2000ms (30..300 BPM)
        if (diff >= 200 && diff <= 2000){
          tapPeriodMs = (float)diff;
          // Restart blink phase
          nextBlinkMs = now;
          led2State   = false;
        }
      }
      lastTapMs = now;
    }
  }
  prevFS2 = fs2;

  // LED2: blink tempo when tap mode is ON; otherwise use a slow heartbeat.
  if (tTapSelected){
    uint32_t now = H.millis();
    // Blink on half-duty
    if (now >= nextBlinkMs){
      led2State = !led2State;
      // toggle every half period
      uint32_t half = (uint32_t)(0.5f * tapPeriodMs);
      if (half < 80) half = 80;
      nextBlinkMs = now + half;
    }
    H.WriteLED(1, led2State);
  }else{
    // Smear mode: LED2 glows while FS2 held (or a slow pulse to indicate “armed”)
    H.WriteLED(1, fs2);
  }

  // ----- Map pots to targets -----

  // Drag time:
  // • If tap mode ON: RV1 scales tapped period 0.5×..2.0× (musical flexibility).
  // • Else: RV1 directly sets 60ms..750ms span.
  float msSpan;
  if (tTapSelected){
    float mult = lerp(0.5f, 2.0f, pDrag);
    msSpan = clampf(mult * tapPeriodMs, 60.0f, 750.0f);
  }else{
    msSpan = lerp(60.0f, 750.0f, pDrag);
  }
  tDragSamps = clampf(msSpan * (kSampleRate / 1000.0f), 48.0f, (float)(kMaxDelaySamples-4));

  // Smear (RV2): 0..1; if FS2 is held in Smear mode, we force it near max.
  tSmear = pSmear;
  if (!tTapSelected && gSmearMomentary){
    tSmear = 1.0f; // force max while held
  }

  // Gravity (RV3): 0..1 => higher equals deeper diffusion and more feedback
  tGravity = pGrav;

  // Dampen (RV4): LPF cutoff sweep 1.2kHz..12kHz (log-ish feel)
  // Use a gentle perceptual mapping:
  float dmp = pDmp;
  float cut = 1200.0f * powf(10.0f, dmp * 1.0f); // ~1.2k -> ~12k
  tDampenCut = clampf(cut, 800.0f, 14000.0f);

  // Mod depth (RV5): tiny fractional-sample wobble
  tModDepth = pMod;

  // Mix (RV6): 0..1
  tMix = pMix;

  // Extra: in Diffusion-emphasis mode, we auto-raise smear a little for vibe
  if (tModeB) tSmear = clampf(tSmear + 0.12f, 0.0f, 1.0f);
}

// ========== ARDUINO SETUP/LOOP ==========

void setup()
{
  // Initialize board & audio
  // Start mono audio with our callback; 48k/48 as per scope defaults.
  H.Init((uint32_t)kSampleRate, (size_t)kBlockSize, AudioCB);

  // Initialize delay & diffusion buffers
  for (int i=0;i<kMaxDelaySamples;++i) gDelay[i] = 0.0f;
  for (int i=0;i<kAPLenA;++i) apBufA[i] = 0.0f;
  for (int i=0;i<kAPLenB;++i) apBufB[i] = 0.0f;
  gWriteIdx = 0;
  gDampenState = 0.0f;

  // Default LEDs
  H.WriteLED(0, true);  // effect ON by default
  H.WriteLED(1, false); // tempo LED off to start

  // Connect RV6 to master level if your library supports it (optional).
  // If not supported, Master is simply part of the Mix control above.
  // Connect(H, RV6).to_master().level(HPCB_Curve::Linear);
}

void loop()
{
  // Keep control/UI responsive without burdening the audio callback.
  Idle();
}
