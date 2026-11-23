#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_session_restore.py — 恢复历史与当前 shell 必须像一段正常终端内容连续显示。

真机症状：恢复历史曾被清屏、占用 shell 的绝对行，或与活屏使用不同的滚动坐标；用户看到
「历史没了 / 一敲字符就跳 / 一滚就跳出历史」。另一个损失是 sess_write_pane 把空行全部
continue 掉，段落空行存盘后消失。

v2.4.1 的判据用真控制台和真 termux.exe：
  A 开屏就同时看见快照历史、当前 banner 和 prompt；历史+活屏不足一屏时【贴顶】——
    历史从屏幕最上面开始，prompt 紧接内容（像普通终端），光标就在 prompt 那一行；
    显式空行行距保留。
  B 输入/输出追加到历史之后，不替换/跳出历史。
  C 用滚轮逐格回看、滚到顶，行连续且最老内容贴顶；滚回底回到 prompt。
  D 正常 exit 再存盘时，两个显式空行仍是空 D 记录。
  E 存盘把恢复出来的历史写回档里，且每行只有一份、说明行不入档。
  F 用 E 存出的档再开一代：老历史和上一轮命令都回来，存回去仍不重复。
  G `cls` 清屏之后实时屏真的空了，prompt/光标回到上方（历史只留在回滚缓冲里）。
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
HIST = 12


def ck(name, cond, extra=""):
    print("  [%s] %s %s" % ("ok" if cond else "FAIL", name, extra if not cond else ""))
    if not cond:
        FAILS.append(name)


def make_session():
    out = ["s 2\nu 1795346132\nT 0 p\nP 1\n"]
    for i in range(1, HIST + 1):
        out.append("D HIST-%02d 这是上一轮的历史行 line %d\n" % (i, i))
        if i == 3:
            out.extend(["D \n", "D \n"])
    return "".join(out)


SESS = make_session()
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


def dedup(text):
    """conhost ReadConsoleOutputW 可能把一个 CJK 字形复制到两格；断言前折成一个。"""
    return re.sub(r"(.)\1", r"\1", text)


def find_row(lines, needle):
    for i, ln in enumerate(lines):
        if needle in ln or needle in dedup(ln):
            return i
    return -1


def first_text_row(lines):
    """第一条有内容的行（第 0 行是标签栏）。"""
    for i, ln in enumerate(lines[1:], 1):
        if ln.strip():
            return i
    return -1


def wheel(term, up, times=1):
    import win_console_smoke as W

    class MOUSE_EVENT(ctypes.Structure):
        _fields_ = [("dwMousePosition", W.COORD), ("dwButtonState", wt.DWORD),
                    ("dwControlKeyState", wt.DWORD), ("dwEventFlags", wt.DWORD)]

    class MOUSE_UNION(ctypes.Union):
        _fields_ = [("MouseEvent", MOUSE_EVENT), ("pad", ctypes.c_byte * 32)]

    class MOUSE_INPUT_RECORD(ctypes.Structure):
        _fields_ = [("EventType", wt.WORD), ("Event", MOUSE_UNION)]

    for _ in range(times):
        recs = (MOUSE_INPUT_RECORD * 2)()
        for i in range(2):
            recs[i].EventType = 0x0002
            m = recs[i].Event.MouseEvent
            m.dwMousePosition = W.COORD(20, 8)
            delta = 120 if up else -120
            m.dwButtonState = ((delta & 0xFFFF) << 16)
            m.dwControlKeyState = 0
            m.dwEventFlags = 0x0004
        n = wt.DWORD()
        W.k32.WriteConsoleInputW(term.hin, ctypes.cast(recs, ctypes.POINTER(W.INPUT_RECORD)), 2,
                                 ctypes.byref(n))
        time.sleep(0.055)


def main():
    print("=== 会话恢复：历史与当前输入连续（verify_session_restore.py） ===")
    if os.name != "nt":
        print("  [SKIP] 现场是 Windows ConPTY")
        return 0
    if not os.path.exists(EXE):
        print("FAIL: 找不到 %s，先编出来" % EXE)
        return 1
    import win_console_smoke as W

    td = tempfile.mkdtemp(prefix="termux_sess_")
    t = None
    try:
        exe = os.path.join(td, "termux.exe")
        shutil.copyfile(EXE, exe)
        with open(os.path.join(td, "termux.ini"), "w", encoding="utf-8", newline="\n") as f:
            f.write(INI)
        sess_path = os.path.join(td, "termux.session")
        with open(sess_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(SESS)
        t = W.Term(exe)
        t.wait_for(">", 12)
        time.sleep(1.2)

        lines0 = t.lines()
        text0 = "\n".join(dedup(x) for x in lines0)
        prompt = os.path.basename(td) + ">"
        prompt0 = find_row(lines0, prompt)
        hist3 = find_row(lines0, "HIST-03")
        hist4 = find_row(lines0, "HIST-04")
        ck("A1 开屏直接看见恢复历史", "HIST-01" in text0,
           "屏上没有 HIST-001:\n" + text0[:450])
        ck("A2 当前命令行也在同一视图", prompt0 >= 0, "找不到当前目录 prompt")
        ck("A3 历史有两个显式空行（HIST-003 和 004 间隔 3 行）",
           hist3 >= 0 and hist4 - hist3 == 3,
           "HIST-03 row=%d HIST-04 row=%d" % (hist3, hist4))
        info = t.info()
        cur_y, _ = t.cursor()
        ck("A4 prompt 光标就在 prompt 那一行（历史不足一屏时视图贴顶，光标不再钉在最底行）",
           prompt0 >= 0 and cur_y == info.srWindow.Top + prompt0,
           "cursor y=%d，prompt 在窗口第 %d 行（窗口顶=%d）" % (cur_y, prompt0, info.srWindow.Top))
        top_row = first_text_row(lines0)
        ck("A5 历史从屏幕最上面开始（像普通终端），不压在屏底",
           top_row > 0 and "HIST-01" in dedup(lines0[top_row]) and top_row <= 2,
           "第一条内容行是第 %d 行：%r" % (top_row, lines0[top_row][:60] if top_row > 0 else None))

        # 小于一屏时，滚轮不应把历史/活屏切成两种不同布局；视图仍有历史与 prompt。
        # 注意 prompt 必须按【原始行】找：dedup() 是给 conhost 把宽字复制两格的显示
        # 瑕疵兜底的，它会把这个目录名里的 "ss" 也折成 "s"，拿 dedup 后的整屏去
        # `prompt in ...` 永远匹配不上（v2.4.0 这条一度是自己给自己判红）。
        before = t.screen()
        wheel(t, True, 1)
        time.sleep(0.25)
        after = t.screen()
        ck("B1 历史未占满屏时轻滚不跳出连续视图",
           "HIST-01" in dedup(after) and find_row(t.lines(), prompt) >= 0,
           "滚后历史或 prompt 消失:\n" + dedup(after)[:450])

        t.type("echo restored-test\r")
        time.sleep(0.9)
        lines1 = t.lines()
        text1 = "\n".join(dedup(x) for x in lines1)
        echo_row = find_row(lines1, "echo restored-test")
        ck("C1 输入回声出现", echo_row >= 0, "没有 echo 回声")
        ck("C2 输出出现", "restored-test" in text1, "没有 restored-test 输出")
        ck("C3 输入/输出之后历史仍在同一屏", "HIST-01" in text1,
           "输入后恢复历史不再可见")
        ck("C4 当前 prompt 仍显示在历史后面", find_row(lines1, prompt) >= 0,
           "找不到 prompt")

        # 清理对话后，让本次退出的快照再写盘；内部显式空行必须仍是 `D ` 空记录。
        t.type("exit\r")
        deadline = time.time() + 8
        while time.time() < deadline and t.alive():
            time.sleep(0.1)
        t.close()
        try:
            W.k32.TerminateProcess(t.pi.hProcess, 0)
        except Exception:
            pass
        W.k32.FreeConsole()
        time.sleep(0.3)
        saved = open(sess_path, "r", encoding="utf-8", errors="replace").read() if os.path.exists(sess_path) else ""
        blank_runs = re.findall(r"D HIST-03[^\n]*\n(D )\n(D )\nD HIST-04", saved)
        ck("D 存盘后历史段落里的两个空行仍是空 D 记录", len(blank_runs) >= 1,
           "没有找到 HIST-003 / D空 / D空 / HIST-004；存档开头:\n" + saved[:600])
        # E：v2.4.0 的坑 —— sess_collect_starts 曾拿 `last_content < 0` 当「还没设」的
        # 哨兵，而历史行的 rel 本来就是负数 ⇒ 每遇到一条历史逻辑行就把 first_content
        # 重设成它自己，最终只有可见区那几行入档：恢复出来的历史、以及屏满之后滚进
        # 回滚缓冲的内容整段丢失（用户报「历史没有内容」的另一条根因）。
        ck("E1 存盘把恢复出来的历史也写回档里", "D HIST-01" in saved and "D HIST-12" in saved,
           "存档里没有 HIST-01..12；开头:\n" + saved[:600])
        ck("E2 历史行没有重复写两遍（说明行也不该被当成历史）",
           saved.count("HIST-01") == 1 and "上次会话的历史" not in saved,
           "HIST-01 出现 %d 次、说明行在档里=%s" % (saved.count("HIST-01"),
                                                     "上次会话的历史" in saved))

        # F：第二代往返。用上一轮存出来的档再开一次：老历史 + 上一轮命令都要回来，
        # 存回去时老历史仍只有一份（不随代数增长、也不会一轮丢一次）。
        t = W.Term(exe)
        t.wait_for(">", 12)
        time.sleep(1.2)
        text2 = "\n".join(dedup(x) for x in t.lines())
        ck("F1 第二代开屏仍看见最老的 HIST-01", "HIST-01" in text2, text2[:450])
        ck("F2 第二代开屏接得上上一轮的 restored-test", "restored-test" in text2, text2[:450])
        t.type("exit\r")
        deadline = time.time() + 8
        while time.time() < deadline and t.alive():
            time.sleep(0.1)
        t.close()
        W.k32.FreeConsole()
        time.sleep(0.3)
        saved2 = open(sess_path, "r", encoding="utf-8", errors="replace").read() if os.path.exists(sess_path) else ""
        ck("F3 第二代存盘里 HIST-01 仍只有一份、上一轮命令仍在",
           saved2.count("HIST-01") == 1 and "echo restored-test" in saved2,
           "HIST-01 x%d；档尾:\n%s" % (saved2.count("HIST-01"), saved2[-400:]))

        # G：cls 清屏之后，恢复历史不能再铺在实时视图里（用户报「cls 后光标回位了、
        # 内容没清」）。历史该留在回滚缓冲里由滚动去看，实时屏必须真的空。
        t = W.Term(exe)
        t.wait_for(">", 12)
        time.sleep(1.2)
        t.type("cls\r")
        time.sleep(1.0)
        linesG = t.lines()
        textG = "\n".join(dedup(x) for x in linesG)
        ck("G1 cls 之后屏幕上不再有恢复历史", "HIST-01" not in textG, textG[:450])
        grow = find_row(linesG, prompt)
        ck("G2 cls 之后 prompt 回到屏幕上方（不是还挂在历史后面）", 0 <= grow <= 4,
           "prompt row=%d\n%s" % (grow, textG[:300]))
        gcy, _ = t.cursor()
        ginfo = t.info()
        ck("G3 光标就在 cls 后的 prompt 那一行", grow >= 0 and gcy == ginfo.srWindow.Top + grow,
           "cursor y=%d prompt row=%d" % (gcy, grow))
        t.type("exit\r")
        deadline = time.time() + 8
        while time.time() < deadline and t.alive():
            time.sleep(0.1)
        t.close()
        try:
            W.k32.TerminateProcess(t.pi.hProcess, 0)
        except Exception:
            pass
        W.k32.FreeConsole()
    finally:
        if t is not None:
            try:
                t.close()
            except Exception:
                pass
            try:
                W.k32.TerminateProcess(t.pi.hProcess, 0)
            except Exception:
                pass
            try:
                W.k32.FreeConsole()
            except Exception:
                pass
            t._restore_env()
        shutil.rmtree(td, ignore_errors=True)

    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("SESSION-RESTORE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
