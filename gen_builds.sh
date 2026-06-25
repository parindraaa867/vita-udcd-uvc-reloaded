#!/usr/bin/env bash

rm -rf builds
mkdir -p builds

# Single universal build: auto-detects OLED (PCH-1000) vs LCD (PCH-2000) at
# runtime, so one file works on every model.
make clean
make
cp udcd_uvc.skprx builds/udcd_uvc.skprx

# Single-buffer fallback (PARALLEL=0), in case the double allocation can't be
# satisfied on a given unit.
make clean
make PARALLEL=0
cp udcd_uvc.skprx builds/udcd_uvc_singlebuf.skprx

make clean
