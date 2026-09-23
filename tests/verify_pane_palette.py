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
  E. (v2.0.7) 走设置页「窗格配色」子页 (Ctrl+B s → W) 改默认背景/红色/复位，
     验证 ini 落盘与窗格字节都变。
  F. (v2.0.8) pane_scrollbar / pane_scrollbar_track 滚动条颜色（悬停右缘后抓字节）。
  G. (v2.0.8) 窗格配色页两列的值起始列一致（libvterm 回放，宽字符对齐）。

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


def run_settings_ui(keys_after_open, marker):
    """E 组（v2.0.7）：不写 ini，改走设置页 UI ——
    Ctrl+B s 进设置 → W 进「窗格配色」子页 → 按 keys_after_open 操作 →
    Esc Esc 出子页 → Ctrl+B n 回 shell 窗格 → printf 一行普通文字 + 红字。
    返回 (宿主收到的字节, termux.ini 内容)。"""
    td = tempfile.mkdtemp(prefix="termux_palette_ui_")
    exe = os.path.join(td, "termux")
    shutil.copy2(EXE, exe)
    os.chmod(exe, 0o755)
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(td)
        os.environ["TERM"] = "xterm-256color"; os.environ["SHELL"] = "/bin/sh"; os.environ["PS1"] = "$ "
        try:
            os.execv(exe, ["termux"])
        finally:
            os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
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

    def send(b, t=0.5):
        os.write(fd, b)
        drain(t)

    drain(1.5)
    send(b"\x02s", 0.8)            # 设置页
    send(b"W", 0.5)                 # 窗格配色子页
    for k, t in keys_after_open:
        send(k, t)
    send(b"\x1b", 0.4); send(b"\x1b", 0.4)   # 出子页（两次 Esc）
    send(b"\x02n", 0.8)            # 回 shell 窗格
    send(("printf '%s\\n'; printf '\\033[31mRED_%s\\033[0m\\n'\r" % (marker, marker)).encode(), 2.0)
    ini = ""
    try:
        with open(os.path.join(td, "termux.ini"), encoding="utf-8") as f:
            ini = f.read()
    except OSError:
        pass
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    shutil.rmtree(td, ignore_errors=True)
    return bytes(got), ini


def run_hover_scrollbar(ini_text):
    """起 termux，seq 1 200 造历史，再发一条 SGR 鼠标移动到最右列（滚动条只在悬停时画），
    返回悬停之后收到的字节。"""
    td = tempfile.mkdtemp(prefix="termux_palette_sb_")
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
    os.write(fd, b"seq 1 200; printf 'PLAIN_F\\n'\r")
    drain(2.0)
    del got[:]
    os.write(fd, ("\x1b[<35;%d;12M" % C).encode())   # 鼠标移动到最右列
    drain(1.0)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    shutil.rmtree(td, ignore_errors=True)
    return bytes(got)


def capture_pane_page():
    """起 termux(40x120)，Ctrl+B s → W，返回收到的全部字节（窗格配色页整屏）。"""
    td = tempfile.mkdtemp(prefix="termux_palette_page_")
    exe = os.path.join(td, "termux")
    shutil.copy2(EXE, exe)
    os.chmod(exe, 0o755)
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(td)
        os.environ["TERM"] = "xterm-256color"; os.environ["SHELL"] = "/bin/sh"; os.environ["PS1"] = "$ "
        try:
            os.execv(exe, ["termux"])
        finally:
            os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
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
    os.write(fd, b"\x02s"); drain(0.8)
    os.write(fd, b"W"); drain(0.8)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    shutil.rmtree(td, ignore_errors=True)
    return bytes(got)


# 每行输出「值段」的起始列：紧跟在色块（两个纯底色空格）之后的那一格。
# 只看第 6..15 行（0 基 5..14），每行应恰有两个（左右两列）。
VTERM_COLS_C = r"""
#include <vterm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    int R = atoi(argv[1]), C = atoi(argv[2]);
    VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
    VTermScreen *scr = vterm_obtain_screen(vt); vterm_screen_reset(scr, 1);
    FILE *f = fopen(argv[3], "rb"); if (!f) return 2;
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) vterm_input_write(vt, buf, n);
    for (int r = 5; r < 15; r++) {
        for (int c = 0; c < C; c++) {
            VTermScreenCell cell; VTermPos p = {r, c};
            vterm_screen_get_cell(scr, p, &cell);
            /* 值段以 '(' 或 '#' 开头，且前一格是空格、前两格是色块（非空白字符或空格，
               颜色不在这里判），用「'(' 后面跟的不是标签括号」来区分：标签括号后紧跟汉字，
               值括号后紧跟「跟」「内」；'#' 只出现在值里。 */
            if (cell.chars[0] == '#') { printf(" %d", c); continue; }
            if (cell.chars[0] == '(') {
                VTermScreenCell nx; VTermPos q = {r, c + 1};
                vterm_screen_get_cell(scr, q, &nx);
                if (nx.chars[0] == 0x8DDF /* 跟 */ || nx.chars[0] == 0x5185 /* 内 */) printf(" %d", c);
            }
        }
        printf("\n");
    }
    return 0;
}
"""


def ini_pane_lines(ini):
    """ini 里真正生效的 pane_* 行（注释行里也有 pane_background 字样，不能直接 in）。"""
    return [l.strip() for l in ini.splitlines() if l.strip().startswith("pane_")]


def sgr_last_before(data, needle):
    """最后一个 needle 前最近的 SGR（设置页 UI 流程里回显被重绘打碎，只有输出行完整）。"""
    hits = [m.start() for m in re.finditer(re.escape(needle), data)]
    if not hits:
        return None
    seg = data[max(0, hits[-1] - 200):hits[-1]]
    m = list(re.finditer(rb"\x1b\[([0-9;]*)m", seg))
    return m[-1].group(1).decode() if m else ""


def sgr_before(data, needle, nth=None):
    """画 needle 那一行用的 SGR（最近的一个 SGR 序列）。
    命令行回显里也含 needle（printf 的字面量 `PLAIN\\n'` / `RED_X\\033[0m`），
    但字面量后面紧跟着反斜杠，程序输出后面不会 —— 用这个区分，并取最后一个
    非回显命中。早先按「第 nth 个」数，CI 慢机器上回显行被重绘两次就数错了
    （v2.0.7 CI 上 C 项因此假红：拿到的是回显的 0;37;40）。nth 参数保留只为兼容。"""
    del nth
    hits = [m.start() for m in re.finditer(rb"(?<![A-Z_])" + re.escape(needle) + rb"(?!\\)", data)]   # 也排除 RED_PLAIN_x 里的子串
    if not hits:
        return None
    i = hits[-1]
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

    # E. 设置页 UI（v2.0.7）：不用手改 ini，在「窗格配色」子页里改
    #   E1 默认选中「默认背景」：Enter → 打 ffffff → Enter
    e1, ini1 = run_settings_ui([(b"\r", 0.4), (b"ffffff\r", 0.6)], "UI_E1")
    ck("E1 设置页改默认背景 → termux.ini 写入 pane_background = #ffffff",
       "pane_background = #ffffff" in ini_pane_lines(ini1), "pane 行=%r" % ini_pane_lines(ini1))
    se1 = sgr_last_before(e1, b"UI_E1")
    ck("E1 设置页改完立即生效：窗格普通文字底色发 48;2;255;255;255",
       se1 is not None and "48;2;255;255;255" in se1, "实际 SGR=%r" % se1)
    #   E2 左列往下 5 行 = 「红色」(索引 1)：Enter → ff0000 → Enter；再 R 复位默认背景？不，
    #      这里只验证红色：ini 只应有 pane_red，不应有 pane_background
    e2, ini2 = run_settings_ui([(b"\x1b[B", 0.2)] * 5 + [(b"\r", 0.4), (b"ff0000\r", 0.6)], "UI_E2")   # 左列：背景/前景/滑块/轨道/黑/红
    ck("E2 设置页改「红色」→ termux.ini 写入 pane_red = #ff0000",
       ini_pane_lines(ini2) == ["pane_red = #ff0000"], "pane 行=%r" % ini_pane_lines(ini2))
    se2 = sgr_last_before(e2, b"RED_UI_E2")
    ck("E2 SGR 31 红字发 38;2;255;0;0",
       se2 is not None and "38;2;255;0;0" in se2, "实际 SGR=%r" % se2)
    #   E3 改完再按 R 复位当前项：ini 里不应再有该键
    e3, ini3 = run_settings_ui([(b"\r", 0.4), (b"ffffff\r", 0.6), (b"r", 0.5)], "UI_E3")
    ck("E3 R 复位当前项 → ini 不再含 pane_background", ini_pane_lines(ini3) == [], "pane 行=%r" % ini_pane_lines(ini3))
    se3 = sgr_last_before(e3, b"UI_E3")
    ck("E3 复位后普通文字回到 16 色 ;40", se3 is not None and re.search(r"(^|;)40$", se3) is not None, "实际 SGR=%r" % se3)

    # F. (v2.0.8) 滚动条颜色：pane_scrollbar / pane_scrollbar_track。用户反馈浅色
    #    pane_background 下内置深色渐变滚动条「和背景相同」。产生历史后把鼠标移到最右列
    #    让滚动条出现，断言 thumb / track 用了配置色；不配时不出现该色（渐变表里没有纯红/纯蓝）。
    f0 = run_hover_scrollbar(None)
    ck("F0 前置：悬停右缘后画出了滚动条轨道", "│".encode() in f0)
    ck("F0 未配置时滚动条不是纯红/纯蓝（内置渐变）", b"48;2;255;0;0m" not in f0 and b"48;2;0;0;255m" not in f0)
    f1 = run_hover_scrollbar("[theme]\npane_scrollbar = #ff0000\npane_scrollbar_track = #0000ff\n")
    ck("F1 pane_scrollbar=#ff0000 → 滑块发 48;2;255;0;0", b"48;2;255;0;0m " in f1)
    ck("F1 pane_scrollbar_track=#0000ff → 轨道底色发 48;2;0;0;255", b"48;2;0;0;255m" in f1)
    sf = sgr_last_before(f1, b"PLAIN_F")
    ck("F1 只配滚动条时普通文字仍是 16 色 ;40（不影响 palette 透传）",
       sf is not None and re.search(r"(^|;)40$", sf) is not None, "实际 SGR=%r" % sf)
    #    F2 走设置页：左列第 3 行 = 滚动条滑块
    e4, ini4 = run_settings_ui([(b"\x1b[B", 0.2)] * 2 + [(b"\r", 0.4), (b"00ff00\r", 0.6)], "UI_F2")
    ck("F2 设置页改「滚动条滑块」→ ini 写入 pane_scrollbar = #00ff00",
       ini_pane_lines(ini4) == ["pane_scrollbar = #00ff00"], "pane 行=%r" % ini_pane_lines(ini4))

    # G. (v2.0.8) 对齐：两列的值「(跟随终端)/(内置渐变)/#hex」起始列必须各自一致。
    #    v2.0.7 用 %-16s 按字节补齐中文标签，「默认前景(字色)」和「黑色」的值差了 2 列
    #    （用户反馈「没对齐」）。用 libvterm 回放（pyte 对宽字符列算不准，不能用）。
    if os.path.exists("/usr/include/vterm.h"):
        td = tempfile.mkdtemp(prefix="termux_palette_align_")
        src = os.path.join(td, "vt.c"); vt = os.path.join(td, "vt"); dump = os.path.join(td, "g.bin")
        with open(src, "w") as f:
            f.write(VTERM_COLS_C)
        with open(dump, "wb") as f:
            f.write(capture_pane_page())
        r = subprocess.run(["gcc", "-O1", src, "-o", vt, "-lvterm"], capture_output=True, text=True)
        if r.returncode == 0:
            out = subprocess.run([vt, "40", "120", dump], capture_output=True, text=True).stdout
            left, right = set(), set()
            nrows = 0
            for line in out.splitlines():
                cs = [int(x) for x in line.split()]
                if len(cs) >= 2:
                    nrows += 1
                    left.add(cs[0]); right.add(cs[1])
            ck("G 前置：抓到 10 行、每行两列（20 项）", nrows == 10, "只有 %d 行：%r" % (nrows, out))
            ck("G 左列值起始列全部相同", len(left) == 1, "左列起始列集合=%r" % sorted(left))
            ck("G 右列值起始列全部相同", len(right) == 1, "右列起始列集合=%r" % sorted(right))
        else:
            print("  [SKIP] libvterm 编译失败：%s" % r.stderr.strip()[:200])
        shutil.rmtree(td, ignore_errors=True)
    else:
        print("  [SKIP] G 对齐检查 —— 本机没有 libvterm-dev")

    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("PANE PALETTE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
