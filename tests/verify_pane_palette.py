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
            for page, needle, label, need_lb in [
                (b"W", "亮白", "窗格配色页：最后一项「亮白」在右侧区域内", 1),
                (b"K", "[改]", "键位页：[改] 按钮在屏幕内", 0),
                (b"", "[改]", "启动/菜单项页：[改] 按钮在屏幕内", 0),
                (b"B", "[-]", "行为页：scrollback [-] 按钮在屏幕内", 0),
            ]:
                d = os.path.join(td, "i.bin")
                with open(d, "wb") as f:
                    keys = [b"\x02s"]
                    if page:
                        keys.append(page)
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
        if os.environ.get("TERMUX_NO_VTERM"): return None
        if "bin" not in _cache:
            td = tempfile.mkdtemp(prefix="termux_palette_txt_")
            src = os.path.join(td, "vtext.c"); exe = os.path.join(td, "vtext")
            with open(src, "w") as f: f.write(VTERM_TEXT_C)
            r = subprocess.run(["gcc", "-O1", src, "-o", exe, "-lvterm"], capture_output=True, text=True)
            _cache["bin"] = exe if r.returncode == 0 else ""
            if not _cache["bin"]:
                print("  [SKIP] J/K/L 组 —— vterm 文本 dump 编译失败：%s" % r.stderr.strip()[:160])
        if not _cache["bin"]: return None
        dump = os.path.join(tempfile.mkdtemp(prefix="termux_palette_d_"), "s.bin")
        with open(dump, "wb") as f: f.write(data)
        out = subprocess.run([_cache["bin"], str(rows), str(cols), dump], capture_output=True, text=True).stdout
        os.remove(dump)
        return out.splitlines()

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
        ck("K1 12 行 × 5 个菜单项：不再出现「添加(共N项)」死提示，[+] 行是普通的添加条目",
           "添加(共" not in jk1 and "[+] 添加新条目" in jk1, jk1[:400])
        # 一整串 ↓ 一次写入：31 个动作全部滚一遍，比逐键 0.4s 快两个数量级
        k2 = capture_page_keys(12, 100, [b"\x02s", b"\x1bOQ", b"\x1b[B" * 24])
        tk2 = vt_text(12, 100, k2) or []
        jk2 = "\n".join(tk2)
        ck("K2 12 行 × 外观页：连续 ↓ 后语义色区滚进可见区（background 行可见）",
           "background" in jk2 and "语义颜色" in jk2, jk2[:200])
        ck("K2 有滚动时提示行右端标出行窗口 (a-b/20)", "/20)" in jk2, "")
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
        ck("M1 12 行 × 外观页：滚轮向下把「语义颜色 (Palette)」+ background 行滚进可见区",
           "■ 语义颜色 (Palette)" not in m1a and "■ 语义颜色 (Palette)" in m1b and "background" in m1b,
           "")
        # 注意：12 行下即使没滚过，提示行也已经带 (3-11/20) 标记（选中行被夹进可见区），
        # 所以「标记是否出现」不是可用信号，可用的是「标记里的起始行变没变」。
        wm = re.compile(r"提示.*?\((\d+)-(\d+)/(\d+)\)", re.S)
        wa = wm.search(m1a); wb = wm.search(m1b)
        ck("M1 滚轮滚动后提示行的行窗口起始行后移（真滚动，不是选中项平移）",
           wa is not None and wb is not None and int(wb.group(1)) > int(wa.group(1))
           and wb.group(3) == wa.group(3) == "20",
           "base=%s scrolled=%s" % (wa and wa.groups(), wb and wb.groups()))
        # M2 滚轮向上回到原位（先滚开再滚回：否则「页首在屏内」在旧版也成立）
        # 滚到底（再往上滚一步都越界、被夹住）后连滚 9 下向上 → 必须逐字回到基线那一屏。
        m2 = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s"] + home + [b"\x1bOQ",
                                                                    wheel(60, 8, 9),
                                                                    wheel(60, 8, 9, down=False)])) or [])
        w2 = wm.search(m2)
        ck("M2 滚轮向上能回到页首（标题行重新出现、行窗口回到基线 (3-11/20)）",
           "■ 配色主题 (Theme)" not in m1b and "■ 配色主题 (Theme)" in m2
           and w2 is not None and w2.group(1) == wa.group(1) == "3",
           "base=%s after-roundtrip=%s" % (wa and wa.groups(), w2 and w2.groups()))
        # M3 启动项页 / 详情页
        ini3 = "[menu]\n1 = sh, /bin/sh\n2 = two, /bin/bash\n3 = three, /bin/sh\n"
        m3a = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s"], ini=ini3)) or [])
        m3b = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", wheel(60, 8, 3)], ini=ini3)) or [])
        ck("M3 12 行 × 启动项页：滚轮向下能把第 3 个菜单项滚进可见区",
           "three" not in m3a and "three" in m3b, "")
        m3c = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", wheel(60, 8, 3),
                                                                      b"\r", wheel(60, 8, 2)], ini=ini3)) or [])
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
            t = vt_text(rows, cols, capture_page_keys(rows, cols, [b"\x02s", b"\r"], ini=ini7)) or []
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
        if os.environ.get("TERMUX_NO_VTERM"):
            return None
        if "exe" not in _c:
            td = tempfile.mkdtemp(prefix="termux_meas_")
            cs = os.path.join(td, "meas.c"); ex = os.path.join(td, "meas")
            with open(cs, "w") as f:
                f.write(VTERM_MEAS_C)
            r = subprocess.run(["gcc", "-O1", cs, "-o", ex, "-lvterm"], capture_output=True, text=True)
            _c["exe"] = ex if r.returncode == 0 else ""
            if not _c["exe"]:
                print("  [SKIP] N5 —— 边框列号 dump 编译失败：%s" % r.stderr.strip()[:160])
        if not _c["exe"]:
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

    nini5 = ("[menu]\n1 = sh, /bin/sh\n2 = 一个非常长的菜单项名字用来验证截断, /bin/bash\n"
             "3 = three, /bin/sh\n4 = four, /bin/sh\n5 = five, /bin/sh\n")
    nlong = "一" * 20                       # 60 字节：老结构体（32 字节）装不下，会被从中间切断
    nini_long = "[menu]\n1 = sh, /bin/sh\n2 = %s, /bin/bash\n" % nlong

    n1a = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s"], ini=nini5)) or [])
    n1b = "\n".join(vt_text(12, 100, capture_page_keys(12, 100, [b"\x02s", wheel(60, 6, 3)], ini=nini5)) or [])
    ck("N1 12 行 × 侧栏列表是窗口：滚轮能把第 4/5 项滚进来（不再写死成「添加(共N项)」）",
       "  [1] sh" in n1a and "  [4] four" not in n1a and "  [4] four" in n1b and "  [1] sh" not in n1b
       and "添加(共" not in n1a, "")
    n1c = "\n".join(vt_text(13, 100, capture_page_keys(13, 100, [b"\x02s", wheel(60, 6, 2)], ini=nini5)) or [])
    n1d = "\n".join(vt_text(13, 100, capture_page_keys(13, 100, [b"\x02s", wheel(60, 6, 2),
                                                                  wheel(60, 6, 4, down=False)], ini=nini5)) or [])
    ck("N1 13 行 × 滚轮向下换窗口、向上能滚回第 1 项",
       "  [3] three" in n1c and "  [5] five" in n1c and "  [1] sh" not in n1c
       and "  [1] sh" in n1d and "  [2] 一" in n1d, "")
    n1h = vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s"], ini=nini5)) or []
    n1col = [l.split("│")[0] for l in n1h]          # 只看侧栏那一列，右侧画什么不影响判据
    n1last = max(i for i, l in enumerate(n1col) if re.search(r"\[\d\]", l))
    n1add = next((i for i, l in enumerate(n1col) if "[+] 添加新条目" in l), -1)
    ck("N1 24 行 × 条目全都放得下 → 表头不带 (a-b/N)，[+] 紧贴列表末行（侧栏不空出一段）",
       any("导航选项" in l and "(" not in l for l in n1col)
       and n1add == n1last + 1 and "  [5] five" in "\n".join(n1col),
       "末项行=%d [+]行=%d" % (n1last + 1, n1add + 1))

    n2 = vt_text(24, 50, capture_page_keys(24, 50, [b"\x02s", b"\r"], ini=nini5)) or []
    n2j = "\n".join(n2)
    ck("N2 50 列 × 详情页：四个字段标签都在（不再被硬切掉）",
       all(k in n2j for k in ["2. 启动命令行", "3. 启动目录", "4. 启动默认颜色"]), n2j[:200])
    ck("N2 50 列 × 详情页：放不下的行以 ... 结尾，且没有一行越过 50 列",
       n2j.count("...") >= 3 and all(dispw(l) <= 50 for l in n2),
       "%d 处 ...，最长 %d 列" % (n2j.count("..."), max([dispw(l) for l in n2] or [0])))

    nhov_base = vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s"], ini=nini5)) or []
    nhov = vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s", b"\x1b[<35;12;8M"], ini=nini5)) or []
    nhovj = "\n".join(nhov)
    ck("N3 鼠标停在被截断的菜单项名上 → 气泡给出全文（老版：光标在侧栏时压根不弹）",
       "一个非常长的菜单项名字用来验证截断" in nhovj and "┌" in nhovj, "")
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
    nlt = "\n".join(vt_text(24, 100, capture_page_keys(24, 100, [b"\x02s", b"\x1b[<35;12;8M"],
                                                        ini=nini_long)) or [])
    ck("N7 长名字悬停：气泡是全文，且没有半个 UTF-8 字符留下的 '?'",
       nlong in nlt and "?" not in nlt, "")


    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("PANE PALETTE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
