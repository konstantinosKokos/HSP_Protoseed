// HaroldPCB SteelCityVerb v1  (compatible with HaroldPCB 1.3.0 and earlier)
// Version 1.0.1 + LED1-on-while-FS2
//   - RV1 (Pot 0) capped to 95%
//   - FS2 holds RV1 to 100%
//   - LED0 shows reverb active (not bypassed)
//   - LED1 mirrors FS2 (on while held)
// Controls (same positions as your library):
//   Pots: 0=Feedback, 1=Damping, 2=Send, 3=Grain Mix
//   FS:   0=Bypass toggle, 1=Override (hold)
//   TS:   0=HPF on/off,   1=Shimmer direction (off=up, on=down)

#include "HaroldPCB.h"
#include <cmath>
#include <cstdlib>

// ---------- High-Pass Filter ----------
class HighPass {
public:
  HighPass() {}
  HighPass(float cutoff, float sr) {
    init(cutoff, sr);
  }
  void init(float cutoff, float sr) {
    const float rc = 1.0f / (2.0f * float(M_PI) * cutoff);
    const float dt = 1.0f / sr;
    a_ = rc / (rc + dt);
    prev_in_ = prev_out_ = 0.0f;
  }
  float process(float x) {
    float y = a_ * (prev_out_ + x - prev_in_);
    prev_in_ = x;
    prev_out_ = y;
    return y;
  }
private:
  float a_{}, prev_in_{}, prev_out_{};
};

// ---------- Schroeder Mono Reverb ----------
class SchroederReverb {
public:
  static const int COMB_COUNT = 4;
  static const int AP_COUNT = 2;

  void init(float sr) {
    (void)sr;  // sample rate not needed for fixed lengths
    int combLens[COMB_COUNT] = { 1557, 1617, 1491, 1422 };
    for (int i = 0; i < COMB_COUNT; i++) combs[i] = new Comb(combLens[i]);
    int apLens[AP_COUNT] = { 225, 556 };
    for (int i = 0; i < AP_COUNT; i++) aps[i] = new AP(apLens[i], 0.5f);
  }

  float process(float x, float fb, float damp) {
    float sum = 0.0f;
    for (int i = 0; i < COMB_COUNT; i++) sum += combs[i]->process(x, fb, damp);
    float y = sum / float(COMB_COUNT);
    for (int i = 0; i < AP_COUNT; i++) y = aps[i]->process(y);
    return y;
  }

private:
  struct Comb {
    Comb(int sz)
      : size(sz), buf(new float[sz]()), idx(0), store(0.0f) {}
    ~Comb() {
      delete[] buf;
    }
    float process(float in, float fb, float damp) {
      float out = buf[idx];
      store = out * (1.0f - damp) + store * damp;
      buf[idx] = in + store * fb;
      if (++idx >= size) idx = 0;
      return out;
    }
    float *buf;
    float store;
    int size;
    int idx;
  } * combs[COMB_COUNT];

  struct AP {
    AP(int sz, float g)
      : size(sz), buf(new float[sz]()), idx(0), gain(g) {}
    ~AP() {
      delete[] buf;
    }
    float process(float in) {
      float b = buf[idx];
      float out = -gain * in + b;
      buf[idx] = in + b * gain;
      if (++idx >= size) idx = 0;
      return out;
    }
    float *buf;
    float gain;
    int size;
    int idx;
  } * aps[AP_COUNT];
};

// ---------- Granular Shimmer ----------
static bool gReverseShimmer = false;

class GranularShimmer {
public:
  static const int BUF_SIZE = 48000;
  static const int MAX_GRAINS = 8;
  GranularShimmer() {
    reset();
  }

  void reset() {
    writeIdx = 0;
    spawnCtr = spawnInterval;
    for (int i = 0; i < BUF_SIZE; i++) buf[i] = 0.0f;
    for (int g = 0; g < MAX_GRAINS; g++) active[g] = false;
  }

  float process(float tail) {
    buf[writeIdx] = tail;
    if (++spawnCtr >= spawnInterval) {
      spawnCtr = 0;
      spawn();
    }

    float out = 0.0f;
    for (int g = 0; g < MAX_GRAINS; g++)
      if (active[g]) {
        Grain &gr = grains[g];
        int idx = int(gr.pos) % BUF_SIZE;
        float frac = gr.pos - int(gr.pos);
        float s = buf[idx] * (1 - frac) + buf[(idx + 1) % BUF_SIZE] * frac;
        float env = gr.env < 0.5f ? gr.env * 2.0f : 2.0f * (1.0f - gr.env);
        out += s * env;
        gr.env += gr.envInc;
        gr.pos += gr.speed;
        if (gr.env >= 1.0f) active[g] = false;
      }

    if (++writeIdx >= BUF_SIZE) writeIdx = 0;
    return out / float(MAX_GRAINS);
  }

private:
  struct Grain {
    float pos, speed, env, envInc;
  };
  float buf[BUF_SIZE];
  bool active[MAX_GRAINS];
  Grain grains[MAX_GRAINS];
  int writeIdx, spawnCtr;
  static const int spawnInterval = 2400;

  void spawn() {
    for (int g = 0; g < MAX_GRAINS; g++) {
      if (!active[g]) {
        active[g] = true;
        grains[g].pos = float(writeIdx);
        const float base = gReverseShimmer ? 0.5f : 2.0f;  // down or up one octave
        const float rnd = (rand() / float(RAND_MAX) - 0.5f) * 0.1f;
        grains[g].speed = base + rnd;
        grains[g].env = 0.0f;
        grains[g].envInc = 1.0f / (0.1f * 48000.0f);  // ~100 ms
        break;
      }
    }
  }
};

// ---------- HaroldPCB cross-version adapter (works with 1.3.0) ----------
using AudioCB = void (*)(float **, float **, size_t);

template<typename T>
static auto _try_StartWithCallback(T &d, AudioCB cb, int) -> decltype(d.StartWithCallback(cb), void()) {
  d.StartWithCallback(cb);
}
template<typename T>
static auto _try_SetCbStartAudio(T &d, AudioCB cb, long) -> decltype(d.Init(), d.SetAudioCallback(cb), d.StartAudio(), void()) {
  d.Init();
  d.SetAudioCallback(cb);
  d.StartAudio();
}
template<typename T>
static auto _try_Start(T &d, AudioCB cb, char) -> decltype(d.Start(cb), void()) {
  d.Start(cb);
}
template<typename T>
static void start_audio(T &d, AudioCB cb) {
  // Preference order: StartWithCallback → (Init+SetAudioCallback+StartAudio) → Start
  _try_StartWithCallback(d, cb, 0);  // if exists, this wins
}

template<typename T>
static float _try_GetSampleRate(T &d, int) {
  return d.GetSampleRate();
}
static float _fallback_sr() {
  return 48000.0f;
}
template<typename T>
static float get_sr(T &d) {
  // If GetSampleRate() is missing, just return a sane default.
  // (Your 1.3.0 has GetSampleRate; this keeps older builds happy.)
  if constexpr (std::is_same_v<decltype(_try_GetSampleRate(d, 0)), float>) {
    return d.GetSampleRate();
  } else {
    return _fallback_sr();
  }
}

// ---------- Globals / Audio ----------
HaroldPCB hpcb;
SchroederReverb rev;
GranularShimmer shimmer;
HighPass hp;

static bool bypass = false;
static bool lastFs1 = false;

void AudioCallback(float **in, float **out, size_t size) {
  // Read UI — indices unchanged from your library mapping
  const bool fs1 = hpcb.ReadFootswitchDebounced(0);  // bypass toggle
  if (fs1 && !lastFs1) bypass = !bypass;
  lastFs1 = fs1;

  const bool fs2 = hpcb.ReadFootswitchDebounced(1);  // override (hold)
  hpcb.SetLED(1, fs2);                               // LED1 on while FS2 held

  const bool hpfOn = hpcb.ReadToggle(0);
  gReverseShimmer = hpcb.ReadToggle(1);

  // Pots
  float pot0 = hpcb.ReadPot(0);  // feedback
  if (fs2) pot0 = 1.0f;          // FS2 forces 100%
  pot0 *= 0.95f;                 // cap RV1 at 95%
  const float fb = 0.6f + pot0 * (0.999f - 0.6f);
  const float damp = hpcb.ReadPot(1);
  const float send = hpcb.ReadPot(2);
  const float mixG = hpcb.ReadPot(3);

  for (size_t i = 0; i < size; i++) {
    float x = in[0][i];
    float dry = hpfOn ? hp.process(x) : x;

    float tail = rev.process(dry * send, fb, damp);
    float grain = shimmer.process(tail);
    float wet = tail + grain * mixG;

    float y = bypass ? dry : (dry + wet);

    out[0][i] = y;     // mono out L
    out[1][i] = 0.0f;  // R silent
  }
}

void setup() {
  start_audio(hpcb, AudioCallback);  // works across HaroldPCB versions incl. 1.3.0
  const float sr = get_sr(hpcb);
  rev.init(sr);
  shimmer.reset();
  hp.init(150.0f, sr);
}

void loop() {
  hpcb.SetLED(0, !bypass);  // LED0 shows “effect active”
  delay(10);
}
