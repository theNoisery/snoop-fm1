#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Render the bitmap fonts to a C header (assets/fonts/):
  S  labels / units / status   Latin-1 32..255 at 1x
  L  large values / titles     32..95 (digits, signs, capitals), the same glyphs at 2x
Terminus Bold 8x16 (BDF, SIL OFL 1.1). TTF fonts also work through render()
(anti-aliased, tabular figures via the OpenType `tnum` feature).

Glyph format: per glyph an advance width, a bitmap width and an offset; the
bitmap starts FONT_PAD pixels left of the pen position (room for side
bearings); rows top to bottom. A font whose pixels are all on or off (the
Terminus pixel font) is stored at 1 bit a pixel (SLOOP 2.6: 5376 bytes
instead of 21504), the glyph's rows one after the other, high bit first, each
glyph from a whole byte, and marked bits = 1; an anti-aliased one (TTF) stays at
2 pixels per byte (high nibble first), alpha 0..15, bits = 0 (gfx.c cv_text).
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONTS = Path(__file__).resolve().parents[1] / "assets" / "fonts"
# (name, file, px, scale, pixel, first, last): pixel fonts are rendered without
# anti-aliasing at their design size and enlarged by an integer factor
# SLOOP 2.6: Terminus Bold (the same 8 x 16 cell and advances as the regular one: no layout moves), drawn in
# capitals only (gfx.c glyph); the regular ter-u16n.bdf stays for the web editor's font (gen_webfont.py)
SIZES = [("S", "ter-u16b.bdf", 16, 1, True, 32, 255),   # Latin-1 (Hügelton needs the umlaut)
         ("L", "ter-u16b.bdf", 16, 2, True, 32, 95)]   # values / titles: digits, signs, capitals
PAD = 2
# SNOOP 0.12: the OP look's fonts, Inter (SIL OFL 1.1, assets/fonts/Inter-LICENSE.txt; subset to Latin-1), anti-aliased,
# tabular figures. Each glyph cropped to its ink (gfx.c cv_text: bits 2 = 2 pixels a byte, bits 3 = run-length coded);
# (name, file, px, first, last, chars, rle, lower, cap): chars = None for every character first..last, else only those
# (the others empty); lower: the font folds capitals to lower case (the labels, as the OP-1's); cap: the capitals' top
# this far under the line's top, as Terminus' (S 3, L 6: Inter drops in where it was; ink above it, cut), None: the
# line from the highest ink
INTER = [("IS", "Inter-Medium-latin1.otf", 14, 32, 126, None, False, True, 3),     # labels, lists, units
         ("IV", "Inter-SemiBold-latin1.otf", 16, 32, 126, None, False, False, 3),  # values, names
         ("IB", "Inter-Bold-latin1.otf", 30, 32, 95, None, True, False, 6),        # titles, large values: capitals
         ("IM", "Inter-Bold-latin1.otf", 40, 37, 57, "%-.0123456789", True, False, None),   # the visualiser's digits:
         ("ID", "Inter-Bold-latin1.otf", 88, 37, 57, "%-.0123456789", True, False, None)]   # two in a column; the tempo


def upscale(px_, w, h, scale):
    """integer nearest-neighbour enlargement of a w x h glyph"""
    if scale == 1:
        return px_
    big = []
    for y in range(h):
        row = []
        for x in range(w):
            row += [px_[y * w + x]] * scale
        big += row * scale
    return big


def render_bdf(path, scale, first, last):
    """BDF bitmap font (Terminus 8x16): exact pixels, cell = the font bounding box."""
    lines = path.read_text(errors="replace").splitlines()
    fbb = next(l for l in lines if l.startswith("FONTBOUNDINGBOX")).split()
    cw, ch, fx, fy = int(fbb[1]), int(fbb[2]), int(fbb[3]), int(fbb[4])
    base = ch + fy                                     # rows above the baseline
    glyphs, i = {}, 0
    while i < len(lines):
        if lines[i].startswith("ENCODING"):
            code = int(lines[i].split()[1])
            j = i
            while not lines[j].startswith("BBX"):
                j += 1
            bw, bh, bx, by = map(int, lines[j].split()[1:5])
            dw = cw
            k = i
            while not lines[k].startswith("BITMAP"):
                if lines[k].startswith("DWIDTH"):
                    dw = int(lines[k].split()[1])
                k += 1
            rows = [int(r, 16) for r in lines[k + 1:k + 1 + bh]]
            nbits = ((bw + 7) // 8) * 8
            cell = [0] * (cw * ch)
            top = base - (by + bh)
            for r, bits in enumerate(rows):
                for c in range(bw):
                    if bits >> (nbits - 1 - c) & 1:
                        x, y = bx + c, top + r
                        if 0 <= x < cw and 0 <= y < ch:
                            cell[y * cw + x] = 15
            glyphs[code] = (dw, cell)
            i = k + bh
        i += 1
    if 0xDC not in glyphs and ord("U") in glyphs:      # Ü for "HÜGELTON": U with dots on its top row
        dw, cell = glyphs[ord("U")]
        cell = cell[:]
        lit = [x for y in range(ch) for x in range(cw) if cell[y * cw + x]]
        if lit:
            top = min(y for y in range(ch) for x in range(cw) if cell[y * cw + x])
            for x in range(cw):
                cell[top * cw + x] = 15 if x in (min(lit), max(lit)) else 0
            for x in range(cw):
                cell[(top + 1) * cw + x] = 0
            glyphs[0xDC] = (dw, cell)
    missing = [chr(c) for c in range(first, min(last, 126) + 1) if c not in glyphs]
    if missing:
        print(f"font {path.name}: no glyph for {''.join(missing)!r} (drawn as '?')")
    out = []
    for c in range(first, last + 1):
        dw, cell = glyphs.get(c, glyphs[ord("?")])
        bw = cw + 2 * PAD                              # keep the PAD convention of the TTF path
        px_ = []
        for y in range(ch):
            px_ += [0] * PAD + cell[y * cw:(y + 1) * cw] + [0] * PAD
        out.append((dw * scale, bw * scale, upscale(px_, bw, ch, scale)))
    return ch * scale, out


def render(ttf, px, scale, pixel, first, last):
    if ttf.endswith(".bdf"):
        return render_bdf(FONTS / ttf, scale, first, last)
    font = ImageFont.truetype(str(FONTS / ttf), px)
    feats = None if pixel else ["tnum"]
    asc, desc = font.getmetrics()
    top = max(0, font.getbbox("A8|(", features=feats)[1] - 1)
    h = asc + desc - top
    glyphs = []
    for c in range(first, last + 1):
        ch = chr(c)
        adv = int(round(font.getlength(ch, features=feats)))
        bw = adv + 2 * PAD
        img = Image.new("L", (bw, asc + desc), 0)
        ImageDraw.Draw(img).text((PAD, 0), ch, font=font, fill=255, features=feats)
        img = img.crop((0, top, bw, top + h))
        px_ = [(15 if v >= 128 else 0) if pixel else min(15, (v + 8) // 17) for v in img.tobytes()]
        glyphs.append((adv * scale, bw * scale, upscale(px_, bw, h, scale)))
    return h * scale, glyphs


def render_cropped(ttf, px, first, last, chars, cap):
    """each glyph cropped to its ink: (adv, x0 from the pen, y0 from the line's top, w, h, alpha 0..15 rows); the line
    from the top of the highest ink to the bottom of the lowest (digits only: as tall as a digit)"""
    font = ImageFont.truetype(str(FONTS / ttf), px)
    feats = ["tnum"]
    want = [chr(c) for c in range(first, last + 1)]
    m = px * 2
    out, tops, bots = [], [], []
    for ch in want:
        adv = int(round(font.getlength(ch, features=feats)))
        img = Image.new("L", (px * 4, px * 4), 0)
        if chars is None or ch in chars:
            ImageDraw.Draw(img).text((m, m), ch, font=font, fill=255, features=feats, anchor="ls")
        bb = img.getbbox()
        if bb and ch != " ":
            g = img.crop(bb)
            a = [min(15, (v + 8) // 17) for v in g.tobytes()]
            w, h = g.size
            # (a faint row or column at an edge: kept, it is part of the anti-aliasing)
            out.append([adv, bb[0] - m, bb[1] - m, w, h, a])
            tops.append(bb[1] - m)
            bots.append(bb[3] - m)
        else:
            out.append([adv if (chars is None or ch in chars) else 0, 0, 0, 0, 0, []])
    asc, desc = font.getmetrics()
    top = min(tops) if cap is None else out[ord("H") - first][2] - cap
    bot = max(bots)
    for g in out:
        g[2] -= top
        if g[2] < 0:                                   # (ink above the line: cut)
            cut = -g[2]
            g[5] = g[5][cut * g[3]:]
            g[4] -= cut
            g[2] = 0
    return bot - top, out


def rle(a):
    """nibbles: 1..14 an alpha; 0 n: n + 1 pixels of 0; 15 n: n + 1 pixels of 15 (runs over the rows)"""
    nib, i = [], 0
    while i < len(a):
        v = a[i]
        if v in (0, 15):
            j = i
            while j < len(a) and a[j] == v and j - i < 16:
                j += 1
            nib += [v, j - i - 1]
            i = j
        else:
            nib.append(v)
            i += 1
    if len(nib) & 1:
        nib.append(0)
    return [(nib[k] << 4) | nib[k + 1] for k in range(0, len(nib), 2)]


def unrle(d, n):
    out, nib = [], [x for b in d for x in (b >> 4, b & 15)]
    k = 0
    while len(out) < n:
        v = nib[k]
        k += 1
        if v in (0, 15):
            out += [v] * (nib[k] + 1)
            k += 1
        else:
            out.append(v)
    return out


def cropped(lines, name, ttf, px, first, last, chars, use_rle, lower, cap):
    h, glyphs = render_cropped(ttf, px, first, last, chars, cap)
    data, offs, box, adv = [], [], [], []
    for a, x0, y0, w, gh, al in glyphs:
        offs.append(len(data))
        assert -PAD <= x0 < 256 - PAD and 0 <= y0 < 256 and w < 256 and gh < 256
        box += [x0 + PAD, y0, w, gh]
        adv.append(a)
        if use_rle:
            enc = rle(al)
            assert unrle(enc, len(al)) == al, "the run-length code must give the glyph back"
            data += enc
        else:
            for y in range(gh):
                row = al[y * w:(y + 1) * w] + [0]
                for x in range(0, w, 2):
                    data.append((row[x] << 4) | row[x + 1])
    assert len(data) < 65536
    lines.append(f"static const uint8_t FONT_{name}_DATA[{len(data)}] = {{")
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    lines.append("};")
    n = len(glyphs)
    lines.append(f"static const uint16_t FONT_{name}_OFF[{n}] = {{" + ", ".join(map(str, offs)) + "};")
    lines.append(f"static const uint8_t FONT_{name}_ADV[{n}] = {{" + ", ".join(map(str, adv)) + "};")
    lines.append(f"static const uint8_t FONT_{name}_BOX[{4 * n}] = {{" + ", ".join(map(str, box)) + "};")
    lines.append(f"static const felucca_font_t FONT_{name} = {{ {h}, {PAD}, {first}, {last}, {2 if lower else 0}, "
                 f"FONT_{name}_ADV, FONT_{name}_BOX, FONT_{name}_OFF, FONT_{name}_DATA, {3 if use_rle else 2} }};")
    lines.append("")
    if lower:                                          # the same glyphs, the case as given (no bytes of its own)
        lines.append(f"static const felucca_font_t FONT_{name}C = {{ {h}, {PAD}, {first}, {last}, 0, "
                     f"FONT_{name}_ADV, FONT_{name}_BOX, FONT_{name}_OFF, FONT_{name}_DATA, {3 if use_rle else 2} }};")
        lines.append("")
    size = len(data) + n * 7
    print(f"font {name}: {ttf} {px}px h {h}, digit adv {adv[ord('0') - first]}, {len(data)} B + {n * 7} B")
    return size


def main(out):
    lines = [f"/* generated by tools/gen_font.py from {SIZES[0][1]} */",
             "#pragma once", "#include <stdint.h>", f"#define FONT_PAD {PAD}  /* x scale for L, see below */", ""]
    bases = {}                                         # file -> (name, first, h, glyphs, one) of its 1x font
    for name, ttf, px, scale, pixel, first, last in SIZES:
        h, glyphs = render(ttf, px, scale, pixel, first, last)
        if scale > 1 and pixel and ttf in bases:
            # SLOOP 2.4: a pixel font at 2x is the 1x font's own bitmaps, enlarged while drawing (gfx.c
            # cv_text): no second copy in flash (24 KB). Same glyphs, same pixels: checked here.
            bname, bfirst, bh, bglyphs, bone = bases[ttf]
            assert scale == 2 and first == bfirst and h == bh * 2, "L must be S at 2x, same first glyph"
            for (adv, bw, g), (badv, bbw, bg) in zip(glyphs, bglyphs):
                assert adv == badv * 2 and bw == bbw * 2 and g == upscale(bg, bbw, bh, 2)
            lines.append(f"static const uint8_t FONT_{name}_ADV[{len(glyphs)}] = {{" +
                         ", ".join(str(a) for a, _, _ in glyphs) + "};")
            lines.append(f"static const uint8_t FONT_{name}_BW[{len(glyphs)}] = {{" +
                         ", ".join(str(b) for _, b, _ in glyphs) + "};")
            lines.append(f"static const felucca_font_t FONT_{name} = {{ {h}, {PAD * scale}, {first}, {last}, 1, "
                         f"FONT_{name}_ADV, FONT_{name}_BW, FONT_{bname}_OFF, FONT_{bname}_DATA, {1 if bone else 0} }};")
            lines.append("")
            print(f"font {name}: {ttf} {px}px h {h}, digit adv {glyphs[ord('0') - first][0]}, "
                  f"the {bname} bitmaps at {scale}x (0 B)")
            continue
        data, offs = [], []
        one = all(v in (0, 15) for _, _, g in glyphs for v in g)   # on / off only: 1 bit a pixel
        for adv, bw, g in glyphs:
            offs.append(len(data))
            if one:
                acc = 0
                for i, v in enumerate(g + [0] * (-len(g) % 8)):
                    acc = (acc << 1) | (v != 0)
                    if i % 8 == 7:
                        data.append(acc)
                        acc = 0
                unp = [15 if data[offs[-1] + i // 8] >> (7 - i % 8) & 1 else 0 for i in range(len(g))]
                assert unp == g, "1-bit packing must give the glyph back"
                continue
            for y in range(h):
                row = g[y * bw:(y + 1) * bw] + [0]
                for x in range(0, bw, 2):
                    data.append((row[x] << 4) | row[x + 1])
        assert len(data) < 65536
        lines.append(f"static const uint8_t FONT_{name}_DATA[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append(f"static const uint16_t FONT_{name}_OFF[{len(offs)}] = {{" + ", ".join(map(str, offs)) + "};")
        lines.append(f"static const uint8_t FONT_{name}_ADV[{len(glyphs)}] = {{" +
                     ", ".join(str(a) for a, _, _ in glyphs) + "};")
        lines.append(f"static const uint8_t FONT_{name}_BW[{len(glyphs)}] = {{" +
                     ", ".join(str(b) for _, b, _ in glyphs) + "};")
        lines.append(f"static const felucca_font_t FONT_{name} = {{ {h}, {PAD * scale}, {first}, {last}, 0, "
                     f"FONT_{name}_ADV, FONT_{name}_BW, FONT_{name}_OFF, FONT_{name}_DATA, {1 if one else 0} }};")
        lines.append("")
        if scale == 1:
            bases[ttf] = (name, first, h, glyphs, one)
        print(f"font {name}: {ttf} {px}px h {h}, digit adv {glyphs[ord('0') - first][0]}, {len(data)} B")
    total = sum(cropped(lines, *f) for f in INTER)
    print(f"fonts Inter: {total} B")
    Path(out).write_text("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv[1])
