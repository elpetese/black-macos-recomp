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
mkdir black && cd black
git clone https://github.com/elpetese/black-macos-recomp.git
git clone https://github.com/elpetese/black-macos-runtime.git xboxrecomp
git -C xboxrecomp submodule update --init --recursive
pip3 install capstone unicorn
```

Keep them as siblings: `black-macos-recomp/` and `xboxrecomp/`
(or set `XBOXRECOMP_DIR`).

## 3. One command: disc -> playable build

```sh
cd black-macos-recomp
./reproduce.sh /path/to/your/Black.iso
```

`reproduce.sh` does, in order (read it; every step is a plain command):

1. **Extract the disc** with `tools.xiso unpack` into `game_files/`
   (git-ignored; never commit it) and copy `default.xbe` to the toolkit.
2. **Parse the XBE** (`tools.xbe_parser`) and unpack `analysis/black-analysis.tar.xz`.
   That archive holds only the function database for Black: addresses, sizes,
   call graph, labels, calling-convention guesses. It contains no code bytes,
   assets or text from the game. It is the result of the toolkit's analysis
   stages (below) refined over many boot/crash iterations, and it is what makes
   the lifted code match the working port.
3. **Lift to C** with `tools.recomp ... --weak-feedback-seeds`, writing
   `src/recomp/gen/` (local, git-ignored, ~1.5M lines, ~1 min).
4. **Build** with CMake into `build-mac/`.

Faster Metal (ANGLE) build: see section 9.

## 4. Where the analysis data comes from (to redo it yourself)

The archive is not magic; the toolkit stages that produce it are:

```sh
cd xboxrecomp
python3 -m tools.disasm game_files/default.xbe --text-only \
    --seed-functions ../black-macos-recomp/seeds/icall_targets.json
python3 -m tools.func_id game_files/default.xbe
python3 -m tools.abi_analysis game_files/default.xbe
```

`seeds/hand_seeds.json` (hand-found functions only reachable through
pointers) and `seeds/icall_targets.json` (761 indirect-call targets observed
at run time) feed the disassembler. A from-scratch run gives a similar but not
identical function set; the runtime loop below is how the set was grown:
run, read `ICALL`/`UNIMPL` lines, add targets to the seeds, re-run the stages,
re-lift, rebuild. Use the shipped archive unless you are extending the port.

## 5. Notes

- Results from the shipped archive are byte-identical to the author's
  generated code.
- Python 3.9+ works; the toolkit documents 3.10+.

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

## 9. Faster renderer: build ANGLE (Metal backend)

The default build uses desktop OpenGL (~15 fps in level 1 at 1920x1440). The
ANGLE build runs the same xemu renderer on OpenGL ES translated to Metal by
Google's ANGLE (~30 fps). ANGLE is not in this repository (it is large and has
its own licence); build it once and point CMake at the two libraries.

Tools: Xcode command line tools, Python 3, git, ~10 GB free disk, ~30-60 min.

```sh
# 1. depot_tools (Chromium's build tooling)
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PWD/depot_tools:$PATH"

# 2. ANGLE source
mkdir angle && cd angle
fetch angle            # or: gclient config + sync, per ANGLE's docs/DevSetup.md
cd angle

# 3. Configure: arm64, Metal backend only, release
gn gen out/Release --args='target_cpu="arm64" is_debug=false is_component_build=false angle_enable_metal=true angle_enable_gl=false angle_enable_vulkan=false angle_enable_swiftshader=false'

# 4. Build the two libraries the port needs
autoninja -C out/Release libEGL libGLESv2
ls out/Release/libEGL.dylib out/Release/libGLESv2.dylib
```

Then build the port against them:

```sh
cd ../../black-macos-recomp
cmake -S . -B build-angle -DCMAKE_BUILD_TYPE=Release \
      -DXEMU_ANGLE_LIB_DIR=/full/path/to/angle/angle/out/Release
cmake --build build-angle --target black-recomp -j8
./play.sh          # ANGLE=1 is the default; ANGLE=0 uses build-mac
```

CMake copies `libEGL.dylib` and `libGLESv2.dylib` next to the binary. The
ANGLE headers the port compiles against are already in
`xboxrecomp/third_party/angle_headers`.

Notes:
- Use ANGLE's own documented steps if the flags above have drifted
  (`docs/DevSetup.md` and `docs/BuildingAngle` in the ANGLE tree). The
  requirement is just: arm64 `libEGL.dylib` + `libGLESv2.dylib` with the Metal
  backend enabled.
- Do not copy the libraries out of someone else's app bundle and redistribute
  them; build your own or keep them local. They are git-ignored here.
- Verify it is ANGLE/Metal at run time: `build-angle/last-run.log` should
  show the ANGLE/EGL context being created. If the picture is wrong, try
  `FASTGPU=0 ./play.sh`.
- These ANGLE steps are the standard ANGLE procedure; they have not been
  re-run from scratch for this repo (the author's libraries came from a
  prebuilt copy).
