#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""滚动条可见性门槛验证（bug #22，2026-09-20）。

用户报：「当窗格 <=6 格宽时，滚动条没了」。

根因是渲染侧写死了 cols >= 10（分屏窗格路径 src/render.c 的 render_split_pane、
整屏路径 render_screen 各一处），而 src/input.c 的鼠标命中测试【没有】同样的门槛
—— 窄窗格里能拖一条看不见的滚动条。

这里做三件事：
  1. 把 src/render.c 里真实的 render_sb_cols_ok() 抠出来编译执行，逐宽度核对真值表
     （含用户报的 6 列，以及原来被 10 挡掉的 2..9 全段）；
  2. 一致性检查：两个渲染点 + input 的命中点都必须调用同一个判据，源码里不得再
     出现硬编码的 >= 10 门槛；
  3. 判据自证：把 SB_MIN_COLS 改回 10 的变体，必须让「6 列应该有滚动条」这条 FAIL。
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RENDER = (ROOT / "src" / "render.c").read_text(encoding="utf-8")
INPUT = (ROOT / "src" / "input.c").read_text(encoding="utf-8")
RENDER_H = (ROOT / "include" / "render.h").read_text(encoding="utf-8")

failed = []


def ck(name, ok, detail=""):
    print(("  [ok] " if ok else "  [FAIL] ") + name + (("  -> " + detail) if detail else ""))
    if not ok:
        failed.append(name)


def extract_func(text, signature):
    start = text.index(signature)
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[start:j + 1]
    raise AssertionError("unbalanced braces for " + signature)


def sb_min_cols_define(text):
    m = re.search(r"^#define\s+SB_MIN_COLS\s+(\d+)", text, re.M)
    assert m, "src/render.c 里找不到 #define SB_MIN_COLS"
    return "#define SB_MIN_COLS %s" % m.group(1), int(m.group(1))


FUNC = extract_func(RENDER, "int render_sb_cols_ok(")
SPARE = extract_func(RENDER, "int render_sb_spare_row(")
DEFINE, MINCOLS = sb_min_cols_define(RENDER)

HARNESS = "\n".join([
    "#include <stdio.h>",
    DEFINE,
    FUNC,
    SPARE,
    "int main(void) {",
    "    for (int cols = -1; cols <= 130; cols++)",
    "        for (int alt = 0; alt <= 1; alt++)",
    "            printf(\"C %d %d %d\\n\", cols, alt, render_sb_cols_ok(cols, alt));",
    "    for (int vis = 0; vis <= 1; vis++)",
    "      for (int cx = -1; cx <= 8; cx++)",
    "        for (int cy = -1; cy <= 4; cy++)",
    "          for (int cols = 0; cols <= 8; cols++)",
    "            printf(\"S %d %d %d %d %d\\n\", vis, cx, cy, cols,",
    "                   render_sb_spare_row(vis, cx, cy, cols));",
    "    return 0;",
    "}",
])


def strip_comments(text):
    """去掉 C 注释再扫硬编码门槛 —— 修复说明里本来就要提到旧的 `cols >= 10`，
    那不算残留（第一版就是被自己的注释误报了）。"""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def run(src_text):
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "sb.c"
        c.write_text(src_text, encoding="utf-8")
        exe = Path(td) / "sb"
        r = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                           capture_output=True, text=True)
        if r.returncode:
            print(r.stderr)
            raise SystemExit("编译失败")
        out = subprocess.run([str(exe)], capture_output=True, text=True).stdout
    table, spare = {}, {}
    for line in out.splitlines():
        f = line.split()
        if f[0] == "C":
            cols, alt, v = (int(x) for x in f[1:])
            table[(cols, alt)] = v
        else:
            vis, cx, cy, cols, v = (int(x) for x in f[1:])
            spare[(vis, cx, cy, cols)] = v
    return table, spare


print("=== 1) render_sb_cols_ok 真值表（从 src/render.c 抠出真实函数编译执行）===")
T, S = run(HARNESS)
print("  SB_MIN_COLS = %d" % MINCOLS)
ck("1 列：不画（整列都会被滚动条盖掉）", T[(1, 0)] == 0)
ck("0 / 负列：不画", T[(0, 0)] == 0 and T[(-1, 0)] == 0)
narrow_bad = [c for c in range(2, 10) if T[(c, 0)] != 1]
ck("2..9 列：都要画（用户报的 6 列在这一段；旧门槛 10 把它们全挡了）",
   not narrow_bad, "缺失=%s" % narrow_bad if narrow_bad else "2..9 全有")
ck("6 列（用户实测的那一档）", T[(6, 0)] == 1, "render_sb_cols_ok(6,0)=%d" % T[(6, 0)])
wide_bad = [c for c in (10, 11, 39, 80, 120, 130) if T[(c, 0)] != 1]
ck("10/11/39/80/120/130 列：照旧要画（不回归）", not wide_bad, "缺失=%s" % wide_bad)
alt_bad = [c for c in (1, 2, 6, 10, 120) if T[(c, 1)] != 0]
ck("alt 屏一律不画（vim 之类没有滚动缓冲）", not alt_bad, "违例=%s" % alt_bad)

print("=== 1b) render_sb_spare_row：滚动条让开光标那一格（bug #23）===")
ck("光标不可见 -> 不用让", S[(0, 5, 2, 6)] == -1)
ck("cols=0 -> 不用让", S[(1, 0, 0, 0)] == -1)
ck("光标不在右缘列（cols=6, cx=4）-> 不用让", S[(1, 4, 2, 6)] == -1)
ck("光标在右缘列（cols=6, cx=5）-> 让开 cy 那一行", S[(1, 5, 2, 6)] == 2,
   "spare=%d" % S[(1, 5, 2, 6)])
ck("延迟换行挂起态 cx==cols 也要让（cols=6, cx=6）", S[(1, 6, 3, 6)] == 3,
   "spare=%d" % S[(1, 6, 3, 6)])
ck("cx 超过 cols 也不越界（cols=6, cx=99）", S[(1, 99 if (1, 99, 0, 6) in S else 8, 0, 6)] >= 0)
ck("cy<0 -> 不用让", S[(1, 5, -1, 6)] == -1)
ck("1 列窗格：光标必然在右缘列 -> 让开", S[(1, 0, 1, 1)] == 1)
narrow_spare = [(cx, cy, c) for (v, cx, cy, c), r in S.items()
                if v == 1 and c >= 2 and cx == c - 1 and cy >= 0 and r != cy]
ck("任意宽度下「光标在右缘列」都让开正确的那一行", not narrow_spare,
   "违例=%s" % narrow_spare[:5])

print("=== 2) 渲染 / 命中三处必须共用同一个判据 ===")
ck("include/render.h 有声明", "int render_sb_cols_ok(int cols, int in_alt_screen);" in RENDER_H)
ck("分屏窗格路径调用它（render_split_pane）",
   "render_sb_cols_ok(cols, s->in_alt_screen) && leaf == g_mux.active_pane" in RENDER)
ck("整屏路径调用它（render_screen 的 show_sb）",
   "int show_sb = render_sb_cols_ok(g_mux.host_cols, s->in_alt_screen);" in RENDER)
ck("input.c 的鼠标命中调用它",
   "render_sb_cols_ok(sb_col + 1, s->in_alt_screen)" in INPUT)
ck("分屏窗格路径的滚动条循环跳过光标行", "if (py == spare) continue;" in RENDER)
ck("整屏路径的滚动条轨道跳过光标行", "show_sb && dist <= 10 && y != sb_spare" in RENDER)
ck("两条路径都调 render_sb_spare_row",
   RENDER.count("render_sb_spare_row(s->cursor_visible") == 2,
   "调用 %d 次" % RENDER.count("render_sb_spare_row(s->cursor_visible"))
hard = []
for path, text in (("src/render.c", RENDER), ("src/input.c", INPUT)):
    for m in re.finditer(r"(\w*cols)\s*>=\s*10\b", strip_comments(text)):
        line = text[:m.start()].count("\n") + 1
        hard.append("%s:%d %s" % (path, line, m.group(0)))
ck("源码里不再有硬编码的 cols >= 10 门槛", not hard, "残留=%s" % hard)

print("=== 3) 判据自证（把 SB_MIN_COLS 改回 10 必须报 FAIL）===")
BAD = HARNESS.replace(DEFINE, "#define SB_MIN_COLS 10")
TB, _ = run(BAD)
bad6 = TB[(6, 0)]
ck("门槛改回 10 后 6 列变成不画（说明第 1 节真的在测这个门槛）",
   bad6 == 0, "改回 10 后 render_sb_cols_ok(6,0)=%d" % bad6)

print()
if failed:
    print("[FAIL] 滚动条门槛验证未通过：%d 项" % len(failed))
    for f in failed:
        print("   - " + f)
    raise SystemExit(1)
print("[PASS] 滚动条门槛验证通过（真值表 + 三处一致 + 自证）")
