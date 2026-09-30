#!/bin/zsh
# Run Black and play it yourself (keyboard + mouse in the game window).
#   ./play.sh                 normal
#   RECOMP_MOUSE_SENS=800 ./play.sh    slower mouse look (default 1800)
#   (default: full screen; Cmd+Return toggles it any time)
#   WINDOWED=1 ./play.sh      start in a window instead
#   WINDOW=900 ./play.sh      window height in points (with WINDOWED=1)
#   ASPECT=4:3 ./play.sh      picture shape (default 16:9, the game renders
#                             widescreen; 4:3 = with side bars)
#   SCALE=2 ./play.sh         internal resolution: default 3 (1920x1440);
#                             2 = 1280x960 if the level load ever freezes
#   FOV=70 ./play.sh          field of view in degrees (default 90; the
#                             original game uses 70; aiming zoom stays
#                             proportional)
#   FASTGPU=1 ./play.sh       EXPERIMENTAL, off by default: GPU dirty tracking and no
#                             per-flip surface readback (+20% fps, not verified in
#                             effect-heavy scenes; turn it off if the picture is wrong)
#   AA=0 ./play.sh            no FXAA edge smoothing (on by default)
#   FILTER=nearest ./play.sh  hard pixels instead of smooth scaling
#   FAST=1 ./play.sh          cut every movie (logos, credits, mission
#                             cinematic) to its header + 3 packets, so the first
#                             level is reached in ~1.5 min instead of ~7.
#                             FAST=<n> keeps n packets (about 0.5-2 s each).
# Log of the run: build-mac/last-run.log
set -e
HERE="${0:A:h}"
BIN="$HERE/build-mac/black-recomp"
GAME="${BLACK_GAME_DIR:-/path/to/project/black/extracted/Black (USA).xiso}"

[ -x "$BIN" ]  || { echo "Falta $BIN. Compila primero:  make -C \"$HERE/build-mac\" black-recomp"; exit 1; }
[ -f "$GAME/default.xbe" ] || { echo "No encuentro $GAME/default.xbe (exporta BLACK_GAME_DIR=<carpeta del juego>)"; exit 1; }

echo "Black arrancando. Ventana del juego: dale foco y usa el teclado."
echo "Enter = Start (título)   Espacio = aceptar   flechas = menús   Tab = atrás"
echo "Pantalla completa por defecto (Cmd+Intro alterna). WINDOWED=1 para ventana."
echo "En el nivel: WASD mover, ratón o IJKL mirar, clic izq. disparar, clic der. apuntar, Esc suelta el ratón."
[ -n "$FAST" ] && echo "MODO RÁPIDO: vídeos recortados (FAST=$FAST). Sin FAST se reproducen completos."
echo "Ctrl+C aquí cierra el juego. Log: $HERE/build-mac/last-run.log"

cd "$HERE/build-mac"
# FAST=1 (or any non-number) -> RECOMP_XMV_PACKETS=3; FAST=<n> -> n; unset -> full movies.
if [ -n "$FAST" ]; then
    case "$FAST" in ''|*[!0-9]*|0|1) export RECOMP_XMV_PACKETS=3 ;; *) export RECOMP_XMV_PACKETS="$FAST" ;; esac
fi
[ -z "$WINDOWED" ] && export RECOMP_FULLSCREEN=1
[ -n "$WINDOW" ] && export RECOMP_WINDOW="$WINDOW"
[ -n "$FILTER" ] && export RECOMP_FILTER="$FILTER"
[ -n "$ASPECT" ] && export RECOMP_ASPECT="$ASPECT"
[ -n "$FASTGPU" ] && export RECOMP_NO_FLIP_DOWNLOAD=1 RECOMP_DIRTY_TRACKING=1
export RECOMP_SCALE="${SCALE:-3}"
export RECOMP_FOV="${FOV:-90}"
[ -n "$AA" ] && export RECOMP_AA="$AA"
# RECOMP_KEYBOARD=0: the window reads the keys; the terminal reader is not needed.
# The last three are diagnostics that cost nothing and make last-run.log explain
# a hang: menu action names, a dump of every thread if no frame is presented for
# 30 s, and one line whenever the pad state the game reads changes.
exec env RECOMP_FPSLOG=1 RECOMP_MENULOG=1 RECOMP_HANG_DUMP=1 RECOMP_XINLOG=1 RECOMP_XEMU_GPU=1 RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 \
         RECOMP_FB_WINDOW=1 RECOMP_KEYBOARD=0 \
    ./black-recomp "$GAME" 2> last-run.log
