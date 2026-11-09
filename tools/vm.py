#!/usr/bin/env python3
"""Drive NodyOS inside a headless QEMU for testing.

    tools/vm.py start [--relative] [--image FILE]   boot nody.img (or FILE) in the background
    tools/vm.py do "<commands>"        run ';'-separated commands (see below)
    tools/vm.py stop                   quit QEMU

Commands:
    move X Y            move the pointer (absolute pixels)
    down [btn] / up [btn]   press / release a button (left, right, middle)
    click X Y [btn]     move, press, release
    dclick X Y          double click
    drag X0 Y0 X1 Y1 [btn]  press at one point, move in steps, release at another
    wheel N             scroll (N > 0: away from the user)
    key NAME            press a key (QEMU qcode: a, ret, esc, delete, spc, tab ...)
    combo MOD KEY       e.g. combo ctrl s
    type TEXT           type lower-case text, digits, '.', '-', '_'
    wait SECONDS
    shot FILE.png       save a screenshot
    reset               hard-reset the machine (like pressing the reset button)
    rel DX DY           relative mouse motion (PS/2 path)
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = os.path.join(ROOT, "build", "qmp.sock")
PIDF = os.path.join(ROOT, "build", "qemu.pid")
SERIAL = os.path.join(ROOT, "build", "serial.log")
W, H = 1024, 768


class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.s.connect(SOCK)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("cannot connect to QEMU (is it running? tools/vm.py start)")
        self.f = self.s.makefile("rw")
        self._read()                       # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QEMU closed the connection")
            msg = json.loads(line)
            if "event" in msg:
                continue
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

    def events(self, evs):
        self.cmd("input-send-event", events=evs)


def abs_events(x, y):
    # QEMU absolute axes run 0..0x7fff; the guest maps them back to pixels
    ax = min(0x7FFF, int(x * 32768 / W) + 16)
    ay = min(0x7FFF, int((y + 0.5) * 32768 / H))
    return [{"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]


class VM:
    def __init__(self):
        self.q = QMP()

    def move(self, x, y):
        self.q.events(abs_events(x, y))
        time.sleep(0.03)

    def button(self, name, down):
        self.q.events([{"type": "btn", "data": {"down": down, "button": name}}])
        time.sleep(0.05)

    def click(self, x, y, btn="left"):
        self.move(x, y)
        self.button(btn, True)
        self.button(btn, False)

    def drag(self, x0, y0, x1, y1, btn="left", steps=12):
        self.move(x0, y0)
        self.button(btn, True)
        for i in range(1, steps + 1):
            self.move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
        time.sleep(0.05)
        self.button(btn, False)

    def key(self, name, mods=()):
        seq = list(mods) + [name]
        self.q.events([{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": k}}} for k in seq])
        time.sleep(0.03)
        self.q.events([{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": k}}} for k in reversed(seq)])
        time.sleep(0.03)

    def type(self, text):
        names = {".": "dot", "-": "minus", " ": "spc", "_": None}
        for ch in text:
            if ch == "_":
                self.key("minus", ("shift",))
            elif ch.isupper():
                self.key(ch.lower(), ("shift",))
            else:
                self.key(names.get(ch, ch))

    def shot(self, path):
        path = os.path.abspath(path)
        self.q.cmd("screendump", filename=path, format="png")

    def run(self, script):
        for raw in script.split(";"):
            p = raw.split()
            if not p:
                continue
            c, a = p[0], p[1:]
            if c == "move":
                self.move(float(a[0]), float(a[1]))
            elif c == "down":
                self.button(a[0] if a else "left", True)
            elif c == "up":
                self.button(a[0] if a else "left", False)
            elif c == "click":
                self.click(float(a[0]), float(a[1]), a[2] if len(a) > 2 else "left")
            elif c == "dclick":
                self.click(float(a[0]), float(a[1]))
                time.sleep(0.08)
                self.click(float(a[0]), float(a[1]))
            elif c == "drag":
                self.drag(float(a[0]), float(a[1]), float(a[2]), float(a[3]), a[4] if len(a) > 4 else "left")
            elif c == "wheel":
                n = int(a[0])
                for _ in range(abs(n)):
                    self.button("wheel-up" if n > 0 else "wheel-down", True)
                    self.button("wheel-up" if n > 0 else "wheel-down", False)
            elif c == "key":
                self.key(a[0])
            elif c == "combo":
                self.key(a[-1], tuple(a[:-1]))
            elif c == "type":
                self.type(raw.strip()[5:])
            elif c == "wait":
                time.sleep(float(a[0]))
            elif c == "shot":
                self.shot(a[0])
            elif c == "reset":
                self.q.cmd("system_reset")
            elif c == "rel":
                self.q.events([{"type": "rel", "data": {"axis": "x", "value": int(a[0])}},
                               {"type": "rel", "data": {"axis": "y", "value": int(a[1])}}])
                time.sleep(0.03)
            else:
                raise SystemExit(f"unknown command: {c}")


def start(relative=False, image="nody.img"):
    stop(quiet=True)
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    for p in (SOCK, SERIAL):
        if os.path.exists(p):
            os.remove(p)
    args = ["qemu-system-i386",
            "-drive", f"file={os.path.join(ROOT, image)},format=raw,if=ide,index=0,media=disk",
            "-m", "256", "-vga", "std", "-rtc", "base=localtime",
            "-display", "none", "-serial", f"file:{SERIAL}",
            "-qmp", f"unix:{SOCK},server,nowait"]
    if relative:
        args += ["-machine", "vmport=off"]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, start_new_session=True)
    with open(PIDF, "w") as f:
        f.write(str(proc.pid))
    QMP()                                  # wait until the monitor answers
    print(f"qemu started, pid {proc.pid}")


def stop(quiet=False):
    if not os.path.exists(PIDF):
        return
    try:
        pid = int(open(PIDF).read())
        try:
            QMP().cmd("quit")
        except SystemExit:
            pass
        for _ in range(30):
            try:
                os.kill(pid, 0)
                time.sleep(0.1)
            except OSError:
                break
        else:
            os.kill(pid, 9)
    except (OSError, ValueError):
        pass
    os.remove(PIDF)
    if not quiet:
        print("qemu stopped")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if sys.argv[1] == "start":
        image = sys.argv[sys.argv.index("--image") + 1] if "--image" in sys.argv else "nody.img"
        start(relative="--relative" in sys.argv, image=image)
    elif sys.argv[1] == "stop":
        stop()
    elif sys.argv[1] == "do":
        VM().run(" ".join(sys.argv[2:]))
    else:
        raise SystemExit(__doc__)
