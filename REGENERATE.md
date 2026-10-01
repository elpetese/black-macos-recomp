# Regenerating src/recomp/gen

Use `./reproduce.sh /path/to/Black.iso`; see METHODOLOGY.md. Manual form
(from `../xboxrecomp`, after unpacking `analysis/black-analysis.tar.xz` there):

    python3 -m tools.recomp game_files/default.xbe --game-name Black --all \
        --split 1000 --weak-feedback-seeds \
        --exclude-manual ../black-macos-recomp/src/recomp_natives.c \
        --gen-dir ../black-macos-recomp/src/recomp/gen

`--weak-feedback-seeds` is required.
