#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""drive.py —— 在真 pty 里驱动 termux-linux 的可复用测试驱动。

冒烟测试走的是「一切正常」的剧本；这个模块是给对抗测试用的，额外提供：
  - resize()   改 pty 尺寸并触发 SIGWINCH（冒烟测试从来没测过）
  - fds()      读 /proc/<pid>/fd，查 fd 泄漏
  - kids()     查残留子进程
  - frame()    用项目自己的 tools/frame2txt.py 解码最后一帧
"""
import fcntl
import os
import pty
import re
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAME2TXT = os.path.join(ROOT, "tools", "frame2txt.py")
EXE = os.environ.get("TERMUX_SMOKE_EXE", os.path.join(ROOT, "termux-linux"))


def _become_foreground():
    """在子进程里（fork 之后、exec 之前）建好【控制终端】。

    ★ 光 os.setsid() 不够。内核只在「pty 的前台进程组」尺寸变化时才发 SIGWINCH；
      没有控制终端的会话拿不到，于是 resize 看起来完全失效（/proc/<pid>/stat 的
      tty_nr 是 0）。真终端里 termux 本来就是前台进程，所以这是 harness 的缺陷
      而不是产品 bug —— 但不修的话 resize 这条路径一次都测不到。
    """
    os.setsid()
    fd = os.open(os.ttyname(0), os.O_RDWR)
    fcntl.ioctl(fd, termios.TIOCSCTTY, 0)
    if fd > 2:
        os.close(fd)


class Term:
    def __init__(self, cols=100, rows=24, dump=True, env=None):
        self.cols, self.rows = cols, rows
        self.tmp = tempfile.mkdtemp(prefix="termux_drv_")
        e = dict(os.environ)
        e["TERM"] = "xterm-256color"
        if dump:
            e["TERMUX_DUMP"] = "1"
        if env:
            e.update(env)
        self.master, slave = pty.openpty()
        self.set_winsize(cols, rows)
        self.p = subprocess.Popen([EXE], stdin=slave, stdout=slave, stderr=slave,
                                  cwd=self.tmp, env=e,
                                  preexec_fn=_become_foreground)
        os.close(slave)
        fcntl.fcntl(self.master, fcntl.F_SETFL, os.O_NONBLOCK)
        self.raw = b""

    # ---- 基本操作 ----
    def set_winsize(self, cols, rows):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        self.cols, self.rows = cols, rows

    def drain(self, t):
        end = time.time() + t
        while time.time() < end:
            try:
                self.raw += os.read(self.master, 65536)
            except (BlockingIOError, OSError):
                pass
            time.sleep(0.02)

    def send(self, data, wait=0.6):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master, data)
        self.drain(wait)

    def resize(self, cols, rows, wait=0.8):
        """改尺寸。内核会给前台进程组发 SIGWINCH。"""
        self.set_winsize(cols, rows)
        self.drain(wait)

    # ---- 观测 ----
    def alive(self):
        return self.p.poll() is None

    def fds(self):
        try:
            return sorted(os.listdir("/proc/%d/fd" % self.p.pid))
        except OSError:
            return []

    def fd_targets(self):
        out = []
        for f in self.fds():
            try:
                out.append(os.readlink("/proc/%d/fd/%s" % (self.p.pid, f)))
            except OSError:
                out.append("?")
        return out

    def kids(self):
        r = subprocess.run(["bash", "-c", "ps --ppid %d -o pid=,comm= 2>/dev/null" % self.p.pid],
                           capture_output=True, text=True)
        return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]

    def nframes(self):
        d = os.path.join(self.tmp, "render_dump.log")
        if not os.path.exists(d):
            return 0
        return len(re.findall(rb"\[render len \d+ model (\d+)x(\d+) host (\d+)x(\d+)\]\n",
                              open(d, "rb").read()))

    def frame_sizes(self):
        d = os.path.join(self.tmp, "render_dump.log")
        if not os.path.exists(d):
            return []
        # 用 finditer 取整段匹配；findall 带分组会返回元组，不好用。
        return [m.group(0) for m in re.finditer(
            rb"\[render len \d+ model \d+x\d+ host \d+x\d+\]", open(d, "rb").read())]

    def frame(self, idx=9999):
        d = os.path.join(self.tmp, "render_dump.log")
        if not os.path.exists(d):
            return ""
        cp = subprocess.run([sys.executable, FRAME2TXT, d, str(idx), str(self.cols)],
                            capture_output=True, text=True)
        # 剥掉 frame2txt 的 "  N| " 行号前缀（它自带竖线，会污染断言）
        return "\n".join(re.sub(r"^\s*\d+\|", "", ln) for ln in cp.stdout.split("\n"))

    def grid(self, idx=9999):
        return [ln.rstrip() for ln in self.frame(idx).split("\n")]

    # ---- 收尾 ----
    def quit(self, timeout=5):
        """SIGTERM 退出，同时继续读 pty（否则 write 会阻塞，误判成杀不掉）。"""
        if not self.alive():
            return self.p.returncode
        self.p.send_signal(15)
        end = time.time() + timeout
        while self.alive() and time.time() < end:
            self.drain(0.05)
        if self.alive():
            self.p.kill()
            self.p.wait(timeout=5)
            return "TIMEOUT"
        return self.p.returncode

    def close(self):
        try:
            os.close(self.master)
        except OSError:
            pass


PREFIX = b"\x02"
