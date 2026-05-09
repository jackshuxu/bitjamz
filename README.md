# Running

From the project root, build cmd

```bash
cmake -S . -B build && cmake --build build
```

run it

```bash
./build/bitjams
```

rebuild from scratch

```bash
rm -rf build
cmake -S . -B build && cmake --build build
```

# PRD

Nowadays we have a tui for everything, but there's somehow no tui grooveboxes (a self-contained electronic music production device that combines multiple components in one unit) on the market (I did a quick search on google and https://terminaltrove.com/ and I found nothing, so i guess we are the only one...)
So our goal for the final project is to build a networked collaborative TUI sequencer: each user runs a terminal instrument similar to the Teenage Engineering Pocket Operator(a lightweight groove box with built in synth, drums, bass sounds) and jams together over a shared session.

![[Screenshot 2026-04-27 at 11.16.15 PM.png|509]]
_a Pocket Operator "orchestra"_

**The tui daw**
![[Screenshot 2026-04-27 at 11.20.26 PM.png|509]]
_prototype tui daw_

We prototyped a TUI Digital Audio Workstation in C++ with multiple drum voices and one minimal synth, running entirely in the terminal with no external assets. The interface renders as a 16-step sequencer grid, users move a cursor across steps with arrow keys and toggle cells with spacebar. A playhead advances across the grid at a fixed BPM, and a loop counter tracks how many times the pattern has cycled.
All drum sounds are synthesized procedurally at runtime via a callback-based audio engine using miniaudio. The kick is a sine wave with a pitch envelope sweeping from 180 Hz to 40 Hz over ~30ms, decaying over 300ms. The snare layers white noise with a 200 Hz square wave and a short high-pass bias. The hi-hat is band-passed noise with a sharp 50ms decay. The clap is three staggered noise bursts 8ms apart with a transient click at onset. To achieve a lo-fi, 8-bit aesthetic consistent with the PO aesthetic, all voices are quantized to 8-bit resolution before mixing to get a retro sound, each sample is cast through `int8_t` and back, crunching the waveform to 256 levels.

Disregarding the networking, the system runs three threads: a main thread handling raw terminal input and redrawing the screen at ~30 fps, a timing thread that fires every 125ms to advance the step counter and trigger voices, and miniaudio's own audio callback thread which mixes active voices into the output buffer. The timing and audio threads communicate through a small set of atomic flags to avoid locks on the hot path. Terminal raw mode is installed at startup and restored in an `atexit` handler so a crash does not leave the shell broken.
A stereo visualization is rendered alongside the grid as a live Lissajous-style sterogram, giving performers a visual read on the current mix.
We anticipate adding a lot more features in the future, such as 16 bars, live audio effects, volume control etc.

**synchronizing sessions**
A core challenge of networked collaborative music is that audio is far more sensitive to timing irregularities than general data transfer. Even small delays produce flamming — the audible artifact of notes that were intended to land together arriving slightly apart. Any sync model must confront this directly.
The most immediate approach is full real-time sync: notes are broadcast and played the instant they are input, encoded as `{beat, pitch, instrument}` tuples and applied on arrival. This requires one-way latency under ~10ms, which is feasible on a LAN with clock alignment (e.g. Ableton Link) but essentially impossible over the public internet, where jitter alone routinely exceeds that threshold.
The second approach queues inputs and applies them atomically at the next loop boundary. All clients share a beat clock and wrap in unison, but remote notes don't sound until the following cycle. Latency is hidden by the loop length, typically two to four seconds at 120 BPM. A server broadcasts a `loop_tick` at each boundary; clients accumulate deltas and apply them on receipt. Tempo drift across machines remains a long-term concern though, so we need to figure out how to sync the clocks.
The third approach removes the shared timeline entirely. Each client plays independently and remote inputs populate the grid whenever they arrive. This requires no clock sync and tolerates disconnection gracefully. Other player's notes arrives, and simply populates your sequencer.

**tools and stack**
The prototype is written in C++ as a single source file, built with CMake and linked against Apple's CoreAudio frameworks on macOS. Audio I/O is handled by miniaudio, a single-header C library that wraps platform audio APIs and drives the synthesis callback. The terminal interface uses raw ANSI escape codes directly although we might switch to a tui library later.

**AI use**

- Networking code should be fully hand written, we could ask the AI for ideation
- DSP code could be generated if we run out of time, but we want this to be an opportunity to build it out ourselves
- UI / visualizers could be fully AI generated to save time given the scope of the project
