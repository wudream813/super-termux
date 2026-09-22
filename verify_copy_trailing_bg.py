#!/usr/bin/env python3
"""verify_copy_trailing_bg.py

v1.8.17 回归：复制成 HTML 时，行尾【带背景色的空格】不能被当行尾空白裁掉。

colortool -c 的每个色块都是 "  gYw  "——末尾两个空格同样带着背景色（底色块）。
旧代码用"最后一个非空格 cell"作为 HTML 行右边界，把最后一个色块的彩色尾随
空格丢了，粘到 Word/浏览器里最后一列色块右半底色缺失。

本脚本从【真实 src/input.c】抽取 cliphtml_row_right_boundary() 函数体，编译
执行，断言：
  1. 带 bg_valid 的尾随空格被保留（右边界延伸到这些空格）；
  2. 透明（无底色）尾随空格仍被裁掉；
  3. 带 fg_valid 的彩色空格也保留；
  4. 空行 / 全空格行返回 x_start-1（与旧 valid_x1 语义一致）。
变异验证：若把"带颜色空格保留"改回"遇空格即停"，用例 1/2 会 FAIL。
"""
import os
import re
import subprocess
import sys
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 无 -fsanitize）
#   统一收在 tests/hbuild.py；降级时会自己往 stderr 打 [SKIP-SANITIZER]。
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402

import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
INPUT_C = os.path.join(ROOT, "src", "input.c")

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "cliphtml.h"

%(func)s

static int failures = 0;
static void check(const char *name, int got, int want) {
    if (got != want) { printf("[FAIL] %s: got %d want %d\n", name, got, want); failures++; }
    else             { printf("[ok]   %s: %d\n", name, got); }
}

/* 造一行 cells：n 个格，按 spec 填。spec 字符串逐字符描述：
   '.' 透明空格(无fg无bg)  'g' 非空格字符(无颜色)  'B' 带背景色空格  'F' 带前景色空格 */
static void build(ClipHtmlCell *c, const char *spec) {
    int n = (int)strlen(spec);
    for (int i = 0; i < n; i++) {
        memset(&c[i], 0, sizeof(c[i]));
        char s = spec[i];
        c[i].ch = (s == 'g') ? (unsigned short)'g' : (unsigned short)' ';
        if (s == 'B') { c[i].bg_valid = 1; c[i].br = 0xc5; c[i].bg = 0x0f; c[i].bb = 0x1f; }
        if (s == 'F') { c[i].fg_valid = 1; c[i].r = 0x13; c[i].g = 0xa1; c[i].b = 0x0e; }
    }
}
/* 最后一个非空格 cell 的下标（模拟旧 valid_x1 的最终值） */
static int last_nonspace(const ClipHtmlCell *c, int n) {
    int v = -1;
    for (int i = 0; i < n; i++) if (c[i].ch != 0 && c[i].ch != (unsigned short)' ') v = i;
    return v;
}

int main(void) {
    ClipHtmlCell c[64];

    /* 1) colortool 色块行：'  gYw  ' 最后一个色块的尾随背景空格必须保留。
          真实结构：非空格 g 之后紧跟带底色空格 B（块内补白），再往后才是透明空格。 */
    build(c, "..ggBBBB..........");
    {
        int vns = last_nonspace(c, 18);
        int r = cliphtml_row_right_boundary(c, 0, vns, 17);
        check("1 色块尾随背景空格保留(应到最后一个B=7)", r, 7);
    }

    /* 2) 非空格之后紧跟透明空格：裁到非空格处 */
    build(c, "ggg......");
    {
        int vns = last_nonspace(c, 9);
        int r = cliphtml_row_right_boundary(c, 0, vns, 8);
        check("2 透明尾随空格裁掉(应=2)", r, 2);
    }

    /* 3) 带前景色的尾随空格也保留 */
    build(c, "ggFFF......");
    {
        int vns = last_nonspace(c, 10);
        int r = cliphtml_row_right_boundary(c, 0, vns, 9);
        check("3 前景色尾随空格保留(应=4)", r, 4);
    }

    /* 4) 底色空格后又跟透明空格：停在底色空格，不吞透明部分 */
    build(c, "ggBBB..");
    {
        int vns = last_nonspace(c, 7);
        int r = cliphtml_row_right_boundary(c, 0, vns, 6);
        check("4 底色空格后透明空格不保留(应=4)", r, 4);
    }

    /* 5) 整行只有透明空格（全空行）：返回 x_start-1 */
    build(c, "........");
    {
        int vns = last_nonspace(c, 8);  /* = -1 */
        int r = cliphtml_row_right_boundary(c, 0, vns, 7);
        check("5 全空行返回 -1", r, -1);
    }

    /* 6) 块选 x_start=10：最后一个非空格在 10，背景空格到 13，之后透明 */
    build(c, "..........gBBB...");
    {
        int vns = last_nonspace(c, 17);  /* 10 */
        int r = cliphtml_row_right_boundary(c, 10, vns, 16);
        check("6 块选背景空格保留(应=13)", r, 13);
    }

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nALL COPY-TRAILING-BG CHECKS PASSED\n");
    return 0;
}
"""


def extract_func(src, name):
    idx = src.find(name)
    if idx < 0:
        raise SystemExit(f"找不到函数 {name}")
    # 回退到该行行首的 "static"
    line_start = src.rfind("\n", 0, idx) + 1
    # 从函数签名末尾的 '{' 起做花括号配对
    brace = src.find("{", idx)
    depth = 0
    i = brace
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[line_start:i + 1]
        i += 1
    raise SystemExit("花括号不配对")


def main():
    src = open(INPUT_C, encoding="utf-8").read()
    func = extract_func(src, "cliphtml_row_right_boundary")
    harness = HARNESS.replace("%(func)s", func)
    with tempfile.TemporaryDirectory() as td:
        hc = os.path.join(td, "harness.c")
        binp = hbuild.exe_path(td, "harness")
        open(hc, "w", encoding="utf-8").write(harness)
        inc = os.path.join(ROOT, "include")
        p = subprocess.run(
            ["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
             "-Wall", "-Wextra", "-Werror", "-I", inc, hc, "-o", binp],
            capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stdout)
            print(p.stderr)
            raise SystemExit("编译失败")
        r = subprocess.run([binp], capture_output=True, text=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            raise SystemExit(1)
    print("[OK] verify_copy_trailing_bg passed")


if __name__ == "__main__":
    main()
