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
  H. (v2.0.9) 预设方案应用 / 编辑时光标位置 / 窄终端单列+滚动。
  I. (v2.0.9) 60 列窄终端下各设置子页的按钮仍在屏幕内（不折行、不裁掉）。

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


_VTEXT_OK = True      # libvterm 不可用时由 vt_text()/border_pos() 置 False


def ck(name, cond, extra=""):
    # 依赖 libvterm 的那些组（G/I/J/K/L/M/N/O）在没有 libvterm 的机器上【不能算失败】：
    # 以前只是不画它们，但 N1 那种「顺手用 vt_text 结果求 max()」的写法会直接
    # ValueError 崩掉整个脚本（CI 的 Release Linux 作业没装 libvterm-dev，实测就是这样
    # 红了一整条流水线）。现在统一降级成 SKIP。
    if not cond and not _VTEXT_OK and re.match(r"^[G-O]\d", name):
        print("  [SKIP] %s —— 本机没有 libvterm-dev" % name)
        return
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
        with open(os.path.join(td, "termux.ini"), encoding="utf-8", errors="replace") as f:
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
# 只看第 7..16 行（0 基 6..15），每行应恰有两个（左右两列）。
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
    for (int r = 6; r < 16; r++) {
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


# argv[4] = 要找的文字：找到则输出 "col <起始列>"，没找到输出 "notfound"。
# 用于断言窄终端下按钮/内容没有被裁到屏幕外（或折行到侧栏行上）。
VTERM_NOWRAP_C = r"""
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
    const char *needle = argv[4];
    for (int r = 0; r < R; r++) {
        char line[1024]; int cc[1024]; int k = 0;
        for (int c = 0; c < C; c++) {
            VTermScreenCell cell; VTermPos p = {r, c};
            vterm_screen_get_cell(scr, p, &cell);
            if (cell.width == 0 || cell.chars[0] == (uint32_t)-1) continue;
            unsigned ch = cell.chars[0] ? cell.chars[0] : ' ';
            char u[5]; int m = 0;
            if (ch < 0x80) u[m++] = (char)ch;
            else if (ch < 0x800) { u[m++] = (char)(0xC0 | (ch >> 6)); u[m++] = (char)(0x80 | (ch & 0x3F)); }
            else { u[m++] = (char)(0xE0 | (ch >> 12)); u[m++] = (char)(0x80 | ((ch >> 6) & 0x3F)); u[m++] = (char)(0x80 | (ch & 0x3F)); }
            for (int i = 0; i < m && k < 1000; i++) { cc[k] = c; line[k++] = u[i]; }
        }
        line[k] = 0;
        char *hit = strstr(line, needle);
        if (hit) {
            /* leftblank：该行最左 20 列（侧栏区）是否全空。右侧内容折行时会从行首
             * 续写（v2.0.8 里「亮白…」顶掉了整行），leftblank = 0 即为折行污染。 */
            int lb = 1;
            for (int c = 0; c < 20 && c < C; c++) {
                VTermScreenCell cell; VTermPos p = {r, c};
                vterm_screen_get_cell(scr, p, &cell);
                unsigned ch = cell.chars[0];
                if (ch && ch != (uint32_t)-1 && ch != ' ') { lb = 0; break; }
            }
            printf("col %d row %d leftblank %d\n", cc[(int)(hit - line)] + 1, r + 1, lb);
            return 0;
        }
    }
    printf("notfound\n"); return 1;
}
"""


VTERM_SIG_C = r"""
/* v2.1.7：每行一个签名 = 该行所有格子的 (前景亮度和, 背景亮度和)。真彩色按 r+g+b 累加，
 * 调色板色按 1000+序号 累加，默认色记 0。用途：证明「过渡动画走完的静止屏」和
 * 「关掉动画」逐格同文同色 —— 只看文本会漏掉背景/前景被动画留在屏上的偏差。
 * argv: rows cols file */
#include <vterm.h>
#include <stdio.h>
#include <stdlib.h>
static long addcol(VTermState *st, VTermColor col, int is_default) {
    if (is_default) return 0;
    if ((col.type & VTERM_COLOR_TYPE_MASK) == VTERM_COLOR_INDEXED) return 1000 + col.indexed.idx;
    vterm_state_convert_color_to_rgb(st, &col);
    return (long)col.rgb.red + col.rgb.green + col.rgb.blue;
}
int main(int argc, char **argv) {
    (void)argc;
    int R = atoi(argv[1]), C = atoi(argv[2]);
    VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
    VTermScreen *scr = vterm_obtain_screen(vt); vterm_screen_reset(scr, 1);
    VTermState *st = vterm_obtain_state(vt);
    FILE *f = fopen(argv[3], "rb"); if (!f) return 2;
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) vterm_input_write(vt, buf, n);
    for (int r = 0; r < R; r++) {
        long fs = 0, bs = 0;
        for (int c = 0; c < C; c++) {
            VTermScreenCell cell; VTermPos p = {r, c};
            vterm_screen_get_cell(scr, p, &cell);
            fs += addcol(st, cell.fg, !!(cell.fg.type & VTERM_COLOR_DEFAULT_FG));
            bs += addcol(st, cell.bg, !!(cell.bg.type & VTERM_COLOR_DEFAULT_BG));
        }
        printf("%ld %ld\n", fs, bs);
    }
    return 0;
}
"""


def capture_page_keys(rows, cols, keys, ini=None, keep_ini=False):
    """起 termux(rows x cols)，依次发 keys，返回收到的全部字节。
    ini 非空时先写 termux.ini（用来造出「5 个菜单项」这类需要配置的场景）。"""
    td = tempfile.mkdtemp(prefix="termux_palette_keys_")
    exe = os.path.join(td, "termux")
    shutil.copy2(EXE, exe)
    os.chmod(exe, 0o755)
    if ini is not None:
        with open(os.path.join(td, "termux.ini"), "w", encoding="utf-8") as f:
            f.write(ini)
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(td)
        os.environ["TERM"] = "xterm-256color"; os.environ["SHELL"] = "/bin/sh"; os.environ["PS1"] = "$ "
        try:
            os.execv(exe, ["termux"])
        finally:
            os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
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
    for k in keys:
        os.write(fd, k); drain(0.4)
    drain(0.6)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    ini_txt = None
    if keep_ini:                    # v2.1.2：要核对 ini 落盘内容（长名字是否被截）时读回来
        try:
            with open(os.path.join(td, "termux.ini"), encoding="utf-8", errors="replace") as f:
                ini_txt = f.read()
        except OSError:
            ini_txt = ""
    shutil.rmtree(td, ignore_errors=True)
    return (bytes(got), ini_txt) if keep_ini else bytes(got)


# argv[4] = 要找的文字。若它以 '#' 开头：找到含它的行，要求光标在同一行且列 = 该串末尾之后 → "ok"；
# 否则：只报告它出现在哪一行（"row N"），没找到 → "notfound"。
VTERM_CURSOR_C = r"""
#include <vterm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static int row_text(VTermScreen *scr, int r, int C, char *line, int *cellcol /* cellcol[byteidx] */) {
    int n = 0;
    for (int c = 0; c < C; c++) {
        VTermScreenCell cell; VTermPos p = {r, c};
        vterm_screen_get_cell(scr, p, &cell);
        if (cell.width == 0 || cell.chars[0] == (uint32_t)-1) continue;   /* 宽字符后半格 */
        unsigned ch = cell.chars[0] ? cell.chars[0] : ' ';
        char u[5]; int k = 0;
        if (ch < 0x80) u[k++] = (char)ch;
        else if (ch < 0x800) { u[k++] = (char)(0xC0 | (ch >> 6)); u[k++] = (char)(0x80 | (ch & 0x3F)); }
        else { u[k++] = (char)(0xE0 | (ch >> 12)); u[k++] = (char)(0x80 | ((ch >> 6) & 0x3F)); u[k++] = (char)(0x80 | (ch & 0x3F)); }
        for (int i = 0; i < k && n < 1000; i++) { cellcol[n] = c; line[n++] = u[i]; }
    }
    line[n] = 0; return n;
}
int main(int argc, char **argv) {
    int R = atoi(argv[1]), C = atoi(argv[2]);
    VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
    VTermScreen *scr = vterm_obtain_screen(vt); vterm_screen_reset(scr, 1);
    VTermState *st = vterm_obtain_state(vt);
    FILE *f = fopen(argv[3], "rb"); if (!f) return 2;
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) vterm_input_write(vt, buf, n);
    const char *needle = argv[4];
    VTermPos cur; vterm_state_get_cursorpos(st, &cur);
    for (int r = 0; r < R; r++) {
        char line[1024]; int cc[1024];
        row_text(scr, r, C, line, cc);
        char *hit = strstr(line, needle);
        if (!hit) continue;
        int start = (int)(hit - line);
        if (needle[0] != '#') { printf("row %d col %d\n", r, cc[start]); return 0; }
        int endcol = cc[start + (int)strlen(needle) - 1] + 1;
        if (cur.row == r && cur.col == endcol) { printf("ok\n"); return 0; }
        printf("cursor row=%d col=%d, expected row=%d col=%d\n", cur.row, cur.col, r, endcol); return 0;
    }
    printf("notfound\n"); return 1;
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


VTERM_MEAS_C = r'''
/* v2.1.2：把一段宿主字节流回放成屏幕，按行打印所有制表符边框字符所在的【1 基显示列】。
 * 浮层「右侧没对齐」这类问题，光看文本行看不出来（宽字符/尾随空格会骗人），
 * 必须拿到真实的屏幕列号。argv: rows cols file */
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
    for (int r = 0; r < R; r++) {
        char mark[512]; int mk = 0, lastn = 0;
        for (int c = 0; c < C; c++) {
            VTermScreenCell cell; VTermPos p = { r, c };
            vterm_screen_get_cell(scr, p, &cell);
            unsigned ch = cell.chars[0];
            if (cell.width != 0 && ch && ch != ' ') lastn = c + 1;
            if (ch == 0x2502 || ch == 0x250c || ch == 0x2510 || ch == 0x2514 || ch == 0x2518)
                mk += snprintf(mark + mk, sizeof(mark) - mk, " %c@%d",
                               ch == 0x2502 ? '|' : (ch == 0x250c ? 'L' : (ch == 0x2510 ? 'R'
                                                  : (ch == 0x2514 ? 'l' : 'J'))), c + 1);
        }
        if (!mk) continue;
        printf("r%-2d last=%d%s\n", r + 1, lastn, mark);
    }
    return 0;
}
'''


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
    #   E1 ↓ 到「默认背景」：Enter → 打 ffffff → Enter
    e1, ini1 = run_settings_ui([(b"\x1b[B", 0.2), (b"\r", 0.4), (b"ffffff\r", 0.6)], "UI_E1")   # ↓ 从方案行到「默认背景」
    ck("E1 设置页改默认背景 → termux.ini 写入 pane_background = #ffffff",
       "pane_background = #ffffff" in ini_pane_lines(ini1), "pane 行=%r" % ini_pane_lines(ini1))
    se1 = sgr_last_before(e1, b"UI_E1")
    ck("E1 设置页改完立即生效：窗格普通文字底色发 48;2;255;255;255",
       se1 is not None and "48;2;255;255;255" in se1, "实际 SGR=%r" % se1)
    #   E2 左列往下 5 行 = 「红色」(索引 1)：Enter → ff0000 → Enter；再 R 复位默认背景？不，
    #      这里只验证红色：ini 只应有 pane_red，不应有 pane_background
    e2, ini2 = run_settings_ui([(b"\x1b[B", 0.2)] * 6 + [(b"\r", 0.4), (b"ff0000\r", 0.6)], "UI_E2")   # 方案行→背景/前景/滑块/轨道/黑/红
    ck("E2 设置页改「红色」→ termux.ini 写入 pane_red = #ff0000",
       ini_pane_lines(ini2) == ["pane_red = #ff0000"], "pane 行=%r" % ini_pane_lines(ini2))
    se2 = sgr_last_before(e2, b"RED_UI_E2")
    ck("E2 SGR 31 红字发 38;2;255;0;0",
       se2 is not None and "38;2;255;0;0" in se2, "实际 SGR=%r" % se2)
    #   E3 改完再按 R 复位当前项：ini 里不应再有该键
    e3, ini3 = run_settings_ui([(b"\x1b[B", 0.2), (b"\r", 0.4), (b"ffffff\r", 0.6), (b"r", 0.5)], "UI_E3")
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
    e4, ini4 = run_settings_ui([(b"\x1b[B", 0.2)] * 3 + [(b"\r", 0.4), (b"00ff00\r", 0.6)], "UI_F2")
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

    # H. (v2.0.9) 预设方案 / 窄终端单列 / 编辑时光标位置
    #   H1 方案行 →×5 = GitHub Light，Enter 应用：ini 出现 20 个 pane_ 键，窗格文字白底
    #   v2.1.0：方案行 Enter = 打开方案列表，列表里 Enter = 应用，所以是两次 Enter
    h1, inih = run_settings_ui([(b"\x1b[C", 0.2)] * 5 + [(b"\r", 0.6), (b"\r", 0.8)], "UI_H1")
    ck("H1 应用「GitHub Light」方案 → ini 写入全部 20 个 pane_* 键", len(ini_pane_lines(inih)) == 20, "pane 行=%r" % ini_pane_lines(inih))
    ck("H1 方案里 pane_background = #ffffff", "pane_background = #ffffff" in ini_pane_lines(inih))
    sh = sgr_before(h1, b"UI_H1")   # sgr_before 排除回显字面量与 RED_ 前缀，取程序输出那行
    ck("H1 应用方案后窗格普通文字发 48;2;255;255;255 / 38;2;36;41;47",
       sh is not None and "48;2;255;255;255" in sh and "38;2;36;41;47" in sh, "实际 SGR=%r" % sh)
    if os.path.exists("/usr/include/vterm.h"):
        td = tempfile.mkdtemp(prefix="termux_palette_cur_")
        src = os.path.join(td, "vt.c"); vt = os.path.join(td, "vt")
        with open(src, "w") as f:
            f.write(VTERM_CURSOR_C)
        r = subprocess.run(["gcc", "-O1", src, "-o", vt, "-lvterm"], capture_output=True, text=True)
        if r.returncode == 0:
            #   H2 光标：120 列，↓ 到默认背景，Enter，打 "ab" → 光标必须紧跟在 "#ab" 之后（同一行）
            dump = os.path.join(td, "h2.bin")
            with open(dump, "wb") as f:
                f.write(capture_page_keys(40, 120, [b"\x02s", b"W", b"\x1b[B", b"\r", b"ab"]))
            out = subprocess.run([vt, "40", "120", dump, "#ab"], capture_output=True, text=True).stdout.strip()
            ck("H2 编辑「默认背景」时光标紧跟在 #ab 之后（同一行、'#'列+3）", out == "ok", "实际 %r" % out)
            #   H3 右列：→ 到右列首项（青色），Enter（预填 6 位），光标在 '#'+7
            dump3 = os.path.join(td, "h3.bin")
            with open(dump3, "wb") as f:
                f.write(capture_page_keys(40, 120, [b"\x02s", b"W", b"\x1b[B", b"\x1b[C", b"\r"]))
            out3 = subprocess.run([vt, "40", "120", dump3, "#00cdcd"], capture_output=True, text=True).stdout.strip()
            ck("H3 右列项编辑时光标在预填 6 位之后", out3 == "ok", "实际 %r" % out3)
            #   H4 窄终端 60 列：两列装不下 → 单列，右列内容不再被裁掉；「亮白」滚动后可见
            dump4 = os.path.join(td, "h4.bin")
            with open(dump4, "wb") as f:
                f.write(capture_page_keys(24, 60, [b"\x02s", b"W"] + [b"\x1b[B"] * 20))
            out4 = subprocess.run([vt, "24", "60", dump4, "亮白                (跟随终端)"], capture_output=True, text=True).stdout.strip()
            m4 = re.match(r"row (\d+) col (\d+)", out4)
            ck("H4 60 列 × 24 行：单列 + 滚动后「亮白 (跟随终端)」完整可见，且在右侧区域内（列 >= 24，"
               "v2.0.8 是右列折行盖到侧栏/底栏上）", m4 is not None and int(m4.group(2)) >= 24, "实际 %r" % out4)
        else:
            print("  [SKIP] libvterm 编译失败：%s" % r.stderr.strip()[:200])
        shutil.rmtree(td, ignore_errors=True)
    else:
        print("  [SKIP] H2-H4 —— 本机没有 libvterm-dev")

    # I. (v2.0.9) 窄终端（60 列 × 24 行）：设置各子页不折行、按钮不被裁到屏幕外
    if os.path.exists("/usr/include/vterm.h"):
        td = tempfile.mkdtemp(prefix="termux_palette_narrow_")
        src = os.path.join(td, "vt.c"); vt2 = os.path.join(td, "vt2")
        with open(src, "w") as f:
            f.write(VTERM_NOWRAP_C)
        r = subprocess.run(["gcc", "-O1", src, "-o", vt2, "-lvterm"], capture_output=True, text=True)
        if r.returncode == 0:
            # (页键, needle, 说明, 是否要求该行左侧 20 列为空)
            #   只有窗格配色页能要求 leftblank：它滚到底时左侧对应的是侧栏空白区；
            #   键位/启动/行为页左侧本来就有侧栏菜单，只断言按钮没被裁出屏幕。
            # v2.1.4：[+] [P] 与每行的 [改][删] 从「启动」页搬进新的「条目管理」页，
            # 所以这一项先按 m 进那一页，再找 [改]。
            for page, needle, label, need_lb in [
                (b"W", "亮白", "窗格配色页：最后一项「亮白」在右侧区域内", 1),
                (b"K", "[改]", "键位页：[改] 按钮在屏幕内", 0),
                ((b"m",), "[改]", "条目管理页：[改] 按钮在屏幕内", 0),
                (b"B", "[-]", "行为页：scrollback [-] 按钮在屏幕内", 0),
            ]:
                d = os.path.join(td, "i.bin")
                with open(d, "wb") as f:
                    keys = [b"\x02s"]
                    if page:
                        keys.extend(page if isinstance(page, tuple) else [page])
                    if page == b"W":
                        keys += [b"\x1b[B"] * 20
                    f.write(capture_page_keys(24, 60, keys))
                out = subprocess.run([vt2, "24", "60", d, needle], capture_output=True, text=True).stdout.strip()
                m = re.match(r"col (\d+) row (\d+) leftblank (\d+)", out)
                ok_i = m is not None and int(m.group(1)) >= 24 and (not need_lb or m.group(3) == "1")
                ck("I 60 列 × 24 行 —— " + label +
                   ("，且所在行左侧 20 列（侧栏空白区）没被折行内容占掉" if need_lb else ""),
                   ok_i, "实际 %r" % out)
            # 不折行：窗格配色页右侧内容没有溢出到第 2 行（?7l 生效的间接证据：行内文字被截断而非绕行）
            d = os.path.join(td, "i2.bin")
            with open(d, "wb") as f:
                f.write(capture_page_keys(24, 60, [b"\x02s", b"W"]))
            out = subprocess.run([vt2, "24", "60", d, "侧栏"], capture_output=True, text=True).stdout.strip()
            ck("I 60 列 × 24 行 —— 侧栏「启动 (Startup)」整行仍在其行内（右侧内容没有折行盖过来）",
               out.startswith("col 1") or out == "notfound", "实际 %r" % out)
        else:
            print("  [SKIP] libvterm 编译失败：%s" % r.stderr.strip()[:200])
        shutil.rmtree(td, ignore_errors=True)
    else:
        print("  [SKIP] I 组 —— 本机没有 libvterm-dev")


    # ======================= J 组：方案选择浮层（v2.1.0）=======================
    # 用户反馈「窗格配色要可以选择」：方案行 Enter 弹出完整列表（8 项 + 色块预览），
    # ↑/↓ 选、数字键直达、Enter 应用、Esc 只关浮层。
    VTERM_TEXT_C = r"""
#include <vterm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    int R = atoi(argv[1]), C = atoi(argv[2]);
    VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
    VTermScreen *scr = vterm_obtain_screen(vt); vterm_screen_reset(scr, 1);
    FILE *f = fopen(argv[3], "rb"); char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) vterm_input_write(vt, buf, n);
    for (int r = 0; r < R; r++) {
        for (int c = 0; c < C; c++) {
            VTermScreenCell cell; VTermPos p = {r, c};
            vterm_screen_get_cell(scr, p, &cell);
            if (cell.width == 0 || cell.chars[0] == (uint32_t)-1) continue;
            unsigned ch = cell.chars[0] ? cell.chars[0] : ' ';
            if (ch < 0x80) putchar(ch);
            else if (ch < 0x800) { putchar(0xC0|(ch>>6)); putchar(0x80|(ch&0x3F)); }
            else { putchar(0xE0|(ch>>12)); putchar(0x80|((ch>>6)&0x3F)); putchar(0x80|(ch&0x3F)); }
        }
        putchar('\n');
    }
    return 0;
}
"""

    def vt_text(rows, cols, data, _cache={}):
        """把一段宿主字节流回放进 libvterm，返回屏幕文本行列表（宽字符后半格跳过）。"""
        global _VTEXT_OK
        if os.environ.get("TERMUX_NO_VTERM"):
            _VTEXT_OK = False
            return None
        if "bin" not in _cache:
            td = tempfile.mkdtemp(prefix="termux_palette_txt_")
            src = os.path.join(td, "vtext.c"); exe = os.path.join(td, "vtext")
            with open(src, "w") as f: f.write(VTERM_TEXT_C)
            r = subprocess.run(["gcc", "-O1", src, "-o", exe, "-lvterm"], capture_output=True, text=True)
            _cache["bin"] = exe if r.returncode == 0 else ""
            if not _cache["bin"]:
                _VTEXT_OK = False
                print("  [SKIP] J/K/L 组 —— vterm 文本 dump 编译失败：%s" % r.stderr.strip()[:160])
        if not _cache["bin"]: return None
        dump = os.path.join(tempfile.mkdtemp(prefix="termux_palette_d_"), "s.bin")
        with open(dump, "wb") as f: f.write(data)
        out = subprocess.run([_cache["bin"], str(rows), str(cols), dump], capture_output=True, text=True).stdout
        os.remove(dump)
        return out.splitlines()

    def vt_sig(rows, cols, data, _sc={}):
        """逐行「前景 + 背景」签名（见 VTERM_SIG_C）。没有 libvterm 时返回 None。"""
        if os.environ.get("TERMUX_NO_VTERM"):
            return None
        if "sbin" not in _sc:
            td = tempfile.mkdtemp(prefix="termux_sig_")
            src = os.path.join(td, "vsig.c"); exe = os.path.join(td, "vsig")
            with open(src, "w") as f: f.write(VTERM_SIG_C)
            r = subprocess.run(["gcc", "-O1", src, "-o", exe, "-lvterm"], capture_output=True, text=True)
            _sc["sbin"] = exe if r.returncode == 0 else ""
            if not _sc["sbin"]:
                print("  [SKIP] 前景/背景签名工具编译失败：%s" % r.stderr.strip()[:160])
        if not _sc["sbin"]: return None
        dump = os.path.join(tempfile.mkdtemp(prefix="termux_sig_d_"), "s.bin")
        with open(dump, "wb") as f: f.write(data)
        out = subprocess.run([_sc["sbin"], str(rows), str(cols), dump], capture_output=True, text=True).stdout
        os.remove(dump)
        return out.splitlines()

    VTERM_CELLS_C = r"""
/* 逐格输出背景色：Trrggbb = 24 位色，Pn = 调色板下标，'-' = 默认背景。
 * v2.1.6：设置页的滚动条改成「与终端原生滚动条同款」——滑块是整格换底色、不写字符，
 * 所以只看屏幕文本会「看不见条子」。判据必须落到单元格属性上（顺带把「指针靠近才出现」
 * 这条终端语义也能量出来：远离时整列都是默认背景）。 */
#include <vterm.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    (void)argc;
    int R = atoi(argv[1]), C = atoi(argv[2]);
    VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
    VTermScreen *scr = vterm_obtain_screen(vt); vterm_screen_reset(scr, 1);
    FILE *f = fopen(argv[3], "rb"); char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) vterm_input_write(vt, buf, n);
    for (int r = 0; r < R; r++) {
        for (int c = 0; c < C; c++) {
            VTermScreenCell cell; VTermPos p = {r, c};
            vterm_screen_get_cell(scr, p, &cell);
            VTermColor bc = cell.bg;
            if (bc.type & VTERM_COLOR_DEFAULT_BG) putchar('-');
            else if ((bc.type & VTERM_COLOR_TYPE_MASK) == VTERM_COLOR_RGB)
                printf("T%02x%02x%02x", bc.rgb.red, bc.rgb.green, bc.rgb.blue);
            else printf("P%u", (unsigned)bc.indexed.idx);
            putchar(' ');
        }
        putchar('\n');
    }
    return 0;
}
"""

    def vt_cells(rows, cols, data, _cc={}):
        """返回 rows 列表，每格是背景色串（'-' = 默认）。没有 libvterm 时返回 None。"""
        if os.environ.get("TERMUX_NO_VTERM"):
            return None
        if "cbin" not in _cc:
            td = tempfile.mkdtemp(prefix="termux_cells_")
            src = os.path.join(td, "vcell.c"); exe = os.path.join(td, "vcell")
            with open(src, "w") as f: f.write(VTERM_CELLS_C)
            r = subprocess.run(["gcc", "-O1", src, "-o", exe, "-lvterm"], capture_output=True, text=True)
            _cc["cbin"] = exe if r.returncode == 0 else ""
        if not _cc["cbin"]:
            return None
        dump = os.path.join(tempfile.mkdtemp(prefix="termux_cells_d_"), "s.bin")
        with open(dump, "wb") as f: f.write(data)
        out = subprocess.run([_cc["cbin"], str(rows), str(cols), dump], capture_output=True, text=True).stdout
        os.remove(dump)
        return [l.split(" ") for l in out.rstrip("\n").split("\n")]

    def qbgsum(tok):
        """背景色串的「亮度」：'-'/调色板返回 -1（不参与比较），Trrggbb 返回 r+g+b。"""
        if not tok or not tok.startswith("T") or len(tok) < 7:
            return -1
        try:
            return int(tok[1:3], 16) + int(tok[3:5], 16) + int(tok[5:7], 16)
        except ValueError:
            return -1

    j1 = capture_page_keys(24, 100, [b"\x02s", b"W", b"\r"])
    tj1 = vt_text(24, 100, j1)
    if tj1 is None:
        print("  [SKIP] J/K/L 组 —— 本机没有 libvterm-dev")
    else:
        joined = "\n".join(tj1)
        ck("J1 窗格配色页按 Enter → 弹出方案列表浮层（标题 + 8 个方案名）",
           "窗格配色方案" in joined and all(n in joined for n in
               ["Campbell", "One Half Light", "One Half Dark", "Solarized Light",
                "Solarized Dark", "GitHub Light", "Dracula", "Nord"]),
           "")
        ck("J1 浮层带方框（顶边/底边都在）", "┌" in joined and "└" in joined, "")
        ck("J1 方案行文案提示 Enter 打开列表", "Enter 打开方案列表" in joined, "")
        # 浮层每行要有真实色块预览（append_swatch 的形状：底色 SGR + 两空格 + 复位）。
        # 配置全空时窗格页本身不画色块，所以「标题之后」出现的色块只可能来自浮层。
        pi = j1.find("┌─ 窗格配色方案".encode())
        nsw = len(re.findall(rb"\x1b\[48;2;\d+;\d+;\d+m  \x1b\[0m", j1[pi:])) if pi >= 0 else 0
        ck("J1 浮层每行带色块预览（方案的实际颜色，不是空框）", nsw >= 32, "色块 %d 个" % nsw)
        #   J2：数字键 6 = 直接选第 6 个方案并应用（GitHub Light）
        _, inj2 = run_settings_ui([(b"\r", 0.6), (b"6", 0.9)], "UI_J2")
        pj2 = ini_pane_lines(inj2)
        ck("J2 浮层里按数字 6 → 应用 GitHub Light（ini 写满 20 键、背景 #ffffff）",
           len(pj2) == 20 and "pane_background = #ffffff" in pj2, "pane 行=%r" % (pj2[:2],))
        #   J3：Esc 只关浮层，不改配置
        _, inj3 = run_settings_ui([(b"\r", 0.6), (b"\x1b", 0.6)], "UI_J3")
        ck("J3 浮层按 Esc → 关浮层且不写配置", len(ini_pane_lines(inj3)) == 0,
           "pane 行=%r" % ini_pane_lines(inj3)[:2])
        #   J4：浮层打开时吞掉其它键（不能顺手改到窗格槽位选择）
        _, inj4 = run_settings_ui([(b"\r", 0.6), (b"\x1b[B", 0.4), (b"\x1b", 0.6)], "UI_J4")
        ck("J4 浮层里按 ↓ 只移动高亮，不落到槽位表", len(ini_pane_lines(inj4)) == 0, "")

        # ======================= K 组：矮终端整页滚动（v2.1.0）=======================
        # 终端只有 12 行时，v2.0.9 的外观页从第 13 行起整片画到屏幕外；侧栏的四个子页
        # 入口也会被挤掉。现在：侧栏自适应 + 每页可滚，↑/↓ 越界自动翻页。
        many_items = ("[menu]\r\n"
                      "1 = one, /bin/sh\r\n2 = two, /bin/sh\r\n3 = three, /bin/sh\r\n"
                      "4 = four, /bin/sh\r\n5 = five, /bin/sh\r\n")
        k1 = capture_page_keys(12, 100, [b"\x02s"], ini=many_items)
        tk1 = vt_text(12, 100, k1) or []
        jk1 = "\n".join(tk1)
        ck("K1 12 行终端：侧栏 [A]/[K]/[B]/[W] 四个入口与 [Ctrl+S] 都在屏内",
           all(x in jk1 for x in ["[A] 外观", "[K] 键位", "[B] 行为", "[W] 窗格配色", "[Ctrl+S] 保存配置"]),
           "")
        # v2.1.2 起侧栏列表是「可滚动的窗口」，不再用「添加(共5项)」这种死提示；
        # 还剩几项由表头/提示行的 (a-b/N) 说明。这里改判：[+] 行回到普通的「添加新条目」，
        # 第 4/5 项能不能滚进来看 N1。
        # v2.1.4：侧栏底部那条变成「[M] 条目管理」（新建/预设库/↑↓改删都在那一页里）。
        ck("K1 12 行 × 5 个菜单项：不再出现「添加(共N项)」死提示，底部入口是「条目管理」",
           "添加(共" not in jk1 and "[M] 条目管理" in jk1
           and "[+] 添加新条目" not in jk1 and "[P] 快速预设库" not in jk1, jk1[:400])
        # 一整串 ↓ 一次写入：31 个动作全部滚一遍，比逐键 0.4s 快两个数量级
        k2 = capture_page_keys(12, 100, [b"\x02s", b"\x1bOQ", b"\x1b[B" * 24])
        tk2 = vt_text(12, 100, k2) or []
        jk2 = "\n".join(tk2)
        # v2.1.6（用户第 5 条）：提示行跟着页面滚，不再钉在最底下一行 —— 于是 (a-b/N) 标记
        # 也只画在提示行身上。所以「连续 ↓」这条只成语义色区有没有被滚进来看，
        # 「行窗口标记」另给一个「滚到页尾（提示行在屏内）」的场景。
        ck("K2 12 行 × 外观页：连续 ↓ 后语义色区滚进可见区（background / cyan 行可见）",
           "background" in jk2 and "cyan" in jk2, jk2[:200])
        # （M 组自己那支 wheel() 定义在下面，这里按同一编码手搓：65 = 滚轮向下）
        k2w = capture_page_keys(12, 100, [b"\x02s", b"\x1bOQ",
                                          b"".join(b"\x1b[<65;60;8M\x1b[<65;60;8m" for _ in range(10))])
        jk2w = "\n".join(vt_text(12, 100, k2w) or [])
        ck("K2 滚到页尾：提示行（这一页最后一行）在屏内，右端标出行窗口 (a-b/20)",
           "提示: ↑/↓ 选择" in jk2w and "/20)" in jk2w and "14-22/20" in jk2w, jk2w[-160:])
        # 键位页：先从 40 行的完整截图里取出动作名序列的首/尾两项，再回看 12 行下
        # 滚到底的画面 —— 断言与动作表顺序无关（加动作不会误报）。
        tall = vt_text(40, 100, capture_page_keys(40, 100, [b"\x02s", b"\x1bOR"])) or []
        names = []
        for line in tall:
            l = line.split("│")[-1]     # 只取右侧内容区，别把侧栏文字当成行首
            m = re.match(r"^\s*(?:▶)?\s*([a-z][a-z0-9-]{2,})\s{2,}", l)
            if m and m.group(1) not in names:
                names.append(m.group(1))
        ck("K3 前置：40 行下键位页能列出全部动作名", len(names) >= 20, "%d 个" % len(names))
        if len(names) >= 2:
            k3 = capture_page_keys(12, 100, [b"\x02s", b"\x1bOR", b"\x1b[B" * 60])
            jk3 = "\n".join(vt_text(12, 100, k3) or [])
            ck("K3 12 行 × 键位页：滚到底能看到最后一个动作，且第 2 个动作已滚出",
               names[-1] in jk3 and names[1] not in jk3,
               "first=%s last=%s" % (names[1], names[-1]))

        # ======================= L 组：窄屏截断 + 悬停气泡（v2.1.0）=======================
        # 装不下时行尾留「...」；鼠标停在被截断的行上，以跟随鼠标的浮层给出全文。
        hint_full = "Ctrl+S 保存, Esc 返回"     # 提示行末尾那段，截断后看不见
        l0 = capture_page_keys(24, 60, [b"\x02s", b"\x1bOQ"])
        tl0 = vt_text(24, 60, l0) or []
        joined0 = "\n".join(tl0)
        row_hint = next((i + 1 for i, l in enumerate(tl0) if "提示: ↑/↓ 选择" in l), 0)
        ck("L1 60 列 × 外观页：提示行确实被截断（行尾出现 ...，尾部文字不可见）",
           row_hint > 0 and "..." in tl0[row_hint - 1] and hint_full not in joined0,
           "row=%d" % row_hint)
        if row_hint:
            l1 = capture_page_keys(24, 60, [b"\x02s", b"\x1bOQ",
                                           ("\x1b[<32;%d;%dM" % (40, row_hint)).encode()])
            tl1 = vt_text(24, 60, l1) or []
            joined1 = "\n".join(tl1)
            ck("L2 鼠标悬停在被截断的行上 → 浮层气泡出现（含框线）",
               "┌" in joined1 and "└" in joined1, "")
            ck("L2 气泡里能看到被截掉的尾部文字", hint_full in joined1, "")
            ck("L2 气泡跟随鼠标：出现在悬停行附近（不超过上下 4 行）",
               any(abs((i + 1) - row_hint) <= 4 and ("│" in tl1[i] or "┌" in tl1[i] or "└" in tl1[i])
                   for i in range(len(tl1))), "")
        #   L3：没被截断的行不该冒出气泡
        l2 = capture_page_keys(24, 100, [b"\x02s", b"\x1bOQ", b"\x1b[<32;30;3M"])
        tl2 = "\n".join(vt_text(24, 100, l2) or [])
        ck("L3 100 列下提示行没被截断 → 不画气泡（无框线）", "┌─" not in tl2, "")

    # ======================= M 组：滚轮 + 颜色编辑浮层 + 色块条自适应（v2.1.1）=======================
    # 用户反馈：①「过窄时，颜色编辑被截断」②「终端过矮时，设置右边窗格无法滚轮滚动」
    def wheel(col, row, n, down=True):
        btn = 65 if down else 64
        return "".join("\x1b[<%d;%d;%dM\x1b[<%d;%d;%dm" % (btn, col, row, btn, col, row)
                       for _ in range(n)).encode()

    tj = vt_text(24, 100, b"") if not os.environ.get("TERMUX_NO_VTERM") else None
    if tj is None:
        print("  [SKIP] M 组 —— 本机没有 libvterm-dev")
    else:
        # M1 矮终端（12 行）滚轮：外观页语义色区滚进可见区
        home = [b"\x1b", b"\x02s"]      # Esc 退出设置页 → s 再进：滚动态会复位
        m1a = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s"] + home + [b"\x1bOQ"])) or [])
        m1b = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s"] + home + [b"\x1bOQ", wheel(60, 8, 2)])) or [])
        ck("M1 12 行 × 外观页：滚轮向下把语义色区（background 行）滚进可见区",
           "background" not in m1a and "background" in m1b, "")
        # v2.1.6：提示行跟着滚 ⇒ 基线里它不在屏上，「标记起始行」不再是可用信号。
        # 「真滚动」的直接证据：页首那一行（■ 配色主题）滚出去了 —— 选中项平移不会这样。
        ck("M1 滚轮滚动后页首那行滚出可见区（真滚动，不是选中项平移）",
           "■ 配色主题 (Theme)" in m1a and "■ 配色主题 (Theme)" not in m1b, "")
        wm = re.compile(r"提示.*?\((\d+)-(\d+)/(\d+)\)", re.S)
        wa = wb = None
        # M2 滚轮向上回到原位（先滚开再滚回：否则「页首在屏内」在旧版也成立）
        # 滚到底（再往上滚一步都越界、被夹住）后连滚 9 下向上 → 必须逐字回到基线那一屏。
        m2 = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s"] + home + [b"\x1bOQ",
                                                                    wheel(60, 8, 9),
                                                                    wheel(60, 8, 9, down=False)])) or [])
        w2 = wm.search(m2)
        ck("M2 滚轮向上能回到页首（页首行重新出现、语义色区又滚出去）",
           "■ 配色主题 (Theme)" not in m1b and "■ 配色主题 (Theme)" in m2
           and "background" not in m2, "滚后=%r 回来=%r" % (m1b[:120], m2[:120]))
        # M3 条目管理页 / 详情页（v2.1.5：启动页只剩「当前默认」那几行，条目表整张在 [M] 页）
        ini5m3 = ("[menu]\n1 = sh, /bin/sh\n2 = two, /bin/bash\n3 = three, /bin/sh\n"
                  "4 = four, /bin/sh\n5 = five, /bin/sh\n")
        m3a = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"m"], ini=ini5m3)) or [])
        m3b = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"m", wheel(60, 8, 3)], ini=ini5m3)) or [])
        ck("M3 12 行 × 条目管理页：滚轮（指针在右侧表上）能把后面的条目滚进可见区",
           "five" not in m3a and "▶[5]  five" in m3b and "three" in m3b,
           "基线=%r 滚后=%r" % (m3a[-160:], m3b[-160:]))
        # v2.1.4：启动项页的 Enter 只设「启动默认」，进详情页要先 m 去条目管理页再 Enter。
        m3c = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", wheel(60, 8, 3),
                                                                      b"m", b"\r", wheel(60, 8, 2)],
                                                              ini=ini3)) or [])
        ck("M3 12 行 × 菜单项详情页：滚轮能滚到第 3 个字段（启动目录）",
           "3. 启动目录" in m3c and "4. 启动默认颜色" not in m3a, "实际 %r" % m3c[:80])
        # M4 窗格配色页滚轮
        m4a = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"W"])) or [])
        m4 = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"W", wheel(60, 8, 7)])) or [])
        ck("M4 12 行 × 窗格配色页：滚轮向下把最后一项「亮白 (跟随终端)」滚进可见区",
           "亮白" not in m4a and "亮白" in m4, "")
        # M5 颜色编辑浮层：任何宽度都不截断
        for rows, cols in [(12, 30), (12, 44), (24, 60)]:
            t = "\n".join(vt_text(rows, cols, capture_page_keys(rows, cols, [b"\x02s", b"W",
                                                                              wheel(max(cols // 2, 20), 8, 6),
                                                                              b"\r", b"ab"])) or [])
            ck("M5 %dx%d：窄终端下 Enter 编辑颜色 → 浮层完整显示 名称 + #ab（表行会被裁掉）" % (rows, cols),
               "颜色编辑" in t and "#ab" in t and "┌" in t and "┘" in t, "")
        # M6 浮层光标：紧跟已输入位（1 基 = 框左沿 + 6 + 2）
        t60 = vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"W", wheel(50, 8, 6), b"\r", b"ab"])) or []
        row60 = next((i + 1 for i, l in enumerate(t60) if "#ab" in l), 0)
        cur60 = capture_page_keys(12, 100, [b"\x02s", b"W", wheel(50, 8, 6), b"\r", b"ab"])
        ck("M6 浮层光标落在值段之后（与「#ab」同一行）",
           row60 > 0 and ("\x1b[%d;%dH\x1b[?25h" % (row60, 46)).encode() in cur60,
           "row=%d" % row60)
        # M7 详情页色块条按宽度限格：窄到放不下 8 格时少画几格并标 +N，不再越界
        ini7 = "[menu]\n1 = sh, /bin/sh\n"
        def colorrow(rows, cols):
            t = vt_text(rows, cols, capture_page_keys(rows, cols,
                                                       [b"\x02s", b"m", b"\r"], ini=ini7)) or []
            return next((l for l in t if re.search(r"默认\s+\[?1\]?\s", l)), "")
        r40, r60, r100 = colorrow(24, 40), colorrow(24, 60), colorrow(24, 100)
        ck("M7 40 列 × 详情页：色块条只画放得下的几格 + 标 +N（第 8 格不再被裁出屏幕）",
           "N" in r40 and "8" not in r40, "40列=%r" % r40[-40:])
        ck("M7 60 列 × 详情页：8 格刚好放得下 → 不标 +N",
           "N" not in r60 and "8" in r60, "60列=%r" % r60[-40:])
        ck("M7 100 列 × 详情页：8 格全在（值 8 可见），色块不越界", "8" in r100, "100列=%r" % r100[-40:])

    # ======================= N 组：侧栏滚动 / 窄页省略 / 侧栏气泡 / 长名字（v2.1.2）=======================
    # 用户反馈 5 条：①导航选项要能滚，而不是省略成「添加(共4项)」②「启动默认颜色」这类
    # 标签过窄时没有 … ③光标在导航选项里应能触发旁边的悬停提示 ④配色方案浮层右框不齐
    # ⑤颜色编辑浮层右框不齐。前四条判据在下面，浮层右框（④⑤）用真实屏幕列号判。
    def dispw(t):
        import unicodedata
        w = 0
        for ch in t:
            if unicodedata.combining(ch):
                continue
            w += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
        return w

    def border_pos(rows, cols, data, _c={}):
        """返回 [(行号, 最后一个非空列, {'L':列,'R':列,'|':[列...]})]；没有 libvterm 时 None。"""
        global _VTEXT_OK
        if os.environ.get("TERMUX_NO_VTERM"):
            _VTEXT_OK = False
            return None
        if "exe" not in _c:
            td = tempfile.mkdtemp(prefix="termux_meas_")
            cs = os.path.join(td, "meas.c"); ex = os.path.join(td, "meas")
            with open(cs, "w") as f:
                f.write(VTERM_MEAS_C)
            r = subprocess.run(["gcc", "-O1", cs, "-o", ex, "-lvterm"], capture_output=True, text=True)
            _c["exe"] = ex if r.returncode == 0 else ""
            if not _c["exe"]:
                _VTEXT_OK = False
                print("  [SKIP] N5 —— 边框列号 dump 编译失败：%s" % r.stderr.strip()[:160])
        if not _c["exe"]:
            _VTEXT_OK = False
            return None
        dp = os.path.join(tempfile.mkdtemp(prefix="termux_meas_d_"), "s.bin")
        with open(dp, "wb") as f:
            f.write(data)
        out = subprocess.run([_c["exe"], str(rows), str(cols), dp], capture_output=True, text=True).stdout
        os.remove(dp)
        res = []
        for line in out.splitlines():
            m = re.match(r"^r(\d+)\s+last=(\d+)\s*(.*)$", line)
            if not m:
                continue
            pos = {}
            for kind, col in re.findall(r"([LRlJ|])@(\d+)", m.group(3)):
                pos.setdefault(kind, []).append(int(col))
            res.append((int(m.group(1)), int(m.group(2)), pos))
        return res

    def tip_move(lines, needle):
        """v2.1.5：在屏幕文本里找 needle（被截断成「...」的那段）所在行 + 起始显示列，
        拼出「把指针悬到那一格上」的 SGR 序列。行/列都不写死 —— 侧栏与右栏的行位会随
        屏高变，写死就是「只在某一档成立」的判据。找不到就返回空串（判据会红，比假绿好）。"""
        for i, l in enumerate(lines):
            j = l.find(needle)
            if j >= 0:
                return ("\x1b[<35;%d;%dM" % (dispw(l[:j]) + 3, i + 1)).encode()
        return b""

    nini5 = ("[menu]\n1 = sh, /bin/sh\n2 = 一个非常长的菜单项名字用来验证截断, /bin/bash\n"
             "3 = three, /bin/sh\n4 = four, /bin/sh\n5 = five, /bin/sh\n")
    nlong = "一" * 20                       # 60 字节：老结构体（32 字节）装不下，会被从中间切断
    nini_long = "[menu]\n1 = sh, /bin/sh\n2 = %s, /bin/bash\n" % nlong

    # v2.1.5：侧栏不再列条目（用户：「左侧不要再额外放 [1] cmd 那一串」）⇒ 「第 4/5 项滚不滚得
    # 进来」这件事整个挪到右侧的「[M] 条目管理」页；指针停在侧栏上滚也算（同一页只有一个窗口）。
    n1a = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"m"], ini=nini5)) or [])
    n1b = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"m", wheel(6, 6, 3)], ini=nini5)) or [])
    ck("N1 12 行 × 条目管理页：指针停在侧栏上滚，滚的也是这一页（侧栏没条目列表可滚了）",
       "[1]  sh" in n1a and "[5]" not in n1a and "▶[5]  five" in n1b and "添加(共" not in n1a,
       "基线=%r 滚后=%r" % (n1a[-160:], n1b[-160:]))
    n1c = "\n".join(vt_text(13, 100, capture_page_keys(13, 100, [b"\x02s", b"m", wheel(6, 6, 4)], ini=nini5)) or [])
    n1d = "\n".join(vt_text(13, 100, capture_page_keys(13, 100, [b"\x02s", b"m", wheel(6, 6, 4),
                                                                  wheel(6, 6, 9, down=False)], ini=nini5)) or [])
    ck("N1 13 行 × 滚轮向下换窗口、向上能滚回第 1 项（窗口回得到页首）",
       "[5]" in n1c and "▶[1]" not in n1c and "▶[1]  sh" in n1d, "向下=%r 回滚=%r" % (n1c[-90:], n1d[-90:]))
    n1h = vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s"], ini=nini5)) or []
    n1col = [l.split("│")[0] for l in n1h]          # 只看侧栏那一列，右侧画什么不影响判据
    n1add = next((i for i, l in enumerate(n1col) if "[M] 条目管理" in l), -1)
    n1start = next((i for i, l in enumerate(n1col) if "启动 (Startup)" in l), -1)
    # v2.1.6（用户第 2 条）：侧栏「启动」下面那行「默认：终端」被删掉 ⇒ [M] 紧贴启动行。
    ck("N1 24 行 × 侧栏只剩导航项：不列条目、不给条目区留空档，「默认：」那行已删、[M] 紧贴启动行",
       any("导航选项" in l and "(" not in l for l in n1col)
       and not [l for l in n1col if re.search(r"\[\d\]", l)]
       and "默认：" not in "\n".join(n1col)
       and n1add == n1start + 1 and "[+] 添加新条目" not in "\n".join(n1col),
       "启动行=%d [M]行=%d 条目行=%r" % (n1start + 1, n1add + 1,
                                        [l for l in n1col if re.search(r"\[\d\]", l)][:2]))

    n2 = vt_text(24, 50, capture_page_keys(24, 50, [b"\x02s", b"m", b"\r"], ini=nini5)) or []
    n2j = "\n".join(n2)
    ck("N2 50 列 × 详情页：四个字段标签都在（不再被硬切掉）",
       all(k in n2j for k in ["2. 启动命令行", "3. 启动目录", "4. 启动默认颜色"]), n2j[:200])
    ck("N2 50 列 × 详情页：放不下的行以 ... 结尾，且没有一行越过 50 列",
       n2j.count("...") >= 3 and all(dispw(l) <= 50 for l in n2),
       "%d 处 ...，最长 %d 列" % (n2j.count("..."), max([dispw(l) for l in n2] or [0])))

    # v2.1.5：侧栏那份条目列表撤掉后，「名字被截断」只剩右侧表格一处 ⇒ 悬停气泡也必须在那里给
    # （渲染端 render_menu_rows 现在给名称列/命令行列各登记一条气泡）。
    nhov_base = vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s", b"m"], ini=nini5)) or []
    nhov = vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s", b"m", tip_move(nhov_base, "一个非...")],
                                              ini=nini5)) or []
    nhovj = "\n".join(nhov)
    ck("N3 条目管理页：鼠标停在被截断的名字上 → 气泡给出全文（侧栏列表撤掉后唯一的落点）",
       "一个非常长的菜单项名字用来验证截断" in nhovj and "┌" in nhovj,
       "悬停序列=%r" % tip_move(nhov_base, "一个非..."))
    ck("N3 气泡落在分隔线右侧，不盖住侧栏，也不把侧栏各行顶掉",
       "\n".join(nhov_base) != nhovj
       and all(l.split("│")[0] == b.split("│")[0] for l, b in zip(nhov, nhov_base))
       and max(l.index("┌") for l in nhov if "┌" in l) > 20, "")
    nk = "\n".join(vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s", b"\x1bOB", b"\x1b[<35;12;7M"],
                                                       ini=nini5)) or [])
    ck("N4 选中项挪到「启动」行、鼠标停在侧栏空白处 → 不该弹出邻居条目的气泡", "┌─" not in nk, "")

    def popup_right_frame(rows, cols, keys):
        """浮层右框是否齐：┐ / ┘ / 正文右 │ 必须同列。返回 (右列, 例外) 或 None。"""
        bp = border_pos(rows, cols, capture_page_keys(rows, cols, keys, ini=None))
        if not bp:
            return None
        tops = [r for r in bp if "L" in r[2]]
        bots = [r for r in bp if "J" in r[2]]
        if not tops:
            return None
        tr, _l, m = tops[0]
        if "R" not in m:
            return None
        L, R = m["L"][0], m["R"][0]
        if R <= L:
            return None
        bad = []
        br = max(r[0] for r in bots) if bots else 0
        for r in bp:
            if "J" in r[2] and r[2]["J"][-1] != R:
                bad.append(("┘@%d!=%d" % (r[2]["J"][-1], R)))
            if r[0] <= tr or (br and r[0] >= br) or "L" in r[2] or "J" in r[2]:
                continue
            right = [x for x in r[2].get("|", []) if x > L]
            if right and right[-1] != R:
                bad.append("r%d 右│@%d!=%d" % (r[0], right[-1], R))
        return (R, bad)

    for nrow, ncol, ntag, nkeys in [(30, 100, "配色方案浮层 100 列", [b"\x02s", b"W", b"\r"]),
                                    (30, 60, "配色方案浮层 60 列", [b"\x02s", b"W", b"\r"]),
                                    (24, 100, "颜色编辑浮层 100 列",
                                     [b"\x02s", b"W", wheel(50, 8, 6), b"\r", b"ab"]),
                                    (12, 30, "颜色编辑浮层 30 列",
                                     [b"\x02s", b"W", wheel(15, 8, 6), b"\r", b"ab"])]:
        pr = popup_right_frame(nrow, ncol, nkeys)
        if pr is None:
            print("  [SKIP] N5 %s —— 没抓到浮层框" % ntag)
            continue
        ck("N5 %s：顶框 ┐ / 底框 ┘ / 正文右 │ 同列（右框不短一格）" % ntag,
           not pr[1], "右框应=%d 例外=%r" % (pr[0], pr[1]))

    nraw, nini = capture_page_keys(24, 100, [b"\x02s", b"\x13"], ini=nini_long, keep_ini=True)
    nline = [l for l in (nini or "").splitlines() if l.startswith("2 =")]
    ck("N7 60 字节长名字：Ctrl+S 落盘后 ini 里仍是完整的 20 个字（老结构体只有 32 字节）",
       nline and nlong in nline[0], repr(nline))
    nlt_base = vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s", b"m"], ini=nini_long)) or []
    nlt = "\n".join(vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s", b"m",
                                                                  tip_move(nlt_base, "一...")],
                                                        ini=nini_long)) or [])
    ck("N7 长名字悬停：气泡是全文，且没有半个 UTF-8 字符留下的 '?'",
       nlong in nlt and "?" not in nlt, "")

    # ======================= O 组：窄终端设置页左右两栏互不越界（v2.1.3）=======================
    # 用户报「40 列时设置页左右两栏互相盖住、分隔线 │ 整段消失」。机制有两条，都得钉住：
    #   (1) 右栏某行（菜单表格 [+]/[P] 按钮行、说明行）写到 host_cols 之外 ⇒ 终端自动折行，
    #       把【下一行的行首】整段盖掉 —— 侧栏 [A]/[K]/[B]/[W] 就是这么没的；
    #   (2) 侧栏那批按 20~22 列写死的定宽标签串，在 sb_w 被窄终端夹小时盖掉 col sb_w 的 │。
    # 判据用真实屏幕列号（libvterm）：每行最右非空列 ≤ host_cols；凡是内容越过侧栏宽度的
    # 行，col sb_w 必须仍是 │。libvterm 不可用时 border_pos() 返回 None ⇒ 只判第一条。
    oin = ("[menu]\n1 = sh, /bin/sh\n2 = 一个非常长的菜单项名字用来验证截断, /bin/bash\n"
           "3 = three, /bin/sh\n4 = four, /bin/sh\n5 = five, /bin/sh\n")
    # 翻页只用真键位：启动页的分派是 F2..F5（裸 a/k/b/w 不是这一页的键，v2.1.6 之前
    # 这几个键其实一直停在启动页上测 ⇒ 七条判据测的是同一页，白测）。
    OPAGES = (("启动项页", [b"\x02s"]), ("外观页", [b"\x02s", b"\x1bOQ"]),
              ("键位页", [b"\x02s", b"\x1bOR"]), ("行为页", [b"\x02s", b"\x1bOS"]),
              ("窗格页", [b"\x02s", b"\x1bOT"]), ("菜单项详情页", [b"\x02s", b"m", b"\r"]),
              # v2.1.4 新增的一页（增删改都在里面），同样要满足「不折行 / 不盖侧栏」。
              ("条目管理页", [b"\x02s", b"m"]))
    for oc in (40, 50):
        osb = 22 if oc >= 44 else max(15, oc // 2)
        for oname, okeys in OPAGES:
            obytes = capture_page_keys(24, oc, okeys, ini=oin)
            ot = vt_text(24, oc, obytes) or []
            over = [r for r in range(1, 25) if r <= len(ot) and dispw(ot[r - 1]) > oc]
            ck("O %d 列 × %s：每行的显示宽度都不超过终端宽度（超宽会折行盖掉下一行行首）"
               % (oc, oname), not over, "越界行=%r" % over[:4])
            ob = border_pos(24, oc, obytes)
            if ob is None:
                continue
            olast = {r: (last, pos) for r, last, pos in ob}
            obad = []
            for r in range(3, 24):
                if r not in olast:
                    continue
                last, pos = olast[r]
                if last >= osb and osb not in pos.get("|", []):
                    obad.append((r, last))
            ck("O %d 列 × %s：内容越过侧栏宽度的行仍带 col %d 的分隔线（左右两栏没互盖）"
               % (oc, oname, osb), not obad, "缺分隔线的行=%r" % obad[:4])
            # v2.1.6：上面那条只查「这一行越过了侧栏宽度 ⇒ 分隔线还在」，而折行盖进侧栏时
            # 那一行的【最左一段】本身就是不该出现的正文 —— 直接反过来查：侧栏那一段
            # （col 1..osb-1）只允许出现已知的侧栏字符串。行为页的开关行曾经把 desc 整串
            # 直写出去，折到下一行行首，正是从这里漏掉的（24x50 实测 r7 开头 = 「鼠标支持（…」）。
            ozs = []
            for r in range(3, 24):
                if r > len(ot):
                    break
                zone = ""
                acc = 0
                for ch in ot[r - 1]:
                    if acc >= osb - 1:
                        break
                    zone += ch
                    acc += 2 if ord(ch) > 0x2E80 else 1
                z = zone.strip().lstrip("▶").strip()
                if z and not any(z.startswith(k) for k in
                                 ("导航选项", "─", "启动 (Startup)", "[M] 条目管理",
                                  "[A] 外观", "[K] 键位", "[B] 行为", "[W] 窗格配色",
                                  "[Ctrl+S] 保存配置", "┌", "│", "└", "┐", "┘")):
                    ozs.append((r, z[:18]))
            ck("O %d 列 × %s：侧栏那一段（col 1..%d）只有侧栏自己的字符串，没有正文挤进来"
               % (oc, oname, osb - 1), not ozs, "越界行=%r" % ozs[:3])

        # O3 行窗口标记 (a-b/N)：它贴着右端写，窄终端上必须收到侧栏之前
        owheel = "\x1b[<65;%d;8M\x1b[<65;%d;8m" % (oc - 6, oc - 6)
        ot = vt_text(14, oc, capture_page_keys(14, oc,
                                              [b"\x02s", owheel.encode(), owheel.encode()],
                                              ini=oin)) or []
        omark = [(r, dispw(l)) for r, l in enumerate(ot[:14], 1) if re.search(r"\(\d+-\d+/\d+\)", l)]
        obb = border_pos(14, oc, capture_page_keys(14, oc,
                                                   [b"\x02s", owheel.encode(), owheel.encode()],
                                                   ini=oin)) or []
        omap = {r: (last, pos) for r, last, pos in obb}
        obad2 = [(r, last) for r, (last, pos) in omap.items()
                 if re.search(r"\(\d+-\d+/\d+\)", ot[r - 1] if r <= len(ot) else "")
                 and last >= osb and osb not in pos.get("|", [])]
        ck("O %d 列 × 14 行：滚动后的行窗口标记 (a-b/N) 不越界、不盖掉 col %d 的分隔线"
           % (oc, osb), not [x for x in omark if x[1] > oc] and not obad2,
           "越界=%r 缺│=%r" % ([x for x in omark if x[1] > oc][:2], obad2[:2]))

    # ======================= P 组：窄终端 Shift+滚轮横向滚动（v2.1.4）=======================
    # 窄视口放不下「名称列 + 按钮」时按固定阶梯让位：命令行 → 列间隔 → 收窄名称列 →
    # 丢 [↑][↓] → 连 [改][删] 一起丢（退成纯键盘）。40 列正好落在最后一档，56 列画得下。
    # 注意：ini 里的命令行必须是能跑起来的程序，否则 pane 一开就退 ⇒ 沙箱里整个 app 跟着
    # 退出，一帧都抓不到（「长内容」改用启动目录那一列造，该目录不必存在：chdir 失败会退回
    # HOME，见 src/platform_posix.c）。但这里绝不能写死某个人的绝对路径 —— 那样判据只在某台
    # 机器上成立，换台机器就假红/假绿，故统一取临时目录下的长名字。
    pdir = os.path.join(tempfile.gettempdir(), "termux-hscroll-fixture-for-a-long-column")
    pini = ("[menu]" + chr(10) + "1 = sh, /bin/sh, " + pdir + chr(10) +
            "2 = two, /bin/sh" + chr(10))
    ph = "\x1b[<69;30;10M\x1b[<69;30;10m"      # Shift+滚轮向下（b=69：64=滚轮 + 1=下 + 4=Shift）
    # 这四条判据都建立在「真实屏幕文本」上 ⇒ 非有 libvterm 回放不可。缺 libvterm 的机器
    # （macOS runner）必须像 J/K/L 组那样明确 SKIP：拿空帧去比宽度会得到「最长=0」「滚后=''」，
    # 看着像窄终端横滚真的坏了，其实那台机器根本没渲染过一帧。
    pv0 = vt_text(24, 40, capture_page_keys(24, 40, [b"\x02s", b"m"], ini=pini))
    if pv0 is None:
        print("  [SKIP] P 组 —— 本机没有 libvterm-dev")
    else:
        p0 = pv0 or []
        p2 = vt_text(24, 40, capture_page_keys(24, 40, [b"\x02s", b"m", ph.encode(), ph.encode()],
                                                ini=pini)) or []
        j0, j2 = "\n".join(p0), "\n".join(p2)
        ck("P 40 列 × 条目管理页：只有序号 + 名称（这一档按设计不画按钮），且不折行不盖侧栏",
           bool(p0) and "▶[1]" in j0 and "sh" in j0 and "[改]" not in j0 and "[删]" not in j0
           and "..." in j0 and all(dispw(l) <= 40 for l in p0),
           "最长=%d 首行=%r" % (max([dispw(l) for l in p0] or [0]), (p0 or [""])[9:11]))
        # 行窗口标记不再写死成 (3-23/24)：页尾现在按条目数算（12+条目数），
        # 判据改成「横滚不许动到纵向行窗」——两边取出来逐字比。
        vmark = lambda ls: re.findall(r"\(\d+-\d+/\d+\)", "\n".join(ls))
        ck("P 40 列 × Shift+滚轮：画面跟着左右滚，序号仍钉在视口左端，行窗口标记不动",
           bool(p2) and p0 != p2 and "\u25b6[1]" in j2 and vmark(p0) == vmark(p2)
           and all(dispw(l) <= 40 for l in p2),
           "滚后=%r 标记=%r/%r" % (j2[:160], vmark(p0), vmark(p2)))
        q0 = vt_text(24, 56, capture_page_keys(24, 56, [b"\x02s", b"m"], ini=pini)) or []
        q2 = vt_text(24, 56, capture_page_keys(24, 56, [b"\x02s", b"m", ph.encode(), ph.encode()],
                                                ini=pini)) or []
        k0, k2 = "\n".join(q0), "\n".join(q2)
        ck("P 56 列 × 条目管理页：默认就同时看得见序号 / 名称 / [改][删]（按钮贴视口右端）",
           bool(q0) and "▶[1]" in k0 and "sh" in k0 and "[改][删]" in k0
           and all(dispw(l) <= 56 for l in q0),
           "最长=%d 表行=%r" % (max([dispw(l) for l in q0] or [0]), (q0 or [""])[9:11]))
        # 按钮是「钉在视口右端」的（滚到底也一直在），滚的是中间那截：名称尾部被推出去、
        # 后面的内容滚进来 —— 所以判据是「画面变了 + 行窗口没变 + 不折行」。
        ck("P 56 列 × Shift+滚轮：中段跟着滚（按钮钉右端不消失），行窗口不变、不折行",
           bool(q2) and q0 != q2 and "[改][删]" in k2 and "▶[1]" in k2
           and vmark(q0) == vmark(q2) and all(dispw(l) <= 56 for l in q2),
           "滚后=%r 标记=%r/%r" % (k2[:200], vmark(q0), vmark(q2)))
    # ======================= Q 组：设置页滚动条（v2.1.5）=======================
    # 用户报「设置页太窄/太矮时，右侧面板加滚动条：可以左右滚也可以上下滚；聚焦时要滚到聚焦
    # 位置」。落点（都是零额外预算的）：纵条 = 终端最右一列，只在这一页真溢出时出现；横条 =
    # 表头那 9 列标记位；终端矮到侧栏那一排入口（[M][A][K][B][W]）摆不下时，侧栏再自己开一个
    # 窗口 + 一根细条子。三条判据都走 vterm 屏态：条子是每帧【最后】才写的，抓原始字节会
    # 停在帧中间（假红），这一点已经踩过一次。
    qini9 = "[menu]" + chr(10) + "".join("%d = 项目%02d, /bin/sh" % (i, i) + chr(10)
                                         for i in range(1, 10))

    def qcell(lines, row, col):
        """第 row 行（1 基）第 col 显示列上的字符；宽字符算在它起始列上，右半格返回空。"""
        l = lines[row - 1] if 0 < row <= len(lines) else ""
        acc = 0
        for ch in l:
            cw = 2 if ord(ch) > 0x2E80 else 1
            if acc + cw > col:
                return ""
            acc += cw
            if acc >= col:
                return ch
        return ""

    def qbars(lines, row0, row1, col):
        return "".join(qcell(lines, r, col) for r in range(row0, row1 + 1))

    qnear = lambda col, row: ("%s[<35;%d;%dM" % (chr(27), col, row)).encode()

    def qcol(cs, row0, row1, col):
        """cs = vt_cells 的结果；取第 row0..row1 行、第 col 列的背景色串。"""
        out = []
        for r in range(row0, row1 + 1):
            if 0 < r <= len(cs) and 0 < col <= len(cs[r - 1]):
                out.append(cs[r - 1][col - 1])
        return out

    # v2.1.6（用户第 6 条）：设置页的条子与【终端原生滚动条】逐条对齐 —— 滑块是「整格换底色、
    # 不写字符」，指针不在附近时整条不画。所以：
    #   · 判据一律落在单元格背景色上（看屏幕文本会「看不见条子」，那是画法不是 bug）；
    #   · 每一帧都先把指针放到条子附近（离得远的反向判据由 Q1b/Q4b 把关）。
    q1d = capture_page_keys(12, 100, [b"\x02s", b"m", qnear(99, 7)], ini=qini9)
    q0 = vt_text(12, 100, q1d) or []
    q0c = vt_cells(12, 100, q1d)
    if q0c is None:
        print("  [SKIP] Q 组 —— 本机没有 libvterm-dev")
    else:
        tb = [qbgsum(x) for x in qcol(q0c, 3, 11, 100)]
        lit = [v for v in tb if v >= 0]
        ck("Q1 12 行 × 条目管理页：指针贴近最右列 ⇒ 该列出滑块（亮底）+轨道（暗底），轨上有 │",
           "│" in qbars(q0, 3, 11, 100) and len(lit) >= 6
           and max(lit) > min(lit) and all(dispw(l) <= 100 for l in q0),
           "最右列底色=%r 文本=%r" % (tb, qbars(q0, 2, 12, 100)))
        qfd = capture_page_keys(12, 100, [b"\x02s", b"m", qnear(50, 7)], ini=qini9)
        qfc = vt_cells(12, 100, qfd) or []
        qft = vt_text(12, 100, qfd) or []
        ck("Q1b 指针离得远（正文里）⇒ 整条不画：最右一列全是默认背景、没有 │（终端同款）",
           qfc and all(x == "-" for x in qcol(qfc, 3, 12, 100)) and "│" not in "".join(l[99:100] for l in qft),
           "最右列底色=%r" % ([x for x in qcol(qfc, 2, 12, 100)],))
        # Q2：把滑块从顶端一路拖到底 ⇒ 页面真滚到底，滑块的亮底块也跟着挪到轨道下端。
        q2d = (b"\x1b[<0;100;3M" + b"\x1b[<32;100;7M" + b"\x1b[<32;100;11M" + b"\x1b[<0;100;11m")
        qd = capture_page_keys(12, 100, [b"\x02s", b"m", q2d], ini=qini9)
        qdt = vt_text(12, 100, qd) or []
        qdc = vt_cells(12, 100, qd) or []
        up = [qbgsum(x) for x in qcol(qdc, 3, 5, 100)]
        dn = [qbgsum(x) for x in qcol(qdc, 9, 11, 100)]
        ck("Q2 拖滑块一路到底：页面真滚到底（动作条 [设为默认] 露出来），亮底块也挪到轨道下端",
           q0 != qdt and "\u8bbe\u4e3a\u9ed8\u8ba4" in "\n".join(qdt)
           and max(dn) > max(up + [-1]) and max(up) >= 0,
           "上段=%r 下段=%r" % (up, dn))
        qt = vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"m",
                      b"\x1b[<0;100;11M\x1b[<35;100;11M"], ini=qini9)) or []
        ck("Q3 点轨道下端 = 往下翻一页（不用按住拖也能滚）",
           q0 != qt and "[5]" in "\n".join(qt), "点完=%r" % "\n".join(qt)[-90:])
        qw = vt_text(24, 80, capture_page_keys(24, 80, [b"\x02s", b"m"], ini=qini9)) or []
        ck("Q4 24 行 × 80 列（基准档）：这一页装得下 ⇒ 最右一列不许常驻一条，80 列排版不变",
           not any(c in ("\u2588", "\u2502") for c in qbars(qw, 3, 24, 80))
           and "[1]  项目01" in "\n".join(qw),
           "第80列=%r" % qbars(qw, 2, 24, 80))
        qwc = vt_cells(24, 80, capture_page_keys(24, 80, [b"\x02s", b"m", qnear(79, 12)], ini=qini9))
        ck("Q4b 24 行 × 80 列：指针贴着最右列也没条子（页没溢出 ⇒ 预留量与排版一律不变）",
           qwc is not None and all(x == "-" for x in qcol(qwc, 3, 24, 80)),
           "第80列=%r" % ([x for x in qcol(qwc or [], 3, 24, 80)],))
        # 横条：落在面板最后一行（正文每帧清到那儿、一行都不写）⇒ 不占任何预算。
        ph40 = "[menu]" + chr(10) + "1 = sh, /bin/sh, " + pdir + chr(10) + "2 = two, /bin/sh" + chr(10)
        h0d = capture_page_keys(24, 40, [b"\x02s", b"m", qnear(30, 24)], ini=ph40)
        hb0 = vt_text(24, 40, h0d) or []
        hbc = vt_cells(24, 40, h0d) or []
        brow = hb0[23] if len(hb0) > 23 else ""
        # 条子那一段 = 底行上【连续的「非默认背景」格】（滑块是整格换底色、不写字符，
        # 所以不能按 ─ 找 —— ─ 只覆盖轨道那几格）。
        def qrun(cs, row, c0, c1):
            if not (0 < row <= len(cs)):
                return []
            out = [(c, qbgsum(cs[row - 1][c - 1])) for c in range(c0, c1 + 1)]
            out = [t for t in out if t[1] >= 0]
            return out
        r0 = qrun(hbc, 24, 22, 40)
        li = [v for _, v in r0]
        ck("Q5 40 列 × 条目管理页：面板最后一行铺着横条（轨道 ─ + 滑块亮底），滑块贴在左端",
           "─" in brow and len(li) >= 6 and max(li) > min(li) and li[0] == max(li)
           and all(dispw(l) <= 40 for l in hb0),
           "底行=%r 亮段=%r" % (brow, li))
        # 按住滑块拖到右端 ⇒ 藏起来的列尾滚进视口（长目录名的尾巴出现），全程不折行。
        st0 = r0[0][0] if r0 else 22          # 滑块贴在条子左端 ⇒ 从那儿按住它
        hd = (("\x1b[<0;%d;24M" % st0).encode() + qnear(st0 + 4, 24) + qnear(40, 24)
              + b"\x1b[<0;40;24m")
        h1d = capture_page_keys(24, 40, [b"\x02s", b"m", qnear(30, 24), hd], ini=ph40)
        hb1 = vt_text(24, 40, h1d) or []
        hbc1 = vt_cells(24, 40, h1d) or []
        r1 = qrun(hbc1, 24, 22, 40)
        li1 = [v for _, v in r1]
        def qfirst_max(run):
            if not run: return 999
            m = max(v for _, v in run)
            return min(c for c, v in run if v == m)
        f0, f1 = qfirst_max(r0), qfirst_max(r1)
        # 拖到右端：亮块（=滑块）从条子左端挪到右端，页面跟着横滚（画面变了）、照样不折行。
        ck("Q5 横条拖着走：把滑块拖到右端 ⇒ 亮块从条子左端挪到右端，横滚真的动了（不用 Shift+滚轮）",
           hb0 != hb1 and len(li1) >= 6 and f1 - f0 >= 8 and f1 >= 30
           and all(dispw(l) <= 40 for l in hb1),
           "拖前滑块起于 %d，拖后起于 %d（亮段 %r → %r）" % (f0, f1, li, li1))
        # 极矮档：侧栏入口摆不下 ⇒ 分隔线那一列变细滚动条（同样是 hover 才出现）
        s6d = capture_page_keys(9, 100, [b"\x02s", qnear(20, 5)], ini=qini9)
        qs = vt_text(9, 100, s6d) or []
        qsc = vt_cells(9, 100, s6d) or []
        dcol = 0
        for l in qs:
            if "\u2502" in l:
                dcol = dispw(l[:l.index("\u2502")]) + 1
                break
        sb6 = [qbgsum(x) for x in qcol(qsc, 3, 8, dcol)] if dcol else []
        sb6lit = [v for v in sb6 if v >= 0]
        qsw = vt_text(9, 100, capture_page_keys(9, 100, [b"\x02s", qnear(20, 5), wheel(6, 5, 3)], ini=qini9)) or []
        ck("Q6 9 行极矮档：侧栏入口摆不下 ⇒ 分隔线那一列变细滚动条，滚轮能把 [W] 窗格配色翻出来",
           dcol > 0 and len(sb6lit) >= 4 and max(sb6lit) > min(sb6lit)
           and "[W] 窗格配色" not in "\n".join(qs) and "[W] 窗格配色" in "\n".join(qsw),
           "分隔线列=%d 底色=%r 基线=%r 滚后=%r" % (dcol, sb6, "\n".join(qs)[-120:], "\n".join(qsw)[-120:]))
        qf = vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", b"m", b"\x1b[B" * 8], ini=qini9)) or []
        ck("Q7 12 行 × 条目管理页：连按 ↓ 时行窗跟着聚焦行走（滚到聚焦位置，不是把 ▶ 顶出屏幕）",
           "[9]" not in "\n".join(q0) and "\u25b6[9]" in "\n".join(qf)
           and "[1]  项目01" in "\n".join(qf), "按8次↓=%r" % "\n".join(qf)[-90:])

    # ================== R 组：切页 / 浮层的过渡动画（v2.1.7）==================
    # 「没那么生硬」和「别太长」都是主观话，落到判据上只有四件事：
    #   1) 静止那一屏不许被动画碰过一格（文本 + 逐格前景背景色都要与 off 相同）；
    #   2) 变化的那几帧确实在渐入（同一行被写了多种「不是原色」的颜色，一档一档往上）；
    #   3) 档数很少（110ms / 15ms 一片 ≈ 7 档），点一下不用等；ini 改档位就跟着变；
    #   4) 浮层的淡入是「从上往下」：越靠下的行，最后一档暗色出现得越晚。
    # 量法：面板每帧都在 always 段发一次 \x1b[?7l ⇒ 按它切块就得到「帧」；每一行都以
    # ESC<行>;<列>H 起笔 ⇒ 行内出现过的 38/48 真彩色就是这帧这行的颜色。真色集合由
    # anim=off 那一次跑提供（两个二进制只差这个开关），所以「这行这帧的颜色不在真色集合里」
    # = 这一格此刻正被动画压暗。全程只读原始字节，不依赖终端模拟。
    rA_cup = re.compile(rb"\x1b\[(\d+);(\d+)H")
    rA_tri = re.compile(rb"0?(?:[34]8);2;(\d{3});(\d{3});(\d{3})")

    def rA_rowsums(block, row):
        """一帧（block）里第 row 行写过的所有颜色亮度（r+g+b 之和）集合。"""
        got = set()
        for m in rA_cup.finditer(block):
            if int(m.group(1)) != row:
                continue
            nxt = rA_cup.search(block, m.end())
            got |= {int(a) + int(g) + int(b) for a, g, b in
                    rA_tri.findall(block[m.end(): nxt.start() if nxt else len(block)])}
        return got

    def rA_dim(data_off, data_on, lo=3, hi=20, win=12):
        """{行: [帧号,…]}：该行在「最后 win 帧」里被压暗（写过非真色）的那些帧号。"""
        boff = data_off.split(b"\x1b[?7l")
        bon = data_on.split(b"\x1b[?7l")
        if not bon:
            return {}, 0
        true = {}
        for blk in boff:
            for r in range(lo, hi + 1):
                v = rA_rowsums(blk, r)
                if v:
                    true.setdefault(r, set()).update(v)
        base = len(bon) - win
        per = {}
        for r in sorted(true):
            hits = [i for i in range(max(0, base), len(bon))
                    if rA_rowsums(bon[i], r) - true[r]]
            if hits:
                per[r] = hits
        return per, len(bon)

    def rA_frames25(data):
        """按「帧尾光标段」切帧：?25h / ?25l 每帧必发且只发一次，静止帧一字节都不发。"""
        return [f for f in re.split(rb"(?=\x1b\[\?25[hl])", data) if f]

    def rA_pad(lines, cols):
        return [l.ljust(cols)[:cols] for l in lines]

    def rA_shift_d(a, rest, cols, lo=1):
        """a 这一屏相对 rest 是横移几列（+右进 / −左进 / 0 就是同一屏 / None 不匹配公式）。"""
        a = rA_pad(a, cols); rest = rA_pad(rest, cols)
        if len(a) != len(rest) or not a:
            return None
        n = len(a) - lo
        if n <= 0:
            return 0
        if a == rest:
            return 0
        best = (0, None)
        for d in range(1, min(cols, 96) + 1):
            rig = sum(1 for i in range(lo, lo + n) if a[i] == " " * d + rest[i][:cols - d])
            if rig > best[0]:
                best = (rig, d)
            lef = sum(1 for i in range(lo, lo + n) if a[i] == rest[i][d:].ljust(cols))
            if lef > best[0]:
                best = (lef, -d)
        return best[1] if best[0] >= 4 else None

    rA_screens = {}
    def rA_screen_at(rows, cols, tag, frames, k):
        key = (tag, k)
        if key not in rA_screens:
            rA_screens[key] = vt_text(rows, cols, b"".join(frames[:k])) or []
        return rA_screens[key]

    def rA_slide_scan(tag, data, rows, cols, want):
        """在最后 12 帧里找一个「整页横移 want 方向」的动画帧，返回 (帧号, 列数) 或 None。"""
        fr = rA_frames25(data)
        rest = rA_screen_at(rows, cols, tag + "rest", fr, len(fr))
        got = None
        for k in range(max(1, len(fr) - 12), len(fr)):
            d = rA_shift_d(rA_screen_at(rows, cols, tag, fr, k), rest, cols)
            if d is not None and (d > 0) == (want > 0) and abs(d) >= 2:
                got = (k, d)
        return got, len(fr), rest

    def rini(anim):
        return "[general]" + chr(10) + "anim = " + anim + chr(10)

    SWITCH = [b"\x02s", b"\x1bOQ"]          # 进设置页 → F2 到「外观 / 主题」
    r_off = capture_page_keys(24, 100, SWITCH, ini=rini("off"))
    r_nrm = capture_page_keys(24, 100, SWITCH, ini=rini("normal"))
    r_short = capture_page_keys(24, 100, SWITCH, ini=rini("short"))
    r_slow = capture_page_keys(24, 100, SWITCH, ini=rini("200"))
    r_junk = capture_page_keys(24, 100, SWITCH, ini=rini("probably-a-typo"))
    d_off, n_boff = rA_dim(r_nrm, r_off)   # 用 normal 的颜色集合当参照：off 里不该有任何「多余」颜色
    d_nrm, n_nrm = rA_dim(r_off, r_nrm)    # 反过来：normal 相对 off 多出来的就是淡入的档
    d_short, _ = rA_dim(r_off, r_short)
    d_slow, _ = rA_dim(r_off, r_slow)
    d_junk, _ = rA_dim(r_off, r_junk)
    lv = lambda d: max((len(v) for v in d.values()), default=0)
    ck("R1 anim=off：整个过程没有一帧写过 normal 里不存在的颜色，也只画了 %d 帧（不多花一帧）" % n_boff,
       not d_off and n_boff <= 10,
       "off 多出的压暗行=%r 帧数=%d" % ({r: v for r, v in d_off.items()}, n_boff))
    ck("R2 anim=normal（110ms）：切页/切标签确有行在一档一档地亮起来，最多 %d 档（4~12 档，够看出渐变又不到 1/8 秒）" % lv(d_nrm),
       4 <= lv(d_nrm) <= 12 and len(d_nrm) >= 8,
       "档数=%d 参与行=%d" % (lv(d_nrm), len(d_nrm)))
    # 「整页一起淡」的准确说法：所有行都是在【同一帧】回到原色的（没有谁排队在后面），
    # 而且各行压暗的帧数相近（不是有的行只暗一帧、有的暗八帧）。
    uni_nrm = {max(v) for v in d_nrm.values()}
    lv_nrm = sorted(len(v) for v in d_nrm.values())
    ck("R2b 切页是「整页一起淡」：各行同一帧回到原色（收尾批次 ≤2），且各行进度相近",
       bool(d_nrm) and len(uni_nrm) <= 2 and lv_nrm and min(lv_nrm) * 10 >= 7 * max(lv_nrm),
       "收尾帧集合=%r 档数=%r" % (sorted(uni_nrm), lv_nrm))
    # 「三档严格递增」里 110 与 200 这一环，取决于这台机器采得出多少帧：CI 的 macOS
    # runner 实测 short=7 / normal=10 / 200=10 —— 两档都撞上同一次访问里能采到的帧数上限
    # （浮层只在那 0.4s 的 drain 里出帧），于是中间那一环根本没法比，而程序本身没毛病
    # （本机同一条判据是 4 < 8 < 13 这样严格递增的）。所以按分辨率分两种问法：
    # 采得开 ⇒ 必须严格递增（一个字都不放宽）；采不开 ⇒ 只硬判两端之差（差 140ms，
    # 任何采样都看得见），并且把「这一环没测出来」写在判据名里，不藏着。
    r3_res = lv(d_nrm) < lv(d_slow)
    if r3_res:
        ck("R3 时长真按 ini 走（本机采得开）：三档严格递增 short(60) < normal(110) < 200ms，"
           "且 200ms 比 short 多 3 档以上",
           lv(d_short) < lv(d_nrm) < lv(d_slow) and lv(d_slow) - lv(d_short) >= 3,
           "short=%d normal=%d 200=%d" % (lv(d_short), lv(d_nrm), lv(d_slow)))
    else:
        ck("R3 时长真按 ini 走（本机采样采不出 110 与 200 的档数差 ⇒ 只判两端）："
           "short(60) < normal(110) ≤ 200ms，且 200ms 比 short 多 3 档以上",
           lv(d_short) < lv(d_nrm) <= lv(d_slow) and lv(d_slow) - lv(d_short) >= 3,
           "short=%d normal=%d 200=%d" % (lv(d_short), lv(d_nrm), lv(d_slow)))
    ck("R3b 认不出的单词不静默关掉动画（typo → 按默认 110ms 走）",
       4 <= lv(d_junk) <= 12 and abs(lv(d_junk) - lv(d_nrm)) <= 3,
       "typo=%d 档，normal=%d 档" % (lv(d_junk), lv(d_nrm)))
    ck("R3c 动画期间多花的帧数有限（110ms ≈ 8 帧内，不会让主循环空转超过 16 帧）",
       0 <= n_nrm - n_boff <= 16, "off=%d 帧，normal=%d 帧" % (n_boff, n_nrm))
    t_off, t_nrm = vt_text(24, 100, r_off), vt_text(24, 100, r_nrm)
    if t_off is None:
        print("  [SKIP] R4/R5 —— 本机没有 libvterm-dev")
    else:
        ck("R4 动画走完后的静止屏：文本 + 逐格前景背景色都与 anim=off 逐行相同（不留一丝痕迹）",
           t_off == t_nrm and vt_sig(24, 100, r_off) == vt_sig(24, 100, r_nrm),
           "首个差异文本行=%r 签名首个差异=%r" % (
               next((i for i in range(min(len(t_off), len(t_nrm))) if t_off[i] != t_nrm[i]), -1),
               next((i for i, a in enumerate(vt_sig(24, 100, r_off) or [])
                     if a != (vt_sig(24, 100, r_nrm) or [])[i]), -1)))
        # 窄/矮两档：动画只改颜色数字，长度一字不变 ⇒ 静止屏也必须与 off 一致
        for rows, cols in ((12, 100), (24, 40)):
            o = capture_page_keys(rows, cols, SWITCH, ini=rini("off"))
            n = capture_page_keys(rows, cols, SWITCH, ini=rini("normal"))
            to, tn = vt_text(rows, cols, o), vt_text(rows, cols, n)
            ck("R4b %d×%d 档：开动画的静止屏与 off 逐行同文同色，且没有任何一行超出列数" % (rows, cols),
               to == tn and vt_sig(rows, cols, o) == vt_sig(rows, cols, n)
               and all(dispw(l) <= cols for l in tn),
               "差异行=%r" % (next((i for i in range(min(len(to), len(tn))) if to[i] != tn[i]), -1),))
        # 浮层（窗格配色页按 Enter 弹「方案列表」）：自上而下逐行点亮 ⇒ 越靠下的行，
        # 最后一档暗色出现得越晚；关动画时同样的键完全不会压暗。
        POP = [b"\x02s", b"W", b"\r"]
        p_off = capture_page_keys(24, 100, POP, ini=rini("off"))
        p_nrm = capture_page_keys(24, 100, POP, ini=rini("normal"))
        pd_off, pn_off = rA_dim(p_nrm, p_off)   # off 相对 normal 不该有任何「多余」颜色
        pd, pn = rA_dim(p_off, p_nrm)
        lastrow = {r: v[-1] for r, v in pd.items()}
        rows_sorted = sorted(lastrow)
        ck("R5 浮层出现 = 自上而下逐行点亮（行号越大、最后一档暗色越晚：收尾分好几批，且不倒序）",
           len(rows_sorted) >= 6 and len({max(v) for v in pd.values()}) >= 2
           and (max(lastrow.values()) - min(lastrow.values()) if lastrow else 0) >= 2
           and all(lastrow[rows_sorted[i]] <= lastrow[rows_sorted[i + 1]] + 1
                   for i in range(len(rows_sorted) - 1)) and not pd_off and bool(pd),
           "各行最后压暗帧=%r（帧数 off=%d normal=%d）" % (lastrow, pn_off, pn))
        # 分屏 + 动画：状态复位必须挂在「整帧没画面板」这件事上，不能挂在某个窗格的
        # else 分支里 —— 两个窗格时另一支会把状态冲掉，动画就每秒重启、屏幕永久留暗色。
        d_off = capture_page_keys(24, 120, [b"\x02s", b"\x02-"], ini=rini("off"))
        d_nrm = capture_page_keys(24, 120, [b"\x02s", b"\x02-"], ini=rini("normal"))
        ck("R6b 设置页 + 左右分屏：开动画的静止屏与 off 逐格同文同色（分屏不会让动画卡住）",
           vt_text(24, 120, d_off) == vt_text(24, 120, d_nrm)
           and vt_sig(24, 120, d_off) == vt_sig(24, 120, d_nrm),
           "首个差异=%r" % (next((i for i, a in enumerate(vt_text(24, 120, d_off) or [])
                                  if a != (vt_text(24, 120, d_nrm) or [])[i]), -1),))

        # 连点 40 下切页、而且是一次写进 tty（上一段动画根本没走完就来下一段）：
        # 动画不许吞键、不许把排版碰坏，静止屏仍要与 off 逐格相同。
        burst = b"".join(k for k in (b"\x1bOQ", b"\x1bOR", b"\x1b[15~", b"\x1bOS") for _ in range(10))
        s_off = capture_page_keys(24, 100, [b"\x02s", burst], ini=rini("off"))
        s_nrm = capture_page_keys(24, 100, [b"\x02s", burst], ini=rini("normal"))
        so, sn = vt_text(24, 100, s_off), vt_text(24, 100, s_nrm)
        ck("R7 一口气 40 次切页（动画没走完就再切）：静止屏与 off 逐格同文同色、24 行齐、无超宽行",
           so == sn and len(sn) == 24 and all(dispw(l) <= 100 for l in sn)
           and vt_sig(24, 100, s_off) == vt_sig(24, 100, s_nrm),
           "行数=%d 首个差异=%r" % (len(sn), next((i for i in range(min(len(so), len(sn)))
                                                    if so[i] != sn[i]), -1)))

        ck("R5b 浮层静止屏同样与 off 一致（边框、方案名、底色都不留动画痕迹）",
           vt_text(24, 100, p_off) == vt_text(24, 100, p_nrm)
           and vt_sig(24, 100, p_off) == vt_sig(24, 100, p_nrm),
           "签名差异行=%r" % (next((i for i, a in enumerate(vt_sig(24, 100, p_off) or [])
                                    if a != (vt_sig(24, 100, p_nrm) or [])[i]), -1),))


    # ============ R 组续（v2.1.8）：切标签左右滑入 / 气泡与 toast 淡入 / 向底色混合 ============
    # 这台机器没有 libvterm（CI 的 macOS runner）时，屏幕级判据一律打 SKIP：拿空帧去比
    # 位移/淡入既会假红（R8~R13），拿 None == None 去比静止屏又会假绿（R9/R15）—— 两种都
    # 没意义。字节级的两条（R10 的 CUP 列号、R16 的 NUL）不看屏幕 ⇒ 照常硬判。
    def rA_ck(name, cond, extra=""):
        if not _VTEXT_OK:
            print("  [SKIP] %s —— 本机没有 libvterm-dev" % name)
            return
        ck(name, cond, extra)
    # 滑动的量法不重新实现 C 里的位移，而是拿「整屏文本」套一条闭式：
    #   从右进（d>0）：第 r 行 = d 个空格 + 静止那一屏第 r 行的前 cols-d 列
    #   从左进（d<0）：第 r 行 = 静止那一屏第 r 行去掉前 |d| 列，右边补 |d| 个空格
    # 之所以成立：铺底/清行那一类 chunk（只有空格）故意不位移 ⇒ 页面没推进来的那一截必然是
    # 干净的底色空格。于是「存在这样一帧」=「整页确实横移过」，方向 = 两套公式谁唯一匹配，
    # 全是终端语义层面的事实，不看 C 的实现细节。
    rA_cup_r = re.compile(rb"\x1b\[(\d+);(\d+)H")
    rA_tri_any = re.compile(rb"[34]8;2;(\d{1,3});(\d{1,3});(\d{1,3})")

    def rA_frames25(data):
        """按「帧尾光标段」切帧：?25h / ?25l 每帧必发且只发一次，静止帧一字节都不发。"""
        return [f for f in re.split(rb"(?=\x1b\[\?25[hl])", data) if f]

    def rA_rowscan(block):
        """{行: set(三分量)}：这一帧每条行定位之后写过的所有真彩色（1~3 位都认）。"""
        got = {}
        for m in rA_cup_r.finditer(block):
            nxt = rA_cup_r.search(block, m.end())
            seg = block[m.end(): nxt.start() if nxt else len(block)]
            st = got.setdefault(int(m.group(1)), set())
            st |= {(int(a), int(b), int(c)) for a, b, c in rA_tri_any.findall(seg)}
        return got

    def rA_pad(lines, cols):
        return [l.ljust(cols)[:cols] for l in lines]

    def rA_shift_d(a, rest, cols, lo=1):
        """a 相对 rest 横移了几列（+右进 / −左进 / 0 同一屏 / None 不匹配公式）。
        只接受「唯一最大值」：整屏空白那些行对任何 d 都成立，靠正文行把真的那个 d 顶出来。"""
        a = rA_pad(a, cols); rest = rA_pad(rest, cols)
        if len(a) != len(rest) or not a or a == rest:
            return None
        idx = range(lo, len(a))
        score = []
        for d in range(1, min(cols, 96) + 1):
            score.append((sum(1 for i in idx if a[i] == " " * d + rest[i][:cols - d]), d))
            score.append((sum(1 for i in idx if a[i] == rest[i][d:].ljust(cols)), -d))
        score.sort(reverse=True)
        if not score or score[0][0] < 4:
            return None
        if len(score) > 1 and score[1][0] == score[0][0]:
            return None
        return score[0][1]

    rA_screens = {}

    def rA_screen_at(rows, cols, tag, frames, k):
        key = (tag, k)
        if key not in rA_screens:
            rA_screens[key] = vt_text(rows, cols, b"".join(frames[:k])) or []
        return rA_screens[key]

    def rA_slide_scan(tag, data, rows, cols, want):
        """在最后 14 帧里找「整页向 want 方向横移」的动画帧 ⇒ (首帧, 末帧, 帧数, 静止屏, 各帧结果)。
        首帧位移最大，判「裁掉了多少」比末帧好看得多。"""
        fr = rA_frames25(data)
        rest = rA_screen_at(rows, cols, tag + "rest", fr, len(fr))
        first, last, ds = None, None, []
        for k in range(max(1, len(fr) - 14), len(fr)):
            d = rA_shift_d(rA_screen_at(rows, cols, tag, fr, k), rest, cols)
            ds.append(d)
            if d is not None and (d > 0) == (want > 0) and abs(d) >= 2:
                if first is None:
                    first = (k, d)
                last = (k, d)
        return first, last, len(fr), rest, ds

    ROWS_C, COLS_C = 24, 100
    SHELL_TXT = b"seq 1 14\r"              # 让 shell 那一屏有正文，位移公式才有唯一解
    t_next = capture_page_keys(ROWS_C, COLS_C, [b"\x02s"], ini=rini("normal"))
    t_next_off = capture_page_keys(ROWS_C, COLS_C, [b"\x02s"], ini=rini("off"))
    t_prev = capture_page_keys(ROWS_C, COLS_C, [SHELL_TXT, b"\x02s", b"\x02p"], ini=rini("normal"))
    g_next, g_next_last, n_next, rest_next, ds_n = rA_slide_scan("gn", t_next, ROWS_C, COLS_C, +1)
    g_prev, g_prev_last, n_prev, rest_prev, ds_p = rA_slide_scan("gp", t_prev, ROWS_C, COLS_C, -1)
    g_no = rA_slide_scan("gno", t_next_off, ROWS_C, COLS_C, +1)[0]
    rA_ck("R8 切到下一个标签 = 整页横向滑入（首帧 %s 起整页右移 %+d 列，一路滑到 %+d 列），关动画时一帧都没有"
       % (g_next[0] if g_next else None, g_next[1] if g_next else 0, g_next_last[1] if g_next_last else 0),
       g_next is not None and g_no is None, "off 也匹配到位移=%r normal 各帧=%r" % (g_no, ds_n))
    rA_ck("R8b 切回上一个标签方向是反的（首帧 %s 整页左移 %d 列）—— 不是所有切换都从右边推"
       % (g_prev[0] if g_prev else None, g_prev[1] if g_prev else 0),
       g_prev is not None, "各帧位移=%r 静止屏非空行=%d"
       % (ds_p, sum(1 for l in rA_pad(rest_prev, COLS_C) if l.strip())))

    rA_ck("R9 滑动收尾后不留残影：静止屏与 anim=off 同文同色（含标签栏那行）",
       vt_text(ROWS_C, COLS_C, t_next) == vt_text(ROWS_C, COLS_C, t_next_off)
       and vt_sig(ROWS_C, COLS_C, t_next) == vt_sig(ROWS_C, COLS_C, t_next_off),
       "首个差异=%r" % (next((i for i, a in enumerate(vt_text(ROWS_C, COLS_C, t_next) or [])
                              if a != (vt_text(ROWS_C, COLS_C, t_next_off) or [])[i]), -1),))
    fr_next = rA_frames25(t_next)
    wide = [(k, c) for k in range(len(fr_next))
            for _, c in re.findall(rb"\x1b\[(\d+);(\d+)H", fr_next[k]) if int(c) > COLS_C]
    ck("R10 动画期间每条 CUP 的列号都在屏内（%d 帧里没有一个越界列 ⇒ 不会折行溢出到下一行）"
       % len(fr_next), not wide, "越界=%r" % wide[:4])

    # 标签栏那一行不参与位移。量法：把每条行的可见字节（抹掉所有 CSI 之后）比一比 ——
    # 正文被裁短（右移 d 列 ⇒ 尾巴掉 d 列），标签栏一格不少。
    rA_csi = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

    def rA_vis_bytes(block, row):
        """某一行写过的「可见字节」数：按 CUP 切开，把属于该行的片段拼起来再抹掉所有 CSI。"""
        pieces, cur, pos = [], None, 0
        while pos < len(block):
            mm = rA_cup_r.search(block, pos)
            if not mm:
                if cur == row:
                    pieces.append(block[pos:])
                break
            if cur == row:
                pieces.append(block[pos:mm.start()])
            cur = int(mm.group(1))
            pos = mm.end()
        return len(rA_csi.sub(b"", rA_cup_r.sub(b"", b"".join(pieces))))
    k_slide = (g_next or (max(1, len(fr_next) - 4), 0))[0]
    k_rest = max((i for i, f in enumerate(fr_next) if b"\x1b[1;1H" in f), default=len(fr_next) - 1)
    v1a, v1r = rA_vis_bytes(fr_next[k_slide], 1), rA_vis_bytes(fr_next[k_rest], 1)
    v2a, v2r = rA_vis_bytes(fr_next[k_slide], 2), rA_vis_bytes(fr_next[k_rest], 2)
    rA_ck("R10b 标签栏只跟着亮、不跟着滑：位移最大那帧（帧 %s，%+d 列）标签栏可见字节 %d 与静止帧 %d "
       "一样多，而正文第 2 行从 %d 被裁到 %d" % (k_slide, (g_next or (0, 0))[1], v1a, v1r, v2r, v2a),
       v1a == v1r and v1r > 0 and 0 < v2a < v2r,
       "标签栏=%r/%r 第2行=%r/%r" % (v1a, v1r, v2a, v2r))

    # ---- 气泡 / toast 的淡入：从「它出现的那一帧」起算，不靠固定窗口（帧数随时长抖） ----
    BOX_TOP, BOX_BOT = "┌".encode(), "└".encode()
    BG0 = (13, 17, 23)

    def rA_float_fade(data_off, data_on, mark, box=False, lo=2, hi=26):
        """(气泡/toast 出现的帧号, {行: 淡入帧数}, 允许的帧集合, 淡入期间出现过的颜色)。"""
        bon, boff = rA_frames25(data_on), rA_frames25(data_off)
        k0 = next((i for i, f in enumerate(bon) if mark in f), None)
        if k0 is None:
            return None, {}, set(), set()
        rows0 = set()
        if box:
            seg = bon[k0]
            for sym, nm in ((BOX_TOP, "top"), (BOX_BOT, "bot")):
                mm = re.search(rb"\x1b\[(\d+);\d+H(?:(?!\x1b\[\d+;\d+H).)*?" + re.escape(sym), seg, re.S)
                if mm:
                    rows0.add(int(mm.group(1)))
            if len(rows0) == 2:
                rows0 = set(range(min(rows0), max(rows0) + 1))
        if not rows0:
            rows0 = {int(a) for a, c in rA_cup_r.findall(bon[k0])}
        true = {}
        for blk in boff:
            for r, ts in rA_rowscan(blk).items():
                true.setdefault(r, set()).update(ts)
        dim, seen = {}, set()
        for i in range(k0, len(bon)):
            for r, ts in rA_rowscan(bon[i]).items():
                if not (lo <= r <= hi):
                    continue
                ex = ts - true.get(r, set())
                if ex:
                    dim.setdefault(r, set()).add(i)
                    seen |= ex
        return k0, {r: sorted(v) for r, v in dim.items()}, rows0, seen

    def rA_tail_chunks(data, mark):
        """mark 出现之后还发了几帧（= 有没有为它多画动画）。"""
        fr = rA_frames25(data)
        k0 = next((i for i, f in enumerate(fr) if mark in f), None)
        return None if k0 is None else len(fr) - k0

    tip_ini_off, tip_ini_nrm = nini5 + rini("off"), nini5 + rini("normal")
    tip_base_scr = vt_text(ROWS_C, COLS_C, capture_page_keys(ROWS_C, COLS_C, [b"\x02s", b"m"],
                                                             ini=tip_ini_nrm)) or []
    tip_mv = tip_move(tip_base_scr, "一个非...")
    T_ON = [b"\x02s", b"m", tip_mv]
    tip_off = capture_page_keys(ROWS_C, COLS_C, T_ON, ini=tip_ini_off)
    tip_nrm = capture_page_keys(ROWS_C, COLS_C, T_ON, ini=tip_ini_nrm)
    k_tip, d_tip, rows_tip, seen_tip = rA_float_fade(tip_off, tip_nrm, BOX_TOP, box=True)
    rA_ck("R12 悬停气泡出现那一帧起确有淡入：只淡气泡占的第 %s~%s 行，最多 %d 帧在渐变（首帧 %s）"
       % (min(rows_tip) if rows_tip else "—", max(rows_tip) if rows_tip else "—",
          max((len(v) for v in d_tip.values()), default=0), k_tip),
       bool(tip_mv) and k_tip is not None and len(rows_tip) >= 2 and bool(d_tip)
       and set(d_tip) <= rows_tip and max(len(v) for v in d_tip.values()) >= 3,
       "气泡行=%r 被淡行=%r 悬停序列=%r" % (sorted(rows_tip), d_tip, tip_mv))
    n_t_off, n_t_nrm = rA_tail_chunks(tip_off, BOX_TOP), rA_tail_chunks(tip_nrm, BOX_TOP)
    rA_ck("R12b anim=off 时气泡一帧都不多画（气泡出现后 off=%s 帧、normal=%s 帧）" % (n_t_off, n_t_nrm),
       n_t_off is not None and n_t_nrm is not None and n_t_off <= 2 and n_t_nrm - n_t_off >= 2,
       "off=%s normal=%s" % (n_t_off, n_t_nrm))
    # 淡入是「向页面底色混合」而不是「向黑压暗」：淡到最狠那一帧应该贴到 (13,17,23) 上。
    near = [t for t in seen_tip if all(abs(t[i] - BG0[i]) <= 2 for i in range(3))]
    rA_ck("R11 混合基准是页面底色：淡入期间写出过 %r 这类「贴着底色」的颜色（向黑压暗会掉到 "
       "0~8，永远到不了 13/17/23）" % (sorted(near)[:2],),
       bool(near), "淡入期间的非原色=%r" % sorted(seen_tip)[:8])

    # 气泡已在、指针从甲行挪到乙行（两行都被截断）：全文跟着换，但不许再来一段动画。
    # 两个长名字必须放在第 2、3 项：第 1 项的名字也会出现在标签栏那一行，tip_move 会先
    # 命中标签栏 —— 那就变成「气泡从无到有」，测不到「搬行不许重播」。
    nini_two = ("[menu]\n1 = sh, /bin/sh\n2 = %s, /bin/bash\n3 = %s, /bin/sh\n"
                "4 = four, /bin/sh\n5 = five, /bin/sh\n" % ("乙" * 24, "丙" * 24))
    tb2 = vt_text(ROWS_C, COLS_C, capture_page_keys(ROWS_C, COLS_C, [b"\x02s", b"m"],
                                                     ini=nini_two + rini("normal"))) or []
    mv_a = tip_move(tb2, "乙乙")
    mv_b = tip_move(tb2, "丙丙")
    sw_on = [b"\x02s", b"m", mv_a, mv_b]
    sw_off = capture_page_keys(ROWS_C, COLS_C, sw_on, ini=nini_two + rini("off"))
    sw_nrm = capture_page_keys(ROWS_C, COLS_C, sw_on, ini=nini_two + rini("normal"))
    fr_sw = rA_frames25(sw_nrm)
    box_fr = [i for i, f in enumerate(fr_sw) if BOX_TOP in f]
    # 气泡里的名字最多画 20 字（框宽所限），所以判「一长串同一个字」在不在屏上，不判整串
    k_b = next((i for i, f in enumerate(fr_sw) if ("丙" * 12).encode() in f), None)
    k_sw, d_sw_all, _, _ = rA_float_fade(sw_off, sw_nrm, BOX_TOP, box=True)
    d_sw = {}
    if box_fr and k_b is not None and k_b > box_fr[0]:
        # 只问「丙那串第一次出现之后」还发没发暗帧：发了就是搬行被当成了新一次出现
        d_sw = {r: [k for k in v if k >= k_b] for r, v in d_sw_all.items()}
        d_sw = {r: v for r, v in d_sw.items() if v}
    sn_sw = "\n".join(vt_text(ROWS_C, COLS_C, sw_nrm) or [])
    rA_ck("R12c 气泡从乙行挪到丙行：全文跟着换（屏上只剩丙那串），但不重播淡入",
       bool(mv_a) and bool(mv_b) and mv_a != mv_b and bool(box_fr) and k_b is not None
       and k_b > box_fr[0] and ("丙" * 12) in sn_sw and ("乙" * 12) not in sn_sw
       # 前半：第一次出现必须真淡（不淡的判据在 v2.1.7 上也会绿，等于没测）；后半：搬行不许再淡
       and bool(d_sw_all) and not d_sw,
       "气泡帧=%s..%s 首次淡入=%r 丙文首帧=%s 搬行后又淡的=%r" %
       (box_fr[0] if box_fr else None, box_fr[-1] if box_fr else None,
        {r: len(v) for r, v in d_sw_all.items()}, k_b, d_sw))

    T_TOAST = [b"\x02s", b"\x02-"]          # 设置页里按 Ctrl+B - ⇒「设置 / 帮助页面不能分屏」
    TO_MARK = "不能分屏".encode()
    to_off = capture_page_keys(ROWS_C, COLS_C, T_TOAST, ini=rini("off"))
    to_nrm = capture_page_keys(ROWS_C, COLS_C, T_TOAST, ini=rini("normal"))
    k_to, d_to, rows_to, seen_to = rA_float_fade(to_off, to_nrm, TO_MARK)
    to_scr = vt_text(ROWS_C, COLS_C, to_nrm) or []
    to_row = next((i + 1 for i, l in enumerate(to_scr) if "不能分屏" in l), -1)
    rA_ck("R13 底部 toast 出现也有淡入：只淡 toast 自己那一行（屏幕第 %d 行），其余 %d 行一格不动"
       % (to_row, ROWS_C - (1 if to_row in d_to else 0)),
       k_to is not None and to_row > 0 and set(d_to) == {to_row}
       and max((len(v) for v in d_to.values()), default=0) >= 3,
       "toast 行=%d 被淡行=%r 档=%r" % (to_row, d_to, max((len(v) for v in d_to.values()), default=0)))
    rA_ck("R13b toast 的静止屏与 anim=off 同文同色（淡完不留一丝痕迹，行宽也没被碰）",
       vt_text(ROWS_C, COLS_C, to_off) == vt_text(ROWS_C, COLS_C, to_nrm)
       and vt_sig(ROWS_C, COLS_C, to_off) == vt_sig(ROWS_C, COLS_C, to_nrm)
       and all(dispw(l) <= COLS_C for l in (vt_text(ROWS_C, COLS_C, to_nrm) or [])),
       "首个差异=%r" % (next((i for i, a in enumerate(vt_text(ROWS_C, COLS_C, to_off) or [])
                              if a != (vt_text(ROWS_C, COLS_C, to_nrm) or [])[i]), -1),))

    # 窄屏：横滑期间的裁剪必须仍守得住屏宽（右侧被裁掉是设计，越界折行不是）
    for rows, cols in ((24, 40), (12, 100)):
        nrow = capture_page_keys(rows, cols, [b"\x02s"], ini=rini("normal"))
        f2 = rA_frames25(nrow)
        ov = [c for fr in f2 for _, c in re.findall(rb"\x1b\[(\d+);(\d+)H", fr) if int(c) > cols]
        ln = vt_text(rows, cols, nrow) or []
        rA_ck("R14 %d×%d 档：切标签的动画帧里没有任何一列越过屏宽，静止屏 %d 行齐" % (rows, cols, len(ln)),
           not ov and len(ln) == rows and all(dispw(l) <= cols for l in ln),
           "越界列=%r 行数=%d" % (ov[:4], len(ln)))

    # 连点 30 下切标签（上一段动画没走完就再来一段）：不许吞键、不许留暗色
    burst = b"".join(b"\x02s\x02p" for _ in range(15))
    b_off = capture_page_keys(ROWS_C, COLS_C, [b"\x02n", burst], ini=rini("off"))
    b_nrm = capture_page_keys(ROWS_C, COLS_C, [b"\x02n", burst], ini=rini("normal"))
    bo, bn = vt_text(ROWS_C, COLS_C, b_off) or [], vt_text(ROWS_C, COLS_C, b_nrm) or []
    rA_ck("R15 一口气 30 次切标签（动画没走完就再切）：静止屏与 off 逐格同文同色、%d 行齐、无超宽行"
       % ROWS_C,
       bo == bn and len(bn) == ROWS_C and all(dispw(l) <= COLS_C for l in bn)
       and vt_sig(ROWS_C, COLS_C, b_off) == vt_sig(ROWS_C, COLS_C, b_nrm),
       "行数=%d 首个差异=%r" % (len(bn), next((i for i in range(min(len(bo), len(bn)))
                                               if bo[i] != bn[i]), -1)))

    # 位移重写是「先搬进另一个缓冲、再把两段长度回写」：长度与内容一旦不吻合，缓冲尾巴上
    # 从没写过的字节就会被当成光标段直接发给终端（表现为 NUL / 控制字节，画面看着还“正常”，
    # 因为终端把 NUL 吞了）。静止帧与动画帧一起数，四段捕获都不许有。
    def rA_nul(data):
        return [i for i, f in enumerate(rA_frames25(data)) if b"\x00" in f]
    nul = {}
    for nm, dat in (("切标签→", t_next), ("切标签←", t_prev), ("气泡", tip_nrm), ("toast", to_nrm)):
        bad = rA_nul(dat)
        if bad:
            nul[nm] = bad[:3]
    ck("R16 动画帧的字节流里没有 NUL（动画改写过长度 ⇒ 尾巴必须是自己写过的字节）",
       not nul, "%r" % (nul,))

    print()
    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("PANE PALETTE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
