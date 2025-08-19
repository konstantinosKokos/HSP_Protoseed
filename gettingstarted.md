# Getting Started with HaroldPCB

This project is an **Arduino library** — you don’t need a Makefile or the raw Daisy toolchain.  
Just install HaroldPCB like any other Arduino library and build sketches directly.

---

## 1) Requirements
- **Arduino IDE 2.x**
- **Daisy Seed** board + USB cable
- This repo (`HaroldPCB`)

> This repo intentionally avoids the Daisy make/CMake toolchain; DaisyDuino is vendored and included for you. ✅

---

## 2) Install the Library
### Option A — ZIP (fastest)
1. Zip the repo so `library.properties` is at the top of the archive.
2. Arduino IDE → **Sketch → Include Library → Add .ZIP Library…**
3. Select your zip. You should now see **HaroldPCB** under **File → Examples**.

### Option B — Manual
1. Close Arduino IDE.
2. Copy the repo folder into your Arduino libraries directory:
   - Windows: `Documents/Arduino/libraries/HaroldPCB`
   - macOS/Linux: `~/Documents/Arduino/libraries/HaroldPCB`
3. Reopen Arduino IDE.

---

## 3) Select the Daisy Seed
- **Tools → Board → Daisy Seed**
- **Tools → Port** → pick the correct port (DFU mode may not show a COM port; upload still works).

---

## 4) Open an Example
**File → Examples → HaroldPCB** → pick:
- `HSP_Passthrough` (hello world)
- `HSP_Tremolo` (first effect)

Each sketch starts with:

```cpp
#include <HaroldPCB.h>
HaroldPCB hpcb;
5) Build & Upload
Click Verify, then Upload.
No extra third-party libraries or Makefile needed — DaisyDuino is vendored and included.

6) Minimal Sketch Pattern
cpp
Copy
Edit
#include <HaroldPCB.h>
HaroldPCB hpcb;

void AudioCallback(float in, float &out) {
  out = in; // passthrough
}

void setup() {
  hpcb.Init();                  // 48 kHz, 48-sample block
  hpcb.StartAudio(AudioCallback);
}

void loop() {
  hpcb.Idle();                  // polls pots/toggles/footswitches (debounced)
}
7) Useful Tips
Mono only: Left in/out; Right out is hard-muted by the library.

Footswitch feel (debounce/long/double) can be tuned:

cpp
Copy
Edit
HPCB_FootswitchTiming t;
t.debounce_ms       = 12;
t.longpress_ms      = 600;
t.multiclick_gap_ms = 300;
hpcb.SetFootswitchTiming(t);
Latency vs CPU: Smaller blocks = lower latency, higher CPU load.
Change with hpcb.Init(96000, 4); if needed.

8) Philosophy
Readable over clever. Explicit over implicit. Self-contained over dependency sprawl.