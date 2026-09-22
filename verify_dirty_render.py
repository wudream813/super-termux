#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脏区渲染回归 (v1.8.12)。

从真源码 src/framediff.c 抽出整帧增量逻辑，用 gcc + ASAN/UBSan 编译执行。

framediff 把整帧 VT 流按绝对光标定位（CUP \x1b[r;cH）切成「每行一段字节」，
与上一帧影子逐行 memcmp：没变的行不发。验证：

1. 首帧整行都发（影子为空）。
2. 内容不变的第二帧：增量为空（0 字节）—— 这是省字节的关键。
3. 只有第 5 行变化：增量里只含第 5 行的 CUP 段，不含其它行。
4. always 段（OSC 标题 \x1b]0;..\x07）每帧都发。
5. invalidate / 终端行数变化后强制整帧。
6. 光标显隐序列（\x1b[?25h/l）随所在行走，光标移动行会脏。
"""

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
SRC = (ROOT / "src" / "framediff.c").read_text(encoding="utf-8")

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framediff.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* 构造一行终端内容：CUP 到 (row,col1) + SGR + 文本 + 复位。 */
static char *row_line(char *buf, size_t cap, int row, int col, const char *sgr, const char *text) {
    snprintf(buf, cap, "\x1b[%d;%dH%s%s\x1b[0m", row, col, sgr, text);
    return buf;
}

static char *frame_full(char *buf, size_t cap, int rows, const char *tag) {
    size_t pos = 0;
    for (int r = 1; r <= rows; r++) {
        char line[256];
        row_line(line, sizeof(line), r, 1, "\x1b[0;38;2;200;200;200m", tag);
        /* 每行不同文本，避免完全相同 */
        char seg[320];
        snprintf(seg, sizeof(seg), "\x1b[%d;1H%srow-%d-%s\x1b[0m",
                 r, "\x1b[0;38;2;200;200;200m", r, tag);
        size_t n = strlen(seg);
        if (pos + n < cap) { memcpy(buf + pos, seg, n); pos += n; }
    }
    buf[pos] = 0;
    return buf;
}

static void test_basic(void) {
    FrameDiff fd; framediff_init(&fd);
    char big[65536];

    /* 帧 1：3 行 + 一个 OSC 标题（always）。 */
    char f1[1024];
    size_t p = 0;
    const char *osc = "\x1b]0;termux\x07";
    memcpy(f1 + p, osc, strlen(osc)); p += strlen(osc);
    char ln[128];
    for (int r = 1; r <= 3; r++) {
        row_line(ln, sizeof(ln), r, 1, "\x1b[0;38;2;10;20;30m", "hello");
        size_t n = strlen(ln);
        memcpy(f1 + p, ln, n); p += n;
    }
    f1[p] = 0;

    char out[65536];
    framediff_begin_frame(&fd, 3);
    framediff_scan(&fd, f1, strlen(f1));
    size_t n1 = framediff_emit(&fd, NULL, 0); (void)n1;
    CHECK(n1 > 0, "first frame should emit something");
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }
    CHECK(strstr(out, osc) != NULL, "OSC title must be in always segment");
    CHECK(strstr(out, "hello") != NULL, "first frame must contain row text");

    /* 帧 2：完全相同 -> 增量应只有 always（OSC），不含任何行文本。 */
    framediff_begin_frame(&fd, 3);
    framediff_scan(&fd, f1, strlen(f1));
    size_t n2 = framediff_emit(&fd, NULL, 0); (void)n2;
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }
    CHECK(strstr(out, osc) != NULL, "OSC must still emit on unchanged frame");
    CHECK(strstr(out, "hello") == NULL,
          "unchanged rows must NOT be emitted, but found row text (n2=%zu)", n2);

    /* 帧 3：只改第 2 行。 */
    char f3[1024];
    p = 0;
    memcpy(f3 + p, osc, strlen(osc)); p += strlen(osc);
    for (int r = 1; r <= 3; r++) {
        const char *txt = (r == 2) ? "CHANGED" : "hello";
        row_line(ln, sizeof(ln), r, 1, "\x1b[0;38;2;10;20;30m", txt);
        size_t n = strlen(ln);
        memcpy(f3 + p, ln, n); p += n;
    }
    f3[p] = 0;
    framediff_begin_frame(&fd, 3);
    framediff_scan(&fd, f3, strlen(f3));
    size_t n3 = framediff_emit(&fd, NULL, 0); (void)n3;
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }
    CHECK(strstr(out, "CHANGED") != NULL, "changed row 2 must emit");
    /* 第 1/3 行未变：它们的文本 "hello" 不应出现（第 2 行已不含 hello）。 */
    char *cup2 = strstr(out, "\x1b[2;1H");
    CHECK(cup2 != NULL, "changed row must carry its CUP \\x1b[2;1H");
    char *cup1 = strstr(out, "\x1b[1;1H");
    char *cup3 = strstr(out, "\x1b[3;1H");
    CHECK(cup1 == NULL && cup3 == NULL, "unchanged rows 1 and 3 must not emit");
    (void)big;

    framediff_free(&fd);
}

static void test_invalidate_and_resize(void) {
    FrameDiff fd; framediff_init(&fd);
    char f1[4096];
    frame_full(f1, sizeof(f1), 4, "A");

    char out[65536];
    framediff_begin_frame(&fd, 4);
    framediff_scan(&fd, f1, strlen(f1));
    framediff_emit(&fd, NULL, 0);
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }

    /* 相同帧 -> 无行文本 */
    framediff_begin_frame(&fd, 4);
    framediff_scan(&fd, f1, strlen(f1));
    size_t n2 = framediff_emit(&fd, NULL, 0); (void)n2;
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }
    CHECK(strstr(out, "row-1-A") == NULL, "unchanged frame should skip rows");

    /* invalidate 后整帧 */
    framediff_invalidate(&fd);
    framediff_begin_frame(&fd, 4);
    framediff_scan(&fd, f1, strlen(f1));
    framediff_emit(&fd, NULL, 0);
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }
    CHECK(strstr(out, "row-1-A") != NULL, "after invalidate all rows must emit");

    /* 行数变化（resize）后整帧 */
    framediff_begin_frame(&fd, 4);
    framediff_scan(&fd, f1, strlen(f1));
    framediff_emit(&fd, NULL, 0);
    framediff_emit(&fd, out, sizeof(out));   /* 建立 4 行影子 */
    char f5[4096];
    frame_full(f5, sizeof(f5), 5, "A");
    framediff_begin_frame(&fd, 5);
    framediff_scan(&fd, f5, strlen(f5));
    framediff_emit(&fd, NULL, 0);
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }
    CHECK(strstr(out, "row-5-A") != NULL, "after resize new rows must emit");
    CHECK(strstr(out, "row-1-A") != NULL, "after resize existing rows must also emit (force all)");

    framediff_free(&fd);
}

static void test_cursor_toggle(void) {
    FrameDiff fd; framediff_init(&fd);
    char out[65536];

    /* 帧：行 1-3，光标在第 2 行末 \x1b[?25h */
    char f1[1024];
    snprintf(f1, sizeof(f1),
             "\x1b[1;1H%sone\x1b[0m"
             "\x1b[2;1H%stwo\x1b[0m"
             "\x1b[3;1H%sthree\x1b[0m"
             "\x1b[2;5H\x1b[?25h",
             "\x1b[0;38;2;1;2;3m", "\x1b[0;38;2;1;2;3m", "\x1b[0;38;2;1;2;3m");
    framediff_begin_frame(&fd, 3);
    framediff_scan(&fd, f1, strlen(f1));
    framediff_emit(&fd, NULL, 0);
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }

    /* 帧 2：内容相同，光标移到第 3 行 -> 第 2、3 行都脏（2 去掉光标，3 加上）。 */
    char f2[1024];
    snprintf(f2, sizeof(f2),
             "\x1b[1;1H%sone\x1b[0m"
             "\x1b[2;1H%stwo\x1b[0m"
             "\x1b[3;1H%sthree\x1b[0m"
             "\x1b[3;6H\x1b[?25h",
             "\x1b[0;38;2;1;2;3m", "\x1b[0;38;2;1;2;3m", "\x1b[0;38;2;1;2;3m");
    framediff_begin_frame(&fd, 3);
    framediff_scan(&fd, f2, strlen(f2));
    framediff_emit(&fd, NULL, 0);
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); out[_n] = 0; }
    CHECK(strstr(out, "\x1b[3;6H") != NULL, "cursor move to row 3 must dirty row 3");
    CHECK(strstr(out, "\x1b[2;1H") != NULL, "old cursor row 2 must re-emit (cursor left it)");
    CHECK(strstr(out, "\x1b[1;1H") == NULL, "unrelated row 1 must not emit");

    framediff_free(&fd);
}

/* v1.8.13 回归：colortool 色块行设了背景之后，后续默认色的变化行单独发出时
 * 必须自成一个 SGR 作用域。framediff 增量帧只把变化行的整段字节发给终端，终端
 * 实际的 SGR 状态停在上一帧最后发出的行（色块行的背景），而不是整帧顺序里本行
 * 的上一行。渲染器因此必须在每行 CUP 后先复位，令任意行单独发出都与整帧一致。
 * 本测试用修复后渲染器应产出的字节形态验证集成契约：增量切片里变化行段的 CUP
 * 之后紧跟复位，且未变化色块行的背景 SGR 不会泄漏进增量。 */
static void test_sgr_row_scope(void) {
    FrameDiff fd; framediff_init(&fd);
    char out[65536];

    const char *f1 =
        "\x1b[1;1H\x1b[0m\x1b[0;48;2;100;100;100m####\x1b[0m\x1b[K"
        "\x1b[2;1H\x1b[0m\x1b[0;38;2;200;200;200mtext\x1b[0m\x1b[K";
    framediff_begin_frame(&fd, 2);
    framediff_scan(&fd, f1, strlen(f1));
    { size_t _n = framediff_emit(&fd, NULL, 0); (void)_n; }
    { size_t _n = framediff_emit(&fd, out, sizeof(out)); (void)_n; }

    /* 帧 2：只有第 2 行变化（text -> NEXT）；第 1 行色块完全不动。 */
    const char *f2 =
        "\x1b[1;1H\x1b[0m\x1b[0;48;2;100;100;100m####\x1b[0m\x1b[K"
        "\x1b[2;1H\x1b[0m\x1b[0;38;2;200;200;200mNEXT\x1b[0m\x1b[K";
    framediff_begin_frame(&fd, 2);
    framediff_scan(&fd, f2, strlen(f2));
    { size_t _n = framediff_emit(&fd, NULL, 0); (void)_n; }
    { size_t n = framediff_emit(&fd, out, sizeof(out)); out[n] = 0; }

    char *cup2 = strstr(out, "\x1b[2;1H");
    CHECK(cup2 != NULL, "changed default row 2 must emit");
    if (cup2) {
        CHECK(memcmp(cup2 + 6, "\x1b[0m", 4) == 0,
              "emitted row 2 chunk must begin with \x1b[0m (self-contained SGR scope)");
    }
    CHECK(strstr(out, "\x1b[1;1H") == NULL, "unchanged color-block row 1 must not be re-emitted");
    CHECK(strstr(out, "48;2;100;100;100") == NULL,
          "background SGR of unchanged row 1 must NOT leak into the delta");
    CHECK(strstr(out, "NEXT") != NULL, "row 2 new text must be present");

    framediff_free(&fd);
}

int main(void) {
    test_basic();
    test_invalidate_and_resize();
    test_cursor_toggle();
    test_sgr_row_scope();
    if (failures) { printf("[FAIL] %d check(s) failed\n", failures); return 1; }
    printf("[OK] 脏区渲染验证通过（首帧全发 / 未变帧 0 行 / 只发变化行 / OSC always / invalidate / resize / 光标随行）。\n");
    return 0;
}
"""


def main():
    print("=== 脏区渲染 (verify_dirty_render.py) ===")
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "harness.c").write_text(HARNESS, encoding="utf-8")
        exe = td / "t"
        cmd = ["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
               "-Wall", "-Wextra", "-Werror",
               "-I", str(ROOT / "include"),
               str(ROOT / "src" / "framediff.c"),
               str(td / "harness.c"), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            sys.exit("FAIL: compile error")
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            sys.exit(1)

    check_render_sgr_scope()


def check_render_sgr_scope():
    """源码不变量 (v1.8.13)：终端行循环里，每行 CUP 之后必须先 \\x1b[0m 复位并重置
    颜色哨兵，保证任意一行被 framediff 单独切片发出时都自成 SGR 作用域（不跨行
    携带 SGR）。render.c 依赖 Win32 API 无法在此编译执行，故以源码锚点锁死修复，
    变异（删掉行首复位/哨兵重置）会被这里抓到。"""
    print("--- 源码不变量: 每行自成 SGR 作用域 (render.c) ---")
    src = (ROOT / "src" / "render.c").read_text(encoding="utf-8")
    anchor = 'for (int y = 0; y < rr; y++)'
    i = src.find(anchor)
    assert i != 0 and i != -1, "render.c: 找不到终端行循环锚点 %r" % anchor
    # 窗口从 1800 放宽到 2400：v1.8.52 整屏 vo>0 回看改走 reflow 网格后，行循环头
    # 与 cell 循环之间多了一行 rf_row 指针准备代码；不变量本身（行首复位/哨兵重置
    # 必须在 cell 循环前）未变，此处只是跟随合法代码增长放宽扫描范围。
    head = src[i:i + 2400]
    cell_x = head.find("for (int x = 0; x < text_rc; x++)")
    assert cell_x > 0, "render.c: 终端行循环内找不到 cell 循环"
    pre = head[:cell_x]  # 行首 CUP 与本行第一个 cell 之间的所有代码
    assert '\\x1b[0m' in pre, ("render.c: 行首 CUP 之后缺少 \\x1b[0m 复位；"
                               "增量帧单独发出某行时会沿用上一帧末行的 SGR（背景丢失回归）")
    reset_pos = pre.find('\\x1b[0m')
    cup_pos = pre.find('\\x1b[%d;1H')
    assert cup_pos != -1 and reset_pos > cup_pos, "render.c: \\x1b[0m 复位必须在行首 CUP 之后"
    for tok in ("la_attr = 0xFFFF", "la_fv = -1", "la_bv = -1"):
        assert tok in pre[:pre.find('\\x1b[0m') + 6] or tok in pre, (
            "render.c: 行首缺少颜色哨兵重置 %r" % tok)
    assert pre.find('la_attr = 0xFFFF') < cell_x, "render.c: la_attr 哨兵必须在 cell 循环前重置"
    print("[OK] render.c 行首复位 + 颜色哨兵重置在位（任意行单独发出与整帧一致）。")


if __name__ == "__main__":
    main()
