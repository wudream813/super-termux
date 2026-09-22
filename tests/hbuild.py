#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""harness 编译助手 —— 把平台差异收在一个地方。

为什么要有这个模块：Gap A（把主机侧回归搬上 Windows CI）踩到两个只在 Windows
上出现的问题，散在十几个 verify_*.py 里各写一份 sys.platform 判断迟早会漏：

1) **MinGW 的 gcc 会给【无扩展名】的 -o 自动补 .exe。**
   实测（x86_64-w64-mingw32-gcc）：
       -o /tmp/foo        -> 产出 /tmp/foo.exe   ← 脚本随后找 /tmp/foo，找不到
       -o /tmp/foo.bin    -> 产出 /tmp/foo.bin   ← 有扩展名就不补，侥幸没事
   所以 verify_search.py 的 "test_search"、verify_search_refresh.py 的 "sr"、
   verify_palette_search.py 的 "palette_search" 在 Windows 上全会挂，
   而 verify_split.py 的 "h.bin" 不会 —— 这种「一半挂一半不挂」最难查。

2) **MinGW 不支持 -fsanitize=address/undefined。** 仓库里 13 个脚本用了它。
   在 Windows 上必须【显式跳过并大声说明】，不能悄悄当成通过。

用法：
    from tests.hbuild import exe_path, sanitize_flags, gcc

    out = exe_path(tmpdir, "probe")            # Windows 上自动变成 probe.exe
    cmd = [gcc(), "-O1", *sanitize_flags(), "-o", out, src]
"""
import os
import shutil
import subprocess
import sys

IS_WINDOWS = (sys.platform == "win32") or os.name == "nt"


def gcc():
    """主机侧测试用的编译器名。

    MSYS2/MINGW64 和 Linux/macOS 上都是 `gcc`。留成函数是为了以后要换
    （比如 Windows 上想用 clang）时只改这一处。
    """
    return os.environ.get("TERMUX_TEST_CC") or "gcc"


def exe_path(directory, base):
    """返回可执行产物的完整路径，Windows 上补 .exe。

    ★ 必须用这个，不要自己拼 os.path.join(td, "probe")。
      base 已经带扩展名（如 "h.bin"）时原样返回 —— MinGW 只在【无扩展名】
      时才补 .exe，两边行为要对齐。
    """
    if IS_WINDOWS and not os.path.splitext(base)[1]:
        base = base + ".exe"
    return os.path.join(str(directory), base)


_SANITIZER_CACHE = {}


def sanitize_flags(kind="address,undefined"):
    """返回可用的 sanitizer 编译参数；编译器不支持时返回 []。

    ★ 不支持时返回空列表 = 这些脚本会退化成【无 sanitizer 的普通构建】，
      检查项本身还在跑（断言还是那些断言），只是少了内存错误的探测能力。
      调用方应当同时调用 sanitize_report() 把这件事【大声说出来】，
      否则就是「悄悄降级」——比失败更危险。
    """
    if kind in _SANITIZER_CACHE:
        return _SANITIZER_CACHE[kind]
    flags = ["-fsanitize=%s" % kind]
    ok = _probe(flags)
    _SANITIZER_CACHE[kind] = flags if ok else []
    if not ok:
        # ★ 探测失败就在这里【大声说一次】，不用每个调用方各写一遍
        #   sanitize_report()。降级本身是合理的（断言还在跑），但必须可见 ——
        #   悄悄降级比失败更危险。
        sys.stderr.write(
            "\n[SKIP-SANITIZER] %s 不支持 -fsanitize=%s（MinGW/Windows 就是这种情况）。\n"
            "                 退化为普通构建继续跑，断言不变，但【探测不到内存错误】。\n"
            % (gcc(), kind))
        sys.stderr.flush()
    return _SANITIZER_CACHE[kind]


def sanitize_report(kind="address,undefined", stream=None):
    """如果 sanitizer 不可用，往 stderr 打一条醒目的说明。返回是否可用。"""
    # 说明文案已经由 sanitize_flags() 在探测失败时打过一次，这里不再重复。
    return bool(sanitize_flags(kind))


def _probe(flags):
    """真编一个最小程序试一下，别猜编译器支不支持。"""
    import tempfile
    td = tempfile.mkdtemp(prefix="hbuild_probe_")
    src = os.path.join(td, "p.c")
    out = exe_path(td, "p")
    try:
        with open(src, "w") as f:
            f.write("int main(void){return 0;}\n")
        r = subprocess.run([gcc(), "-O1"] + flags + ["-o", out, src],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode == 0 and os.path.exists(out)
    except (OSError, ValueError):
        return False
    finally:
        shutil.rmtree(td, ignore_errors=True)


def have(cmd):
    """命令在 PATH 上吗。用来在跑 .sh 之前先确认 bash/sh 存在。"""
    return shutil.which(cmd) is not None
