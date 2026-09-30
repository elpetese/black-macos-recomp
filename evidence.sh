#!/bin/zsh
# Save what the current/last run recorded: ./evidence.sh [name]
# -> evidence/<name>/  last-run.log, the screenshots, the draw dumps and the
#    pad log with timestamps, plus index.txt lining them up.
set -e
HERE="${0:A:h}"
NAME="${1:-$(date +%Y%m%d-%H%M%S)}"
OUT="$HERE/evidence/$NAME"
mkdir -p "$OUT/shots"
cp "$HERE/build-mac/last-run.log" "$OUT/last-run.log"
find "$HERE/evidence-live" -name "shot_*.bmp" -exec cp {} "$OUT/shots/" \;
grep -E "^\[XIN\]|^\[EVID\]|^\[XDRAW\] =====|^\[FPS\]|^\[MENU\]" "$OUT/last-run.log" > "$OUT/index.txt" || true
echo "Guardado en $OUT ($(ls "$OUT/shots" | wc -l | tr -d ' ') capturas, $(grep -c 'dumping frame' "$OUT/last-run.log") volcados)"
