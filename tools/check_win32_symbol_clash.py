#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_win32_symbol_clash.py —— 抓「测试桩和 libkernel32.a 撞名」这类链接错。

为什么不能靠链接来发现
----------------------
`tests/test_conpty_loader.c` 定义了 LoadLibraryW / GetProcAddress 等 8 个函数
来拦截 `src/conpty_loader.c` 的调用。Linux 上没有 libkernel32，一直没事。
但在真 Windows / MSYS2 上，libkernel32.a 里这 8 个符号【全都已经有定义】，
链接期直接：

    multiple definition of `GetProcAddress';
    libkernel32.a(libkernel32s00735.o): first defined here

★ 本地交叉编译器【复现不了】：Debian 的 mingw-w64 默认库集合和 MSYS2 不同，
  不会那么早把 libkernel32.a 的目标文件拉进来，`-o x.exe` 照样成功。
  所以不能靠「链一遍看看过不过」来判断，必须做【符号级】比对。

做法：把要检查的 .o 里定义的全局符号，和 libkernel32.a 里定义的全局符号求交集。
有交集 => 在真 Windows 上必然 multiple definition。

用法：
    python3 tools/check_win32_symbol_clash.py a.o b.o ...
退出码 0 = 没有冲突；1 = 有冲突（并打印是哪几个符号）。
"""
import os
import re
import subprocess
import sys

NM = os.environ.get("TERMUX_WIN_NM") or "x86_64-w64-mingw32-nm"
CC = os.environ.get("TERMUX_WIN_CC") or "x86_64-w64-mingw32-gcc"

# 只关心这几个导入库里的符号。kernel32 是必撞的那个；其余几个顺手一起查，
# 因为 tests/stub/windows.h 里还桩了 ShellExecuteW（shell32）之类的名字。
LIBS = ("libkernel32.a", "libuser32.a", "libshell32.a")


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def defined_symbols(obj):
    """目标文件里【定义】的全局符号（T/D/B/R，不含 U 未定义引用）。"""
    rc, out = run([NM, "-g", "--defined-only", obj])
    if rc != 0:
        return None
    names = set()
    for ln in out.splitlines():
        parts = ln.split()
        if len(parts) >= 3 and parts[1] in ("T", "D", "B", "R"):
            names.add(parts[2])
    return names


def lib_symbols():
    """所有相关导入库里定义的全局符号，以及它们来自哪个库。"""
    table = {}
    for lib in LIBS:
        rc, path = run([CC, "-print-file-name=" + lib])
        path = path.strip()
        if rc != 0 or not path or not os.path.exists(path) or path == lib:
            continue                     # 这个工具链里没有该库，跳过
        rc, out = run([NM, "-g", "--defined-only", path])
        if rc != 0:
            continue
        for ln in out.splitlines():
            parts = ln.split()
            if len(parts) >= 3 and parts[1] in ("T", "D", "B", "R"):
                table.setdefault(parts[2], lib)
    return table


def main(argv):
    objs = argv[1:]
    if not objs:
        sys.stderr.write("用法: check_win32_symbol_clash.py <a.o> [b.o ...]\n")
        return 2

    rc, _ = run([NM, "--version"])
    if rc != 0:
        sys.stderr.write("[SKIP] 找不到 %s，无法做符号级检查（装上 mingw-w64 即可）。\n"
                         "       注意：不做这个检查的话，撞名只会在 Windows 作业\n"
                         "       链接期暴露，白白多等 3 分钟。\n" % NM)
        return 0

    libs = lib_symbols()
    if not libs:
        sys.stderr.write("[SKIP] 没找到任何 Win32 导入库，跳过。\n")
        return 0

    total = 0
    for obj in objs:
        if not os.path.exists(obj):
            sys.stderr.write("[FAIL] 找不到目标文件 %s\n" % obj)
            total += 1
            continue
        mine = defined_symbols(obj)
        if mine is None:
            sys.stderr.write("[FAIL] nm 读不了 %s\n" % obj)
            total += 1
            continue
        clash = sorted(s for s in mine if s in libs)
        if clash:
            total += len(clash)
            print("  [FAIL] %s 定义了 %d 个和 Win32 导入库重名的符号：" % (obj, len(clash)))
            for s in clash:
                print("         %-24s 也定义在 %s" % (s, libs[s]))
            print("         → 在真 Windows 上链接必然 multiple definition。")
            print("           修法：在 tests/loaderstub/windows.h（或 tests/stub/windows.h）")
            print("           里 #define 重映射成 termux_stub_* 之类的私有名字。")
        else:
            print("  [ok]   %s 没有和 Win32 导入库重名的符号" % obj)

    print()
    if total:
        print("%d 个符号冲突" % total)
        return 1
    print("符号级检查通过：%d 个目标文件都没有和 Win32 导入库撞名" % len(objs))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
