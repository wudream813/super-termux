#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「去掉 bug #18 修复」的坏 vt.c，供 tests/verify_cascade.sh 第 7 段自检用。

用法:  make_eolcont_regress_vt.py <原 vt.c> <输出 vt.c>

改动只有一处：把 screen_lf 滚动分支里「滚出来的新底行标成软换行续行」那个
if 块的条件写死成 0（等价于删掉这条规则），其余逐字不动。
自检的意义：如果去掉修复后判据仍然 PASS，说明判据是假的。
"""
import io
import sys

OLD = "        if (eol_cont && !s->in_alt_screen && s->line_wrap && s->cursor_y > 0 &&"
NEW = "        if (0 && eol_cont && !s->in_alt_screen && s->line_wrap && s->cursor_y > 0 &&"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    t = io.open(src, encoding='utf-8').read()
    if t.count(OLD) != 1:
        print('找不到待改的条件行（命中 %d 次）' % t.count(OLD), file=sys.stderr)
        return 1
    io.open(dst, 'w', encoding='utf-8').write(t.replace(OLD, NEW))
    return 0


if __name__ == '__main__':
    sys.exit(main())
