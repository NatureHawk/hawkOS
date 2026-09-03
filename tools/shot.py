#!/usr/bin/env python3
"""Boot hawkOS headless, drive it, and capture the framebuffer as a PNG.

QEMU is started with no display and a QMP socket. We talk to that socket to
issue screendumps and to inject keyboard/mouse input, which makes the GUI
testable without a human at the keyboard. The PPM that QEMU writes is
converted to PNG here rather than shelling out to a converter, so the only
dependency is the Python standard library.

Usage:
    python3 tools/shot.py out.png [--wait SECS] [--script FILE] [--net]

The optional script file holds one directive per line:
    wait <secs>
    key <qcode>              e.g. `key ret`, `key a`, `key spc`
    type <text>              literal characters, one key event each
    mouse <x> <y>            move the pointer to guest pixel coordinates
    click <x> <y>            move there, then press+release the left button
    wheel <n>                scroll n notches; positive is down
    shot <file.png>          capture right now
"""

import json
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

ISO = "hawkos.iso"
DISK = "disk.img"
QMP = "/tmp/hawkos-qmp.sock"
SERIAL = "serial.log"


# --------------------------------------------------------------- PPM -> PNG

def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, "rb") as f:
        data = f.read()

    # P6 header: magic, width, height, maxval — any of them may be followed
    # by comments, so parse token by token rather than by fixed offsets.
    pos = 0
    tokens = []
    while len(tokens) < 4:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    pos += 1

    assert tokens[0] == b"P6", "not a binary PPM"
    w, h = int(tokens[1]), int(tokens[2])
    pixels = data[pos : pos + w * h * 3]

    raw = bytearray()
    for y in range(h):
        raw.append(0)  # PNG per-scanline filter: none
        raw += pixels[y * w * 3 : (y + 1) * w * 3]

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")

    with open(png_path, "wb") as f:
        f.write(png)
    return w, h


# ------------------------------------------------------------------- QMP

class Qmp:
    def __init__(self, path, timeout=30):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.f = self.sock.makefile("rwb")
        self._read()                      # greeting
        self.cmd("qmp_capabilities")
        # Where we believe the guest's cursor is. mouse_init() parks it at
        # the centre of the screen, so callers set this once the resolution
        # is known.
        self.mx = 0
        self.my = 0
        # Guest resolution, filled in after the first screendump. Absolute
        # events are expressed in a 0..32767 box, so we need it to convert.
        self.sw = 0
        self.sh = 0
        # hawkOS enables QEMU's vmmouse when the vmport is present, which is
        # the default. Absolute events land exactly where aimed, so there is
        # no homing dance and no accumulated error.
        self.absolute = True

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def cmd(self, name, **args):
        payload = {"execute": name}
        if args:
            payload["arguments"] = args
        self.f.write((json.dumps(payload) + "\n").encode())
        self.f.flush()
        return self._read()

    def screendump(self, path):
        if os.path.exists(path):
            os.remove(path)
        self.cmd("screendump", filename=path)
        # screendump returns as soon as the request is accepted; wait for the
        # file to stop growing before anyone reads it.
        for _ in range(100):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                a = os.path.getsize(path)
                time.sleep(0.05)
                if os.path.getsize(path) == a:
                    return
            time.sleep(0.05)

    def key(self, qcode):
        self.cmd(
            "input-send-event",
            events=[{"type": "key",
                     "data": {"down": True, "key": {"type": "qcode", "data": qcode}}}],
        )
        self.cmd(
            "input-send-event",
            events=[{"type": "key",
                     "data": {"down": False, "key": {"type": "qcode", "data": qcode}}}],
        )

    def rel(self, dx, dy):
        # The guest has a PS/2 mouse driver, so it only ever sees relative
        # deltas. A PS/2 packet carries a signed 9-bit delta, so anything
        # larger has to be split or the sign bits wrap and the pointer jumps
        # the wrong way.
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            dx -= sx
            dy -= sy
            evs = []
            if sx: evs.append({"type": "rel", "data": {"axis": "x", "value": sx}})
            if sy: evs.append({"type": "rel", "data": {"axis": "y", "value": sy}})
            if evs:
                self.cmd("input-send-event", events=evs)
            time.sleep(0.02)

    def home(self):
        # There is no way to ask the guest where it thinks the pointer is, so
        # drive it hard into the top-left corner instead. The guest driver
        # clamps at 0, which makes (0,0) a known origin no matter where the
        # pointer started or how many events got dropped along the way.
        for _ in range(30):
            self.cmd("input-send-event", events=[
                {"type": "rel", "data": {"axis": "x", "value": -100}},
                {"type": "rel", "data": {"axis": "y", "value": -100}}])
            time.sleep(0.01)
        time.sleep(0.3)
        self.mx = self.my = 0

    def move(self, x, y):
        if self.absolute and self.sw and self.sh:
            ax = int(x * 32767 / (self.sw - 1))
            ay = int(y * 32767 / (self.sh - 1))
            self.cmd("input-send-event",
                     events=[{"type": "abs", "data": {"axis": "x", "value": ax}},
                             {"type": "abs", "data": {"axis": "y", "value": ay}}])
            time.sleep(0.1)
        else:
            # Relative fallback: re-home first, because a dropped packet would
            # otherwise put the tool's idea of the cursor permanently out of
            # step with the guest's and every later click would miss.
            self.home()
            self.rel(x, y)
        self.mx, self.my = x, y

    def wheel(self, notches):
        # QEMU delivers these to the PS/2 mouse as the Z axis of a 4-byte
        # IntelliMouse packet, which is exactly what the guest driver reads.
        button = "wheel-down" if notches > 0 else "wheel-up"
        for _ in range(abs(notches)):
            self.cmd("input-send-event",
                     events=[{"type": "btn", "data": {"down": True, "button": button}}])
            self.cmd("input-send-event",
                     events=[{"type": "btn", "data": {"down": False, "button": button}}])
            time.sleep(0.05)

    def click(self, x, y):
        self.move(x, y)
        time.sleep(0.35)
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.25)
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.35)


CHAR_QCODE = {
    " ": "spc", ".": "dot", ",": "comma", "-": "minus", "=": "equal",
    "/": "slash", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
    "]": "bracket_right", "\\": "backslash", "`": "grave_accent",
}


def type_text(qmp, text):
    for ch in text:
        if ch in CHAR_QCODE:
            qmp.key(CHAR_QCODE[ch])
        elif ch.isdigit():
            qmp.key(ch)
        elif ch.isalpha():
            qmp.key(ch.lower())
        elif ch == ":":
            qmp.cmd("input-send-event", events=[
                {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "shift"}}},
                {"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": "semicolon"}}},
                {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "semicolon"}}},
                {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}},
            ])
        time.sleep(0.03)


def main():
    # The output name is optional and positional, but every flag starts with
    # "--"; without this check a leading flag was taken as the filename and
    # its value fell through into QEMU's own argv.
    if len(sys.argv) > 1 and not sys.argv[1].startswith("--"):
        out = sys.argv[1]
        args = sys.argv[2:]
    else:
        out = "shot.png"
        args = sys.argv[1:]

    wait = 6.0
    script = None
    extra = []

    i = 0
    while i < len(args):
        if args[i] == "--wait":
            wait = float(args[i + 1]); i += 2
        elif args[i] == "--script":
            script = args[i + 1]; i += 2
        elif args[i] == "--net":
            extra += ["-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
            i += 1
        else:
            extra.append(args[i]); i += 1

    if os.path.exists(QMP):
        os.remove(QMP)

    cmd = [
        "qemu-system-i386", "-m", "128M",
        "-cdrom", ISO,
        "-drive", "file=%s,format=raw,if=ide" % DISK,
        "-boot", "d",
        "-display", "none",
        "-serial", "file:" + SERIAL,
        "-qmp", "unix:%s,server,nowait" % QMP,
    ] + extra

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        try:
            qmp = Qmp(QMP)
        except OSError:
            # QEMU died before it could listen; its stderr says why, and that
            # is far more useful than the connection error.
            proc.poll()
            err = proc.stderr.read().decode(errors="replace") if proc.stderr else ""
            raise SystemExit("qemu failed to start (rc=%s)" % proc.returncode + chr(10) + err.strip())
        w = h = None

        def shot(path):
            nonlocal w, h
            ppm = "/tmp/hawkos-shot.ppm"
            qmp.screendump(ppm)
            w, h = ppm_to_png(ppm, path)
            print("wrote %s (%dx%d)" % (path, w, h))

        time.sleep(wait)

        if script:
            # A first capture establishes the resolution, so `click` can map
            # guest pixels onto QEMU's absolute axis.
            shot("/tmp/hawkos-probe.png")
            qmp.sw, qmp.sh = w, h
            if not qmp.absolute:
                qmp.home()
            for raw in open(script):
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                op, _, rest = line.partition(" ")
                rest = rest.strip()
                if op == "wait":
                    time.sleep(float(rest))
                elif op == "key":
                    qmp.key(rest)
                elif op == "type":
                    type_text(qmp, rest)
                elif op == "mouse":
                    x, y = rest.split()
                    qmp.move(int(x), int(y))
                elif op == "click":
                    x, y = rest.split()
                    qmp.click(int(x), int(y))
                elif op == "wheel":
                    qmp.wheel(int(rest))
                elif op == "shot":
                    shot(rest)
                else:
                    print("unknown directive:", line, file=sys.stderr)
        else:
            shot(out)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
