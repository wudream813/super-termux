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
import subprocess
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
    skips = []
    box = {}

    def skip(name, why):
        """环境不具备条件时【明确记为 SKIP 并打印原因】。

        ★ 绝不能改成「悄悄通过」：CI 绿了但没人知道少测了什么，比红着更糟。
        """
        print("  [SKIP] %s —— %s" % (name, why))
        skips.append(name)

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

    # ★★★ 裸跑一次 exe（不经 ConPTY），把 stdout/stderr/退出码都抓下来。
    #   目的：区分「exe 本身有问题」和「exe 没问题但挂到 ConPTY 上就挂」。
    #   GitHub Actions 的 Windows runner 没有交互控制台，所以 main.c:166 那条
    #   "termux: no console attached" 正是【期望】结果 —— 如果这里能拿到这句话，
    #   就证明二进制能加载、能跑到 main，失败点在 ConPTY 挂载那一层。
    try:
        pr = subprocess.run([cd.EXE], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=10, cwd=os.path.dirname(cd.EXE) or ".")
        print("  裸跑: exit=%s  stdout=%r  stderr=%r"
              % (pr.returncode, pr.stdout[:120], pr.stderr[:120]))
    except subprocess.TimeoutExpired as ex:
        print("  裸跑: 超时10s（说明它进了 TUI 在等输入）stdout=%r stderr=%r"
              % ((ex.stdout or b"")[:120], (ex.stderr or b"")[:120]))
    except OSError as ex:
        print("  裸跑: OSError %s" % ex)

    # ★★★ 对照实验：用【同一个驱动】直接起 cmd.exe（不经过 termux）。
    #   CI 第十五轮查明：termux 的窗格读线程总共只收到过 16 字节
    #   （termux_dump.log 只有 "[pane 0 len 16]"，就是 ConPTY 自己的初始化序列），
    #   cmd.exe 的横幅【从来没进过 termux】，却出现在了 runner 的真实控制台上。
    #   也就是说子 shell 绑到了宿主控制台而不是嵌套伪控制台。
    #   但 src/pane.c:252-303 的 ConPTY 接线逐项对照微软 echocon 都是对的，
    #   所以必须先分清是 termux 的问题还是这个环境/驱动模式的问题。
    #   这个对照能一刀切开：cmd.exe 自己也回不来 ⇒ 环境/驱动；能回来 ⇒ termux。
    try:
        # ★ 必须给【绝对路径】：CreateProcessW 一旦传了 lpApplicationName 就
        #   【不搜 PATH】，写 "cmd.exe" 会直接 ERROR_FILE_NOT_FOUND (2)。
        #   CI 第十七轮就是这么白跑了一轮。ComSpec 是 cmd 的规范位置。
        cmd_exe = os.environ.get("ComSpec") or r"C:\Windows\System32\cmd.exe"
        cp = cd.Term(cols=100, rows=30, dump=False, exe=cmd_exe)
        cp.drain(2.5)
        print("  对照(裸 cmd.exe 走同一个 ConPTY 驱动): alive=%s exit_code=%s 收到=%d 字节"
              % (cp.alive(), cp.exit_code(), len(cp.raw)))
        print("         前 160 字节=%r" % (cp.raw[:160],))
        # 判据：cmd.exe 【本体】的输出有没有回到管道。conhost 自己的初始化只有
        # 85 字节（\x1b[?9001h / ?1004h / ?25l / 2J / m / H / OSC 标题 / ?25h），
        # cmd 的横幅里一定有 "[Version"。CI 第十八轮实测：裸 cmd.exe 同样
        # alive=False exit_code=0、只收到 85 字节，横幅跑到了 runner 控制台上
        # —— 所以这是【环境】问题，不是 termux 的问题。
        pane_env_ok = bool(cp.alive()) or (b"[Version" in cp.raw)
        try:
            cp.close()
        except Exception:
            pass
    except Exception as ex:
        print("  对照(裸 cmd.exe): 起不来 —— %s" % ex)

    if pane_env_ok:
        print("  ⇒ 对照通过：这个环境下 ConPTY 子进程能正常收发，下面全部按硬断言跑。")
    else:
        print("  ⇒ 对照失败：这个环境下【裸 cmd.exe 也绑不到伪控制台】"
              "（输出跑到宿主控制台、进程立刻 exit 0）。")
        print("     这不是 termux 的问题 —— 同一个驱动、同一套 ConPTY 调用，"
              "换成 bare cmd.exe 结果一样。")
        print("     后果：子 shell 一死，最后一个窗格被回收，termux 跟着退出"
              "（src/main.c:130-135），")
        print("     于是所有【需要活窗格】的断言在这里都测不了，下面记为 SKIP。")

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

        if pane_env_ok:
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

            # 关于页是特殊内部 pane，src/input.c:3015 明确写了「设置页 / 关于页不允许
            # 分屏」—— 不先关掉它，下面的分屏断言必然失败。默认关闭键是 Ctrl+B x
            # （src/keymap.c:81  VKEY_ANY('X') -> ACT_CLOSE_PANE）。
            t.send(cd.PREFIX + b"x", wait=1.0)

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
        else:
            why = ("这个 runner 上 ConPTY 子进程绑不到伪控制台，"
                   "子 shell 立刻退出 ⇒ 窗格被回收 ⇒ termux 退出")
            for nm in ("关于页出现", "关于页标题是 Windows 的（bug #5）",
                       "关于页副标题写的是 ConPTY", "关于页版本号",
                       "关于页仓库链接指向 super-termux",
                       "分屏后出现了窗格分隔", "标记出现在屏幕上",
                       "变窄后标记仍在", "变宽后标记仍在"):
                skip(nm, why)

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
    if skips:
        print("Windows 冒烟测试通过（硬断言全过），但有 %d 项因环境限制被 SKIP：%s"
              % (len(skips), "；".join(skips)))
        print("★ 这些不是「通过」，是「这个 runner 上测不了」。换有真交互控制台的"
              "机器（或本地 Windows Terminal）应当把它们跑成硬断言。")
        return 0
    print("Windows 冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
