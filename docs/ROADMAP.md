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
  machine, with its data ROM from `cx4.rom` or the end of the dump;
- the SPC7110 (decompression, data port, ALU and memory controller; its waits before a result are
  stop points), with a 7 MiB dump's expansion ROM;
- the ST018, ares's ARM7TDMI in ARM state, with a stop point at each clock step (every memory access
  and idle cycle), its firmware from `st018.rom` or the end of the dump;
- the BS-X cartridge's memory controller (MCC) and the memory pack slot on its board and the BS-LOROM,
  BS-HIROM and BS-SA1 boards, with flash packs and their command set. A `.bs` pack loads into the BS-X
  cartridge (`BS-X.bin` from the system folder). The base cartridge matches ares; a pack's own program
  is still to be tried by hand. The pack and the 512 KiB download RAM aren't kept between runs, the
  three mask-ROM packs are run as flash, a slotted game cartridge can't be given a pack from the
  frontend yet, and the Satellaview receiver isn't there;
- the Sufami Turbo's two slots. A `.st` cartridge loads into slot A of the base (`STBIOS.bin` from the
  system folder), and its save RAM is the game's save. Slot B maps its own save RAM where ares maps
  the slot's ROM; with that one difference copied, two cartridges match ares. The frontend can't put
  a cartridge in slot B yet;
- the Super Game Boy's ICD2, with Gambatte as its Game Boy. A `.gb` or `.gbc` game loads with
  `SGB1.sfc` and `sgb1.boot.rom` from the system folder, and the game's save RAM is the save. Gambatte
  tells the ICD2 of each line as its pixels are done and of each JOYP write. Next to ares the picture
  is the same until the two Game Boys' timing parts, a few hundred frames in, and looks the same after.
  The Game Boy's sound is averaged down to the DSP's rate and added to it. The SGB2 runs on its own
  oscillator; a cartridge's clock (MBC3, HuC3) isn't given to the frontend yet;
- the MSU-1, for a game with `game.msu` beside it (its tracks `game-1.pcm` and on). Its 44.1 kHz
  tracks are averaged down to the DSP's rate and added to it, muted while the DSP is, as on ares. Only
  a small test cartridge has been run so far: its picture matches ares and its track plays;
- the Epson RTC-4513 and the Sharp S-RTC. They keep time by the machine's clock; the frontend keeps
  them as RTC memory, and loading one moves it on by the time the game was off.

A new SPC7110 cartridge held "SPC7110 CHECK OK" at the end of its save RAM. Without it the game runs
its factory check program and asks for the power to be cut, as it does on ares, so a new save here
starts with it.
