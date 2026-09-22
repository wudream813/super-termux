#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""复制（HTML Format）16 色调色板映射回归 (v1.8.16)。

背景
----
复制保留颜色走 cliphtml：真彩色（``bg_rgb_on/fg_rgb_on``）直出 #rrggbb，16 色
则按 Campbell 调色板给 RGB。cell 属性字（``CHAR_INFO.Attributes``）里的 nibble
是 **Windows 控制台颜色位** 序（低 4 位前景、高 4 位背景）：红=4、蓝=1、绿=2、
黄=6、青=3、品红=5；而 cliphtml 的 Campbell 表（``cliphtml_palette16``）按
**ANSI/VT 色号** 索引（红=1、蓝=4、绿=2、黄=3、青=6）。

旧代码 ``attr_palette_rgb`` 直接把 Windows nibble 喂给 ANSI 调色板，漏掉了
render.c 往终端写 SGR 时一直在用的 ``m[] = {0,4,2,6,1,5,3,7}``（Windows→ANSI）
转换，结果复制到 Word/浏览器时 **红↔蓝、黄↔青对调**（前景、背景都错）。

本脚本从 **真实 src/input.c 抽取 ``attr_palette_rgb`` 函数**、链接 **真实
src/cliphtml.c** 编译执行，断言：

* Windows 红(fg nibble 4 / bg nibble 4) → Campbell 红 (197,15,31)；
* Windows 蓝(nibble 1) → Campbell 蓝 (0,55,218)；
* 绿(nibble 2) → 绿 (19,161,14)；黄(nibble 6) ↔ 青(nibble 3) 不互换；
* 亮色（nibble|8）映射到 Campbell 亮段。
变异：把 win_to_ansi 转换去掉（旧 bug）即断言失败。
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 无 -fsanitize）
#   统一收在 tests/hbuild.py；降级时会自己往 stderr 打 [SKIP-SANITIZER]。
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402


ROOT = Path(__file__).resolve().parent


def extract_func(text: str, sig: str) -> str:
    start = text.find(sig)
    if start < 0:
        raise RuntimeError(f"function not found: {sig}")
    brace = text.find("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise RuntimeError(f"unterminated function: {sig}")


def main():
    print("=== 复制 16 色调色板映射 (verify_copy_colors.py) ===")
    input_src = (ROOT / "src" / "input.c").read_text(encoding="utf-8")
    func = extract_func(input_src, "static void attr_palette_rgb")

    # 源码不变量：修复必须在（Windows→ANSI 转换表 + 用转换后的索引调用）。
    assert "win_to_ansi" in func, (
        "attr_palette_rgb 缺少 Windows→ANSI 转换（win_to_ansi）；"
        "16 色复制会把红/蓝、黄/青对调")

    HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "cliphtml.h"
#ifndef WORD
typedef unsigned short WORD;
#endif
static void attr_palette_rgb(WORD attr, int is_bg, int *r, int *g, int *b);

/* ---- 从真实 src/input.c 抽取的函数 ---- */
%(func)s
/* ------------------------------------------------ */

static int failures = 0;
#define CHECK(cond, ...) do { if(!(cond)){ printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while(0)

int main(void) {
    struct { const char *name; unsigned attr; int is_bg; int er, eg, eb; } t[] = {
        /* fg nibble 是低 4 位（Windows 色位） */
        { "fg 红(win4)", 0x04, 0, 197, 15, 31 },   /* Campbell 红 #c50f1f */
        { "fg 蓝(win1)", 0x01, 0,   0, 55, 218 },   /* Campbell 蓝 #0037da */
        { "fg 绿(win2)", 0x02, 0,  19,161, 14 },    /* #13a10e */
        { "fg 黄(win6)", 0x06, 0, 193,156,  0 },    /* ANSI 黄 #c19c00 */
        { "fg 青(win3)", 0x03, 0,  58,150,221 },    /* ANSI 青 #3a96dd */
        { "fg 品红(win5)",0x05,0, 136, 23,152 },
        /* bg nibble 是高 4 位：同样的颜色位 <<4 */
        { "bg 红(win4)",  0x40, 1, 197, 15, 31 },
        { "bg 蓝(win1)",  0x10, 1,   0, 55, 218 },
        { "bg 绿(win2)",  0x20, 1,  19,161, 14 },
        /* 亮色（intensity 位 = 8）：win nibble 12 = 8|4 = 亮红；9 = 8|1 = 亮蓝 */
        { "fg 亮红(win12)", 0x0C, 0, 231, 72, 86 },   /* Campbell 亮红 #e74856 */
        { "fg 亮蓝(win9)",  0x09, 0,  59,120,255 },   /* Campbell 亮蓝 #3b78ff */
        { "fg 亮绿(win10)", 0x0A, 0,  22,198, 12 },   /* #16c60c */
        { "fg 亮白(win15)", 0x0F, 0, 242,242,242 },   /* #f2f2f2 */
        { "bg 亮红(win12)", 0xC0, 1, 231, 72, 86 },
        { "bg 亮蓝(win9)",  0x90, 1,  59,120,255 },
    };
    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        int r = -1, g = -1, b = -1;
        attr_palette_rgb((WORD)t[i].attr, t[i].is_bg, &r, &g, &b);
        CHECK(r == t[i].er && g == t[i].eg && b == t[i].eb,
              "%s: 得 (%d,%d,%d) 期望 (%d,%d,%d)（旧 bug 红蓝对调）",
              t[i].name, r, g, b, t[i].er, t[i].eg, t[i].eb);
    }

    /* 红≠蓝 的关键不变量：fg 红 attr 与 fg 蓝 attr 绝不能给出相同 RGB。 */
    int rr,rg,rb,br2,bg2,bb2;
    attr_palette_rgb(0x04, 0, &rr,&rg,&rb);   /* win 红 */
    attr_palette_rgb(0x01, 0, &br2,&bg2,&bb2);/* win 蓝 */
    CHECK(!(rr==br2 && rg==bg2 && rb==bb2), "红与蓝复制色不得相同");
    CHECK(rr > br2, "红通道：红样的 R 应大于蓝样的 R（旧 bug 会反过来）");

    if (failures) { printf("[FAIL] %d 处失败\n", failures); return 1; }
    printf("[OK] attr_palette_rgb Windows 色位 → Campbell RGB 映射正确（红/蓝/黄/青不再对调）。\n");
    return 0;
}
""".replace("%(func)s", func)

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "harness.c").write_text(HARNESS, encoding="utf-8")
        exe = td / "t"
        cmd = ["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
               "-Wall", "-Wextra", "-Werror",
               "-I", str(ROOT / "include"),
               str(ROOT / "src" / "cliphtml.c"),
               str(td / "harness.c"), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            sys.exit("FAIL: compile error")
        r = subprocess.run([str(exe)], capture_output=True, text=True,
                           env={"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"})
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
