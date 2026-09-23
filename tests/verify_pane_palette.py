#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_pane_palette.py — [theme] pane_* 窗格 palette 端到端（v2.0.6）。

用户报告：改了 [theme] background，cmd 背景还是黑的。原因：background 只管
termux 自己的 UI；cmd 的普通文字是 16 色索引属性 0x07，渲染时原样发
\\x1b[0;37;40m，宿主终端按自己的 palette 画 40 = 黑，termux 的主题看不到它。

v2.0.6 加了 pane_foreground / pane_background / pane_black … pane_bright_white
（像 Windows Terminal 的 color scheme）。本测试真 PTY 起 termux：

  A. 不配置：shell 回显的普通文字仍发 16 色 ;40m（一字节不变，兼容）。
  B. 配 pane_background = #ffffff, pane_foreground = #24292f：
     同样的文字必须发 48;2;255;255;255 与 38;2;36;41;47，且不再出现 ;40m 底色。
  C. 配 pane_red = #ff0000：shell 里 printf '\\033[31m' 的红字发 38;2;255;0;0。
  D. libvterm 回放 B：正文行的背景格真的是白的（有 libvterm 时）。

修前二进制跑本脚本：A 过，B/C 必红（那时根本没有这些键）。
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
EXE = os.path.abspath(os.environ.get("TERMUX_PALETTE_EXE") or os.path.join(ROOT, "termux-linux"))
R, C = 24, 80
FAILS = []


def ck(name, cond, extra=""):
    print("  [%s] %s %s" % ("ok" if cond else "FAIL", name, extra if not cond else ""))
    if not cond:
        FAILS.append(name)


def run(ini_text, marker):
    """在独立临时目录起 termux（termux.ini 落在二进制同目录 —— 所以要把二进制
    拷进临时目录，ini 才能被它读到）。返回宿主收到的全部字节。"""
    td = tempfile.mkdtemp(prefix="termux_palette_")
    exe = os.path.join(td, "termux")
    shutil.copy2(EXE, exe)
    os.chmod(exe, 0o755)
    if ini_text is not None:
        with open(os.path.join(td, "termux.ini"), "w", encoding="utf-8") as f:
            f.write(ini_text)
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(td)
        os.environ["TERM"] = "xterm-256color"; os.environ["SHELL"] = "/bin/sh"; os.environ["PS1"] = "$ "
        try:
            os.execv(exe, ["termux"])
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
    # 普通文字 + 一段 SGR 31 红字
    os.write(fd, ("printf '%s\\n'; printf '\\033[31mRED_%s\\033[0m\\n'\r" % (marker, marker)).encode())
    drain(2.0)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    shutil.rmtree(td, ignore_errors=True)
    return bytes(got)


def sgr_before(data, needle, nth=0):
    """第 nth 个 needle 前最近的一个 SGR 序列（就是画它用的属性）。
    注意命令行回显里也含 needle（printf 的字面量），程序输出是【之后】那个。"""
    hits = [m.start() for m in re.finditer(re.escape(needle), data)]
    if len(hits) <= nth:
        return None
    i = hits[nth]
    seg = data[max(0, i - 200):i]
    m = list(re.finditer(rb"\x1b\[([0-9;]*)m", seg))
    return m[-1].group(1).decode() if m else ""


VTERM_C = r"""
#include <vterm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    int R = atoi(argv[1]), C = atoi(argv[2]);
    VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
    VTermScreen *scr = vterm_obtain_screen(vt); vterm_screen_reset(scr, 1);
    VTermState *st = vterm_obtain_state(vt);
    FILE *f = fopen(argv[3], "rb"); if (!f) return 2;
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) vterm_input_write(vt, buf, n);
    /* 找到含 argv[4] 的行，打印该行第一格的背景 RGB */
    for (int r = 0; r < R; r++) {
        char line[512] = {0};
        for (int c = 0; c < C && c < 511; c++) { VTermScreenCell cell; VTermPos p = {r, c};
            vterm_screen_get_cell(scr, p, &cell); unsigned ch = cell.chars[0]; line[c] = (ch && ch < 128) ? (char)ch : ' '; }
        if (strstr(line, argv[4])) {
            VTermScreenCell cell; VTermPos p = {r, 0}; vterm_screen_get_cell(scr, p, &cell);
            vterm_state_convert_color_to_rgb(st, &cell.bg);
            printf("bg=%d,%d,%d\n", cell.bg.rgb.red, cell.bg.rgb.green, cell.bg.rgb.blue);
            return 0;
        }
    }
    printf("notfound\n"); return 1;
}
"""


def main():
    print("=== 窗格 palette pane_* (tests/verify_pane_palette.py) ===")
    if not os.path.exists(EXE):
        print("FAIL: 找不到 %s，先跑 make linux" % EXE, file=sys.stderr)
        return 1

    # A. 无配置：透传
    a = run(None, "PLAIN_A")
    ck("A 前置：回显出现", b"PLAIN_A" in a)
    sa = sgr_before(a, b"PLAIN_A", nth=1)
    ck("A 无配置时普通文字仍是 16 色索引 ;40（兼容，一字节不变）",
       sa is not None and sa.endswith(";37;40"), "实际 SGR=%r" % sa)

    # B. 白底深字
    ini = "[theme]\npane_background = #ffffff\npane_foreground = #24292f\n"
    b = run(ini, "PLAIN_B")
    ck("B 前置：回显出现", b"PLAIN_B" in b)
    sb = sgr_before(b, b"PLAIN_B", nth=1)   # 第 1 个 = printf 的输出行（第 0 个是回显）
    ck("B pane_background=#ffffff → 普通文字底色发 48;2;255;255;255",
       sb is not None and "48;2;255;255;255" in sb, "实际 SGR=%r" % sb)
    ck("B pane_foreground=#24292f → 字色发 38;2;36;41;47",
       sb is not None and "38;2;36;41;47" in sb, "实际 SGR=%r" % sb)
    ck("B 不再发 16 色 ;40 底色", sb is not None and not re.search(r"(^|;)40$", sb), "实际 SGR=%r" % sb)

    # C. 索引色映射
    ini_c = "[theme]\npane_red = #ff0000\n"
    c = run(ini_c, "PLAIN_C")
    # 第 0 个 RED_PLAIN_C 是命令行回显里的字面量（printf '...RED_PLAIN_C...'），
    # 第 1 个才是 printf 输出的红字。
    ck("C 前置：红字输出出现（回显 + 输出 = 2 处）", c.count(b"RED_PLAIN_C") >= 2, "只有 %d 处" % c.count(b"RED_PLAIN_C"))
    sc = sgr_before(c, b"RED_PLAIN_C", nth=1)
    ck("C pane_red=#ff0000 → SGR 31 红字发 38;2;255;0;0",
       sc is not None and "38;2;255;0;0" in sc, "实际 SGR=%r" % sc)
    sc_plain = sgr_before(c, b"PLAIN_C", nth=1)
    ck("C 只配了 pane_red 时普通文字的底色仍是 16 色 40（未配的槽位不动）",
       sc_plain is not None and re.search(r"(^|;)40$", sc_plain) is not None, "实际 SGR=%r" % sc_plain)

    # D. libvterm 语义级
    if os.path.exists("/usr/include/vterm.h"):
        td = tempfile.mkdtemp(prefix="termux_palette_vt_")
        src = os.path.join(td, "vt.c"); vt = os.path.join(td, "vt"); dump = os.path.join(td, "b.bin")
        with open(src, "w") as f:
            f.write(VTERM_C)
        with open(dump, "wb") as f:
            f.write(b)
        r = subprocess.run(["gcc", "-O1", src, "-o", vt, "-lvterm"], capture_output=True, text=True)
        if r.returncode == 0:
            out = subprocess.run([vt, str(R), str(C), dump, "PLAIN_B"], capture_output=True, text=True).stdout.strip()
            ck("D libvterm 回放：PLAIN_B 那一行的背景格是白色 (255,255,255)", out == "bg=255,255,255", "实际 %r" % out)
        else:
            print("  [SKIP] libvterm 编译失败：%s" % r.stderr.strip()[:200])
        shutil.rmtree(td, ignore_errors=True)
    else:
        print("  [SKIP] libvterm 语义级回放 —— 本机没有 libvterm-dev")

    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("PANE PALETTE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
