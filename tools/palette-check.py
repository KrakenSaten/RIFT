#!/usr/bin/env python3
"""Contrast and separation arithmetic for the RIFT palette.

Exists because three colour bugs on this project were invisible to the eye and
obvious to arithmetic, and because two review rounds have now produced contrast
figures that disagree with each other. A number in a comment cannot be checked;
a number this prints can.

RGB565 -> 8 bit uses bit replication, which is what the panel does.
"""
import math, itertools

def expand(v565):
    r = (v565 >> 11) & 0x1F
    g = (v565 >> 5) & 0x3F
    b = v565 & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))

def quant(r, g, b):
    return ((round(r * 31 / 255) & 0x1F) << 11) | \
           ((round(g * 63 / 255) & 0x3F) << 5) | (round(b * 31 / 255) & 0x1F)

def lin(c):
    s = c / 255.0
    return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4

def lum(rgb):
    r, g, b = rgb
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)

def contrast(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)

# ---- OKLab, for separation that predicts "can these be told apart"
def oklab(rgb):
    r, g, b = (lin(c) for c in rgb)
    l = 0.4122214708*r + 0.5363325363*g + 0.0514459929*b
    m = 0.2119034982*r + 0.6806995451*g + 0.1073969566*b
    s = 0.0883024619*r + 0.2817188376*g + 0.6299787005*b
    l_, m_, s_ = (math.copysign(abs(v) ** (1/3), v) for v in (l, m, s))
    return (0.2104542553*l_ + 0.7936177850*m_ - 0.0040720468*s_,
            1.9779984951*l_ - 2.4285922050*m_ + 0.4505937099*s_,
            0.0259040371*l_ + 0.7827717662*m_ - 0.8086757660*s_)

def de(a, b):
    la, aa, ba = oklab(a); lb, ab, bb = oklab(b)
    return math.sqrt((la-lb)**2 + (aa-ab)**2 + (ba-bb)**2)

def chroma(rgb):
    _, a, b = oklab(rgb)
    return math.sqrt(a*a + b*b)

def hue(rgb):
    _, a, b = oklab(rgb)
    return math.degrees(math.atan2(b, a)) % 360

def hue_gap(x, y):
    d = abs(hue(x) - hue(y)) % 360
    return min(d, 360 - d)

BLACK, WHITE = (0, 0, 0), (255, 255, 255)

ACCENT = 0xFA00
OK_NIGHT, OK_DAY = 0x3E40, 0x2B20   # day green moved in the September design round
NAMES = [0x73E0, 0x0429, 0x631E, 0xD170, 0x039C, 0xB81F,
         0x63EC, 0xE00B, 0xB1BC, 0x9334, 0x2BD7, 0xD814]

def report():
    print("== ink on an accent fill ==")
    acc = expand(ACCENT)
    print("accent 0x%04X draws rgb%s" % (ACCENT, acc))
    for label, v in (("white 0xFFFF", 0xFFFF), ("#202020 0x2104", 0x2104),
                     ("black 0x0000", 0x0000)):
        print("  %-16s %.2f:1" % (label, contrast(expand(v), acc)))
    print("  accent as text on white   %.2f:1" % contrast(acc, WHITE))
    print("  accent as text on black   %.2f:1" % contrast(acc, BLACK))

    print("\n== the twelve name colours ==")
    print("  #  RGB565  rgb              blk   wht   hue  chroma  nearest")
    worst = (99, None)
    for i, v in enumerate(NAMES):
        rgb = expand(v)
        cb, cw = contrast(rgb, BLACK), contrast(rgb, WHITE)
        near = min(((de(rgb, expand(o)), j) for j, o in enumerate(NAMES) if j != i))
        flag = "" if min(cb, cw) >= 4.5 else "   <-- FAILS 4.5:1"
        print("  %2d 0x%04X  rgb(%3d,%3d,%3d)  %.2f  %.2f  %3.0f  %.3f  %2d %.3f%s"
              % (i, v, rgb[0], rgb[1], rgb[2], cb, cw, hue(rgb), chroma(rgb),
                 near[1], near[0], flag))
        if near[0] < worst[0]: worst = (near[0], (i, near[1]))
    print("  worst pair: %d and %d at dE %.3f" % (worst[1][0], worst[1][1], worst[0]))

    print("\n== hue distance from the reserved colours ==")
    for i, v in enumerate(NAMES):
        rgb = expand(v)
        print("  %2d  accent %3.0f   night green %3.0f   day green %3.0f"
              % (i, hue_gap(rgb, expand(ACCENT)), hue_gap(rgb, expand(OK_NIGHT)),
                 hue_gap(rgb, expand(OK_DAY))))

    print("\n== day rule / night dim ==")
    print("  0x8C51 draws rgb%s" % (expand(0x8C51),))
    print("  #8C8C8C quantises to 0x%04X" % quant(0x8C, 0x8C, 0x8C))

    print("\n== RADAR waterfall night ramp ==")
    prev = None
    for v in (0x6B6D, 0x9CD3, 0xC618, 0xFFFF):
        rgb = expand(v)
        step = "" if prev is None else "  step %.2f" % contrast(rgb, prev)
        print("  0x%04X rgb%s  on black %5.2f%s" % (v, rgb, contrast(rgb, BLACK), step))
        prev = rgb
    print("  glyph floor #6E6E6E = %d; weakest step green = %d"
          % (0x6E, expand(0x6B6D)[1]))

CHANNELS = [0x73E0, 0x0429, 0x631E, 0xD170]

def channel_table():
    """The four channel identities, in the same columns as the name table.

    The hue column used to be HSV where the name table's was OKLab, so the same
    constant was described as 65 degrees in one comment and 115 in the other.
    OKLab in both now, because it is the space the separation figures are in and
    two hue conventions in one file is how that pair of numbers stopped being
    comparable.
    """
    acc, gn, gd = expand(ACCENT), expand(OK_NIGHT), expand(OK_DAY)
    print("//     RGB565  rgb              blk   wht   hue  chroma"
          "  dE accent  dE night grn  dE day grn")
    for i, v in enumerate(CHANNELS):
        rgb = expand(v)
        print("//  %d  0x%04X  rgb(%3d,%3d,%3d)  %.2f  %.2f  %3.0f  %.3f"
              "      %.3f         %.3f        %.3f"
              % (i + 1, v, rgb[0], rgb[1], rgb[2], contrast(rgb, BLACK),
                 contrast(rgb, WHITE), hue(rgb), chroma(rgb), de(rgb, acc),
                 de(rgb, gn), de(rgb, gd)))

def tables():
    """The two comment tables in RiftLogic.h, generated.

    The columns are the ones the admission test is actually stated in - contrast
    against both fields, hue distance from the three reserved colours, OKLab
    chroma, and the nearest other entry. The old tables carried a "from accent"
    and "from green" column that no standard metric reproduces: entry 0 was
    listed as 49 from the accent where the sRGB Euclidean distance is 153 and the
    hue gap is 81 degrees. Whatever produced those numbers, they could not be
    used to defend or attack an entry, which is the only thing a table like this
    is for.
    """
    acc, gn, gd = expand(ACCENT), expand(OK_NIGHT), expand(OK_DAY)
    print("//     RGB565  rgb              blk   wht   hue  chroma"
          "  accent  green  nearest")
    for i, v in enumerate(NAMES):
        rgb = expand(v)
        near = min(((de(rgb, expand(o)), j) for j, o in enumerate(NAMES) if j != i))
        print("//  %2d 0x%04X  rgb(%3d,%3d,%3d)  %.2f  %.2f  %3.0f  %.3f"
              "     %3.0f    %3.0f   %2d %.3f"
              % (i, v, rgb[0], rgb[1], rgb[2], contrast(rgb, BLACK),
                 contrast(rgb, WHITE), hue(rgb), chroma(rgb), hue_gap(rgb, acc),
                 min(hue_gap(rgb, gn), hue_gap(rgb, gd)), near[1], near[0]))

if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "--tables":
        tables()
    elif len(sys.argv) > 1 and sys.argv[1] == "--channels":
        channel_table()
    else:
        report()
