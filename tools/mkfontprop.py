#!/usr/bin/env python3
"""Rasterise TrueType faces into the anti-aliased proportional font tables
used by the browser (src/fontprop.c).

This parses and rasterises TrueType itself rather than leaning on freetype or
Pillow, because the build box has neither and installing them is not worth a
build dependency for something that runs once and commits its output. Only
the parts of the format the ASCII range actually needs are implemented:
cmap format 4, simple and composite glyf outlines, loca and hmtx.

Coverage is computed by supersampling the filled outline and is emitted as
one byte of alpha per pixel, so the kernel blends glyphs against whatever is
behind them instead of punching hard 1-bit pixels into a dark background.
"""
import struct
import sys

SS = 4          # supersampling factor per axis
FIRST, LAST = 32, 126


# ------------------------------------------------------------------ parsing

class TTF:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        if self.d[:4] == b"ttcf":
            off = struct.unpack(">I", self.d[12:16])[0]
        else:
            off = 0
        num = struct.unpack(">H", self.d[off + 4:off + 6])[0]
        self.tables = {}
        for i in range(num):
            rec = off + 12 + 16 * i
            tag = self.d[rec:rec + 4].decode("latin-1")
            o, ln = struct.unpack(">II", self.d[rec + 8:rec + 16])
            self.tables[tag] = (o, ln)

        head = self.tables["head"][0]
        self.upem = struct.unpack(">H", self.d[head + 18:head + 20])[0]
        self.loca_long = struct.unpack(">h", self.d[head + 50:head + 52])[0]

        hhea = self.tables["hhea"][0]
        self.ascender = struct.unpack(">h", self.d[hhea + 4:hhea + 6])[0]
        self.descender = struct.unpack(">h", self.d[hhea + 6:hhea + 8])[0]
        self.num_hmetrics = struct.unpack(">H", self.d[hhea + 34:hhea + 36])[0]

        self.num_glyphs = struct.unpack(
            ">H", self.d[self.tables["maxp"][0] + 4:self.tables["maxp"][0] + 6])[0]

        self._read_loca()
        self._read_cmap()

    def _read_loca(self):
        o, _ = self.tables["loca"]
        n = self.num_glyphs + 1
        if self.loca_long:
            self.loca = list(struct.unpack(">%dI" % n, self.d[o:o + 4 * n]))
        else:
            short = struct.unpack(">%dH" % n, self.d[o:o + 2 * n])
            self.loca = [v * 2 for v in short]

    def _read_cmap(self):
        o, _ = self.tables["cmap"]
        n = struct.unpack(">H", self.d[o + 2:o + 4])[0]
        best = None
        for i in range(n):
            rec = o + 4 + 8 * i
            pid, eid, sub = struct.unpack(">HHI", self.d[rec:rec + 8])
            if (pid, eid) in ((3, 1), (0, 3), (0, 4), (3, 10), (0, 6)):
                best = o + sub
                break
            if best is None and pid == 0:
                best = o + sub
        if best is None:
            sys.exit("no usable cmap subtable")

        fmt = struct.unpack(">H", self.d[best:best + 2])[0]
        self.cmap = {}
        if fmt != 4:
            sys.exit("cmap format %d not supported" % fmt)

        seg2 = struct.unpack(">H", self.d[best + 6:best + 8])[0]
        seg = seg2 // 2
        base = best + 14
        end = struct.unpack(">%dH" % seg, self.d[base:base + seg2])
        base += seg2 + 2
        start = struct.unpack(">%dH" % seg, self.d[base:base + seg2])
        base += seg2
        delta = struct.unpack(">%dh" % seg, self.d[base:base + seg2])
        base += seg2
        ro_at = base
        rangeoff = struct.unpack(">%dH" % seg, self.d[base:base + seg2])

        for i in range(seg):
            for c in range(start[i], min(end[i], 0xFFFE) + 1):
                if rangeoff[i] == 0:
                    g = (c + delta[i]) & 0xFFFF
                else:
                    addr = ro_at + 2 * i + rangeoff[i] + 2 * (c - start[i])
                    g = struct.unpack(">H", self.d[addr:addr + 2])[0]
                    if g:
                        g = (g + delta[i]) & 0xFFFF
                if g:
                    self.cmap[c] = g

    def advance(self, gid):
        o, _ = self.tables["hmtx"]
        if gid < self.num_hmetrics:
            return struct.unpack(">H", self.d[o + 4 * gid:o + 4 * gid + 2])[0]
        return struct.unpack(">H", self.d[o + 4 * (self.num_hmetrics - 1):
                                          o + 4 * (self.num_hmetrics - 1) + 2])[0]

    # Returns a list of closed contours, each a list of (x, y) in font units.
    def contours(self, gid, depth=0):
        if depth > 4 or gid + 1 >= len(self.loca):
            return []
        go, _ = self.tables["glyf"]
        s, e = go + self.loca[gid], go + self.loca[gid + 1]
        if e <= s:
            return []

        ncont = struct.unpack(">h", self.d[s:s + 2])[0]
        if ncont < 0:
            return self._composite(s + 10, depth)

        p = s + 10
        ends = struct.unpack(">%dH" % ncont, self.d[p:p + 2 * ncont])
        p += 2 * ncont
        ilen = struct.unpack(">H", self.d[p:p + 2])[0]
        p += 2 + ilen

        npts = (ends[-1] + 1) if ncont else 0
        flags = []
        while len(flags) < npts:
            f = self.d[p]; p += 1
            flags.append(f)
            if f & 0x08:
                r = self.d[p]; p += 1
                flags.extend([f] * r)
        flags = flags[:npts]

        xs, v = [], 0
        for f in flags:
            if f & 0x02:
                dx = self.d[p]; p += 1
                v += dx if (f & 0x10) else -dx
            elif not (f & 0x10):
                v += struct.unpack(">h", self.d[p:p + 2])[0]; p += 2
            xs.append(v)

        ys, v = [], 0
        for f in flags:
            if f & 0x04:
                dy = self.d[p]; p += 1
                v += dy if (f & 0x20) else -dy
            elif not (f & 0x20):
                v += struct.unpack(">h", self.d[p:p + 2])[0]; p += 2
            ys.append(v)

        out = []
        start = 0
        for end in ends:
            pts = [(xs[i], ys[i], bool(flags[i] & 1)) for i in range(start, end + 1)]
            start = end + 1
            if pts:
                out.append(flatten(pts))
        return out

    def _composite(self, p, depth):
        out = []
        while True:
            flags, gi = struct.unpack(">HH", self.d[p:p + 4])
            p += 4
            if flags & 0x0001:
                a1, a2 = struct.unpack(">hh", self.d[p:p + 4]); p += 4
            else:
                a1, a2 = struct.unpack(">bb", self.d[p:p + 2]); p += 2

            sx = sy = 1.0
            s01 = s10 = 0.0
            if flags & 0x0008:
                sx = sy = f2dot14(self.d, p); p += 2
            elif flags & 0x0040:
                sx = f2dot14(self.d, p); sy = f2dot14(self.d, p + 2); p += 4
            elif flags & 0x0080:
                sx = f2dot14(self.d, p); s01 = f2dot14(self.d, p + 2)
                s10 = f2dot14(self.d, p + 4); sy = f2dot14(self.d, p + 6); p += 8

            dx, dy = (a1, a2) if (flags & 0x0002) else (0, 0)
            for c in self.contours(gi, depth + 1):
                out.append([(x * sx + y * s10 + dx, x * s01 + y * sy + dy)
                            for (x, y) in c])
            if not (flags & 0x0020):
                break
        return out


def f2dot14(d, p):
    return struct.unpack(">h", d[p:p + 2])[0] / 16384.0


# Expands a TrueType point run into a polyline. Consecutive off-curve points
# imply an on-curve point at their midpoint, which is the one piece of the
# format that trips up a naive reader.
def flatten(pts, steps=8):
    n = len(pts)
    if not any(p[2] for p in pts):
        mx = ((pts[0][0] + pts[-1][0]) / 2.0, (pts[0][1] + pts[-1][1]) / 2.0, True)
        pts = [mx] + pts
        n += 1

    first = next(i for i, p in enumerate(pts) if p[2])
    pts = pts[first:] + pts[:first]

    out = [(float(pts[0][0]), float(pts[0][1]))]
    i = 1
    cur = (float(pts[0][0]), float(pts[0][1]))
    while i <= n:
        px, py, on = pts[i % n]
        if on:
            out.append((float(px), float(py)))
            cur = (float(px), float(py))
            i += 1
            continue

        nx, ny, non = pts[(i + 1) % n]
        if not non:
            nx, ny = (px + nx) / 2.0, (py + ny) / 2.0
            step = 1
        else:
            step = 2
        for s in range(1, steps + 1):
            t = s / float(steps)
            u = 1.0 - t
            out.append((u * u * cur[0] + 2 * u * t * px + t * t * nx,
                        u * u * cur[1] + 2 * u * t * py + t * t * ny))
        cur = (nx, ny)
        i += step
    return out


# -------------------------------------------------------------- rasterising

# Scanline fill with a non-zero winding rule, sampled SS times per pixel in
# each axis. Coverage is the fraction of subsamples inside the outline.
def rasterise(contours, scale, ox, oy, w, h, slant=0.0):
    edges = []
    for c in contours:
        pts = [((x * scale) + (y * scale * slant) + ox, oy - y * scale) for (x, y) in c]
        for i in range(len(pts)):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % len(pts)]
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    if not edges:
        return None

    acc = [[0] * w for _ in range(h)]
    for sy in range(h * SS):
        y = (sy + 0.5) / SS
        xs = []
        for (x0, y0, x1, y1) in edges:
            if (y0 <= y < y1) or (y1 <= y < y0):
                t = (y - y0) / (y1 - y0)
                xs.append((x0 + t * (x1 - x0), 1 if y1 > y0 else -1))
        if not xs:
            continue
        xs.sort()

        wind = 0
        spans = []
        for i in range(len(xs) - 1):
            wind += xs[i][1]
            if wind != 0:
                spans.append((xs[i][0], xs[i + 1][0]))
        if not spans:
            continue

        row = acc[sy // SS]
        for (xa, xb) in spans:
            a = int(xa * SS)
            b = int(xb * SS)
            if b <= a:
                continue
            for sx in range(max(a, 0), min(b, w * SS)):
                row[sx // SS] += 1

    m = SS * SS
    return [[min(255, (v * 255 + m // 2) // m) for v in r] for r in acc]


def trim(cov):
    h = len(cov)
    w = len(cov[0]) if h else 0
    top, bot, left, right = h, -1, w, -1
    for y in range(h):
        for x in range(w):
            if cov[y][x]:
                top = min(top, y); bot = max(bot, y)
                left = min(left, x); right = max(right, x)
    if bot < 0:
        return None
    return (left, top,
            [row[left:right + 1] for row in cov[top:bot + 1]])


# ------------------------------------------------------------------- output

class Face:
    def __init__(self, name, path, size, slant=0.0, embolden=0):
        self.name = name
        self.size = size
        self.slant = slant
        self.embolden = embolden
        self.ttf = TTF(path)
        self.path = path

    def build(self):
        f = self.ttf
        scale = self.size / float(f.upem)
        # Round the cell to whole pixels and leave a pixel of leading, so
        # stacked lines never touch even where an ascender meets a descender.
        ascent = int(round(f.ascender * scale))
        descent = int(round(-f.descender * scale))
        height = ascent + descent + 1

        pad = 4
        bw = int(self.size * 2.2) + pad * 2
        bh = height + pad * 2

        glyphs, blob = [], bytearray()
        for code in range(FIRST, LAST + 1):
            gid = f.cmap.get(code, 0)
            adv = int(round(f.advance(gid) * scale)) + self.embolden
            cont = f.contours(gid)
            cov = rasterise(cont, scale, pad, ascent + pad, bw, bh, self.slant) if cont else None
            t = trim(cov) if cov else None

            if t is None:
                glyphs.append((adv, 0, 0, 0, 0, 0))
                continue

            left, top, bm = t
            if self.embolden:
                bm = smear(bm, self.embolden)
            glyphs.append((adv, len(bm[0]), len(bm), left - pad, top - pad, len(blob)))
            for row in bm:
                blob.extend(row)

        self.glyphs = glyphs
        self.blob = blob
        self.height = height
        self.ascent = ascent
        return self


# Synthetic emboldening: OR the glyph with copies of itself shifted right,
# taking the maximum coverage. Cheaper than shipping a second outline set and
# visually indistinguishable at these sizes.
def smear(bm, n):
    h = len(bm)
    w = len(bm[0]) + n
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(len(bm[0])):
            v = bm[y][x]
            if not v:
                continue
            for d in range(n + 1):
                if out[y][x + d] < v:
                    out[y][x + d] = v
    return out


def emit(faces, path):
    o = []
    o.append("// src/fontprop.c -- anti-aliased proportional font tables.")
    o.append("//")
    o.append("// Generated by tools/mkfontprop.py, which rasterises the outlines from the")
    o.append("// DejaVu and Carlito faces shipped with Debian. One byte of coverage per")
    o.append("// pixel, so glyphs blend with the background rather than punching 1-bit")
    o.append("// holes in it -- at 16px on a dark theme that is the whole difference")
    o.append("// between text that reads as a document and text that reads as a console.")
    o.append("//")
    o.append("// Do not edit by hand: regenerate with `python3 tools/mkfontprop.py`.")
    o.append('#include "header/fontprop.h"')
    o.append("")

    for fc in faces:
        o.append("// %s: %dpx from %s" % (fc.name, fc.size, fc.path.split("/")[-1]))
        o.append("static const uint8_t %s_cov[] = {" % fc.name)
        b = fc.blob
        for i in range(0, len(b), 24):
            o.append("    " + ",".join("%d" % v for v in b[i:i + 24]) + ",")
        o.append("};")
        o.append("static const pf_glyph_t %s_glyphs[PF_GLYPH_N] = {" % fc.name)
        for i, (adv, w, h, left, top, off) in enumerate(fc.glyphs):
            ch = chr(FIRST + i)
            note = "" if ch in ('"', "\\") else "  // '%s'" % ch
            o.append("    { %d, %d, %d, %d, %d, %d },%s" % (adv, w, h, left, top, off, note))
        o.append("};")
        o.append("const pf_face_t %s = { %d, %d, %s_glyphs, %s_cov };"
                 % (fc.name, fc.height, fc.ascent, fc.name, fc.name))
        o.append("")

    open(path, "w", newline="\n").write("\n".join(o))
    total = sum(len(f.blob) for f in faces)
    print("wrote %s (%d coverage bytes across %d faces)" % (path, total, len(faces)),
          file=sys.stderr)


DEJAVU = "/usr/share/fonts/truetype/dejavu/"
CARLITO = "/usr/share/fonts/truetype/crosextra/"

if __name__ == "__main__":
    faces = [
        Face("pf_body",        CARLITO + "Carlito-Regular.ttf", 17),
        Face("pf_body_bold",   CARLITO + "Carlito-Bold.ttf",    17),
        Face("pf_body_italic", CARLITO + "Carlito-Italic.ttf",  17),
        Face("pf_h3",          CARLITO + "Carlito-Bold.ttf",    19),
        Face("pf_h2",          CARLITO + "Carlito-Bold.ttf",    22),
        Face("pf_h1",          CARLITO + "Carlito-Bold.ttf",    29),
    ]
    for f in faces:
        f.build()
        print("  %-16s cell=%dpx ascent=%d bytes=%d"
              % (f.name, f.height, f.ascent, len(f.blob)), file=sys.stderr)
    emit(faces, "src/fontprop.c")
