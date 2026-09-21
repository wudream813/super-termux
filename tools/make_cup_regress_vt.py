#!/usr/bin/env python3
"""生成一个「把 bug #11 的 CUP 置 1 规则加回去」的 vt.c 变体，用于自检
tests/verify_cascade.sh 里的探测器是否真的有效（对已知坏代码必须报 FAIL）。

2026-09-17（bug #18）更新：那条规则依赖的 ScreenBuffer.lf_row 字段已被删除
（它当时已无读者），所以这里改用 vt.c 内部的文件级 static 变量复刻同一套跟踪，
自检语义不变：CUP 落在「刚被 LF 推到过的那一行或其上一行」的末列时，把该行
误标成软换行续行。

本文件刻意不使用任何反斜杠字面量 —— 换行一律用 chr(10) 构造，避免经过
JSON/heredoc 传参时被翻倍（这个坑在 2026-09-17 那天踩了四次）。

用法:  python3 tools/make_cup_regress_vt.py <src/vt.c> <输出路径>
"""
import sys

NL = chr(10)

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(2)

src = open(sys.argv[1], encoding="utf-8").read()

# ---- 1) 文件级 static 跟踪变量 -------------------------------------------
inc = '#include "vt.h"' + NL
assert src.count(inc) == 1, "找不到 #include vt.h"
decl = (inc +
        "/* 自检专用：复刻 bug #11 的「LF 落点行」跟踪（原 ScreenBuffer.lf_row 已删）。 */" + NL +
        "static int regress_lf_row = -1;" + NL)
out = src.replace(inc, decl, 1)

# ---- 2) screen_lf 末尾记下 LF 落点行 -------------------------------------
sig = "static void screen_lf(ScreenBuffer *s, int real_newline) {"
at = out.find(sig)
assert at > 0, "找不到 screen_lf"
end = out.find(NL + "}" + NL, at)
assert end > 0, "找不到 screen_lf 的收尾大括号"
record = ("    if (!s->repaint_active && !s->in_alt_screen) regress_lf_row = s->cursor_y;"
          + NL)
out = out[:end + 1] + record + out[end + 1:]

# ---- 3) CUP 分支里把误标规则加回去 ---------------------------------------
anchor = "            s->wraparound_pending = 0;" + NL
cup_at = out.find("case 'H': case 'f': {")
assert cup_at > 0, "找不到 CUP 分支"
i = out.find(anchor, cup_at)
assert i > 0, "找不到 CUP 分支里的 wraparound_pending 锚点"

lines = [
    "            if (regress_lf_row >= 0 &&",
    "                (s->cursor_y == regress_lf_row || s->cursor_y == regress_lf_row - 1) &&",
    "                s->cursor_x == s->cols - 1 && !s->in_alt_screen && s->line_wrap) {",
    "                int prx = screen_phys_row(s, s->cursor_y);",
    "                if (prx >= 0 && prx < s->total_lines) s->line_wrap[prx] = 1;",
    "            }",
    "            regress_lf_row = -1;",
    "",
]
inject = anchor + NL.join(lines)
out = out[:i] + inject + out[i + len(anchor):]

assert out != src
open(sys.argv[2], "w", encoding="utf-8").write(out)
print("已生成回归变体:", sys.argv[2])
