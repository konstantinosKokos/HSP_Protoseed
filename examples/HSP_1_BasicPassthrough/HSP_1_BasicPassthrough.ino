// HSP_Template_Passthrough.ino — v1.0.4
// by Harold Street Pedals 2025
//
// This is the “Hello World” pedal.
// Sound in → sound out, no changes.
// Use this to learn the shape of an Arduino pedal sketch.

#include <HaroldPCB.h>  // Library that knows how to talk to the pedal hardware

// -----------------------------------------------------------------------------
// COMMENTS
// -----------------------------------------------------------------------------
// Lines that start with // are comments. The computer ignores them.
// Comments are notes for humans, not for the machine.

// -----------------------------------------------------------------------------
// DATA TYPES (Beginner Map)
// -----------------------------------------------------------------------------
// "Data type" = tells the computer what kind of number we’re using.
// Common ones you’ll see here:
//
// int       → whole numbers. Size can vary by board (often 16 or 32 bits).
// float     → numbers with decimals. Used for audio (-1.0 to +1.0).
//
// The “u” in front of an int means “unsigned” (no negative numbers).
// The number after it shows how many bits of storage it has:
//
//   uint8_t   = unsigned 8-bit int (0 to 255)
//   uint16_t  = unsigned 16-bit int (0 to 65,535)
//   uint32_t  = unsigned 32-bit int (0 to ~4 billion)
//
// Why use them? They make it obvious how big the number can be,
// which helps when working with hardware.
//
// In this sketch:
// - SAMPLE_RATE_HZ uses uint32_t (big positive number: 48000).
// - BLOCK_SIZE uses uint16_t (small positive number: 8).
// - Audio in/out uses float (decimal audio numbers).
//
// Think of it like boxes of different sizes:
//   int      = a normal box
//   uint16_t = a small box, only for positive whole numbers
//   uint32_t = a big box, only for positive whole numbers
//   float    = a box that can hold decimals

// -----------------------------------------------------------------------------
// CONSTANTS
// -----------------------------------------------------------------------------
static const uint32_t SAMPLE_RATE_HZ = 48000; // 48,000 slices per second
static const uint16_t BLOCK_SIZE     = 8;     // Work on 8 slices at a time

// -----------------------------------------------------------------------------
// GLOBAL OBJECTS
// -----------------------------------------------------------------------------
static HaroldPCB H;  // HaroldPCB = toolbox for knobs, switches, LEDs

// -----------------------------------------------------------------------------
// FUNCTIONS
// -----------------------------------------------------------------------------
// A function is a block of code that does something.
// "void" means the function does not give back a result.
//
// AudioCB is our audio function. It runs constantly at audio speed.
// "float in"  = input sound number
// "float &out" = output sound number (we fill this in)
void AudioCB(float in, float &out) {
  out = in;  // passthrough → copy input to output
}

// -----------------------------------------------------------------------------
// setup() — runs ONCE when the pedal turns on
// -----------------------------------------------------------------------------
void setup() {
  H.Init(SAMPLE_RATE_HZ, BLOCK_SIZE);  // start audio hardware
  H.StartAudio(AudioCB);                // begin sending sound to AudioCB
  H.SetLED(LED1, true);                 // turn on LED1 to show power-on
}

// -----------------------------------------------------------------------------
// loop() — runs FOREVER in the background
// -----------------------------------------------------------------------------
void loop() {
  H.Idle();  // keep knobs/switches steady

  // Example: light LED1 while FS1 is held
  bool fs1 = H.FootswitchIsPressed(FS1);
  H.SetLED(LED1, fs1);
}

// -----------------------------------------------------------------------------
// USER GUIDE
// -----------------------------------------------------------------------------
//
// What happens here:
// - Sound in = sound out.
// - FS1 turns LED1 on/off.
// That’s all.
//
// Why this sketch matters:
// - Can be modified to prove wiring, audio, and LEDs all work. A test circuit.
// - Shows the shape of every sketch: constants → setup() → loop() → audio function.
//
// Datatype map (short form):
//   int      → whole numbers (size depends on board)
//   float    → decimals, used for audio
//   uint8_t  → 0 to 255
//   uint16_t → 0 to 65,535
//   uint32_t → 0 to ~4 billion
//
// Next steps:
// 1) Try “out = in * 0.5f;” for lower volume.
// 2) Mute with FS1: if(fs1) out = 0.0f;
// 3) Read RV1 for a volume knob.
// 4) Blink LED2 in loop() with delay(500);
//
