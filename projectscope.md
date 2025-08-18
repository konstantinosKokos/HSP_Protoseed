# Project Scope – Harold Street PCB Library & Examples (v1.3.0)

## Overall Purpose
Ship a compact, beginner-friendly firmware library for Harold Street Pedals’ Daisy Seed–based boards. The API hides DaisyDuino boilerplate and hardware quirks behind `HaroldPCB`, so beginners can write clear sketches quickly, while advanced users can extend as needed.

## Core Goals
1) Hide DaisyDuino setup and GPIO quirks behind `HaroldPCB`.
2) Use simple, English-like API calls so code reads like a sentence.
3) Require explicit mapping of controls to parameters in user code (no hidden magic).
4) Keep control polling out of the audio callback (do it in `Idle()`), to avoid audio artifacts.
5) Keep the surface area small and stable; add features deliberately.

## What’s In v1.3.0
- **Audio**: init, start/stop, mono callback thunk, master level.
- **Controls**: pots, toggles, footswitches, LEDs.
- **Footswitch events**: pressed/released + one-shots (long, double, double-long).
- **Debounce helpers**: `SetDebounce(ms)`, `SetLongPress(ms)`, `SetMultiClickGap(ms)` for easy tuning.
- **Active-high LEDs** aligned to current hardware.

## What’s NOT In v1.3.0 (by design)
- No legacy “binding” helpers (e.g., `BindPotTo…`).
- No built-in LFO utilities.
- No effect modules (examples demonstrate patterns instead).

## Library Structure
- **AUDIO**: `Init()`, `StartAudio()`, `StopAudio()`, `SampleRate()`, `BlockSize()`, `SetLevel()`.
- **CONTROLS**:
  - Pots: `ReadPot()`, `ReadPotMapped()`, `ReadPotSmoothed()`.
  - Toggles: `ReadToggle()`.
  - Footswitches: `FootswitchIsPressed/Released()`, one-shots: `FootswitchIsLongPressed/DoublePressed/DoubleLongPressed()`.
  - Timing & debounce: `SetFootswitchTiming()`, `SetDebounce()`, `SetLongPress()`, `SetMultiClickGap()`.
  - LEDs (active-high): `SetLED()`.

## Examples Philosophy
- Start with a single, extremely clear example that reads like prose and teaches the sketch structure and library calls without extra concepts.

## Deliverables & scope

This library officially provides:

  - Core source files: HaroldPCB.h, HaroldPCB.cpp

  - Board helper header: ThirdPartyDaisy.h (optional abstraction)

  - Examples: a canonical Basic* set covering passthrough, boost, tremolo, and five modeling distortion types (LED hard/soft, JFET, BJT, OpAmp, CMOS)

  - Documentation: textbook‑style README, User Guides at end of every sketch

  - Metadata: library.properties, keywords.txt for Arduino integration

  - License: GNU GPL v3 (LICENSE.txt)

  - Project scope file: projectscope.md describing philosophy, naming, and coding standards

  - Scope commitments:

  - Educational first: sketches double as lessons

  - Consistency: formatting, naming, and versioning (v1.0.0 for all examples)

  - Extensibility: encourage mods, with constants up top for tinkerers

  - Accuracy: defaults tested at 48kHz/8b, active HIGH LED logic and switch/potentiometer mapping verified

## Philosophy
Readable over clever. Explicit over implicit. Small and stable over sprawling.
