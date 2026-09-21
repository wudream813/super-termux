#!/usr/bin/env python3
"""生成「两趟 resize 重绘」修复被撤回后的 src/vt.c 变体，用于 verify_cascade.sh
第 9 段的自检：把修复改回去以后，v6 的末态判据【必须】报 FAIL，否则说明判据无效、
上面的 PASS 不可信。

撤回的就是 2026-09-20 那两处：
  1. ESC[?25h 处不再看 screen_repaint_reanchor 的返回值，无条件清 resize_repaint_pending
     —— 于是 conhost 两趟重绘的第一趟（ESC[<rows>;1H 收尾、tail==0）就把标记吃掉；
  2. ESC[H 处恢复成「只要 pending 就重新取快照」—— 第二趟于是存到一份被第一趟
     污染过的快照。

用法: python3 tools/make_repass_regress_vt.py <src/vt.c> <输出.c>
生成器刻意零反斜杠。
"""
import sys

NEW_H = """                            if (at_bottom && pending) {
                                /* 返回 1 = 这趟重绘写到了最后一行、没东西可锚定，
                                 * 是 conhost 两趟重绘的第一趟：pending 和快照都留给
                                 * 下一趟（真机 2026-09-20：第一趟 ESC[29;1H ESC[?25h
                                 * 收尾，第二趟才是 ESC[5;26H + 24 行 ESC[K）。
                                 * 让过一趟就封顶，免得 pending 长期挂着。 */
                                if (screen_repaint_reanchor(s) == 1 &&
                                    ++s->resize_repaint_pass < 2) {
                                    /* 保留 resize_repaint_pending 与 repaint_snap */
                                } else {
                                    s->resize_repaint_pending = 0;
                                    screen_repaint_snapshot_free(s);
                                }
                            } else {
                                s->resize_repaint_pending = 0;
                                screen_repaint_snapshot_free(s);
                            }"""

OLD_H = """                            s->resize_repaint_pending = 0;
                            if (at_bottom && pending) screen_repaint_reanchor(s);
                            else screen_repaint_snapshot_free(s);"""

NEW_S = "                if (s->resize_repaint_pending && !s->repaint_snap) screen_repaint_snapshot(s);"
OLD_S = "                if (s->resize_repaint_pending) screen_repaint_snapshot(s);"


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = open(src, encoding="utf-8").read()
    for new, old in ((NEW_H, OLD_H), (NEW_S, OLD_S)):
        if s.count(new) != 1:
            print("找不到待撤回的片段（出现 %d 次）：%s" % (s.count(new), new[:60]))
            return 1
        s = s.replace(new, old)
    open(dst, "w", encoding="utf-8").write(s)
    print("已生成两趟重绘回归变体: %s" % dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
