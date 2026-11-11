#!/usr/bin/env python3
"""Record a feature walkthrough of NodyOS as a video.

Boots a scratch disk image in a headless QEMU and drives it with real mouse
and keyboard events over one QMP connection, while a second QMP connection
grabs the screen about 30 times a second. Nothing is simulated: the frames
are QEMU's own screen dumps, in real time. ffmpeg then encodes them, with a
caption strip (drawn with Pillow) under the picture. The only edit: the boot
screens, which pass in a fraction of a second, are slowed down so they can be read.

    make IMG=build/demo.img                       fresh scratch disk
    tools/record_demo.py --frames DIR [--out FILE] [--test | --reencode]

Needs ffmpeg and Pillow on the host. Not part of the operating system.
"""
import argparse
import json
import os
import socket
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
W, H, FPS = 1024, 768, 30
BAND = 56
PACE = 0.8                              # < 1 moves the pointer faster
SLOW = 5                                # boot screens are shown this many times slower


class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("cannot connect to QEMU")
        self.f = self.s.makefile("rw")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QEMU closed the connection")
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def cmd(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.f.write(json.dumps(req) + "\n")
        self.f.flush()
        msg = self._read()
        if "error" in msg:
            raise SystemExit(f"QMP {name}: {msg['error']}")
        return msg.get("return")


class Capture(threading.Thread):
    """Dump the screen on a fixed 30 Hz grid; late frames simply skip ticks."""

    def __init__(self, sock, frames):
        super().__init__(daemon=True)
        self.q = QMP(sock)
        self.frames = frames
        self.shots = []                 # (tick, file)
        self.t0 = time.monotonic()
        self.running = True

    def run(self):
        last, n = -1, 0
        while self.running:
            tick = int((time.monotonic() - self.t0) * FPS)
            if tick <= last:
                time.sleep(max(0.0, self.t0 + (last + 1) / FPS - time.monotonic()))
                continue
            path = os.path.join(self.frames, f"f{n:06d}.ppm")
            self.q.cmd("screendump", filename=path)
            self.shots.append((tick, path))
            last, n = tick, n + 1

    def now(self):
        return time.monotonic() - self.t0


class Director:
    """Human-paced input: the pointer glides instead of teleporting."""

    def __init__(self, sock, cap):
        self.q = QMP(sock)
        self.cap = cap
        self.x, self.y = W / 2, H / 2
        self.captions = []              # (time, text)

    def caption(self, text):
        self.captions.append((self.cap.now(), text))
        print(f"{self.cap.now():6.1f}s  {text}")

    def wait(self, s):
        time.sleep(s)

    def _send(self, x, y):
        ax = min(0x7FFF, int(x * 32768 / W) + 16)
        ay = min(0x7FFF, int((y + 0.5) * 32768 / H))
        self.q.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}])
        self.x, self.y = x, y

    def glide(self, x, y, dur=0.45):
        x0, y0 = self.x, self.y
        steps = max(2, int(dur * PACE * 60))
        for i in range(1, steps + 1):
            t = i / steps
            e = t * t * (3 - 2 * t)                     # ease in and out
            self._send(x0 + (x - x0) * e, y0 + (y - y0) * e)
            time.sleep(1 / 60)

    def button(self, down, name="left"):
        self.q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": name}}])
        time.sleep(0.06)

    def click(self, x, y, name="left", dur=0.45, pause=0.35):
        self.glide(x, y, dur)
        time.sleep(0.08)
        self.button(True, name)
        self.button(False, name)
        time.sleep(pause * PACE)

    def dclick(self, x, y, pause=0.4):
        self.glide(x, y)
        for _ in range(2):
            self.button(True)
            self.button(False)
        time.sleep(pause)

    def drag(self, x0, y0, x1, y1, dur=0.9, name="left", approach=0.45, hold=0.15):
        self.glide(x0, y0, approach)
        time.sleep(0.08)
        self.button(True, name)
        self.glide(x1, y1, dur)
        time.sleep(hold)
        self.button(False, name)

    def wheel(self, n):
        name = "wheel-up" if n > 0 else "wheel-down"
        for _ in range(abs(n)):
            self.button(True, name)
            self.button(False, name)

    def key(self, name, mods=()):
        seq = list(mods) + [name]
        for down in (True, False):
            order = seq if down else list(reversed(seq))
            self.q.cmd("input-send-event", events=[
                {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": k}}} for k in order])
            time.sleep(0.035)

    def type(self, text):
        for ch in text:
            self.key(ch)
            time.sleep(0.04)


def boots(serial):
    try:
        return open(serial).read().count("desktop ready")
    except OSError:
        return 0


def wait_boot(serial, count, timeout=20):
    end = time.time() + timeout
    while time.time() < end and boots(serial) < count:
        time.sleep(0.05)


# --- the walkthrough -------------------------------------------------------------------
# Coordinates are screen pixels at 1024x768 with Flow at its default position.
def storyboard(d, serial):
    VIEW = (360, 250)

    d.caption("Booting: the BIOS hands over to NodyOS's own bootloader and kernel (slowed 5x)")
    d.q.cmd("cont")
    wait_boot(serial, 1)
    d.wait(0.1)
    d.caption("NodyOS: an operating system written from scratch. No libc, no third-party code.")
    d.wait(2.0)

    d.caption("Flow, its node-based 3D VFX app, with a live software-rendered viewport")
    d.drag(280, 250, 470, 268, dur=1.6)                 # orbit
    d.wheel(2)
    d.wait(0.4)
    d.drag(450, 262, 350, 240, dur=1.2)
    d.wait(0.3)

    d.caption("Build a graph: every node and every parameter updates the viewport live")
    d.click(40, 46)                                     # New
    d.click(285, 46); d.click(425, 100)                 # + Add Node > Cloner
    d.click(886, 121, pause=0.5)                        # Mode: Radial
    d.drag(824, 144, 844, 144, dur=1.1)                 # Count 8 -> 28
    d.click(285, 46); d.click(425, 184)                 # Step Effector
    d.drag(932, 144, 962, 144, dur=0.9)                 # Move Y up
    d.drag(910, 236, 948, 236, dur=1.0)                 # Rotate Y
    d.click(285, 46); d.click(779, 100)                 # Color
    d.click(886, 121, pause=0.4)                        # Mode: Index
    d.click(285, 46); d.click(543, 121)                 # Animate
    d.click(65, 463, pause=0.3)                         # Play
    d.glide(*VIEW)
    d.wheel(-2)                                         # step back a little
    d.wait(1.0)

    d.caption("Drag a wire to swap what gets cloned")
    d.click(120, 640, name="right")                     # right-click the graph
    d.click(183, 684)                                   # Torus
    d.drag(190, 639, 230, 546, dur=1.0, hold=0.3)       # Torus output -> Cloner "Object"
    d.wait(1.0)
    d.click(275, 527, pause=0.3)                        # select the Cloner
    d.drag(873, 167, 906, 167, dur=0.9)                 # Radius out
    d.drag(906, 167, 886, 167, dur=0.7, approach=0.1)   # and back in
    d.wait(0.9)

    d.caption("Save the scene to NodyFS, the OS's own filesystem, in its own format")
    d.click(200, 46, pause=0.5)                         # Save As
    for _ in range(9):
        d.key("backspace")
    d.type("spiral")
    d.wait(0.5)
    d.key("ret")
    d.wait(1.2)

    d.caption("Particle fluid solver: a jet pours over a sphere inside a tank")
    d.click(90, 46, pause=0.5)                          # Open
    d.dclick(400, 262)                                  # fluid.flw
    d.glide(*VIEW)
    d.wait(3.4)
    d.drag(330, 250, 440, 238, dur=2.4)                 # orbit while it pours
    d.wait(2.0)

    d.caption("A cloner puts a cube on every fluid particle")
    d.click(90, 46, pause=0.5)
    d.dclick(400, 346)                                  # voxels.flw
    d.glide(*VIEW)
    d.wait(1.8)
    d.drag(420, 250, 330, 242, dur=1.8)
    d.wait(0.6)

    d.caption("Window manager: drag windows, click to focus, close")
    d.click(252, 750, pause=0.5)                        # dock: Files
    d.click(360, 750, pause=0.5)                        # dock: System
    d.drag(760, 58, 560, 200, dur=1.0)                  # System by its title bar
    d.click(200, 300, pause=0.5)                        # Files comes to the front
    d.drag(300, 108, 240, 148, dur=0.8)                 # move Files
    d.click(700, 400, pause=0.5)                        # System comes to the front
    d.click(545, 148, pause=0.6)                        # close Files

    d.caption("Restart from the System window...")
    d.click(619, 503, pause=0.0)                        # Restart
    d.caption("Rebooting through the BIOS and the bootloader (slowed 5x)")
    wait_boot(serial, 2)
    d.wait(0.1)
    d.caption("...and the saved scene is still on the disk")
    d.wait(1.4)
    d.click(252, 750, pause=1.0)                        # Files
    d.dclick(300, 247)                                  # spiral.flw
    d.glide(*VIEW)
    d.wait(1.2)
    d.caption("NodyOS  -  one prompt to Claude Fable 5.2  -  github.com/filipvabrousek/NodyOS")
    d.drag(300, 255, 420, 262, dur=2.4)
    d.wait(1.4)


def test_board(d, serial):
    d.caption("capture test")
    d.q.cmd("cont")
    wait_boot(serial, 1)
    d.wait(1.5)
    d.drag(300, 260, 470, 215, dur=1.5)
    d.wait(1.0)
    d.click(90, 46, pause=0.5)
    d.dclick(400, 262)
    d.wait(6.0)


# --- encoding ------------------------------------------------------------------------
def ppm_size(path):
    with open(path, "rb") as f:
        head = f.read(32).split()
    return int(head[1]), int(head[2])


def desktop_up(path):
    """True once the dock is on screen. Anything else after power-on is a boot screen."""
    dock = bytes((0x14, 0x16, 0x1B))
    with open(path, "rb") as f:
        f.readline()
        w, _ = map(int, f.readline().split())
        f.readline()
        base = f.tell()
        for x, y in ((512, 760), (600, 762)):
            f.seek(base + (y * w + x) * 3)
            if f.read(3) == dock:
                return True
    return False


def encode(shots, captions, frames, out):
    from PIL import Image, ImageDraw, ImageFont

    for _, path in shots:                               # BIOS text mode etc.: fit onto a 1024x768 canvas
        if ppm_size(path) != (W, H):
            im = Image.open(path).convert("RGB")
            k = min(W / im.width, H / im.height)
            im = im.resize((int(im.width * k), int(im.height * k)), Image.LANCZOS)
            canvas = Image.new("RGB", (W, H), (0, 0, 0))
            canvas.paste(im, ((W - im.width) // 2, (H - im.height) // 2))
            canvas.save(path, format="PPM")

    # Everything is shown in real time except the boot screens, which pass in a
    # fraction of a second: those frames are repeated SLOW times so they can be read.
    power_on = captions[0][0]
    start = max(0, int((power_on - 0.1) * FPS))
    end = shots[-1][0]
    for tick, path in shots:                            # open on the first real picture: skip the blank
        if tick / FPS < power_on:                       # screen (and QEMU's own "display not initialized"
            continue                                    # notice) that precedes the BIOS
        if tick / FPS > power_on + 3 or Image.open(path).crop((0, 0, W, 250)).getbbox():
            start = tick
            break
    lines, warp, i, total = [], [], 0, 0
    for tick in range(start, end + 1):
        while i + 1 < len(shots) and shots[i + 1][0] <= tick:
            i += 1
        booting = tick / FPS >= power_on and not desktop_up(shots[i][1])
        warp.append(total)
        for _ in range(SLOW if booting else 1):
            lines.append(f"file '{shots[i][1]}'\nduration {1 / FPS:.6f}")
            total += 1
    lines.append(f"file '{shots[i][1]}'")
    flist = os.path.join(frames, "frames.txt")
    open(flist, "w").write("\n".join(lines) + "\n")

    def out_time(t):                                    # capture time -> time in the video
        k = min(max(int(t * FPS) - start, 0), len(warp) - 1)
        return warp[k] / FPS

    font = None
    for name in ("/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
                 "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try:
            font = ImageFont.truetype(name, 23)
            break
        except OSError:
            pass
    clines, path = [], None
    for k, (t, text) in enumerate(captions):
        im = Image.new("RGB", (W, BAND), (13, 15, 19))
        dr = ImageDraw.Draw(im)
        dr.line([(0, 0), (W, 0)], fill=(44, 48, 58))
        box = dr.textbbox((0, 0), text, font=font)
        dr.text(((W - (box[2] - box[0])) / 2, (BAND - (box[3] - box[1])) / 2 - box[1]), text,
                font=font, fill=(232, 234, 240))
        path = os.path.join(frames, f"caption{k:02d}.png")
        im.save(path)
        nxt = out_time(captions[k + 1][0]) if k + 1 < len(captions) else total / FPS
        dur = max(1 / FPS, nxt - (out_time(t) if k else 0.0))
        clines.append(f"file '{path}'\nduration {dur:.4f}")
    clines.append(f"file '{path}'")
    clist = os.path.join(frames, "captions.txt")
    open(clist, "w").write("\n".join(clines) + "\n")

    graph = (f"[0:v]fps={FPS},format=rgb24[a];[1:v]fps={FPS},format=rgb24[b];"
             f"[a][b]vstack=inputs=2:shortest=1,scale=-2:1080:flags=lanczos,format=yuv420p[v]")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", flist,
                    "-f", "concat", "-safe", "0", "-i", clist, "-filter_complex", graph, "-map", "[v]",
                    "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-movflags", "+faststart", out], check=True)
    print(f"{out}: {total / FPS:.1f} s from {len(shots)} screen dumps "
          f"({len(shots) / max(shots[-1][0] / FPS, 0.01):.1f} per second), boot screens slowed {SLOW}x")
    for t, text in captions:
        print(f"  {out_time(t):6.1f}s  {text}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default=os.path.join(BUILD, "rec"))
    ap.add_argument("--image", default=os.path.join(BUILD, "demo.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "nodyos-demo.mp4"))
    ap.add_argument("--test", action="store_true", help="short capture to check speed")
    ap.add_argument("--reencode", action="store_true", help="encode again from the frames of the last take")
    a = ap.parse_args()

    session = os.path.join(a.frames, "session.json")
    if a.reencode:
        data = json.load(open(session))
        encode([tuple(x) for x in data["shots"]], [tuple(x) for x in data["captions"]], a.frames, a.out)
        return

    os.makedirs(a.frames, exist_ok=True)
    for f in os.listdir(a.frames):
        os.remove(os.path.join(a.frames, f))
    ctl, capsock, serial = (os.path.join(BUILD, n) for n in ("rec-ctl.sock", "rec-cap.sock", "rec-serial.log"))
    for p in (ctl, capsock, serial):
        if os.path.exists(p):
            os.remove(p)
    qemu = subprocess.Popen(
        ["qemu-system-i386", "-drive", f"file={a.image},format=raw,if=ide,index=0,media=disk",
         "-m", "256", "-vga", "std", "-rtc", "base=localtime", "-display", "none", "-S",
         "-serial", f"file:{serial}", "-qmp", f"unix:{ctl},server,nowait", "-qmp", f"unix:{capsock},server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        cap = Capture(capsock, a.frames)
        d = Director(ctl, cap)
        cap.start()
        time.sleep(0.4)
        (test_board if a.test else storyboard)(d, serial)
        cap.running = False
        cap.join(timeout=5)
        d.q.cmd("quit")
    finally:
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
    json.dump({"captions": d.captions, "shots": cap.shots}, open(session, "w"))
    encode(cap.shots, d.captions, a.frames, a.out)


if __name__ == "__main__":
    main()
