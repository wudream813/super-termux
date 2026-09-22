#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_lastcol.py — alt 屏全屏程序铺满整宽时，最后一列不能被擦掉（v2.0.5）。

真机现象：Linux 上在 termux 里开 nano 编辑一个 80 列宽的文件，每行最后一个字符
显示为空白；直接跑 nano 正常。

机理：整屏路径 render_frame 每行画完正文后无条件补 "\\x1b[0m\\x1b[K" 清行尾。
正文已经写满宿主整宽（text_rc == host_cols）时，宿主终端的光标处于「延迟折行
挂起」态 —— 逻辑上仍在末列。此时 EL(0) 从末列起清，把刚写的最后一列擦掉。
各家终端对此行为不一致：tmux / pyte 保留，libvterm（neovim / 不少 GUI 终端的
内核）清掉。不能赌，只能不发。

判据（真 PTY 起 termux，把它输出的字节喂给终端仿真）：
  1. 字节级（任何环境都能跑）：正文满宽的那一行末尾，"E" 之后不能紧跟 EL 序列。
  2. 语义级（有 libvterm 时）：libvterm 回放后第 N 列真的是 'E'。
     pyte 在这条上会假绿，所以不用 pyte。

自证：TERMUX_LASTCOL_EXE 指向修前二进制时本脚本必须失败（开发时验过）。
"""
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 子进程会 chdir 到临时目录，相对路径必须先转绝对（make 里传的是 "termux-linux"）。
EXE = os.path.abspath(os.environ.get("TERMUX_LASTCOL_EXE") or os.path.join(ROOT, "termux-linux"))
R, C = 24, 80
FAILS = []


def ck(name, cond, extra=""):
    print("  [%s] %s %s" % ("ok" if cond else "FAIL", name, extra if not cond else ""))
    if not cond:
        FAILS.append(name)


VTERM_C = r"""
#include <vterm.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    int R = atoi(argv[1]), C = atoi(argv[2]);
    VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
    VTermScreen *scr = vterm_obtain_screen(vt); vterm_screen_reset(scr, 1);
    FILE *f = fopen(argv[3], "rb"); if (!f) return 2;
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) vterm_input_write(vt, buf, n);
    for (int r = 0; r < R; r++) {
        for (int c = 0; c < C; c++) {
            VTermScreenCell cell; VTermPos p = {r, c};
            vterm_screen_get_cell(scr, p, &cell);
            unsigned ch = cell.chars[0]; if (!ch) ch = ' ';
            putchar(ch < 128 ? (int)ch : '?');
        }
        putchar('\n');
    }
    return 0;
}
"""


def build_vterm(td):
    if not os.path.exists("/usr/include/vterm.h"):
        return None
    src = os.path.join(td, "vt.c"); exe = os.path.join(td, "vt")
    with open(src, "w") as f:
        f.write(VTERM_C)
    r = subprocess.run(["gcc", "-O1", src, "-o", exe, "-lvterm"], capture_output=True, text=True)
    return exe if r.returncode == 0 else None


def main():
    print("=== alt 屏满宽最后一列 (tests/verify_lastcol.py) ===")
    if not os.path.exists(EXE):
        print("FAIL: 找不到 %s，先跑 make linux" % EXE, file=sys.stderr)
        return 1
    td = tempfile.mkdtemp(prefix="termux_lastcol_")
    # 不依赖 nano：用 sh 脚本模拟「进 alt 屏 + 每行 CUP 到行首 + 写满 C 列，末列 E」
    filler = os.path.join(td, "fill.sh")
    with open(filler, "w") as f:
        f.write("#!/bin/sh\n"
                "printf '\\033[?1049h\\033[H'\n"
                "i=1\n"
                "while [ $i -le %d ]; do\n"
                "  printf '\\033[%%d;1H' $i\n"
                "  printf '%%s' '%s'\n"
                "  i=$((i+1))\n"
                "done\n"
                "sleep 30\n" % (R - 1, "".join(str(j % 10) for j in range(1, C)) + "E"))
    os.chmod(filler, 0o755)

    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(td)
        os.environ["TERM"] = "xterm-256color"; os.environ["SHELL"] = "/bin/sh"
        os.environ["TERMUX_DUMP"] = "1"
        try:
            os.execv(EXE, ["termux"])
        finally:
            os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", R, C, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    got = bytearray()

    def drain(t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    got.extend(os.read(fd, 65536))
                except OSError:
                    break

    drain(1.5)
    os.write(fd, (filler + "\r").encode())
    drain(3.0)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass

    full = "".join(str(j % 10) for j in range(1, C)) + "E"
    ck("termux 确实把满宽行原样发到了宿主（前置条件）", full.encode() in got,
       "输出里没有满宽行，测试环境不对")

    # 判据 1：字节级。满宽行之后不能紧跟 EL。允许中间只有 SGR。
    bad = re.findall(re.escape(full).encode() + rb"(?:\x1b\[[0-9;]*m)*\x1b\[0?K", bytes(got))
    ck("满宽行之后没有紧跟 \\x1b[K（会在延迟折行态擦掉末列）", not bad,
       "命中 %d 处" % len(bad))

    # 判据 2：语义级，用 libvterm 回放。
    vt = build_vterm(td)
    if vt:
        dump = os.path.join(td, "out.bin")
        with open(dump, "wb") as f:
            f.write(bytes(got))
        r = subprocess.run([vt, str(R), str(C), dump], capture_output=True, text=True)
        rows = r.stdout.split("\n")
        body = [ln for ln in rows[1:R] if ln.startswith("1234567890")]
        ck("libvterm 回放：找到正文行（前置条件）", len(body) >= 3, "只有 %d 行" % len(body))
        ck("libvterm 回放：正文每行第 %d 列都是 'E'" % C,
           body and all(ln[C - 1] == "E" for ln in body),
           "末列实际是 %r" % [ln[C - 1] for ln in body[:3]])
    else:
        print("  [SKIP] libvterm 语义级回放 —— 本机没有 libvterm-dev（apt-get install libvterm-dev）")

    shutil.rmtree(td, ignore_errors=True)
    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("LASTCOL PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
