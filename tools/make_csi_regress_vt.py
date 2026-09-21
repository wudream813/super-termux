#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「把 CSI 一律作废 cup_eol_row」的坏 vt.c，供 tests/verify_cascade.sh 自检用。

用法:  make_csi_regress_vt.py <原 vt.c> <输出 vt.c>

改动只有一处：把 execute_csi_internal 里那条「CSI ? Ps h/l 保留判定窗口」的条件
去掉，恢复成无条件 s->cup_eol_row = -1（就是 v15 的写法）。
真机字节（uploads/termux_dump.log 2026-09-17 14:10，pane0 偏移 3878）：
    ESC[29;39H ESC[?25l CR LF "            Users  "
隐藏光标那条 CSI 夹在底行末列 CUP 和 CRLF 中间，无条件作废就会让这条记录被拆开。
"""
import io
import sys

OLD = "    if (!(prefix == '?' && (final == 'h' || final == 'l'))) s->cup_eol_row = -1;"
NEW = "    s->cup_eol_row = -1;"


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
    print('已生成 CSI 回归变体:', dst)
    return 0


if __name__ == '__main__':
    sys.exit(main())
