// HSP_BasicTremolo.ino — v1.0.2 (textbook, with “how” comments)
// by Harold Street Pedals 2025
//
// Classic amplitude tremolo using a low‑frequency oscillator (LFO) to change
// volume up/down over time.
//
// Signal path (mono): Input → LFO-controlled gain → Master (library) → Output
//
// Controls (HaroldPCB):
//   RV1 = Rate (how fast it pulses)
//   RV2 = Depth (how deep the volume swings)
//   TS1 = Shape (OFF=triangle, ON=square)
//   FS1 = Momentary Chop (while held, force full depth = hard on/off feel)
//   FS2 = Bypass toggle (true passthrough)
//   RV6 = Master (post effect; handled by library)
//   LED1 = Chop indicator (lights while FS1 held)
//   LED2 = Effect active (on when not bypassed)

#include <HaroldPCB.h>
#include <math.h>

// -----------------------------------------------------------------------------
// Constants (tweak here)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // fixed
static const uint16_t BLOCK_SIZE = 8;          // fixed

// Tremolo rate range (musical defaults)
static const float RATE_MIN_HZ = 0.20f;
static const float RATE_MAX_HZ = 12.0f;

// Depth range (0 = no tremolo, 1 = full on/off)
static const float DEPTH_MIN = 0.0f;
static const float DEPTH_MAX = 1.0f;

// Optional smoothing for square edges (ms). 0 = raw square (can click).
static const float SQUARE_SMOOTH_MS = 12.0f;

// Safety clamp for gain in case you experiment
static const float GAIN_MIN = 0.0f;
static const float GAIN_MAX = 1.2f;

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;

// Cached control state (set in loop(), read in AudioCB)
static volatile float tremoloRateHz = 2.0f;   // RV1
static volatile float depthAmount = 0.7f;     // RV2
static volatile bool useSquareShape = false;  // TS1 (false=triangle, true=square)
static volatile bool forceChopNow = false;    // FS1 (momentary full depth)
static volatile bool isBypassed = false;      // FS2 (toggle in loop)

// LFO state (audio thread only)
static float wavePhase = 0.0f;  // 0..1 position within one LFO cycle
static float phaseStep = 0.0f;  // how much phase advances per sample

// Square smoothing state (for click-free squares)
static float sqSmooth = 0.0f;

// FS2 edge detect memory (loop() only)
static bool prevFS2 = false;

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

// Advance phase by phaseStep and wrap back into 0..1
static inline float AdvancePhase(float phase, float step) {
  phase += step;
  if (phase >= 1.0f) phase -= 1.0f;
  return phase;
}

// 1‑pole smoothing (used to soften square edges)
// a = 1 - exp(-1/(tau * fs))  where tau = ms/1000
static inline float OnePoleSmoothStep(float ms, float fs) {
  if (ms <= 0.0f) return 1.0f;  // no smoothing
  return 1.0f - expf(-1.0f / ((ms / 1000.0f) * fs));
}

// -----------------------------------------------------------------------------
// Audio callback (runs at audio speed). Uses only cached state from loop().
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  // ----- BYPASS -----
  // If bypassed, we do no processing: out = in (true passthrough).
  if (isBypassed) {
    out = in;
    return;
  }

  // ----- LFO VALUE (HOW we generate it) -----
  // 1) wavePhase tracks where we are in the LFO cycle (0..1).
  // 2) For triangle: fold a sawtooth to get a rise/fall 0..1..0 curve.
  // 3) For square: produce 0 or 1, then optionally smooth with a 1‑pole filter
  //    to avoid clicks (edge rounding).
  float lfoUnipolar = 0.0f;  // LFO value in 0..1 (unipolar)

  if (!useSquareShape) {
    // Triangle via folded saw
    float t = wavePhase;
    lfoUnipolar = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);  // 0..1..0
  } else {
    // Raw square, then optional smoothing
    float raw = (wavePhase < 0.5f) ? 0.0f : 1.0f;
    if (SQUARE_SMOOTH_MS <= 0.0f) {
      lfoUnipolar = raw;
    } else {
      float a = OnePoleSmoothStep(SQUARE_SMOOTH_MS, (float)SAMPLE_RATE_HZ);
      sqSmooth += a * (raw - sqSmooth);  // y += a*(x - y)
      lfoUnipolar = sqSmooth;
    }
  }

  // ----- DEPTH (HOW we apply tremolo) -----
  // We make a gain between 0 and 1 using:
  //
  //   gain = (1 - depth) + depth * lfoUnipolar
  //
  // WHY this works:
  //  • When lfoUnipolar = 0, gain = (1 - depth)  → the quietest point.
  //  • When lfoUnipolar = 1, gain = 1           → full volume.
  //  • depth = 0 gives gain = 1 always (no tremolo).
  //
  // Momentary Chop (FS1): while held, we override depth to 1.0 (full swing).
  float depthNow = forceChopNow ? 1.0f : depthAmount;
  float gain = (1.0f - depthNow) + depthNow * lfoUnipolar;

  // Safety clamp (if you experiment later) and apply to the signal:
  gain = constrain(gain, GAIN_MIN, GAIN_MAX);
  out = in * gain;

  // ----- PHASE ADVANCE (HOW we move the LFO) -----
  // Phase moves by phaseStep each sample. One full cycle is 1.0.
  wavePhase = AdvancePhase(wavePhase, phaseStep);
}

// -----------------------------------------------------------------------------
// Setup (runs once)
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Initial Master from RV6 (post effect, handled by library)
  H.SetLevel(H.ReadPot(RV6));

  // Initial LFO step from default rate
  phaseStep = tremoloRateHz / (float)SAMPLE_RATE_HZ;

  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (control thread). Read knobs/switches here and update cached state.
// -----------------------------------------------------------------------------
void loop() {
  // Keep library services running (debounce, etc.)
  H.Idle();

  // 1) RV1 → Rate (HOW we turn a knob into phase speed)
  // We read RV1, map to [RATE_MIN_HZ..RATE_MAX_HZ] with an exponential feel,
  // then compute phaseStep = rate / sampleRate so the LFO completes 1.0 per cycle.
  tremoloRateHz = H.ReadPotMapped(RV1, RATE_MIN_HZ, RATE_MAX_HZ, HPCB_Curve::Exp10);
  phaseStep = tremoloRateHz / (float)SAMPLE_RATE_HZ;

  // 2) RV2 → Depth (HOW we control swing size)
  // depthAmount lives in [0..1]. 0 = no change; 1 = full on/off.
  depthAmount = H.ReadPotMapped(RV2, DEPTH_MIN, DEPTH_MAX, HPCB_Curve::Linear);

  // 3) TS1 → Shape (HOW we choose triangle vs square)
  // OFF = triangle (smooth), ON = square (with edge smoothing).
  useSquareShape = H.ReadToggle(TS1);

  // 4) FS1 → Momentary Chop (HOW we force full depth)
  // While held, we set forceChopNow = true so AudioCB uses depth=1.0.
  forceChopNow = H.FootswitchIsPressed(FS1);
  H.SetLED(LED1, forceChopNow);  // LED1 shows chop is active

  // 5) FS2 → Bypass (HOW we toggle clean passthrough)
  // Rising-edge toggle: only flip when the switch goes from up→down.
  {
    bool fs2 = H.FootswitchIsPressed(FS2);
    if (fs2 && !prevFS2) isBypassed = !isBypassed;
    prevFS2 = fs2;
  }

  // 6) RV6 → Master (HOW we hand off final volume to the library)
  // When the effect is ON, the knob sets master level. In bypass, force unity.
  if (!isBypassed)
    H.SetLevel(H.ReadPotSmoothed(RV6, 15.0f));  // mild smoothing feels nicer
  else
    H.SetLevel(1.0f);  // unity passthrough in bypass

  // LED2 shows whether the effect is active
  H.SetLED(LED2, !isBypassed);
}
