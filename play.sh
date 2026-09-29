#!/bin/zsh
# Run Black and play it yourself (keyboard + mouse in the game window).
#   ./play.sh                 normal
#   RECOMP_MOUSE_SENS=800 ./play.sh    slower mouse look (default 1800)
#   FULLSCREEN=1 ./play.sh    start full screen (Cmd+Return toggles it any time;
#                             the picture keeps 4:3 with black bars)
#   WINDOW=900 ./play.sh      window height in points (default: 1080, or the
#                             largest 4:3 that fits the screen)
#   ASPECT=4:3 ./play.sh      picture shape (default 16:9: the 4:3 game image is
#                             stretched to fill a widescreen; 4:3 = original)
#   SCALE=3 ./play.sh         render at 3x (1920x1440) for a sharp picture
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
echo "Pantalla completa: Cmd+Intro (o el botón verde). Mantiene 4:3 con barras negras."
echo "En el nivel: WASD mover, ratón o IJKL mirar, clic izq. disparar, clic der. apuntar, Esc suelta el ratón."
[ -n "$FAST" ] && echo "MODO RÁPIDO: vídeos recortados (FAST=$FAST). Sin FAST se reproducen completos."
echo "Ctrl+C aquí cierra el juego. Log: $HERE/build-mac/last-run.log"

cd "$HERE/build-mac"
# FAST=1 (or any non-number) -> RECOMP_XMV_PACKETS=3; FAST=<n> -> n; unset -> full movies.
if [ -n "$FAST" ]; then
    case "$FAST" in ''|*[!0-9]*|0|1) export RECOMP_XMV_PACKETS=3 ;; *) export RECOMP_XMV_PACKETS="$FAST" ;; esac
fi
[ -n "$FULLSCREEN" ] && export RECOMP_FULLSCREEN=1
[ -n "$WINDOW" ] && export RECOMP_WINDOW="$WINDOW"
[ -n "$FILTER" ] && export RECOMP_FILTER="$FILTER"
[ -n "$ASPECT" ] && export RECOMP_ASPECT="$ASPECT"
[ -n "$SCALE" ] && export RECOMP_SCALE="$SCALE"
# RECOMP_KEYBOARD=0: the window reads the keys; the terminal reader is not needed.
exec env RECOMP_XEMU_GPU=1 RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 \
         RECOMP_FB_WINDOW=1 RECOMP_KEYBOARD=0 \
    ./black-recomp "$GAME" 2> last-run.log
