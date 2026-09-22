#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""win_smoke.py —— 在真 Windows + 真 ConPTY 上驱动 termux.exe 的冒烟测试。

为什么单独有这么一个文件
------------------------
移植到三系统之后，Windows 侧**一个运行时测试都没有**：CI 的 windows 作业只做
构建 + 静态检查，因为 tests/drive.py 是 POSIX 的（pty/fcntl/termios）。
于是这几件事从来没在真 Windows 上被验证过：

  * bug #5 / #9 的修复 —— 关于页和帮助页的平台文案。之前只做到「用 gcc -E
    预处理出对应字面量」，那是编译期证据，不是运行期证据。
  * resize。Windows 恰恰是「resize 丢历史」那个原始 bug 的老家，而九轮盲改
    全败就是因为手上没有 Windows 的自动化复现手段。

所以这里只做四件事，每件都是【运行期】断言：
  1) 帮助页的平台串是 Windows 的那一句（不是 Linux/macOS 的）
  2) 关于页的平台串 + 版本号
  3) 分屏真的多出一个窗格（不是只改了状态）
  4) resize 之后历史内容还在（原始 bug 的判据方向）

只在 Windows 上跑。别的平台【显式跳过并返回 0】，同时打印一行说明 ——
不是静默通过：CI 日志里能看到「这一步在 Linux 上没跑」。
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)


def read_version():
    """从 include/common.h 读版本号，和运行期显示的对一遍。"""
    p = os.path.join(ROOT, "include", "common.h")
    s = io.open(p, encoding="utf-8").read()
    m = re.search(r'#define TERMUX_VERSION "([^"]+)"', s)
    return m.group(1) if m else None


def main():
    if os.name != "nt":
        # ★ 显式跳过，不要静默通过。CI 日志里必须能看出这一步没跑。
        print("[SKIP] win_smoke.py 只在 Windows 上跑（当前 os.name=%r）。" % os.name)
        print("       POSIX 侧对应的测试是 make check-posix / tests/posix_smoke.py。")
        return 0

    import conpty_drive as cd

    ver = read_version()
    if not ver:
        print("[FAIL] 读不到 include/common.h 里的 TERMUX_VERSION")
        return 1

    fails = []
    box = {}

    def ck(name, cond, detail=""):
        if cond:
            print("  [ok]   %s" % name)
        else:
            # ★ 失败时把驱动的诊断信息一起打出来。这里没有 Windows，改一轮要等
            #   CI 3~4 分钟，光看「FAIL 帮助页出现」根本不知道该往哪查。
            print("  [FAIL] %s %s" % (name, detail))
            if box.get("t") is not None and not box.get("diag"):
                box["diag"] = True
                print("         诊断: %s" % box["t"].diagnostics())
            fails.append(name)

    print("=== Windows / ConPTY 冒烟测试（termux.exe，版本 %s）===" % ver)
    print("  被测二进制: %s" % cd.EXE)
    if not os.path.exists(cd.EXE):
        print("[FAIL] 找不到 %s，先 make CC=gcc CXX=g++ all" % cd.EXE)
        return 1

    t = cd.Term(cols=100, rows=30)
    box["t"] = t
    print("  启动后: %s" % t.diagnostics())

    def wait(pred, n=40, step=0.15):
        for _ in range(n):
            if pred():
                return True
            t.drain(step)
        return False

    try:
        ck("进程起来了", t.alive())

        # ---- 1) 帮助页：平台串必须是 Windows 的那一句 ----
        t.send(cd.PREFIX + b"?")
        ck("帮助页出现", wait(lambda: "Terminal Multiplexer" in t.frame()))
        f = t.frame()
        want = "版本 v%s | Windows Terminal Multiplexer (Win10 1809+)" % ver
        ck("帮助页平台串是 Windows 的（bug #9）", want in f,
           "期望 %r" % want)
        for bad in ("Linux Terminal Multiplexer", "macOS Terminal Multiplexer"):
            ck("帮助页没串成 %s" % bad.split()[0], bad not in f)

        # ---- 2) 关于页：走命令面板打开 ----
        t.send(cd.PREFIX + b":", wait=0.8)
        t.send("about", wait=0.8)
        t.send(b"\r", wait=1.2)
        ck("关于页出现", wait(lambda: "版本号" in t.frame()))
        f = t.frame()
        ck("关于页标题是 Windows 的（bug #5）", "Windows 终端复用器" in f)
        ck("关于页副标题写的是 ConPTY", "基于 Windows ConPTY" in f)
        ck("关于页版本号 = %s" % ver, ("v" + ver) in f)
        ck("关于页没有「单文件 C」这个歧义措辞", "单文件 C" not in f)
        ck("关于页仓库链接指向 super-termux", "wudream813/super-termux" in f)

        # ---- 3) 分屏真的多出一个窗格 ----
        n0 = t.nframes()
        t.send(cd.PREFIX + b"_", wait=1.0)       # 上下分屏
        wait(lambda: t.nframes() > n0, n=30)
        f = t.frame()
        ck("分屏后出现了窗格分隔", ("─" in f) or ("│" in f) or ("┼" in f),
           "帧里没找到任何分隔字符")

        # ---- 4) resize 之后内容还在 ----
        marker = "SMOKE_MARKER_9137"
        t.send(("echo %s\r" % marker).encode(), wait=1.2)
        ck("标记出现在屏幕上", wait(lambda: marker in t.frame()))
        t.resize(70, 24, wait=1.2)                # 变窄
        ck("变窄后标记仍在", marker in t.frame())
        t.resize(120, 34, wait=1.2)               # 变宽
        ck("变宽后标记仍在", marker in t.frame())

    finally:
        try:
            t.quit()
            t.close()
        except Exception:
            pass

    print()
    if fails:
        print("%d 项失败：%s" % (len(fails), "；".join(fails)))
        return 1
    print("Windows 冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
