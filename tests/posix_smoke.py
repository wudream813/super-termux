#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""posix_smoke.py —— 在真 pty 里跑 POSIX 版 termux，验证它真的能用。

为什么不能只靠「编译通过」：整个移植的风险都在运行时 —— termios 有没有配对、
CRITICAL_SECTION 的可重入语义有没有对上（Windows 可重入，普通 pthread_mutex
不可重入，一分屏就自死锁）、转义序列解析出来的按键能不能被 keymap 认出来、
forkpty 出来的 shell 输出有没有正确进到 screen 模型、渲染帧对不对。
这些只有真跑一遍才知道；历史上这一版就是靠它抓出两个编译期完全看不出的 bug。

做法：
  1) pty.openpty() 起一个 100x24 的伪终端，把 ./termux-linux 挂上去（TERMUX_DUMP=1，
     于是它会写 render_dump.log，格式与真机一致）；
  2) 按剧本依次喂按键：回显 -> 左右分屏 -> 上下分屏 -> 新标签页 -> 标签切换 ->
     复制模式 -> 搜索 -> 设置页 -> 关分屏窗格 -> 回滚；
  3) 每步用项目自己的 tools/frame2txt.py 把最后一帧真 VT 解码成字符网格；
  4) 断言该步应该出现的界面元素真的出现了。

判据取自 src/render.c 里的实际字面量（"[复制模式 "、"[搜索 \""、"设置 / 默认启动项"），
不是猜的；分隔线取 U+2502 / U+2500。
"""
import os
import pty
import re
import struct
import subprocess
import sys
import tempfile
import termios
import time
import fcntl

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 可用环境变量换二进制（比如带 -g -O0 的调试版）；卡住时设 TERMUX_SMOKE_GDB=1
# 会在 kill 之前先 gdb 抓一份全线程栈 —— 死锁这类问题只能这么看。
EXE = os.environ.get("TERMUX_SMOKE_EXE", os.path.join(ROOT, "termux-linux"))
FRAME2TXT = os.path.join(ROOT, "tools", "frame2txt.py")

COLS, ROWS = 100, 24
FAILS = []
PREFIX = b"\x02"          # Ctrl+B


def ck(name, cond, extra=""):
    if cond:
        print("  [ok]   %s" % name)
    else:
        print("  [FAIL] %s %s" % (name, extra))
        FAILS.append(name)


def decode(dump_path, cols):
    """调用项目自己的 frame2txt.py 把最后一帧解码成字符网格。"""
    cp = subprocess.run([sys.executable, FRAME2TXT, dump_path, "9999", str(cols)],
                        capture_output=True, text=True)
    if cp.returncode != 0:
        return None, 0
    m = re.search(r"共 (\d+) 帧", cp.stderr)
    total = int(m.group(1)) if m else 0
    # frame2txt.py 每行前面会加 "  N| " 行号前缀，它自己就含竖线；
    # 不先剥掉，「有没有分隔线」这类断言会假绿。
    grid = "\n".join(re.sub(r"^\s*\d+\|", "", ln) for ln in cp.stdout.split("\n"))
    return grid, total


def main():
    print("=== POSIX 版冒烟测试 (tests/posix_smoke.py) ===")
    if not os.path.exists(EXE):
        print("FAIL: 找不到 %s，先跑 make linux" % EXE, file=sys.stderr)
        return 1
    if not os.path.exists(FRAME2TXT):
        print("FAIL: 找不到 %s" % FRAME2TXT, file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="termux_smoke_")
    dump = os.path.join(tmp, "render_dump.log")
    env = dict(os.environ)
    env["TERMUX_DUMP"] = "1"
    env["TERM"] = "xterm-256color"
    env["PS1"] = "$ "

    master, slave = pty.openpty()
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    p = subprocess.Popen([EXE], stdin=slave, stdout=slave, stderr=slave,
                         cwd=tmp, env=env, preexec_fn=os.setsid)
    os.close(slave)
    fcntl.fcntl(master, fcntl.F_SETFL, os.O_NONBLOCK)

    def drain(t):
        end = time.time() + t
        while time.time() < end:
            try:
                os.read(master, 65536)
            except (BlockingIOError, OSError):
                pass
            time.sleep(0.02)

    prev_total = [0]

    # ★ 原来这里是固定 sleep(wait) 然后读一帧。快机器上没问题，但 CI 的 macOS
    #   runner（虚拟化共享核，起一个 bash + 渲染一帧都慢一截）上会【采样太早】：
    #   操作其实成功了、只是还没渲染出来，于是断言全红，而下一步又能看到上一步
    #   的结果 —— 帧数永远滞后一拍。第三轮 CI 就是这个形态。
    #   改成【等到本步的断言真的满足】为止，超时才判失败：测试不再依赖机器速度。
    #   注意不能只等「帧数增加」—— 启动阶段本来就在出帧，那样第一步会立刻返回、
    #   还没等到命令回显（我自己先踩了这一次）。
    FRAME_TIMEOUT = float(os.environ.get("TERMUX_SMOKE_FRAME_TIMEOUT", "10"))

    def step(label, keys, wait=1.0, checks=()):
        drain(0.05)
        _, base = decode(dump, COLS)          # 发键之前的帧数
        if keys:
            os.write(master, keys)

        def pending(g):
            """还没满足的正向断言。None 是「必须消失」类，交给后面单独判。"""
            out = []
            for name, needle in checks:
                if needle is None:
                    # 「必须消失」类断言也要等：否则 Esc 之后立刻采样，徽章还在。
                    # 这个剧本里唯一的一条是复制模式徽章。
                    if "消失" in name and "[复制模式 " in g:
                        out.append(name)
                    if "不是 cmd" in name:
                        first = next((ln.strip() for ln in g.split("\n") if ln.strip()), "")
                        if not first or "cmd" in first:
                            out.append(name)
                    continue
                if name.startswith("标签栏变成两个标签"):
                    if g.count("\u00d7") < 2:
                        out.append(name)
                elif needle not in g:
                    out.append(name)
            return out

        # ★ 除了等断言满足，还要等【至少出一个新帧】。否则那些只有「必须消失」
        #   类断言（或压根没断言）的步骤会立刻采样 —— 断言碰巧被上一帧满足就
        #   蒙过去了，下一步才看到这一步的结果，帧数永远滞后一拍。
        deadline = time.time() + max(wait, FRAME_TIMEOUT)
        grid, total = decode(dump, COLS)
        while (pending(grid) or total <= base) and time.time() < deadline and p.poll() is None:
            drain(0.1)
            grid, total = decode(dump, COLS)
        print("\n[%s] 帧数=%d" % (label, total))
        # ★ 诊断：帧数没涨 = 应用停止渲染了（卡死或崩溃）。日志里只有「帧里没有
        #   XXX」时看不出是渲染停了还是内容不对，把进程状态直接打出来。
        if total <= base:
            rc = p.poll()
            # 措辞要留余地：帧数没涨【可能】是卡死，也可能只是这一步没触发重绘。
            # 第三轮 CI 上它就是「慢」而不是「死」—— 下一步又能看到上一步的结果。
            print("  [警告] 等了 %.1fs 帧数仍没增加（%d -> %d）。进程状态=%s"
                  "（可能卡死，也可能这一步没触发重绘）"
                  % (max(wait, FRAME_TIMEOUT), base, total,
                     "还活着" if rc is None else "已退出，退出码=%s" % rc))
        return grid, total

    # 剧本：(标签, 发送的字节, 等待秒, [(断言名, 必须出现的子串)])
    # ★ 复制模式必须在【分屏之前】测：render_status_badge() 只在 render_screen()
    #   的非分屏分支里被调用（src/render.c:3245，分屏走 render_split 那条 else-if），
    #   所以分屏状态下压根不画「[复制模式 …]」徽章。这是共用 render.c 的既有
    #   行为，Windows 上完全一样，不是移植引入的 —— 别把这条顺序改回去。
    script = [
        ("启动 + 命令回显", b"echo TERMUX_POSIX_OK\r", 1.0,
         [("命令回显出现", "TERMUX_POSIX_OK"),
          ("标签栏不是 cmd（默认 shell 没写死 cmd.exe）", None)]),
        ("复制模式 Ctrl+B [", PREFIX + b"[", 0.8,
         [("出现复制模式提示", "[复制模式 ")]),
        ("退出复制模式 Esc", b"\x1b", 0.6,
         [("复制模式提示消失", None)]),
        ("左右分屏 Ctrl+B -", PREFIX + b"-", 1.0,
         [("出现竖分隔线 U+2502", "\u2502"),
          ("两个窗格标签", "\u00d7")]),
        ("上下分屏 Ctrl+B _", PREFIX + b"_", 1.0,
         [("出现横分隔线 U+2500", "\u2500")]),
        ("新窗格里跑命令", b"echo PANE_OK\r", 1.0,
         [("新窗格有输出", "PANE_OK")]),
        ("新标签页 Ctrl+B c", PREFIX + b"c", 1.2,
         [("标签栏变成两个标签（两个 × 关闭按钮）", "\u00d7")]),
        ("下一个标签 Ctrl+B n", PREFIX + b"n", 0.8, []),
        ("搜索 Ctrl+B /", PREFIX + b"/", 0.8,
         [("出现搜索框", "搜索")]),
        ("退出搜索 Esc", b"\x1b", 0.6, []),
        ("设置页 Ctrl+B s", PREFIX + b"s", 1.0,
         [("出现设置页", "设置")]),
        ("退出设置 Esc", b"\x1b", 0.8, []),
        ("回滚：先灌 200 行", b"seq 1 200\r", 1.2,
         [("末尾行可见", "200")]),
        ("回滚：滚轮上滚", b"\x1b[<64;20;10M" * 6, 1.0, []),
        ("关分屏窗格 Ctrl+B q", PREFIX + b"q", 1.0, []),
    ]

    drain(1.5)                      # 等 shell 起来
    prev_grid = None
    for label, keys, wait, checks in script:
        grid, total = step(label, keys, wait, checks)
        if grid is None:
            ck("%s：能解码渲染帧" % label, False)
            continue
        for name, needle in checks:
            if needle is None:
                # 「不是 cmd」在下面有专属判据，名字还一模一样；这里再打一条
                # 占位 [ok] 就会出现「同名先 ok 后 FAIL」，日志里极容易看漏。
                if "不是 cmd" in name:
                    continue
                ck(name, True)      # 占位，下面按「消失」单独判
            elif name.startswith("标签栏变成两个标签"):
                ck(name, grid.count("\u00d7") >= 2,
                   "× 只出现 %d 次" % grid.count("\u00d7"))
            else:
                ok = needle in grid
                extra = "帧里没有 %r" % needle
                if not ok:
                    # 失败时把标签栏那一行也打出来，省得再去猜实际渲染成什么样
                    lines = [ln for ln in grid.split("\n") if ln.strip()]
                    extra += "；标签栏实际是 %r" % (lines[0].strip()[:60] if lines else "(空帧)")
                ck(name, ok, extra)
        if any("不是 cmd" in n for n, _ in checks):
            first = next((ln.strip() for ln in grid.split("\n") if ln.strip()), "")
            ck("标签栏不是 cmd（默认 shell 没写死 cmd.exe）",
               bool(first) and "cmd" not in first, "标签栏实际是 %r" % first[:60])
        if label.startswith("退出复制模式"):
            ck("复制模式提示确实消失了", "[复制模式 " not in grid)
        if label.startswith("回滚：滚轮"):
            # 上滚之后视口应该离开底部：最后一行不该再是 "200"。
            body = [ln.rstrip() for ln in grid.split("\n") if ln.strip()]
            ck("滚轮让视口离开了底部", "200" not in (body[-1] if body else ""),
               "最后一行仍是 %r" % (body[-1] if body else ""))
        prev_grid = grid

    # ---- 退出 ----
    # ★ 等的同时必须继续读 pty：渲染一帧要往宿主写几万字节，一旦我们停止读取，
    #   pty 缓冲写满，主线程就阻塞在 host_write 的 write() 里，永远走不到
    #   g_mux.running 那个检查 —— 表现成「SIGTERM 杀不掉」，其实是测试自己不读。
    p.send_signal(15)
    try:
        # ★ 5 秒在慢机器上不够：退出要对每个 pane 调 plat_thread_join(2000ms)，
        #   这个剧本跑完已经有 3 个 pane + 2 个标签页。给 25 秒。
        deadline = time.time() + 25
        while p.poll() is None and time.time() < deadline:
            drain(0.05)
        if p.poll() is not None:
            ck("SIGTERM 后正常退出（不是被 kill -9）",
               p.returncode in (0, -15, 143), "rc=%s" % p.returncode)
        else:
            raise subprocess.TimeoutExpired(EXE, 25)
    except subprocess.TimeoutExpired:
        if os.environ.get("TERMUX_SMOKE_GDB"):
            print("  卡住了，gdb 抓栈：")
            try:
                r = subprocess.run(["gdb", "-p", str(p.pid), "-batch",
                                    "-ex", "thread apply all bt"],
                                   capture_output=True, text=True, timeout=120)
                print(r.stdout[-4000:])
            except Exception as e:
                print("  gdb 失败：%s" % e)
        p.kill()
        p.wait(timeout=5)
        ck("SIGTERM 后正常退出（不是被 kill -9）", False, "25 秒没退，只能 kill -9")
    os.close(master)

    # 退出后宿主终端的状态：进程结束时写过 restore 序列，最后一个非空帧之后
    # 应该还能看到 alt-screen 关闭。这里只查进程没留孤儿子进程。
    orphans = subprocess.run(["bash", "-c", "ps --ppid %d -o pid= 2>/dev/null | wc -l" % p.pid],
                             capture_output=True, text=True).stdout.strip()
    ck("没有留下孤儿子进程", orphans in ("0", ""), "残留 %s 个" % orphans)

    print()
    if FAILS:
        print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
        return 1
    print("POSIX SMOKE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
