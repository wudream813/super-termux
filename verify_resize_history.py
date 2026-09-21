#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""子终端(分屏窗格) resize 时的滚动历史记录回归。

编译 tests/resize_history_repro.c + 真实 src/screen.c / src/vt.c / src/utf8.c
（tests/stub 提供 windows.h 替身）跑 9 组不变量：

  1. resize 之后环形缓冲头 scroll_top 必须归零 —— screen_resize_reflow() 把环
     重建成【绝对布局】（可见区=物理槽 0..nr-1、历史=物理槽 nt-hist..nt-1），
     而所有读方（渲染/复制/搜索/reflow_view）都经 screen_phys_row() 的
     (scroll_top+rel)%total_lines 取行；保留旧环头会让历史与可见屏整体错位。
  2. 内容不足一屏时从顶部排（不底部锚定），否则收窄后内容整体下坠、上方垫空行。
  3. 纯增高不得往时间流【中间】插空行（否则每拖高 k 行就往历史里塞 k 个空行）。
  4. reflow 之后 cursor_y 必须跟着内容走，否则 resize 后第一行新输出会覆盖
     已有内容行。
  5. 上下分屏改高度后 reflow 的落位（提示符锚定、上方不垫空行）。
  6. resize 后 ConPTY 的整屏重绘：提示符必须重新贴底、历史不得被吃掉。
  7. 内容里有重复行时，重绘不得把内容写成重复行（用户报的「历史重复」：真机重放
     内容流 40 行变 56 行、同一文件名出现 13 次）。
  8. 重绘比窗格高（顶行落在本地历史里）时不得丢行、hist_lines 不得变小 —— 旧的
     screen_repaint_align 会在这里转【负向】偏移，把 hist 从 21 削到 14。

  9. 内容正好铺满窗格（hist==0）时，重绘不得吃掉顶部内容 —— 真机 height 4 -> 1、
     banner 两行直接消失（用户报的「历史被直接吃了」）。

第 7、8、9 组直接调用 screen_repaint_align()（与 src/pane.c:96,104 的顺序一致），所以
「历史重复」与「重绘比窗格高时吞行」这两条都在这里钉住。
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    binp = os.path.join(HERE, "tests", ".resize_history_repro.bin")
    cmd = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
           "-Itests/stub", "-Iinclude",
           "tests/resize_history_repro.c", "src/screen.c", "src/vt.c", "src/utf8.c",
           "-o", binp]
    r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    if r.returncode != 0:
        print("Compilation error:", r.stderr)
        return 1
    r = subprocess.run([binp], capture_output=True, text=True)
    print(r.stdout, end="")
    try:
        os.remove(binp)
    except OSError:
        pass
    if r.returncode != 0:
        print(r.stderr, end="")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
