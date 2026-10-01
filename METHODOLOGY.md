# Methodology: reproducing the Black port yourself

This repository holds only the project-specific source (host, hand-written
guest functions, seeds, scripts). **No game code, disc image, `default.xbe`
or asset is distributed.** Everything below is done on your own machine from
your own legally obtained disc. The generated C (`src/recomp/gen/`) is also
not committed: you regenerate it from your own `default.xbe`.

## 0. How the port works

1. `default.xbe` is parsed and disassembled statically; functions are found
   (entry point, call targets, vtables, hand seeds) and each is lifted from
   x86 to C over a register/flag state machine. Nothing emulates the CPU.
2. The generated C is compiled natively for arm64 together with the
   `xboxrecomp` runtime: kernel services, file mapping (`D:\` -> your game
   folder), threads, and the GPU/audio replacements (xemu's NV2A renderer
   and MCPX APU).
3. Where the lifter cannot be right (or a function must be replaced), a
   hand-written version lives in `src/recomp_natives.c`.
4. Work then proceeds by evidence: **boot -> read the log -> find the guest
   function involved -> fix the runtime/native -> rebuild -> boot.** Never
   implement a subsystem before the game actually asks for it.

## 1. Prerequisites (macOS, Apple Silicon)

- Xcode command line tools (clang), CMake >= 3.20, Python 3.10+
- `pkg-config` libraries: SDL3, glib, libepoxy, xxhash
- Optional, for the faster Metal path: ANGLE `libEGL.dylib` and
  `libGLESv2.dylib` built with the Metal backend
- Your own copy of the game disc image

## 2. Get the sources

```sh
git clone https://github.com/elpetese/black-macos-recomp.git
git clone https://github.com/elpetese/black-macos-runtime.git xboxrecomp
git -C xboxrecomp checkout black-lifter-upstream     # if not the default branch
git -C xboxrecomp submodule update --init --recursive
```

Keep them as siblings: `black-macos-recomp/` and `xboxrecomp/`
(or pass `-DXBOXRECOMP_DIR=`).

## 3. Extract the disc (your own copy)

```sh
cd xboxrecomp
python3 -m tools.xiso unpack /path/to/your/Black.iso -o ../black-macos-recomp/game_files
```

`game_files/` must contain `default.xbe`. It is git-ignored; never commit it.
Also copy `default.xbe` to `xboxrecomp/game_files/default.xbe` for the lifter.

## 4. Lift the game to C

```sh
cd xboxrecomp
python3 -m tools.recomp game_files/default.xbe --game-name Black --all \
    --split 1000 --weak-feedback-seeds \
    --exclude-manual ../black-macos-recomp/src/recomp_natives.c \
    --gen-dir ../black-macos-recomp/src/recomp/gen
```

`--weak-feedback-seeds` is required. `seeds/hand_seeds.json` lists functions
reachable only through pointers that static analysis cannot see; this file
grows as the logs report `ICALL` / `UNIMPL` targets.

## 5. Build

```sh
cd ../black-macos-recomp
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target black-recomp -j8
```

Metal/ANGLE build: add `-DXEMU_ANGLE_LIB_DIR=/path/to/angle/libs` and use
`-B build-angle`.

## 6. Run

```sh
BLACK_GAME_DIR=/path/to/extracted/game ./play.sh   # default: ./game_files
```

See `README.md` for controls and environment variables.

## 7. The debugging loop (how every fix here was found)

| Symptom | Tool |
|---|---|
| Hang | `RECOMP_HANG_DUMP=10` dumps every thread after 10 s without a frame |
| Crash | `./crashhunt.sh` repeats runs and keeps each log in `crash-logs/` |
| Unknown indirect call | `ICALL`/`UNIMPL` lines -> add target to `seeds/hand_seeds.json`, regenerate |
| Wrong rendering | `RECOMP_DRAWDUMP=1`, `RECOMP_XEMU_DRAWLOG=L<n>` |
| Wrong input | `RECOMP_XINLOG=1` |
| Lifter bug suspected | build with `-DBLACK_SHADOW=ON`: every translated function is run against the original x86 (Unicorn) and diverging registers/memory are reported (`src/shadow.c`, `tools/shadow_wrap.py`) |
| Menu flow | `RECOMP_MENULOG=1` |

Patch discipline: one change per iteration, keep the log that proves it, and
prefer fixing the runtime/lifter over per-game hacks.

## 8. What you must not commit

`*.iso`, `*.xiso`, `*.xbe`, `game_files/`, `src/recomp/gen*/` (derived from the
game's code), screenshots/evidence dumps and crash logs. `.gitignore` covers
these.
