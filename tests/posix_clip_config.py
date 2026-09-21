#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
posix_clip_config.py — POSIX 侧两条运行时回归（都是真 pty + 真二进制）

覆盖的两个 bug，都是「编译期看不出来、跑起来静默失效」那一类：

  1) 复制模式在 POSIX 上完全失效。
     真正的复制路径是 src/input.c 里的 OpenClipboard/GlobalAlloc/SetClipboardData，
     而 plat_clip_copy() 全仓零调用者 —— POSIX 替身是空实现，于是按 Enter 什么
     都不发生、也不报错。修法在 src/platform_posix.c：SetClipboardData 收到
     CF_UNICODETEXT 时把 WCHAR 缓冲转成 UTF-8 交给 plat_clip_copy()，由它发
     OSC 52（并顺带试 pbcopy/wl-copy/xclip）。判据：宿主的 pty master 上必须
     出现 "\\x1b]52;c;<base64>\\a"，且 base64 解出来就是选中的文本。

  2) 配置文件写错地方 / 根本写不出来。
     resolve_ini_path() 原先用 wcsrchr(exe_path, L'\\\\') 找反斜杠，POSIX 路径是
     正斜杠，找不到就退化成相对路径 "termux.ini" —— 落在【当前工作目录】，换目录
     启动就换一份；而 %USERPROFILE% 在 POSIX 上恒为空，回退分支也永远走不到。
     更深一层：glibc 的 swprintf 把 %s 当【窄】字符处理（传 wchar_t* 进去会在
     第一个 0x00 字节处截断，L"/tmp" 只剩 "/"），所以即便分隔符修对了，
     _snwprintf(L"%s/termux.ini", dir) 也只会得到 "//termux.ini"。
     判据：把二进制复制到临时目录后启动，termux.ini 必须出现在【二进制同目录】，
     且【不能】出现在启动用的 cwd 里。

用法：  python3 tests/posix_clip_config.py
        TERMUX_SMOKE_EXE=/path/to/termux-linux python3 tests/posix_clip_config.py
"""

import base64
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_EXE = os.environ.get("TERMUX_SMOKE_EXE", os.path.join(ROOT, "termux-linux"))

FAILS = []
CHECKS = [0]


def ck(name, cond, extra=""):
    CHECKS[0] += 1
    if cond:
        print("  [ok]   %s" % name)
    else:
        FAILS.append(name)
        print("  [FAIL] %s%s" % (name, ("  -> " + extra) if extra else ""))


def osc52(raw):
    """从 pty master 的原始字节里取出最后一条 OSC 52 的 payload 并解码。"""
    m = None
    for m in re.finditer(rb"\x1b\]52;[^;]*;([A-Za-z0-9+/=]+)", raw):
        pass
    if not m:
        return None
    return base64.b64decode(m.group(1)).decode("utf-8", "replace")


def copy_selection(t, setup, n_left):
    """在 shell 里 echo -n 一段文本（光标停在行尾），进复制模式向左选 n 格后回车。"""
    t.send(b"\x03", 0.3)                 # 清掉当前行，避免残留字符混进选区
    t.send(setup, 0.8)
    t.send(b"\x02[", 0.7)                # Ctrl+B [ 进复制模式，光标在行尾
    t.raw = b""
    t.send(b"\x1b[1;2D" * n_left, 0.6)   # Shift+Left 逐格扩选
    t.send(b"\r", 1.0)                   # Enter = 复制到剪贴板
    return osc52(t.raw)


def main():
    if not os.path.exists(SRC_EXE):
        print("FAIL: 找不到 %s，先跑 make linux" % SRC_EXE, file=sys.stderr)
        return 1

    # 把二进制复制到临时目录再跑：这样「配置落在二进制同目录」这条判据不会往仓库里
    # 写文件，也不会碰到用户真实的 termux.ini。
    bindir = tempfile.mkdtemp(prefix="termux_bin_")
    exe = os.path.join(bindir, os.path.basename(SRC_EXE))
    shutil.copy2(SRC_EXE, exe)
    os.environ["TERMUX_SMOKE_EXE"] = exe
    bin_ini = os.path.join(bindir, "termux.ini")
    # 这条必须放在【任何】实例启动之前：第一个实例一跑就会生成 termux.ini。
    ck("启动前二进制同目录没有 termux.ini", not os.path.exists(bin_ini))

    sys.path.insert(0, os.path.join(ROOT, "tests"))
    import drive                                            # noqa: E402  (要先设好 env)

    # ---------------------------------------------------------------- 剪贴板
    print("=== 1. 复制模式 -> OSC 52 剪贴板 ===")
    t = drive.Term(100, 24)
    t.drain(2.0)

    # 列算术：宽字符占 2 格，从次格往左会吸附到主格（copy_cursor_to_lead）。
    #   "ABCDEFGH"  a0..h7，光标在 8；左 4 格 -> "EFGH"
    #   "中文测试"   中0-1 文2-3 测4-5 试6-7，光标在 8；左 4 格 -> 8->6->4->2 -> "中文测试"
    #                                        左 2 格 -> 8->6->4        -> "测试"
    #   "ab中文cd"  a0 b1 中2-3 文4-5 c6 d7，光标在 8；左 4 格 -> 8->7->6->4->2 -> "中文cd"
    cases = [
        ("ASCII 选 4 格",   b"echo -n ABCDEFGH", 4, "EFGH"),
        ("中文选 2 格",     "echo -n 中文测试".encode("utf-8"), 2, "测试"),
        ("中文选 4 格",     "echo -n 中文测试".encode("utf-8"), 4, "中文测试"),
        ("中英混排选 4 格", "echo -n ab中文cd".encode("utf-8"), 4, "中文cd"),
        ("emoji 选 2 格",   "echo -n X\U0001F389Y".encode("utf-8"), 2, "\U0001F389Y"),
    ]
    for label, setup, n_left, want in cases:
        got = copy_selection(t, setup, n_left)
        if got is None:
            ck("复制 %s" % label, False, "宿主没收到任何 OSC 52（剪贴板没接通）")
        else:
            ck("复制 %s = %r" % (label, want), got == want, "实际得到 %r" % got)

    t.send(b"\x1b", 0.4)
    ck("复制完还活着", t.alive())
    rc = t.quit()
    t.close()
    ck("干净退出 (exit 0)", rc == 0, "exit=%s" % rc)

    # ------------------------------------------------------------ 配置文件路径
    print("=== 2. termux.ini 落在二进制同目录 ===")
    # 清掉第 1 节生成的，重新验证一次完整的「首次启动即建」。
    # 用 if 判存在：bug 复现时这个文件压根不会生成，测试要报 [FAIL] 而不是崩掉。
    if os.path.exists(bin_ini):
        os.remove(bin_ini)
    t2 = drive.Term(100, 24)
    t2.drain(2.5)
    cwd_ini = os.path.join(t2.tmp, "termux.ini")
    ck("cwd（%s）里没有 termux.ini" % t2.tmp, not os.path.exists(cwd_ini),
       "配置又写到当前工作目录了")
    ck("二进制同目录生成了 termux.ini", os.path.exists(bin_ini),
       "resolve_ini_path 没解析出 exe 目录（分隔符 / _snwprintf 的 %s 有问题）")
    if os.path.exists(bin_ini):
        body = open(bin_ini, "rb").read().decode("utf-8", "replace")
        ck("ini 内容非空且带 [general] 段", "[general]" in body and len(body) > 200,
           "只有 %d 字节" % len(body))
        ck("ini 里路径没被截断成空目录（无 '//' 开头）",
           not body.startswith("//"), body[:40])
    rc2 = t2.quit()
    t2.close()
    ck("第二次也干净退出", rc2 == 0, "exit=%s" % rc2)

    # 换个 cwd 再起一次：配置必须还是同一份（不跟着 cwd 跑）
    t3 = drive.Term(100, 24)
    t3.drain(2.0)
    ck("换 cwd 后仍复用二进制同目录那一份",
       os.path.exists(bin_ini) and not os.path.exists(os.path.join(t3.tmp, "termux.ini")))
    t3.quit()
    t3.close()

    shutil.rmtree(bindir, ignore_errors=True)

    print()
    print("posix_clip_config: %d 项检查，%d 项失败" % (CHECKS[0], len(FAILS)))
    if FAILS:
        for f in FAILS:
            print("  FAILED: %s" % f)
        return 1
    print("  [OK] 复制模式剪贴板 + 配置文件路径 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
