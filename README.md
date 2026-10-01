# black-recomp

Static recompilation port of **Black** (Xbox, 2006) to macOS on Apple Silicon.

The game's x86 code is translated function-by-function to C and compiled
natively for arm64. The Xbox kernel, GPU (NV2A) and audio (MCPX APU) are
replaced by the runtime in the `xboxrecomp` toolkit, which is a required
sibling checkout. Nothing here emulates a CPU.

**No game code or assets are in this repository.** You must supply your own
legally obtained copy of the disc. Nothing of the game's is committed, in this
tree or in its history; `.gitignore` keeps `game_files/`, `*.xbe`, `*.iso`,
`*.xiso`, the build directory and the evidence screenshots out.

## Status

Boots to the title screen, plays the intro videos, reaches the first mission
and is playable: move, aim, shoot, reload, objectives. Rendering goes through
the NV2A renderer of xemu extracted from QEMU. The boot is still unstable
during video playback (about one run in three), camera drift after firing is
unresolved, and audio is silent because the APU's DSP56300 GP/EP is a stub.

The port's running notes live in the toolkit repository, in
`black-port-progreso-ajustado.md` and `BLACK_PORT_ESTADO_2026-09-19.md`.

## Layout

| Path | What it is |
|---|---|
| `src/main_posix.c` | Host: boot, XBE load, window, framebuffer present, crash handler |
| `src/recomp_natives.c` | Guest functions written by hand instead of translated |
| `src/recomp_manual.c` | Function-name lookup for the `[MENU]` log, ICALL and UNIMPL diagnostics |
| `src/shadow.c` | Optional differential check against the original x86 (`-DBLACK_SHADOW=ON`) |
| `seeds/hand_seeds.json` | Functions only reachable through pointers, which the analysis cannot find |
| `tools/shadow_wrap.py` | Generator wrapper for the shadow build |
| `play.sh` | Run it |
| `evidence.sh` | Screenshot and pad-state recording |

`src/recomp/gen/` is **generated** and not committed. Regenerate it from the
toolkit; see `REGENERATE.md`. Without it the project will not build.

## Quick start

```sh
mkdir black && cd black
git clone https://github.com/elpetese/black-macos-recomp.git
git clone https://github.com/elpetese/black-macos-runtime.git xboxrecomp
git -C xboxrecomp submodule update --init --recursive
pip3 install capstone unicorn
cd black-macos-recomp && ./reproduce.sh /path/to/your/Black.iso
BLACK_GAME_DIR=$PWD/game_files ./play.sh
```

`reproduce.sh` extracts your disc, lifts the game to C and builds it. It uses
`analysis/black-analysis.tar.xz`, a function database (addresses and call
graph only; no game code or assets). Details: [METHODOLOGY.md](METHODOLOGY.md).

## Build

Needs a sibling `../xboxrecomp` checkout, plus SDL3, glib, epoxy and
libxxhash via pkg-config. On macOS, CMake and clang from Xcode are enough.

```sh
git clone https://github.com/elpetese/black-macos-recomp.git
git clone https://github.com/elpetese/black-macos-runtime.git xboxrecomp   # the toolkit
git -C xboxrecomp submodule update --init --recursive

cd black-macos-recomp
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target black-recomp -j8
```

Full step-by-step reproduction (extract your disc, lift, build, debug): see [METHODOLOGY.md](METHODOLOGY.md).

`CMakeLists.txt` looks for the toolkit at `../xboxrecomp`; point
`-DXBOXRECOMP_DIR=` elsewhere if you keep it somewhere else.

### Metal renderer (ANGLE)

A second build runs xemu's renderer on OpenGL ES through ANGLE's Metal
backend: about 30 fps in level 1 at the default scale, against about 15 for
the OpenGL build. It needs a directory holding ANGLE's `libEGL.dylib` and
`libGLESv2.dylib` (Metal backend); both are copied next to the binary.

```sh
cmake -S . -B build-angle -DCMAKE_BUILD_TYPE=Release \
      -DXEMU_ANGLE_LIB_DIR=/path/to/angle/libs
cmake --build build-angle --target black-recomp -j8
./play.sh                 # ANGLE is the default; ANGLE=0 uses build-mac
```

## Run

```sh
./play.sh                 # full screen, Metal (ANGLE) build, short videos, 1920x1440
FAST=0 ./play.sh          # full videos (first level in ~7 min instead of ~1.5)
WINDOWED=1 ./play.sh       # start in a window
SCALE=2 ./play.sh         # 1280x960 if the level load ever freezes
ASPECT=4:3 ./play.sh      # the game renders widescreen; 4:3 adds side bars
FOV=70 ./play.sh          # the original uses 70 (default here is 90)
ANGLE=0 ./play.sh         # desktop OpenGL build (build-mac) instead of ANGLE
```

`play.sh` needs the game directory. It defaults to `./game_files`; see `METHODOLOGY.md` for the full reproduction steps. Override with
`BLACK_GAME_DIR=<folder containing default.xbe>`. The log of the run goes to
`build-angle/last-run.log` (`build-mac/` with `ANGLE=0`).

## Controls

The game window needs focus.

| Key | Xbox pad | In Black |
|---|---|---|
| W A S D | Left stick | Move |
| Mouse (or I J K L) | Right stick | Look / aim |
| Left click | Right trigger | Fire |
| Right click | Left trigger | Aim down sights (zoom) |
| E (or Space) | A | Use, pick up, open doors; accept in menus |
| C or Ctrl | B | Crouch |
| R | X | Reload |
| Wheel, 1 / 2 (or Q) | Y | Change weapon |
| G or side mouse button | White | Throw grenade |
| F or V | Black | Melee |
| B or X | Left-stick click | Fire mode |
| Shift | Right-stick click | Precision aim |
| Arrows, 3 / 4 | D-pad | Menus, quick select |
| Esc (or Enter) | Start | Pause (Esc also frees the mouse) |
| Tab | Back | Mission objectives |
| Cmd+Return | | Toggle full screen |

To play: Enter on the title, Space or E on START MISSION, NORMAL, the mission
briefing, then wait for "RENDEZVOUS WITH BLACK CELL" (about 40 s after the
level loads; the game ignores the pad before that).

## Environment variables

Set by `play.sh`; useful on their own for diagnosing a hang.

| Variable | What it does |
|---|---|
| `RECOMP_VBLANK=1` | **Required.** Without it the title waits for the NV2A interrupt forever |
| `RECOMP_AC97_READY=1` | AC'97 codec, DSP ACK thread, D3D pushbuffer fence. Without it the boot dies at `sub_00084F90` after 19 file opens instead of 52 |
| `RECOMP_XEMU_GPU=1` | The xemu NV2A renderer |
| `RECOMP_PB_EXEC=1` | Pushbuffer executor |
| `RECOMP_FB_WINDOW=1` | Framebuffer window |
| `RECOMP_USB=1` | Emulated OHCI, for the title's own USB stack |
| `RECOMP_AUDIO_TEST=1` | 440 Hz test tone through the APU |
| `RECOMP_AUDIOLOG=1` | Audio flow every 2 s: buffers, drops, underruns |
| `RECOMP_AUDIO_DUMP=<f>` | Every submitted buffer to a file, s16 stereo 48 kHz |
| `RECOMP_HANG_DUMP=<s>` | If no frame is presented for s seconds, dump every thread |
| `RECOMP_MENULOG=1` | Name the menu actions the front end dispatches |
| `RECOMP_XINLOG=1` | One line whenever the pad state the game reads changes |
| `RECOMP_DRAWDUMP=1` | Per-flip draw dumps (touch `/tmp/black_dump` for the next frame) |
| `RECOMP_FB_SNAP=<dir>/` | One screenshot per second |
| `RECOMP_KEEP_BLACK_PANEL=1` | Draw the opaque black panel the menus ask for; off by default, the filter that hides it is in `main_posix.c` |
| `RECOMP_FOV=<deg>` | Field of view |
| `RECOMP_XEMU_DRAWLOG=L<n>` | Draw log for the n-th frame with more than 300 draws |

## Known issues

- **Boot instability during videos** (high). About one run in three crashes or
  hangs before the menu: a crash in `sub_0013DD20` reaching `0x9xxxxxxx` when an
  XMV video ends, a main-loop crash with the stack overwritten, or a hang at
  ~37 s. Run with `RECOMP_HANG_DUMP`; several of the FPU-precision,
  per-thread dispatch and IRQL fixes made later may already reduce it.
- **Camera drifts to the floor** (high), after firing. Ruled out: the pad
  reports correct axes. The weapon-recoil recovery is overshooting.
- **Video quality** (medium). Heavy blocking; may be another SSE/MMX case in the
  recompiler.
- **Black panel and collapsed menu text** (medium). The filter in `main_posix.c`
  hides the panel, but the title's own menu logic is what asks for it, and it
  also collapses some text to a point. The pause menu is invisible because of it.
- **PS4 controller** (medium). SDL sees 0 joysticks; needs the SDL input
  subsystem and Input Monitoring permission on macOS.
- **Audio** (unresolved). The pipeline reaches SDL2 but the APU's DSP56300
  GP/EP is a stub. The sibling runtime now mixes even numbered APU bins left
  and odd numbered bins right, so Black's observed bins 20 and 19 reach
  separate channels. Its ADPCM decoder also accepts the reserved header byte
  and clamps the initial step index. These changes improve basic playback;
  DSP effects and a faithful final mix still need the real GP/EP. The
  `RECOMP_REAL_DSP=1` implementation is present but not wired into the build
  or APU dispatch yet. Run with `RECOMP_AUDIO_TEST=1` to verify host output;
  that tone bypasses the game's voice and DSP pipeline.

## Credits

- [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) — the
  toolkit this is built on, MIT, Copyright (c) 2026 sp00nz
- [xemu](https://github.com/xemu-project/xemu) — the MCPX APU and NV2A
  renderer, LGPL-2.1, Copyright (c) 2012 espes, 2018-2019 Jannik Vogel,
  2019-2025 Matt Borgerson
- [mborgerson/dsp56300](https://github.com/mborgerson/dsp56300) — DSP56300
  interpreter and JIT, MIT
- Ubisoft / Southend Interactive — *Black*. Not affiliated; no assets here.
