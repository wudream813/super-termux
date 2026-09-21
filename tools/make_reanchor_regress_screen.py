#!/usr/bin/env python3
"""生成一个「reanchor 补回顶部行时把 line_wrap 写死清 0」的 screen.c 变体，
用于自检 tests/verify_cascade.sh 里的 NEEDLES 判据是否真的有效
（对已知坏代码必须报 FAIL，否则那些 PASS 都是假绿）。

2026-09-17 真机症状：拖宽后 dir 记录被拆成「<DIR>」+ 一行纯空格 + 「   arena」，
用户描述为「有些地方多出了空格」。根因是 screen_repaint_reanchor() 补回快照行时
丢了续行标志。

本文件刻意不含任何反斜杠字面量（换行用 chr(10) 构造），避免经过 JSON/heredoc
传参时被翻倍。

用法:  python3 tools/make_reanchor_regress_screen.py <src/screen.c> <输出路径>
"""
import sys

NL = chr(10)

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(2)

src = open(sys.argv[1], encoding="utf-8").read()

fixed = NL.join([
    "        if (s->line_wrap)",
    "            s->line_wrap[dp] = (s->repaint_snap_wrap && y < s->repaint_snap_rows)",
    "                             ? s->repaint_snap_wrap[y] : 0;",
])
assert src.count(fixed) == 1, "找不到 reanchor 的恢复分支，src 可能已改动"

broken = "        if (s->line_wrap) s->line_wrap[dp] = 0;"
out = src.replace(fixed, broken)
assert out != src

open(sys.argv[2], "w", encoding="utf-8").write(out)
print("已生成 reanchor 回归变体:", sys.argv[2])
