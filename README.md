# HaroldPCB Library (Harold Street Pedals, 2025)

A small, opinionated Arduino/C++ library and set of **textbook-style pedal examples** for builders, tinkerers, and nerds. Each example is a working effect and a teaching page, written to be readable for beginners while still exposing useful internals for modders.

> **Defaults across all examples**
> - **Audio:** 48 kHz sample rate, **8-sample** audio blocks (low latency)
> - **LEDs:** **active‑HIGH** — `SetLED(LEDx, true)` turns the LED **on**
> - **Naming:** every example sketch begins with **Basic** (e.g., `BasicTremolo.ino`)
> - **Style:** constants at the top, clean sections, long **User Guide** at bottom

---

## 1) Who this is for
- **Builders**: clear constants up top, easy to tweak & revoice
- **Tinkerers**: small, self‑contained DSP blocks you can combine
- **Nerds**: documented math & transfer curves, deterministic control paths

---

## 2) Repository layout
```
HaroldPCB/
  ├─ src/
  │   ├─ HaroldPCB.h
  │   └─ HaroldPCB.cpp
  ├─ examples/
  │   ├─ BasicPassthrough/
  │   ├─ BasicBoost/
  │   ├─ BasicTremolo/
  │   ├─ BasicLEDHardClip/
  │   ├─ BasicLEDSoftClip/
  │   ├─ BasicJFET/
  │   ├─ BasicBJT/
  │   ├─ BasicOpAmp/
  │   └─ BasicCMOS/
  ├─ keywords.txt
  ├─ library.properties
  └─ LICENSE (GNU GPL v3)
```
> Exact folders may differ slightly; the **examples/** names above reflect the canonical set and control maps below.

---

## 3A) Quick start
1. **Install toolchain**
   - Arduino IDE 2.x or PlatformIO
   - Board support per your HaroldPCB target (same setup you already use to flash other HSP sketches)
2. **Clone / add library**
   - Place the `HaroldPCB` folder into your Arduino `libraries/` (or add to your PlatformIO lib deps)
3. **Open an example**
   - Start with `examples/BasicPassthrough/BasicPassthrough.ino`
4. **Build & flash**
   - Confirm audio passes through; LED logic should be **active‑HIGH**
5. **Tweak constants** at the top of the sketch and reflash

---

## 3B) Installation (DFU on Windows, step-by-step)

These boards ship blank. You’ll need to install the toolchain and flash firmware once to get started.  
This guide assumes you know *nothing* and have *none* of the tools yet.

---

### 1. Install MSYS2 (gives you a Unix-like shell on Windows)
- Go to [https://www.msys2.org](https://www.msys2.org).
- Download the installer and run it (default options are fine).
- Open the new **MSYS2 MinGW 64-bit** shortcut from your Start menu.

---

### 2. Install required packages inside MSYS2
In the MSYS2 window, paste these commands one by one:

```bash
pacman -Syu       # full system update (will close the window, reopen after)
pacman -S git make gcc unzip dfu-util

git lets you download code from GitHub.

make and gcc compile the firmware.

dfu-util is the program that talks to the Daisy Seed in bootloader mode.



---

3. Get the HaroldPCB library and examples

Still inside MSYS2:

cd ~
git clone https://github.com/Harold-Street-Pedal-Company/HSP_Protoseed.git
cd HSP_Protoseed


---

4. Put the Daisy Seed into DFU (bootloader) mode


Hold down the BOOT button on the Seed.

While holding BOOT, press the RESET button once, then release BOOT.

The Seed is now waiting for firmware (LED will be dim).


Windows will show a new device called STM32 BOOTLOADER if it worked.


---

5. Build and flash an example

Inside MSYS2, from the library folder:

make program-dfu

This compiles the default sketch and immediately flashes it with dfu-util.

When it finishes, the Seed reboots and starts running your pedal firmware.



---

6. Re-flash different examples

Change into any example folder (e.g., examples/HSP_Tremolo) and run:

make program-dfu

That’s it. You can keep re-flashing as you experiment.


---

Troubleshooting

Device not found? Check you’re really in DFU mode (BOOT held, tap RESET, release BOOT).

Permission errors? On Windows, always use the MSYS2 MinGW 64-bit shell, not plain PowerShell.

Still stuck? Unplug/replug USB and repeat step 4 carefully.

---


💡 Tip: All examples are DFU-safe. They don’t open a Serial port, so even if you mess up the code, you can always get back into DFU mode and re-flash.

> ⚠️ **Low‑latency default**: 48 kHz / 8‑sample blocks ≈ **0.17 ms** block latency (plus codec/IO). If you change `BLOCK_SIZE`, expect latency/CPU trade‑offs.

> ⚠️ **THERE IS NO SERIAL MONITOR AVAILABLE!**  Nor is there any api for SD card.  they exist in the __disabled__ folder should you require them (you shouldn't) but, they caused me issues during vendoring and this was the quick and dirty solution.

---


## 4) Core programming model (mono)

### 4.1 Minimal sketch skeleton
```cpp
#include <HaroldPCB.h>

static const uint32_t SAMPLE_RATE_HZ = 48000;
static const uint16_t BLOCK_SIZE     = 8;

static HaroldPCB H;

void AudioCB(float in, float &out) {
  out = in; // your DSP here
}

void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);
  H.StartAudio(AudioCB);
}

void loop() {
  H.Idle();                 // services pots/toggles/footswitches
  H.SetLED(LED2, true);     // active‑HIGH
}


### 4.2 Common API calls
- **Lifecycle**: `H.Init(sr, block)`, `H.StartAudio(callback)`, `H.Idle()`
- **Controls**
  - Pots: `H.ReadPot(RV1)` → `0..1`
  - Smoothed pot: `H.ReadPotSmoothed(RV6, 15.0f)` (ms smoothing time)
  - Mapped pot: `H.ReadPotMapped(RV1, min, max, HPCB_Curve::Exp10|Linear)`
  - Toggles: `H.ReadToggle(TS1)` → `bool`
  - Footswitches: `H.FootswitchIsPressed(FS1)` → momentary state (edge detect in `loop()` as needed)
- **Output / UI**
  - LEDs: `H.SetLED(LED1, bool)` (active‑HIGH)
  - Master level: `H.SetLevel(float_0_to_1)` (post effect)

> **Threading rule**: Only use **cached** control values inside `AudioCB`. Read hardware from `loop()` and write to `volatile` globals that the callback consumes.

---

## 5) Example index & control maps
Each example is versioned **v1.0.0**, constants up top, and ends with a beginner‑friendly **User Guide**.

### BasicPassthrough
- **What it teaches**: project skeleton, sections, LED test
- **Controls**: FS1 → LED1 demo; LED2 free

### BasicBoost (clean boost + tone)
- **Controls**: RV1 **Boost (dB)**, RV2 **Tone (treble‑cut)**, RV6 **Master**, FS2 **Bypass**, LED2 **Active**
- **Notes**: linear gain + one pole LPF tone; safety clamp pattern

### BasicTremolo
- **Controls**: RV1 **Rate**, RV2 **Depth**, RV6 **Master**, TS1 **Shape** (triangle/square), FS1 **Chop** (momentary full depth), FS2 **Bypass**, LED1 **Chop** indicator, LED2 **Active**
- **Notes**: LFO phase accumulator; smoothed square to avoid clicks

### BasicLEDHardClip
- **Controls**: RV1 **Drive**, RV2 **Tone**, RV3 **Symmetry**, RV6 **Master**, FS1 **Kick**, FS2 **Bypass**, LED1 **Clip** meter, LED2 **Active**
- **Notes**: asym hard clip thresholds → LED‑style character

### BasicLEDSoftClip
- **Controls**: RV1 **Drive**, RV2 **Tone**, RV3 **Symmetry**, RV6 **Master**, FS1 **Kick**, FS2 **Bypass**, LED1 **Clip** meter, LED2 **Active**
- **Notes**: `tanh()` feedback like soft clip with asymmetry

### BasicJFET
- **Controls**: RV1 **Drive**, RV2 **Grit (alpha)**, RV3 **Bias**, RV4 **Tone**, RV6 **Master**, FS1 **Kick**, FS2 **Bypass**, LED1 **Clip**, LED2 **Active**
- **Notes**: square‑law soft knee + bias driven asymmetry

### BasicBJT
- **Controls**: RV1 **Drive**, RV2 **Vt (steepness)**, RV3 **Asymmetry**, RV4 **Bias**, RV5 **Tone**, RV6 **Master**, FS1 **Kick**, FS2 **Bypass**
- **Notes**: diode‑equation style soft clip; strong odd harmonics

### BasicOpAmp
- **Controls**: RV1 **Drive**, RV2 **Soft threshold**, RV3 **Soft↔Hard mix**, RV4 **Hard threshold**, RV5 **Tone**, RV6 **Master**, FS1 **Kick**, FS2 **Bypass**
- **Notes**: feedback clipper blended with diode hard clip

### BasicCMOS
- **Controls**: RV1 **Drive**, RV2 **Crunch (cubic)**, RV3 **Fizz (fifth)**, RV4 **Pre‑gain**, RV5 **Sag** *(alt: Tone)*, RV6 **Master**, FS1 **Kick**, FS2 **Bypass**
- **Notes**: odd‑polynomial CMOS feel + envelope sag

> **Convention**: FS2 is always the **bypass** toggle; LED2 shows **effect active**. FS1 is reserved for **momentary** actions (kick/chop) and LED1 is typically a **meter/indicator**.

---

## 6) Style rules for contributions
- Start each sketch with:
  ```
  // BasicEffectName.ino — v1.0.0
  // by Harold Street Pedals 2025
  // One‑line description
  ```
- Keep **constants at the top** (clearly commented) — these are the hidden knobs for builders
- Use clear sections: *Includes & Globals → Constants → DSP structs → setup() → loop() → AudioCB → helpers → User Guide*
- Prefer **readability** over cleverness; name things verbosely
- Document *why* as well as *what* (textbook tone for beginners)

---

## 7) Tuning, mapping & curves
- Use `H.ReadPotMapped(..., HPCB_Curve::Exp10)` for **musical sweeps** (log‑like)
- Smooth master/output moves with `H.ReadPotSmoothed(RV6, 10–20 ms)`
- Clamp final output (e.g., `OUT_LIMIT = 1.2f`) to prevent digital overs while experimenting

---

## 8) Troubleshooting
- **No sound**: ensure `H.StartAudio(AudioCB)` is called; verify not bypassed (FS2)
- **LEDs inverted**: library must be **active HIGH** — `SetLED(..., true)` lights the LED
- **Pots feel steppy**: increase smoothing ms or use `ReadPotMapped` with Exp curve
- **Harsh aliasy highs** at extreme drive: lower post tone cutoff or add gentle soft limit; oversampling is an advanced option
- **Footswitch toggle unreliable**: do **edge detection** in `loop()`; don’t toggle on raw level in the callback

---

## 9) Safety & headroom
- Keep a **post DSP trim** (`OUTPUT_TRIM`) and a **safety limiter** (`OUT_LIMIT`) in distortion examples
- With 48k/8b, CPU is tight by design; prefer simple single pole filters and low order shapers for first builds

---

## 10) Roadmap (suggested)
- Tap tempo utilities; envelope helpers; simple 2× oversampling adaptor
- Additional Basic* examples: filter trem, harmonic trem, tilt EQ boost, chorus, phaser, flanger

---

## 11) License & credits
- Copyright © 2025 **Harold Street Pedals**
- License: GNU General Public License v3.0 (see `LICENSE` file)

**Have fun building.** The goal is to make every example feel like a page from the same textbook — approachable for beginners, hackable for experts.
