# black-recomp

Game project for Black (Xbox, 2006) on Apple Silicon. Needs the sibling
`../xboxrecomp` toolkit (branch `black-lifter-upstream`).

Generated code (`src/recomp/gen/`) is not in git; regenerate it from
`../xboxrecomp`:

    python3 -m tools.recomp game_files/default.xbe --game-name Black --all \
        --split 1000 --weak-feedback-seeds \
        --exclude-manual ../black-recomp/src/recomp_natives.c \
        --gen-dir ../black-recomp/src/recomp/gen

`--weak-feedback-seeds` is required. Build with CMake + Make, run with

    RECOMP_XEMU_GPU=1 RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 \
    RECOMP_FB_WINDOW=1 ./black-recomp "<extracted game folder>"
