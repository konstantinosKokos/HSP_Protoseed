// HSP_Template_Passthrough.ino — v1.0.0
// by Harold Street Pedals 2025
// Example template sketch for HaroldPCB library
// A simple passthrough pedal with full documentation style

#include <HaroldPCB.h>

// -----------------------------------------------------------------------------
// Constants (tunable parameters for builders/tinkerers)
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000;  // Fixed: 48 kHz sample rate
static const uint16_t BLOCK_SIZE = 8;          // Fixed: 8-sample audio block

// -----------------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------------
static HaroldPCB H;  // Our HaroldPCB board instance

// -----------------------------------------------------------------------------
// Audio callback (runs at audio rate)
// Processes mono input/output
// -----------------------------------------------------------------------------
void AudioCB(float in, float &out) {
  // This example simply passes input to output unchanged
  out = in;
}

// -----------------------------------------------------------------------------
// Setup (runs once at power-on)
// -----------------------------------------------------------------------------
void setup() {
  // Initialize hardware with fixed sample rate and block size
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);

  // Start audio with our mono callback
  H.StartAudio(AudioCB);
}

// -----------------------------------------------------------------------------
// Loop (runs continuously, but NOT in audio time)
// Use for control reads, LEDs, etc.
// -----------------------------------------------------------------------------
void loop() {
  // Always call Idle() to service pots, toggles, and footswitches
  H.Idle();

  // Example: light LED1 when FS1 is pressed
  bool fs1 = H.FootswitchIsPressed(FS1);
  H.SetLED(LED1, fs1);  // no function just for example.
}

// -----------------------------------------------------------------------------
// User Guide
// -----------------------------------------------------------------------------
//
// Overview:
// This is the simplest possible pedal: it takes audio in and sends it straight out,
// with no processing. It is useful as a reference template for building new pedals.
//
// Controls:
// - FS1: Toggles LED1 when pressed (demonstration only).
// - FS2, RV1–RV6, TS1–TS4: Not used in this template, but wired and ready.
//
// Customizable Parameters:
// - SAMPLE_RATE_HZ: Fixed at 48 kHz for consistency across all examples.
// - BLOCK_SIZE: Fixed at 8 for low latency. Change only if you need longer DSP loops.
//
// Mods for Builders/Tinkerers:
// - Add a simple gain stage by scaling `out = in * 0.5f;` in the callback.
// - Use RV1 as a volume knob by mapping it with `ReadPotMapped()`.
// - Try lighting both LEDs differently based on FS1/FS2 for testing.
// - Replace passthrough with a filter, tremolo, or delay for your first real pedal.
//
