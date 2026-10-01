#!/usr/bin/env bash
# Rebuild the Black port from your own disc image.
#   ./reproduce.sh /path/to/your/Black.iso
# Expects the toolkit as a sibling directory (../xboxrecomp). Nothing from the
# game is downloaded or committed: the disc is yours, the generated C stays
# local (git-ignored). Needs: python3 (+ pip install capstone unicorn), cmake,
# clang, and SDL3, glib, libepoxy, xxhash via pkg-config.
set -euo pipefail
ISO="${1:?usage: $0 /path/to/Black.iso}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TK="${XBOXRECOMP_DIR:-$HERE/../xboxrecomp}"
[ -d "$TK/tools" ] || { echo "toolkit not found at $TK (see METHODOLOGY.md step 2)"; exit 1; }
cd "$TK"

echo "== 1. extract the disc into $HERE/game_files"
mkdir -p "$HERE/game_files" game_files
python3 -m tools.xiso unpack "$ISO" -o "$HERE/game_files"
cp "$HERE/game_files/default.xbe" game_files/default.xbe

echo "== 2. analysis metadata (addresses/call graph only, no game code)"
python3 -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json >/dev/null
tar -xJf "$HERE/analysis/black-analysis.tar.xz" -C "$TK"

echo "== 3. lift to C"
python3 -m tools.recomp game_files/default.xbe --game-name Black --all \
    --split 1000 --weak-feedback-seeds \
    --exclude-manual "$HERE/src/recomp_natives.c" \
    --gen-dir "$HERE/src/recomp/gen"

echo "== 4. build"
cd "$HERE"
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target black-recomp -j8
echo "done. Run: ./play.sh   (ANGLE=0 ./play.sh for this OpenGL build)"
