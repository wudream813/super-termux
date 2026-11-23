#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_scroll_continuity.py — 往上翻历史必须是「一行一行挪」，不能整屏跳。

真机症状（用户报「一滚动就跳到 / 跳出历史」）：
  实时视图按【物理行】画（内容贴顶，banner 在窗格第 1 行、提示符在第 4 行，下面都是空行）；
  而 v1.8.52 起的回看路径按【reflow 网格】画，且是**底部锚定**（最后一行内容贴在视口底）。
  于是滚一格 = 从「贴顶」瞬间变成「贴底」，整屏内容跳过一次 —— 用户看到的就是
  「一滚动就跳到历史」「当前输入和历史接不上」。

口径（v2.4.0）：回看也用物理行 —— 视口 = ring 上从 rel=-vo 起的窗口，与实时视图同一套
坐标。滚一格就只挪一格；滚到顶时最老那行历史正好贴在视口第一行，历史、说明行、本轮
banner、提示符自上而下连续排列。只有渲染宽度窄于模型宽度（拖动分屏边框）时才退回重排网格。
恢复出来的历史从 v2.4.0 起就【常驻可见区上方】，滚回底部时它仍与提示符连成一段 ——
这与 v2.3.9「实时视图里看不见历史」是两条口径，后者已被用户否掉（「开屏就应当看到历史，
边打字边看得到」），所以 C2 判的是「历史还在、且提示符接在它后面」。

本判据用真控制台（同 tests/win_console_smoke.py 的 Term）跑真 exe + 造一份 60 行快照
（必须比视口高，否则滚轮本来就无处可滚，A1 量不到位移）：
  A 滚一格位移是「几行」而不是「半屏/整屏」，且连续两格位移一致；
  B 滚到顶时最老历史贴顶，历史 → 说明行 → banner → 提示符 顺序连续；
  C 滚回底回到原位，且历史仍与提示符连续可见。
非 Windows 直接 SKIP。
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tests"))

EXE = os.path.join(ROOT, "termux.exe")
FAILS = []
HIST = 60      # 必须高于视口：否则恢复的历史整屏可见，滚轮本来就无处可滚


def ck(name, cond, extra=""):
    print("  [%s] %s %s" % ("ok" if cond else "FAIL", name, extra if not cond else ""))
    if not cond:
        FAILS.append(name)


INI = """[general]
theme = github-dark
prefix = C-b
scrollback = 10000
mouse = true
session = on
graphics = off
anim = off
default_startup = 0
"""

SESS = """s 2
u 1795346132
T 0 p
P 1
""" + "".join("D HIST-%03d old line %d\n" % (i, i) for i in range(1, HIST + 1))


def wheel(term, up, times=1):
    import win_console_smoke as W

    class ME(ctypes.Structure):
        _fields_ = [("dwMousePosition", W.COORD), ("dwButtonState", wt.DWORD),
                    ("dwControlKeyState", wt.DWORD), ("dwEventFlags", wt.DWORD)]

    class MU(ctypes.Union):
        _fields_ = [("MouseEvent", ME), ("pad", ctypes.c_byte * 32)]

    class MIR(ctypes.Structure):
        _fields_ = [("EventType", wt.WORD), ("Event", MU)]

    for _ in range(times):
        recs = (MIR * 2)()
        for i in range(2):
            recs[i].EventType = 0x0002
            m = recs[i].Event.MouseEvent
            m.dwMousePosition = W.COORD(20, 8)
            m.dwButtonState = ((120 if up else -120) & 0xFFFF) << 16
            m.dwControlKeyState = 0
            m.dwEventFlags = 0x0004          # MOUSE_WHEELED
        n = wt.DWORD()
        W.k32.WriteConsoleInputW(term.hin, ctypes.cast(recs, ctypes.POINTER(W.INPUT_RECORD)), 2, ctypes.byref(n))
        time.sleep(0.06)


def dedup(text):
    """conhost 读屏时宽字会把同一字符填进两格（显示仍是一个字），比对前先折一下。"""
    return re.sub(r"(.)\1", r"\1", text)


def row_of(lines, needle):
    for i, ln in enumerate(lines):
        if needle in ln or needle in dedup(ln):
            return i
    return -1


def first_text_row(lines):
    for i, ln in enumerate(lines[1:], 1):
        if ln.strip():
            return i
    return -1


def first_hist_index(lines):
    """视口第一条内容行是 HIST-几。滚轮一格应当把它减 3（=3 行），而不是整屏跳。"""
    r = first_text_row(lines)
    if r < 0:
        return None
    m = re.match(r"HIST-(\d+)", lines[r].strip())
    return int(m.group(1)) if m else None


def main():
    print("=== 回看历史的位移必须连续（verify_scroll_continuity.py） ===")
    if os.name != "nt":
        print("  [SKIP] 现场是 Windows ConPTY（Linux 侧用 verify_scrollbar.py 覆盖）")
        return 0
    if not os.path.exists(EXE):
        print("FAIL: 找不到 %s，先编出来" % EXE)
        return 1
    import win_console_smoke as W

    td = tempfile.mkdtemp(prefix="termux_scrollc_")
    try:
        exe = os.path.join(td, "termux.exe")
        shutil.copyfile(EXE, exe)
        with open(os.path.join(td, "termux.ini"), "w", encoding="utf-8", newline="\n") as f:
            f.write(INI)
        with open(os.path.join(td, "termux.session"), "w", encoding="utf-8", newline="\n") as f:
            f.write(SESS)
        t = W.Term(exe)
        try:
            t.wait_for(">", 12)
            time.sleep(1.5)
            live = t.lines()
            base = row_of(live, "Microsoft Windows")
            ck("A0 前置：实时视图里能看到本轮 banner", base > 0, "找不到 banner 行")

            # 位移用「视口第一行是第几条历史」来量：它不依赖 banner 还留在屏上。
            i0 = first_hist_index(live)
            wheel(t, True, 1)
            time.sleep(0.4)
            one = t.lines()
            i1 = first_hist_index(one)
            ck("A1 滚一格只挪几行（不是整屏跳）", i0 and i1 is not None and 0 < i0 - i1 <= 6,
               "第一行从 HIST-%s 变成 %s" % (i0, i1))

            wheel(t, True, 1)
            time.sleep(0.4)
            two = t.lines()
            i2 = first_hist_index(two)
            ck("A2 连续两格位移一致（逐行滑动）",
               i0 and i1 is not None and i2 is not None and i0 - i1 == i1 - i2,
               "第一格 -%s、第二格 -%s" % (i0 - i1 if i1 else None, i1 - i2 if (i1 and i2) else None))

            wheel(t, True, 40)
            time.sleep(0.5)
            top = t.lines()
            first = first_text_row(top)
            ck("B1 滚到顶时最老那行历史贴在最上面", first > 0 and "HIST-001" in top[first],
               "第一条内容行是 %r" % (top[first][:60] if first > 0 else None))
            seq = []
            for r in range(first + 1, min(first + 8, len(top))):
                m = re.match(r"HIST-(\d+)", top[r].strip())
                seq.append(int(m.group(1)) if m else -1)
            ck("B2 顶部往下逐行连续（没有中间丢行）", seq == list(range(2, 2 + len(seq))),
               "顶部可见的历史序号 = %s" % seq)

            wheel(t, False, 40)
            time.sleep(0.5)
            back = t.lines()
            ck("C1 滚回底回到原位", row_of(back, "Microsoft Windows") == base,
               "banner 回到第 %d 行（原本第 %d 行）" % (row_of(back, "Microsoft Windows"), base))
            last_hist = row_of(back, "HIST-%03d" % HIST)
            ck("C2 滚回底后恢复历史仍与提示符连续可见（v2.4.0 口径：历史不藏起来）",
               last_hist >= 0 and row_of(back, ">") > last_hist,
               "历史最后一行 row=%d、提示符 row=%d" % (last_hist, row_of(back, ">")))
            ck("C3 说明行在本轮 banner 之上、历史之下",
               last_hist >= 0 and row_of(back, "上次会话的历史") > last_hist
               and row_of(back, "Microsoft Windows") > row_of(back, "上次会话的历史"),
               "说明行 row=%d、banner row=%d" % (row_of(back, "上次会话的历史"),
                                                 row_of(back, "Microsoft Windows")))
        finally:
            t.close()
            try:
                W.k32.TerminateProcess(t.pi.hProcess, 0)
            except Exception:
                pass
            t._restore_env()
    finally:
        shutil.rmtree(td, ignore_errors=True)

    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("SCROLL-CONTINUITY PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
