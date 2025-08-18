---
# Modeling Distortion Types for Harold Street Pedals
---

These are digital approximations of classic analog distortion topologies. Each can be implemented in the AudioCB using simple waveshapers, soft nonlinearities, or dynamic models. 

1.**LED Hard Clipping**

Analog origin: Simple diode-to-ground clippers (e.g. RAT with LEDs).

Model: Hard cutoff at ±threshold voltage (≈1.6 V for red LEDs).

Digital shape:

float $y = (x > thr) ? thr : (x < -thr) ? -thr : x$;

Sound: Loud, raw, compressed, harmonically rich.


2. **LED Soft Clipping**

Analog origin: Diodes in feedback loop of op-amp (Tube Screamer with LEDs).

Model: Smooth nonlinear (tanh, arctan, or diode equation).

Digital shape:

float $y = thr * tanhf(x / thr)$;

Sound: Smoother saturation, retains dynamics, singing sustain.


3. **JFET Modeling**

Analog origin: Common-source JFET gain stage (DOD 250, boutique ODs).

Model: Square-law transfer with pinch-off region. Approximate with quadratic soft-knee.

Digital shape:

float $y = x - (alpha * x * fabs(x))$;

Sound: Tube-like asymmetry, dynamic, touch sensitive.


4. **BJT Transistor Clipping**

Analog origin: Classic fuzz circuits (Fuzz Face, Tone Bender).

Model: Exponential base-emitter curve$ → I ≈ Is*(exp(Vbe/Vt)-1)$.

Digital shape: Use exponential diode model or simplified asymmetry.

float $y = tanhf(gain * x) + 0.1f * x$; // fuzz with asymmetry

Sound: Aggressive, squishy, compressed. Interacts heavily with input level.


5. **Op-Amp Clipping**

Analog origin: Dist+ / MXR+, ProCo RAT (op-amp gain with clipping).

Model: High gain, then soft or hard clip.

Digital shape:

float pre = $gain * x$;
float y   = $tanhf(pre);$

Sound: Defined, biting distortion. Easy to voice with EQ.


6. **CMOS Inverter Clipping**

Analog origin: CD4049/4069 CMOS fuzz pedals (Hot Tubes, Red Llama).

Model: Rail-to-rail inverter with nonlinear transfer.

Digital shape: Approximate with cubic or sigmoid:

float y = $(2.0f / (1.0f + expf(-pre))) - 1.0f$;

Sound: Crunchy, lo-fi, unique harmonics. “CMOS fizz.”


## Suggested Constants for All Distortions

At the top of each sketch, provide:

	`DRIVE_GAIN_MIN, DRIVE_GAIN_MAX`

	`CLIP_THRESHOLD`

	`SOFTNESS` (for shaping curve steepness)

	`OUTPUT_TRIM`

	`OUT_LIMIT` (safety clamp)
	

## Teaching Approach

- Each distortion type = its own sketch (v1.0.0).

- Use **RV1** = Drive, **RV2** = Tone, **RV6** = Master, **FS2** = bypass, **LED2* = active.

- Explain math inline.

- End with a User Guide describing the sound, circuit origin, constants, and mods.