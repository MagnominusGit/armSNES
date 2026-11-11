# armSNES

A Super Nintendo core in C for ARM and x86 handhelds, ported from [ares](https://github.com/ares-emulator/ares).

It is early work. What is here runs games on plain cartridge boards (LoROM, HiROM and their extended
forms, with or without save RAM), and on boards with an NEC DSP (DSP-1 to DSP-4, ST010, ST011), an
SA-1 or a Super FX. It gives the same picture and sound as ares's most accurate renderer, frame for
frame and sample for sample. Games with a DSP need its firmware (`dsp1.rom`, `dsp1b.rom`, `st010.rom`
and so on, or the `.program.rom` and `.data.rom` halves) in the frontend's system folder, unless the
dump carries it. It builds as a libretro core with saved states; the standalone app comes later. The
plan is in [docs/ROADMAP.md](docs/ROADMAP.md).

## Build

```sh
cmake -S . -B build
cmake --build build
build/snesrun game.sfc 600
```

`build/armsnes_libretro.so` is the core.

`tools/ref/build.sh` builds `aresrun`, the same runner on unmodified ares, to compare against.

## License

GPL-2.0-or-later; see [LICENSE](LICENSE). The code ported from ares keeps ares's notice, in
[LICENSES/ISC-ares.txt](LICENSES/ISC-ares.txt).

armSNES comes with no games. Play only games you own. It is not affiliated with or endorsed by
Nintendo.
