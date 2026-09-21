#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""搜索跟随新输出重算（bug #24，2026-09-20 用户要求「搜索同时，如果终端输出新数据也要 find」）。

原来 run_search() 只在按键/鼠标里被调（搜索框打字、回车、点搜索框），终端来新数据
时【不会】重找 —— 只有 screen_scroll_up 会把已有匹配的 abs_y 跟着滚动挪一下
（src/screen.c），新打印出来的内容里的关键词永远找不到。

现在：pane 读线程收到新输出后 search_mark_dirty()，主循环在 render_screen() 之前
调 search_refresh_live() 重扫一次（每帧最多一次，不是每个 ReadFile 分块一次）。
重扫会重建整张匹配表，所以要用 search_relocate_cur() 把用户正停留的那条找回来
（用户选择：保持停在原来那条，浏览位置不跳）。

这里把 src/input.c 里真实的 search_relocate_cur() 抠出来编译执行，逐例核对；
再核对四处接线；最后自证（把「按坐标找回」换成「要求行号精确相等」必须 FAIL）。
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
INPUT = (ROOT / "src" / "input.c").read_text(encoding="utf-8")
PANE = (ROOT / "src" / "pane.c").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.c").read_text(encoding="utf-8")
# g_* 全局与 dump/log 辅助函数已从 main.c 抽到 globals.c（Windows / POSIX 两个
# main 共用），所以定义类断言要看 globals.c；接线类断言仍然看 main.c。
GLOBALS = (ROOT / "src" / "globals.c").read_text(encoding="utf-8")
TYPES_H = (ROOT / "include" / "types.h").read_text(encoding="utf-8")
INPUT_H = (ROOT / "include" / "input.h").read_text(encoding="utf-8")

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


FUNC = extract_func(INPUT, "int search_relocate_cur(")

PRELUDE = """
#include <stdio.h>
#include <stddef.h>
typedef struct { int abs_y; int start_x; int end_x; } SearchMatch;
"""

# 每个用例：匹配表 + (want_abs_y, want_x) + 期望 index
HARNESS = PRELUDE + FUNC + r"""
static int T(const SearchMatch *ms, int n, int wy, int wx, int want, const char *what) {
    int got = search_relocate_cur(ms, n, wy, wx);
    printf("%s|%d|%d\n", what, got, want);
    return got == want;
}

int main(void) {
    int bad = 0;

    /* 1) 空表 / NULL */
    bad += !T(NULL, 0, 10, 3, -1, "null");
    { SearchMatch a[1] = {{5, 3, 5}}; bad += !T(a, 0, 10, 3, -1, "n=0"); }

    /* 2) 坐标一模一样：认得出来 */
    { SearchMatch a[3] = {{5, 3, 5}, {10, 3, 5}, {40, 8, 12}};
      bad += !T(a, 3, 10, 3, 1, "exact"); }

    /* 3) 终端持续输出：老行被挤出滚动缓冲，同一条匹配的 abs_y 变小。
     *    原来停在 abs_y=100 列 3，重扫后它跑到 abs_y=97 列 3 —— 必须认出来。 */
    { SearchMatch a[3] = {{20, 0, 2}, {97, 3, 5}, {150, 3, 5}};
      bad += !T(a, 3, 100, 3, 1, "shifted-up"); }

    /* 4) 同列有多个候选：取行号最接近的那个 */
    { SearchMatch a[3] = {{97, 3, 5}, {102, 3, 5}, {200, 3, 5}};
      bad += !T(a, 3, 100, 3, 1, "nearest"); }

    /* 5) 没有同列的（那条已被挤出去）：返回 -1，让调用方保持 run_search 给的 0 */
    { SearchMatch a[2] = {{97, 0, 2}, {150, 9, 12}};
      bad += !T(a, 2, 100, 3, -1, "no-same-col"); }

    /* 6) want_x = -1（重扫前没有停留项的列信息）：退化成只按行号找最近 */
    { SearchMatch a[3] = {{10, 0, 2}, {99, 7, 9}, {300, 1, 3}};
      bad += !T(a, 3, 100, -1, 1, "no-col-info"); }

    /* 7) 只有一条匹配：无论坐标差多远都认它（总比跳到别处好） */
    { SearchMatch a[1] = {{0, 5, 8}};
      bad += !T(a, 1, 900, 5, 0, "single"); }

    printf("BAD=%d\n", bad);
    return bad != 0;
}
"""

# 自证用的坏变体：要求 abs_y 精确相等（最常见的错写法）。内容一被推走就认不出来。
BAD_HARNESS = HARNESS.replace(
    "        int d = ms[m].abs_y - want_abs_y;\n        if (d < 0) d = -d;",
    "        int d = (ms[m].abs_y == want_abs_y) ? 0 : 1000000;")


def run(src_text):
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "sr.c"
        c.write_text(src_text, encoding="utf-8")
        exe = Path(td) / "sr"
        r = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                           capture_output=True, text=True)
        if r.returncode:
            print(r.stderr)
            raise SystemExit("编译失败")
        p = subprocess.run([str(exe)], capture_output=True, text=True)
    return p.stdout, p.returncode


print("=== 1) search_relocate_cur（从 src/input.c 抠出真实函数编译执行）===")
if "int d = ms[m].abs_y - want_abs_y;" not in FUNC:
    print("  [FAIL] 自证变体的替换锚点没找到，下面的自证不可信")
    failed.append("selfcheck-anchor")
out, rc = run(HARNESS)
for line in out.splitlines():
    if line.startswith("BAD="):
        continue
    what, got, want = line.split("|")
    ck("用例 %s：期望 %s" % (what, want), got == want, "实际 %s" % got)
ck("C 侧全部用例通过", rc == 0, "returncode=%d" % rc)

print("=== 2) 四处接线 ===")
ck("types.h 声明 g_search_dirty", "extern int g_search_dirty;" in TYPES_H)
ck("globals.c 定义 g_search_dirty", "int g_search_dirty = 0;" in GLOBALS)
ck("main.c 不再重复定义 g_search_dirty（否则链接期重定义）",
   "int g_search_dirty = 0;" not in MAIN)
ck("input.h 声明 search_refresh_live / search_mark_dirty / search_relocate_cur",
   all(x in INPUT_H for x in ("void search_refresh_live(void);",
                              "void search_mark_dirty(void);",
                              "int search_relocate_cur(const SearchMatch *ms, int n, int want_abs_y, int want_x);")))
ck("主循环在 render_screen() 之前调 search_refresh_live()",
   "search_refresh_live();\n                render_screen();" in MAIN)
n_mark = PANE.count("if (idx == g_mux.active_pane) search_mark_dirty();")
ck("pane 读线程两条读路径都置脏标记", n_mark == 2, "出现 %d 次" % n_mark)
ck("search_refresh_live 会消费脏标记并找回停留项",
   "if (!g_search_dirty) return;" in INPUT and
   "search_relocate_cur(g_search_matches, g_search_match_count, want_abs_y, want_x)" in INPUT)
ck("search_mark_dirty 只在搜索真的开着时置位",
   "if (!g_search_mode && !g_search_active) return;" in INPUT)
ck("重扫用 live 模式（不滚动、不动用户视图）",
   "run_search(1);                       /* live：只高亮，不滚动、不动视图 */" in INPUT)

print("=== 3) 判据自证（把「行号最接近」换成「行号精确相等」必须 FAIL）===")
bout, brc = run(BAD_HARNESS)


def wrong_cases(out):
    return [l.split("|")[0] for l in out.splitlines()
            if "|" in l and not l.startswith("BAD=") and l.split("|")[1] != l.split("|")[2]]


bw = wrong_cases(bout)
print("      坏变体答错的用例: %s" % (bw or "（无）"))
# 「nearest」才是区分「行号最接近」与「同列里挑第一个」的那一例：
# 三个同列候选 97 / 102 / 200，原来停在 100 —— 精确相等的写法会退化成挑第一个（97）。
ck("坏变体在 nearest 这一例上答错", "nearest" in bw)
ck("坏变体至少答错一例", bool(bw))
ck("坏变体整体 returncode 非 0", brc != 0, "returncode=%d" % brc)

print()
if failed:
    print("[FAIL] 搜索跟随新输出验证未通过：%d 项" % len(failed))
    for f in failed:
        print("   - " + f)
    raise SystemExit(1)
print("[PASS] 搜索跟随新输出验证通过（真值表 + 四处接线 + 自证）")
