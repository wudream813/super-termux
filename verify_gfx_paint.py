#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_gfx_paint.py —— 图形直通 / 分屏渲染 / 滚动条门槛的渲染层判据。

跑的是【真】render.c（用 tests/stub 的 Win32 替身，不需要真终端），断言：

  1. 转发图片的那一帧，末尾必须是 shell 光标段（帧体 → 图片 → 光标）。图片用的是
     绝对 CUP 且发完不归位，顺序一颠倒宿主光标就留在图片锚点上。
  2. 渲染每帧记的 visible_now + render_pane_should_wake()：分屏里非活动但可见的
     窗格要能点重绘，zoom 盖住的 / 别的标签页的不点。
  3. 分屏逐格渲染时 emoji 的次格（低代理）不得被写空格（否则右半格被擦掉）。
  4. 单窗格没有可回看的历史时不画滚动条（否则整列被滑块盖住、还拖不动）。

判据写死在 tests/gfx_paint_harness.c 里；这里只负责按同一份源文件清单编译并跑。
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = HERE
sys.path.insert(0, os.path.join(HERE, "tests"))
import hbuild  # noqa: E402

SRC = ["render", "split", "framediff", "theme", "screen", "vt", "utf8",
       "keymap", "input", "config", "cliphtml"]


def main() -> int:
    # 判据文案是中文：Windows 控制台默认 cp936，遇到编码不了的字符会直接把脚本搞挂
    # （CI 里靠 PYTHONUTF8 顶着，本地不一定有）。这里显式放宽，别让「输出不了」伪装成
    # 「判据不过」。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    print("=== 图形直通 / 分屏重绘 (verify_gfx_paint.py) ===")
    with tempfile.TemporaryDirectory() as td:
        exe = hbuild.exe_path(td, "gfx_paint")
        cmd = [hbuild.gcc(), "-O1", "-Wall", "-Wextra", "-Werror",
               "-Itests/stub", "-Iinclude"]
        cmd += [os.path.join("src", f + ".c") for f in SRC]
        cmd += ["tests/render_harness_shims.c", "tests/gfx_paint_harness.c",
                "-o", exe]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr, file=sys.stderr)
            print("[FAIL] 编译失败")
            return 1
        r = subprocess.run([exe], cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        print(r.stdout.rstrip())
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            print("[FAIL] 渲染层判据未通过")
            return 1
    print("[OK] verify_gfx_paint 通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
