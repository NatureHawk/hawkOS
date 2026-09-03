import gzip, struct, sys

src = "/usr/share/consolefonts/Lat15-VGA16.psf.gz"
data = gzip.open(src, "rb").read()

if data[0] == 0x36 and data[1] == 0x04:
    mode, charsize = data[2], data[3]
    nglyphs = 512 if (mode & 0x01) else 256
    off = 4
    width = 8
elif data[0:4] == b"\x72\xb5\x4a\x86":
    ver, hdr, flags, nglyphs, charsize, height, width = struct.unpack("<IIIIIII", data[4:32])
    off = hdr
else:
    sys.exit("unrecognised PSF magic")

height = charsize
print("glyphs=%d charsize=%d width=%d height=%d" % (nglyphs, charsize, width, height), file=sys.stderr)
assert width == 8 and height == 16, "expected an 8x16 font"

glyphs = []
for i in range(256):
    if i < nglyphs:
        g = data[off + i*charsize: off + (i+1)*charsize]
    else:
        g = bytes(16)
    glyphs.append(g)

out = []
out.append("// src/font8x16.c -- 8x16 bitmap font, one byte per scanline, MSB is the")
out.append("// leftmost pixel. Extracted from the Lat15-VGA16 console font shipped with")
out.append("// Debian's console-setup package, which carries the IBM VGA ROM glyph shapes")
out.append("// that have been in free use for decades. Generated, not hand-written:")
out.append("// see tools/mkfont.py.")
out.append('#include "header/font.h"')
out.append("")
out.append("const uint8_t font8x16[256][16] = {")
for i, g in enumerate(glyphs):
    row = ", ".join("0x%02X" % b for b in g)
    ch = chr(i) if 32 <= i < 127 else ""
    comment = "  // %d %s" % (i, ("'" + ch + "'") if ch and ch != "'" else "")
    out.append("    { %s },%s" % (row, comment))
out.append("};")
out.append("")
open("src/font8x16.c", "w").write("\n".join(out))
print("wrote src/font8x16.c", file=sys.stderr)
