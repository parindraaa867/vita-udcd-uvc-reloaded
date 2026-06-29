#!/usr/bin/env bash

rm -rf builds
mkdir -p builds

# Single universal build: auto-detects OLED (PCH-1000) vs LCD (PCH-2000) at
# runtime, and chooses single vs double framebuffer at runtime too
# (Compatibility mode in the app + automatic fallback when memory is tight).
make clean
make
cp udcd_uvc.skprx builds/udcd_uvc.skprx
cp udcd_uvc.txt builds/udcd_uvc.txt 2>/dev/null || true

make clean
