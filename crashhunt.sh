#!/bin/zsh
# Run Black and keep the log of that run instead of overwriting the last one.
#
# play.sh writes to build-mac/last-run.log every time, so a crash found while
# playing is gone by the next run. This keeps one timestamped log per run and
# turns on the diagnostics that explain a crash or a hang.
#
#   ./crashhunt.sh              normal, full screen
#   FAST=1 ./crashhunt.sh       cut the videos, reach the first level faster
#   HANG=10 ./crashhunt.sh      dump every thread after 10 s with no new frame
#   WINDOWED=1 ./crashhunt.sh   start in a window
#
# Log: crash-logs/run-<timestamp>.log
# The path is printed when it starts; quote it when reporting a crash.
set -e
HERE="${0:A:h}"
BIN="$HERE/build-mac/black-recomp"
GAME="${BLACK_GAME_DIR:-/path/to/project/black/extracted/Black (USA).xiso}"

[ -x "$BIN" ]  || { echo "Falta $BIN. Compila:  cmake --build \"$HERE/build-mac\" --target black-recomp"; exit 1; }
[ -f "$GAME/default.xbe" ] || { echo "No encuentro $GAME/default.xbe (exporta BLACK_GAME_DIR=<carpeta del juego>)"; exit 1; }

STAMP=$(date +%Y%m%d-%H%M%S)
LOGDIR="$HERE/crash-logs"
LOG="$LOGDIR/run-$STAMP.log"
mkdir -p "$LOGDIR"

mkdir -p "$LOGDIR/shots-$STAMP"

# Keep the last 20 runs; a single long session makes a lot of them.
# The (N) glob makes this a no-op on the first run instead of an error.
for old in "$LOGDIR"/run-*.log(N); do echo "$old"; done | tail -n +21 | xargs rm -f 2>/dev/null || true

echo "Log de esta corrida: $LOG"
echo "Si crashea, déjala tal cual y pásamela. No la borres."
echo "Ventana del juego: dale foco. Enter=Start Espacio=aceptar flechas=menús Tab=atrás"
echo "En el nivel: WASD mover, ratón o IJKL mirar, clic izq. disparar, clic der. apuntar, Esc suelta el ratón."
[ -n "$FAST" ] && echo "MODO RÁPIDO: vídeos recortados (FAST=$FAST)."
echo "Ctrl+C aquí cierra el juego."

cd "$HERE/build-mac"
if [ -n "$FAST" ]; then
    case "$FAST" in ''|*[!0-9]*|0|1) export RECOMP_XMV_PACKETS=3 ;; *) export RECOMP_XMV_PACKETS="$FAST" ;; esac
fi
[ -z "$WINDOWED" ] && export RECOMP_FULLSCREEN=1
[ -n "$WINDOW" ] && export RECOMP_WINDOW="$WINDOW"
[ -n "$FILTER" ] && export RECOMP_FILTER="$FILTER"
[ -n "$ASPECT" ] && export RECOMP_ASPECT="$ASPECT"
[ "${FASTGPU:-1}" != "0" ] && export RECOMP_NO_FLIP_DOWNLOAD=1 RECOMP_DIRTY_TRACKING=1
export RECOMP_SCALE="${SCALE:-3}"
export RECOMP_FOV="${FOV:-90}"
[ -n "$AA" ] && export RECOMP_AA="$AA"
# RECOMP_KEYBOARD=0: the window reads the keys; the terminal reader is not needed.

# RECOMP_HANG_DUMP=<s> is the one that matters for a hang: if no frame is
# presented for s seconds it dumps, per thread, the kernel call in flight, what
# it waits on, live registers, the stack, the DPC queue and a native sample.
# The default here is 20s, well inside the ~37s hang the port notes describe.
export RECOMP_HANG_DUMP="${HANG:-20}"
export RECOMP_HANG_DIR="$LOGDIR"

exec env RECOMP_FB_SNAP="$LOGDIR/shots-$STAMP/shot" RECOMP_DRAWDUMP=1 RECOMP_FPSLOG=1 \
         RECOMP_MENULOG=1 RECOMP_XINLOG=1 RECOMP_XEMU_GPU=1 RECOMP_VBLANK=1 \
         RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_FB_WINDOW=1 RECOMP_KEYBOARD=0 \
    ./black-recomp "$GAME" 2> "$LOG"
