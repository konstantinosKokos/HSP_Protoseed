#💡 Tips for Tinkerers

##1) Keep controls out of the audio callback.
Polling hardware inside the audio thread can cause clicks and glitches. Always read controls in loop() (via Idle()), then feed cached values into your DSP.

##2) Mono only, by design.
The library hard-mutes the right channel. Don’t waste CPU trying to run stereo — left in/out is all you need.

##3) Map pots explicitly.
Always use ReadPotMapped() or ReadPotSmoothed() with clear ranges. Your sketch should read like:

```cpp
float speed = hpcb.ReadPotMapped(RV1, 0.1f, 5.0f);
```

##4) Bypass is a real unity path.
When you call SetBypassed(true), your audio skips all DSP and ignores master level. Good for sanity checks.

##5)Experiment safely.
No Serial is used in examples so you can always reflash in DFU mode without bricking. If you add Serial, do it carefully.

##Readable beats clever.
Use clear variable names and comments. This repo is meant as a teaching tool as much as a DSP sandbox.


###🔧 Tinkering with Sketches — Quick Tutorial

So you’ve opened one of the example `.ino` files and want to make it yours. Here’s the safe way to experiment:

##1. Make a copy first

**Don’t edit the originals in examples/ directly!**

Instead, copy the whole folder to your **Arduino** sketches directory and rename it, e.g.

```
HSP_Tremolo → MyTremoloHack
```

That way you always have a working reference to go back to.

##2. Find the parameter section

At the top of every example sketch you’ll see **constants** for things like min/max speed, depth, cutoff, etc.

Example:

```cpp
const float kMinRateHz = 0.1f;
const float kMaxRateHz = 10.0f;
```

These are the bounds for a pot. Turning RV1 will sweep from `kMinRateHz` up to `kMaxRateHz`.

##3. Change ranges, not math

If you want a faster tremolo, **raise** `kMaxRateHz`.

If you want slower sweeps, **lower** `kMinRateHz`.

You don’t need to dive into DSP formulas — just adjust the numbers in the constants.

##4. Use the mapping helpers

Pot reads should always go through `ReadPotMapped()` or `ReadPotSmoothed()**`.

That’s where the constants come in:

```cpp
float rate = hpcb.ReadPotMapped(RV1, kMinRateHz, kMaxRateHz);
```

Changing the constants automatically changes the knob’s behavior.

##5. Respect the signal path

Sketches are structured like block diagrams: **IN → DSP → OUT**.

When adding your own code, insert it in the flow — don’t scatter it in `loop()` or before `Idle()` unless it’s about controls.

##6. Save, build, test — repeat

Make one change at a time and test on hardware.

If something breaks, go back to your copy of the original and compare.

##7. Keep comments updated

If you adjust `kMaxRateHz` from 10 Hz to 20 Hz, update the comment so the next tinkerer (probably you) knows.

Good comments = fewer surprises later.

⚠️ Pro tip: Don’t touch the library code (HaroldPCB.h / .cpp) unless you really mean to. All normal tweaking should be inside your sketch.

**Happy Hacking!**