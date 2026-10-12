#!/usr/bin/env python3
"""Baut das Workbench-Icon AmiFritzi.info: blaues Viereck, darin ein gelbes
Dreieck mit der Spitze nach unten. Nach dem Muster von AmiCode/tools/appicon.py,
aber ohne Bildvorlage und ohne PIL - das Motiv wird hier gezeichnet.

Zwei Fassungen in einer Datei, wie auf dem Amiga ueblich:
  klassisch  4 Workbench-Farben fuer alte icon.library. Gelb gibt es dort
             nicht, das Dreieck ist weiss.
  GlowIcon   eigene Palette (OS 3.5+, 3.2, PeterK), Rand durchsichtig,
             angewaehlt mit Lichtkranz

Dasselbe Bild dient als Schubladen-Icon fuer das Aminet-Archiv (Drawer.info).

    python3 tools/appicon.py      -> AmiFritzi.info, Drawer.info,
                                     build/icon_preview.png
"""

import os
import struct
import zlib

OUT = "AmiFritzi.info"
OUT_DRAWER = "Drawer.info"
PREVIEW = "build/icon_preview.png"
W, H = 48, 48
EDGE = 2                        # durchsichtiger Rand (Platz fuer den Lichtkranz)
NOPOS = -2147483648

# Dreieck: Oberkante bei TRI_TOP, TRI_ROWS Zeilen hoch, oben 2*TRI_HALF+2 breit,
# symmetrisch um die Mitte zwischen Spalte 23 und 24.
TRI_TOP, TRI_ROWS, TRI_HALF = 11, 24, 14

WB_GREY, WB_BLACK, WB_WHITE, WB_BLUE = 0, 1, 2, 3
WB_PAL = [(0x95, 0x95, 0x95), (0, 0, 0), (255, 255, 255), (0x3B, 0x67, 0xA2)]

# GlowIcon-Palette
G_T, G_LINE, G_BLUE, G_LIGHT, G_DARK, G_YELLOW, G_YLIGHT, G_RING = range(8)
GLOW_PAL = [
    (0x95, 0x95, 0x95),         # durchsichtig
    (0x0A, 0x1E, 0x3C),         # Umriss
    (0x1E, 0x5A, 0xB4),         # Blau
    (0x5A, 0x8C, 0xDC),         # Blau, Lichtkante oben/links
    (0x14, 0x3C, 0x7D),         # Blau, Schattenkante unten/rechts
    (0xFF, 0xCC, 0x00),         # Gelb
    (0xFF, 0xEB, 0x6E),         # Gelb, Lichtkante oben
    (0xFF, 0xE0, 0x80),         # Lichtkranz (angewaehlt)
]


def in_triangle(x, y):
    r = y - TRI_TOP
    if r < 0 or r >= TRI_ROWS:
        return False
    half = round((TRI_ROWS - 1 - r) * TRI_HALF / (TRI_ROWS - 1))
    return 23 - half <= x <= 24 + half


def grids():
    classic = [[WB_GREY] * W for _ in range(H)]
    glow = [[G_T] * W for _ in range(H)]
    lo, hi = EDGE, W - EDGE - 1

    for y in range(lo, hi + 1):
        for x in range(lo, hi + 1):
            if x in (lo, hi) or y in (lo, hi):
                c, g = WB_BLACK, G_LINE
            elif x == lo + 1 or y == lo + 1:
                c, g = WB_WHITE, G_LIGHT
            elif x == hi - 1 or y == hi - 1:
                c, g = WB_BLACK, G_DARK
            else:
                c, g = WB_BLUE, G_BLUE
            classic[y][x] = c
            glow[y][x] = g

    for y in range(H):
        for x in range(W):
            if not in_triangle(x, y):
                continue
            edge = any(not in_triangle(x + dx, y + dy)
                       for dy in (-1, 0, 1) for dx in (-1, 0, 1))
            if edge:
                classic[y][x] = WB_BLACK
                glow[y][x] = G_LINE
            elif y == TRI_TOP + 1:
                classic[y][x] = WB_WHITE
                glow[y][x] = G_YLIGHT
            else:
                classic[y][x] = WB_WHITE
                glow[y][x] = G_YELLOW
    return classic, glow


def glow_selected(gn):
    """Angewaehlt: jeder freie Punkt am Rand des Vierecks leuchtet."""
    out = [row[:] for row in gn]
    for y in range(H):
        for x in range(W):
            if gn[y][x] != G_T:
                continue
            if any(gn[y + dy][x + dx] != G_T
                   for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                   if 0 <= y + dy < H and 0 <= x + dx < W):
                out[y][x] = G_RING
    return out


def rle_pack(values, depth):
    """ByteRun1 ueber einen Bitstrom: 8-Bit-Steuerbytes, Werte depth Bit."""
    stream = []
    i, n = 0, len(values)
    while i < n:
        run = 1
        while i + run < n and values[i + run] == values[i] and run < 128:
            run += 1
        if run >= 2:
            stream.append((257 - run, 8))
            stream.append((values[i], depth))
            i += run
        else:
            lits = []
            while i < n and len(lits) < 128:
                if i + 1 < n and values[i + 1] == values[i]:
                    break
                lits.append(values[i])
                i += 1
            stream.append((len(lits) - 1, 8))
            stream += [(v, depth) for v in lits]

    buf, acc, nbits = bytearray(), 0, 0
    for v, w in stream:
        acc = (acc << w) | (v & ((1 << w) - 1))
        nbits += w
        while nbits >= 8:
            nbits -= 8
            buf.append((acc >> nbits) & 0xFF)
    if nbits:
        buf.append((acc << (8 - nbits)) & 0xFF)
    return bytes(buf)


def imag_chunk(grid, pal):
    depth = max(1, (len(pal) - 1).bit_length())
    img = rle_pack([p for row in grid for p in row], depth)
    palbytes = bytes(c for rgb in pal for c in rgb)
    body = struct.pack(">BBBBBBHH", G_T, len(pal) - 1, 0x01 | 0x02, 1, 0,
                       depth, len(img) - 1, len(palbytes) - 1)
    body += img + palbytes
    return (b"IMAG" + struct.pack(">I", len(body)) + body
            + (b"\0" if len(body) & 1 else b""))


def glow_form(glow):
    face = struct.pack(">BBBBH", W - 1, H - 1, 1, 0x11, len(GLOW_PAL) * 3 - 1)
    body = (b"ICON"
            + b"FACE" + struct.pack(">I", len(face)) + face
            + imag_chunk(glow, GLOW_PAL)
            + imag_chunk(glow_selected(glow), GLOW_PAL))
    return b"FORM" + struct.pack(">I", len(body)) + body


def planar(g, depth):
    words = (W + 15) // 16
    out = bytearray()
    for plane in range(depth):
        for y in range(H):
            bits = 0
            for x in range(W):
                if (g[y][x] >> plane) & 1:
                    bits |= 1 << (words * 16 - 1 - x)
            out += bits.to_bytes(words * 2, "big")
    return bytes(out)


def image_header(depth):
    return struct.pack(">hhhhhIBBI", 0, 0, W, H, depth, 1, 0x03, 0x00, 0)


def build_info(classic, glow, drawer=False):
    gadget = struct.pack(">IhhhhHHH", 0, 0, 0, W, H, 0x0006, 0x0001, 0x0001)
    gadget += struct.pack(">IIIIIHI", 1, 1, 0, 0, 0, 0, 0)
    gadget = gadget[:44]

    do = struct.pack(">HH", 0xE310, 1) + gadget
    do += bytes([2 if drawer else 3, 0])      # WBDRAWER / WBTOOL
    do += struct.pack(">II", 0, 0)
    do += struct.pack(">ii", NOPOS, NOPOS)
    do += struct.pack(">III", 1 if drawer else 0, 0, 0)  # DrawerData
    assert len(do) == 78, len(do)

    out = bytearray(do)
    if drawer:
        # DrawerData (56 Bytes) steht VOR den Bildern: Fenster 320x130 bei
        # 60,50, Mindest-/Hoechstgroesse 90x40 .. 640x200.
        nw = struct.pack(">hhhhBBIIIIIIhhhhH", 60, 50, 320, 130, 0xFF, 0xFF,
                         0, 0, 0, 0, 0, 0, 90, 40, 640, 200, 1)
        out += (nw[:48] + struct.pack(">ii", 0, 0) + b"\0" * 56)[:56]
    out += image_header(2) + planar(classic, 2)
    # Angewaehlt (klassisch): Farben vertauscht, wie Workbench es sonst macht
    out += image_header(2) + planar([[3 - v for v in row] for row in classic], 2)
    out += glow_form(glow)
    return bytes(out)


def write_png(path, rows):
    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d
                + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9))
                + chunk(b"IEND", b""))


def main():
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    classic, glow = grids()
    with open(OUT, "wb") as f:
        f.write(build_info(classic, glow))
    with open(OUT_DRAWER, "wb") as f:
        f.write(build_info(classic, glow, drawer=True))

    # Vorschau: klassisch | GlowIcon | angewaehlt, je vierfach
    z, gap = 4, 10
    sel = glow_selected(glow)
    pw = W * 3 * z + 2 * gap
    rows = [[WB_PAL[0]] * pw for _ in range(H * z)]
    for n, (g, pl) in enumerate(((classic, WB_PAL), (glow, GLOW_PAL),
                                 (sel, GLOW_PAL))):
        for y in range(H):
            for x in range(W):
                for dy in range(z):
                    for dx in range(z):
                        rows[y * z + dy][(n * W + x) * z + dx + n * gap] = pl[g[y][x]]
    os.makedirs(os.path.dirname(PREVIEW), exist_ok=True)
    write_png(PREVIEW, rows)
    print("%s und %s geschrieben (%dx%d), Vorschau %s"
          % (OUT, OUT_DRAWER, W, H, PREVIEW))


if __name__ == "__main__":
    main()
