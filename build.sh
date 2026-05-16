#!/bin/bash
# Build fractalic screensaver for SymbOS using scc

SCC="${SCC:-../scc/bin/cc}"

"$SCC" fractalic.c fractalic_msx.s \
    -N "Fractalic" \
    -o fractali.sav \
    -h 512

python3 add_preview.py
