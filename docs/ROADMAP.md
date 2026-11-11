# Roadmap

armSNES is ares's Super Nintendo core ported to C and laid out like armNES and armGBA. Unmodified
ares, at the commit the port was made from, is the reference it is compared with; where ares and
hardware tests disagree, the hardware tests win.

## How it differs from ares

- **No coroutines.** The CPU drives the machine. The sound CPU, the DSP and the PPU run only when the
  CPU needs them, up to its clock, and can stop in the middle of what they were doing. Everything
  they hold is plain data, so a saved state is the machine and nothing else.
- **One renderer to come.** ares has an exact dot renderer and a faster scanline renderer that needs
  per-game timing. The dot renderer is ported as the reference. The renderer for play will draw a
  line in spans, cut wherever a write lands in the middle of it, with SIMD inside the spans.
- **No game-specific code.** A table may say what hardware a cartridge has. Nothing may behave
  differently because of which game is running.

## Two modes

- **Exact** matches the dot renderer frame for frame and sample for sample.
- **Fast** keeps the same logic with coarser timing: the CPU's clock moves once per bus access, sound
  runs a scanline at a time, the renderer has a thread of its own, and the coprocessors are synced in
  slices. It may only simplify what can't be seen or heard, and it keeps every chip.

## Phases

| Phase | Work | Done when |
| --- | --- | --- |
| 0. Baseline | A reference build of ares with per-frame hashes. Speed of ares and the established cores on the target devices. | The numbers that set the speed targets are in hand. |
| 1. Core | CPU, bus, DMA, sound CPU, DSP and the dot renderer in C. Saved states. A minimal libretro core. | Test games match ares exactly; states are the same on ARM and x86; run-ahead leaves no trace. |
| 2. Renderer | The span renderer, with NEON and AVX2. | No pixel differs from the dot renderer across the library. |
| 3. Chips and slots | The coprocessors, then Satellaview, Sufami Turbo, Super Game Boy and the other controllers. | Each chip's games match ares. |
| 4. Product | Options, netplay, achievements, cheats, MSU-1, the standalone app and its packages. | First release, Exact only. |
| 5. Fast mode | As above, chosen by profile. | The library looks and sounds the same as Exact, at the speed of the fastest established core or better. |
| 6. Speed and packaging | Profile-driven work on Exact. | Full speed wherever phase 0 says it can be had. |

## Where it stands

Phase 0: the reference runner is in `tools/ref`. Device speed numbers are still to be taken.

Phase 1: the CPU, bus, DMA, sound CPU, DSP and dot renderer are ported, for cartridges without a
coprocessor, with saved states and a minimal libretro core. A state taken after a frame and loaded
again changes nothing that follows: run-ahead gives the same states, pictures and sound as a plain
run. PAL games and the reset button match ares too, and an ARM64 build gives the same states,
pictures and sound as x86. With the dot renderer a Raspberry Pi 5 runs at about 85 frames a second.

Phase 2: lines are drawn in spans, a layer at a time, and give the same machine state as the dot
renderer after every frame. SIMD comes with the speed work.

Phase 3: the board database is generated from ares's (`tools/boards.py`). Chips so far:

- the NEC DSP (DSP-1 to DSP-4, ST010, ST011), with firmware from the frontend's system folder or the
  end of the dump;
- the SA-1, on a 65816 that can stop at any clock step: `tools/resumable65816.py` makes it from the
  S-CPU's (`src/w65816r.inc`), with reads, writes and idle cycles as subroutines of the SA-1's state
  machine. A memory-pack slot on its board is left empty for now;
- the Super FX, a state machine of the same kind whose subroutines (a clock step that can finish a
  buffered ROM read or RAM write, a read that waits for the bus, the opcode fetch through the
  instruction cache, the pixel cache) keep their arguments and locals in a frame stack;
- the S-DD1 (decompression as DMA reads) and the OBC1;
- the Cx4, an HG51B whose clock steps, cache fills, page changes and DMA are subroutines of one state
  machine, with its data ROM from `cx4.rom` or the end of the dump.
