#!/usr/bin/env python3
"""Tests kwai against LiteForth's desktop lf over a pty bridge (Linux, macOS).

Usage: test_kwai.py path/to/kwai path/to/lf path/to/lfblocks.fb4

lf runs on one pty (`lf -t`), kwai on another, and this script copies bytes
between the two masters. kwai's port is a symlink, so the script can unplug
it (close the pty and remove the link) and plug a new one in, as a USB CDC
device does when the board is reflashed. It checks that typed lines reach
lf, that `~x` sends a file into blocks by XMODEM, and that kwai reconnects.
"""
import os
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tty

kwai, lf, blocks = (os.path.abspath(a) for a in sys.argv[1:4])
work = tempfile.mkdtemp()
shutil.copy(blocks, os.path.join(work, "lfblocks.fb4"))
link = os.path.join(work, "board")


class Bridge:
    def __init__(self):
        self.lf_m, lf_s = pty.openpty()
        tty.setraw(lf_s)
        self.lf_name = os.ttyname(lf_s)
        self.lf_s = lf_s
        self.k_m = None
        self.lock = threading.Lock()
        self.plug()
        threading.Thread(target=self.run, daemon=True).start()

    def plug(self):
        m, s = pty.openpty()
        tty.setraw(s)                     # no echo before kwai opens it
        name = os.ttyname(s)
        os.close(s)                       # kwai opens it by name
        with self.lock:
            self.k_m = m
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(name, link)

    def unplug(self):
        with self.lock:
            m, self.k_m = self.k_m, None
        os.remove(link)
        os.close(m)

    def run(self):
        while True:
            with self.lock:
                fds = [self.lf_m] + ([self.k_m] if self.k_m is not None else [])
            try:
                r, _, _ = select.select(fds, [], [], 0.05)
            except OSError:
                continue
            for fd in r:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    continue
                with self.lock:
                    other = self.k_m if fd == self.lf_m else self.lf_m
                    if other is None:
                        continue
                    try:
                        while data:
                            data = data[os.write(other, data):]
                    except OSError:
                        pass


bridge = Bridge()
lfp = subprocess.Popen([lf, "-t", bridge.lf_name, "-b", "115200", "-o", "3"], cwd=work,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
kp = subprocess.Popen([kwai, link], cwd=work, stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
out, err = bytearray(), bytearray()


def pump(src, dst):
    while True:
        b = os.read(src.fileno(), 4096)
        if not b:
            return
        dst.extend(b)


threading.Thread(target=pump, args=(kp.stdout, out), daemon=True).start()
threading.Thread(target=pump, args=(kp.stderr, err), daemon=True).start()


def wait_for(buf, text, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if text.encode() in buf:
            return True
        time.sleep(0.05)
    print(f"FAILED: never saw {text!r}\n--- stdout ---\n{out.decode(errors='replace')}"
          f"\n--- stderr ---\n{err.decode(errors='replace')}")
    sys.exit(1)


def type_line(s):
    kp.stdin.write((s + "\n").encode())
    kp.stdin.flush()


ok = []
wait_for(err, "connected")
type_line("1 2 + . 30 4 + .")
wait_for(out, "3 34")
ok.append("a typed line reaches lf and its output comes back")

src = os.path.join(work, "src.f")
with open(src, "w", encoding="utf-8") as f:
    f.write("( BLOCK 40 )\n.( hello from block 40 学 ) cr\n" + "( filler )\n" * 25 +
            "( BLOCK 41 )\n.( block 41 ) cr\n")
type_line("~x src.f")
wait_for(err, "sent src.f", 30)
wait_for(out, "xmodem: 2 blocks written: 40 41")   # exact, so no byte was lost
type_line("40 load 41 load")
wait_for(out, "hello from block 40 学")
wait_for(out, "block 41")
ok.append("~x types `xmodem` and sends a file into blocks (2 blocks, UTF-8 intact)")

big = os.path.join(work, "big.f")                 # 5 full blocks: 16 KB, 16 packets
rows = []
for blk in range(50, 55):
    rows.append(f"( BLOCK {blk} )")
    rows += [f"( {blk}:{r:02} " + "x" * 110 + " )" for r in range(32)]
with open(big, "w") as f:
    f.write("\n".join(rows) + "\n")
type_line("~x big.f")
wait_for(err, "sent big.f", 60)
wait_for(out, "xmodem: 5 blocks written: 50 51 52 53 54")
time.sleep(0.5)
with open(os.path.join(work, "lfblocks.fb4"), "rb") as f:
    data = f.read()
for blk in range(50, 55):
    got = data[blk * 4096:(blk + 1) * 4096]
    for r in range(32):
        line = f"( {blk}:{r:02} ".encode() + b"x" * 110 + b" )"
        if got[r * 128:(r + 1) * 128].rstrip(b" ") != line:
            print(f"FAILED: block {blk} row {r} differs"); sys.exit(1)
ok.append("a 16 KB file (16 1K packets) arrives byte for byte in blocks 50-54")

bridge.unplug()
wait_for(err, "lost")
time.sleep(1.0)
type_line("99 .")                       # nowhere to go
wait_for(err, "not connected")
bridge.plug()
wait_for(err, "is back")
time.sleep(0.3)
type_line("50 6 + .")
wait_for(out, "56")
ok.append("the port vanishing and coming back: kwai says so and carries on")

type_line("~?")
wait_for(err, "~x FILE")
type_line("~q")
kp.wait(timeout=5)
ok.append("~? lists the commands and ~q quits")
lfp.kill()
shutil.rmtree(work)
for o in ok:
    print("ok:", o)
print("kwai tests passed")
