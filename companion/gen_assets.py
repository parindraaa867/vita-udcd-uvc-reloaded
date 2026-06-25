#!/usr/bin/env python3
# Generates the LiveArea / icon assets for the companion app with no external
# image libraries (pure zlib PNG encoder). Run from the companion/ directory:
#   python3 gen_assets.py
import zlib, struct, os, math

# palette (R, G, B)
NAVY   = (18, 26, 38)
NAVY2  = (12, 17, 26)
TEAL   = (0, 214, 180)
LIGHT  = (235, 238, 245)

def canvas(w, h, rgb):
    return bytearray(bytes(rgb) * (w * h)), w, h

def put(buf, w, h, x, y, rgb, a=255):
    if 0 <= x < w and 0 <= y < h:
        i = (y * w + x) * 3
        if a >= 255:
            buf[i:i+3] = bytes(rgb)
        else:
            for k in range(3):
                buf[i+k] = (rgb[k] * a + buf[i+k] * (255 - a)) // 255

def fill_rect(buf, w, h, x, y, rw, rh, rgb, a=255):
    for yy in range(y, y + rh):
        for xx in range(x, x + rw):
            put(buf, w, h, xx, yy, rgb, a)

def disc(buf, w, h, cx, cy, r, rgb, a=255):
    # anti-aliased filled circle (2px soft edge)
    for yy in range(int(cy - r - 2), int(cy + r + 2)):
        for xx in range(int(cx - r - 2), int(cx + r + 2)):
            d = math.hypot(xx + 0.5 - cx, yy + 0.5 - cy)
            cov = r - d + 0.5
            if cov <= 0:
                continue
            cov = min(cov, 1.0)
            put(buf, w, h, xx, yy, rgb, int(a * cov))

def ring(buf, w, h, cx, cy, r, thick, rgb, a=255):
    for yy in range(int(cy - r - 2), int(cy + r + 2)):
        for xx in range(int(cx - r - 2), int(cx + r + 2)):
            d = math.hypot(xx + 0.5 - cx, yy + 0.5 - cy)
            cov = min(r - d + 0.5, 1.0, d - (r - thick) + 0.5)
            if cov > 0:
                put(buf, w, h, xx, yy, rgb, int(a * min(cov, 1.0)))

def lens(buf, w, h, cx, cy, r):
    # simple camera-lens / aperture motif
    ring(buf, w, h, cx, cy, r, max(2, r * 0.10), TEAL)
    disc(buf, w, h, cx, cy, r * 0.66, NAVY2)
    ring(buf, w, h, cx, cy, r * 0.66, max(2, r * 0.07), TEAL, 150)
    disc(buf, w, h, cx, cy, r * 0.30, TEAL)
    # little highlight
    disc(buf, w, h, cx - r * 0.30, cy - r * 0.30, r * 0.10, LIGHT, 180)

def save_png(path, buf, w, h):
    # The Vita's package installer (scePromoterUtil) only accepts INDEXED
    # ("8-bit colormap", PNG colour-type 3) images in sce_sys - truecolour RGB
    # is rejected at ~99% install with error 0x8010113D. So we quantise to a
    # palette of <=256 colours, reducing the quantisation step if needed.
    def build_palette(shift):
        mask = (0xFF << shift) & 0xFF
        pal_index, palette, indices = {}, [], bytearray(w * h)
        for i in range(w * h):
            r = buf[i*3] & mask; g = buf[i*3+1] & mask; b = buf[i*3+2] & mask
            # spread back toward full range so white stays white
            r |= r >> (8 - shift) if shift else 0
            g |= g >> (8 - shift) if shift else 0
            b |= b >> (8 - shift) if shift else 0
            key = (r, g, b)
            idx = pal_index.get(key)
            if idx is None:
                if len(palette) >= 256:
                    return None
                idx = len(palette); pal_index[key] = idx; palette.append(key)
            indices[i] = idx
        return palette, indices

    res = None
    for shift in (3, 4, 5):           # 32 -> 16 -> 8 levels per channel
        res = build_palette(shift)
        if res:
            break
    palette, indices = res

    raw = bytearray()
    for y in range(h):
        raw.append(0)                 # filter: none
        raw += indices[y*w:(y+1)*w]
    comp = zlib.compress(bytes(raw), 9)

    def chunk(typ, data):
        return (struct.pack('>I', len(data)) + typ + data +
                struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 3, 0, 0, 0)   # 8-bit, indexed
    plte = bytearray()
    for (r, g, b) in palette:
        plte += bytes((r, g, b))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
                chunk(b'PLTE', bytes(plte)) + chunk(b'IDAT', comp) +
                chunk(b'IEND', b''))
    print("wrote", path, f"{w}x{h} ({len(palette)} colors)")

# --- icon0.png : 128x128, lens on navy ---
b, w, h = canvas(128, 128, NAVY)
fill_rect(b, w, h, 0, 0, 128, 4, TEAL, 60)        # top accent
fill_rect(b, w, h, 0, 124, 128, 4, NAVY2)          # bottom shade
lens(b, w, h, 64, 64, 42)
save_png("sce_sys/icon0.png", b, w, h)

# --- startup.png : 280x158, smaller lens, centered ---
b, w, h = canvas(280, 158, NAVY)
lens(b, w, h, 140, 74, 40)
fill_rect(b, w, h, 90, 132, 100, 4, TEAL, 200)     # little underline accent
save_png("sce_sys/livearea/contents/startup.png", b, w, h)

# --- bg.png : 840x500, solid navy + one accent stripe (kept simple) ---
b, w, h = canvas(840, 500, NAVY)
fill_rect(b, w, h, 0, 360, 840, 3, TEAL, 130)      # single thin accent line
lens(b, w, h, 700, 150, 70)                         # subtle motif top-right
save_png("sce_sys/livearea/contents/bg.png", b, w, h)
