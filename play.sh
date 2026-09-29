#!/bin/zsh
# Run Black and play it yourself (keyboard + mouse in the game window).
#   ./play.sh                 normal
#   RECOMP_MOUSE_SENS=800 ./play.sh    slower mouse look (default 1800)
# Log of the run: build-mac/last-run.log
set -e
HERE="${0:A:h}"
BIN="$HERE/build-mac/black-recomp"
GAME="${BLACK_GAME_DIR:-/path/to/project/black/extracted/Black (USA).xiso}"

[ -x "$BIN" ]  || { echo "Falta $BIN. Compila primero:  make -C \"$HERE/build-mac\" black-recomp"; exit 1; }
[ -f "$GAME/default.xbe" ] || { echo "No encuentro $GAME/default.xbe (exporta BLACK_GAME_DIR=<carpeta del juego>)"; exit 1; }

echo "Black arrancando. Ventana del juego: dale foco y usa el teclado."
echo "Enter = Start (título)   Espacio = aceptar   flechas = menús   Tab = atrás"
echo "En el nivel: WASD mover, ratón o IJKL mirar, clic izq. disparar, clic der. apuntar, Esc suelta el ratón."
echo "Ctrl+C aquí cierra el juego. Log: $HERE/build-mac/last-run.log"

cd "$HERE/build-mac"
# RECOMP_KEYBOARD=0: the window reads the keys; the terminal reader is not needed.
exec env RECOMP_XEMU_GPU=1 RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 \
         RECOMP_FB_WINDOW=1 RECOMP_KEYBOARD=0 \
    ./black-recomp "$GAME" 2> last-run.log
