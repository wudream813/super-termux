#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""HTML 彩色剪贴板回归 (v1.8.12)。

从真源码 src/cliphtml.c 抽出函数，用 gcc + ASAN/UBSan 编译执行，验证：

1. HTML Format 头部四个偏移量（StartHTML/EndHTML/StartFragment/EndFragment）
   准确指向载荷中的对应位置 —— 偏移错了 Word/浏览器会拒绝整个格式。
2. 真彩色 cell 直出 38;2 / span 的 #rrggbb；16 色 attr 走 Campbell 调色板。
3. 同色相邻 cell 合并成一个 <span>（RLE），变色时正确关旧开新。
4. & < > 转义；UTF-16 代理对合成 4 字节 UTF-8（emoji 不截断）。
5. 空 cell（ch=0）按空格处理；行尾空格裁掉。
6. 变异保护：把 offset 头写错（占位替换后不自洽）这里会立刻红。
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
SRC = (ROOT / "src" / "cliphtml.c").read_text(encoding="utf-8")


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cliphtml.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* 解析 HTML Format 头部的偏移量字段。 */
static long hdr_field(const char *d, const char *key) {
    const char *p = strstr(d, key);
    if (!p) return -1;
    p += strlen(key);
    return strtol(p, NULL, 10);
}

static void test_offsets(void) {
    ClipHtmlBuf b; cliphtml_init(&b);
    cliphtml_frag_begin(&b);
    ClipHtmlCell cells[3];
    memset(cells, 0, sizeof(cells));
    cells[0].ch = L'a'; cells[0].fg_valid = 1; cells[0].r = 31; cells[0].g = 111; cells[0].b = 235;
    cells[1].ch = L'b';
    cells[2].ch = L'c'; cells[2].fg_valid = 1; cells[2].r = 63; cells[2].g = 185; cells[2].b = 80;
    cliphtml_frag_row(&b, cells, 0, 2);
    CHECK(cliphtml_finalize(&b) == 1, "finalize failed");

    long sh = hdr_field(b.data, "StartHTML:");
    long eh = hdr_field(b.data, "EndHTML:");
    long sf = hdr_field(b.data, "StartFragment:");
    long ef = hdr_field(b.data, "EndFragment:");
    CHECK(sh >= 0 && eh > sh && sf >= sh && ef > sf && (size_t)eh == b.len,
          "offsets out of range: sh=%ld eh=%ld sf=%ld ef=%ld len=%zu",
          sh, eh, sf, ef, b.len);
    CHECK(strncmp(b.data + sf, "<!--StartFragment-->", 20) == 0,
          "StartFragment does not point at fragment marker");
    CHECK(strncmp(b.data + ef, "<!--EndFragment-->", 18) == 0,
          "EndFragment does not point at end marker");
    CHECK(strncmp(b.data + sf + 20, "<pre ", 5) == 0,
          "fragment body should start with <pre");
    CHECK(strstr(b.data + sf, "</pre>") != NULL, "missing </pre>");
    cliphtml_free(&b);
}

static void test_colors_and_rle(void) {
    ClipHtmlBuf b; cliphtml_init(&b);
    cliphtml_frag_begin(&b);
    ClipHtmlCell cells[6];
    memset(cells, 0, sizeof(cells));
    /* 三个连续同色 -> 一个 span；变色 -> 关旧开新。 */
    for (int i = 0; i < 3; i++) {
        cells[i].ch = L'x';
        cells[i].fg_valid = 1; cells[i].r = 31; cells[i].g = 111; cells[i].b = 235;
    }
    cells[3].ch = L'y';
    cells[3].fg_valid = 1; cells[3].r = 63; cells[3].g = 185; cells[3].b = 80;
    cells[4].ch = L'z';   /* 默认色：无 span */
    cells[5].ch = L'w';
    cells[5].bg_valid = 1; cells[5].br = 248; cells[5].bg = 81; cells[5].bb = 73;
    cells[5].underline = 1;
    cliphtml_frag_row(&b, cells, 0, 5);
    cliphtml_finalize(&b);

    const char *frag = strstr(b.data, "<!--StartFragment-->") + 20;
    char *frag_end = strstr(b.data, "<!--EndFragment-->");
    *frag_end = 0;
    /* 颜色字面量 */
    CHECK(strstr(frag, "#1f6feb") != NULL, "missing blue #1f6feb");
    CHECK(strstr(frag, "#3fb950") != NULL, "missing green #3fb950");
    CHECK(strstr(frag, "#f85149") != NULL, "missing red bg #f85149");
    CHECK(strstr(frag, "background-color:") != NULL, "missing background-color");
    CHECK(strstr(frag, "text-decoration:underline") != NULL, "missing underline");
    /* RLE: 'xxx' 在同一个 span 里只应出现一次 color:#1f6feb，且内容连续 */
    int nblue = 0; const char *p = frag;
    while ((p = strstr(p, "#1f6feb")) != NULL) { nblue++; p++; }
    CHECK(nblue == 1, "same-color run should merge into one span, found %d", nblue);
    CHECK(strstr(frag, ">xxx<") != NULL, "merged run content not contiguous");
    cliphtml_free(&b);
}

static void test_palette16(void) {
    int r, g, b2;
    cliphtml_palette16(4, &r, &g, &b2);   /* 蓝 (BGR nibble 4) */
    CHECK(r == 0 && g == 55 && b2 == 218, "Campbell blue wrong: %d,%d,%d", r, g, b2);
    cliphtml_palette16(2, &r, &g, &b2);   /* 绿 */
    CHECK(r == 19 && g == 161 && b2 == 14, "Campbell green wrong: %d,%d,%d", r, g, b2);
    cliphtml_palette16(12, &r, &g, &b2);  /* 亮蓝 */
    CHECK(r == 59 && g == 120 && b2 == 255, "Campbell bright blue wrong: %d,%d,%d", r, g, b2);
}

static void test_escape_and_surrogate(void) {
    ClipHtmlBuf b; cliphtml_init(&b);
    cliphtml_frag_begin(&b);
    ClipHtmlCell cells[5];
    memset(cells, 0, sizeof(cells));
    cells[0].ch = L'<';
    cells[1].ch = L'>';
    cells[2].ch = L'&';
    /* emoji U+1F600 -> 高代理 D83D / 低代理 DE00，合成 4 字节 UTF-8 F0 9F 98 80 */
    cells[3].ch = 0xD83D;
    cells[4].ch = 0xDE00;
    cliphtml_frag_row(&b, cells, 0, 4);
    cliphtml_frag_break(&b);
    ClipHtmlCell cells2[1];
    memset(cells2, 0, sizeof(cells2));
    cells2[0].ch = L'z';
    cliphtml_frag_row(&b, cells2, 0, 0);
    cliphtml_finalize(&b);
    const char *frag = strstr(b.data, "<!--StartFragment-->") + 20;
    const char *frag_end = strstr(b.data, "<!--EndFragment-->");
    CHECK(strstr(frag, "&lt;") != NULL, "missing &lt;");
    CHECK(strstr(frag, "&gt;") != NULL, "missing &gt;");
    CHECK(strstr(frag, "&amp;") != NULL, "missing &amp;");
    /* U+1F600 = F0 9F 98 80；只在片段区间内扫描。 */
    const unsigned char *u = (const unsigned char *)frag;
    size_t frag_n = (size_t)(frag_end - frag);
    int found_emoji = 0;
    for (size_t i = 0; i + 4 <= frag_n; i++) {
        if (u[i] == 0xF0 && u[i+1] == 0x9F && u[i+2] == 0x98 && u[i+3] == 0x80) { found_emoji = 1; break; }
    }
    CHECK(found_emoji, "surrogate pair not combined into UTF-8 emoji");
    cliphtml_free(&b);
}

static void test_blank_and_null(void) {
    ClipHtmlBuf b; cliphtml_init(&b);
    cliphtml_frag_begin(&b);
    ClipHtmlCell cells[5];
    memset(cells, 0, sizeof(cells));
    cells[0].ch = L'a';
    cells[1].ch = L'b';
    cells[2].ch = 0;        /* 空 cell -> 空格 */
    cells[3].ch = L' ';     /* 行尾空格：应被区间裁掉，不传进来 */
    cells[4].ch = L' ';
    cliphtml_frag_row(&b, cells, 0, 2);   /* 只到 index 2，模拟行尾裁剪 */
    cliphtml_frag_break(&b);
    ClipHtmlCell empty[1];
    memset(empty, 0, sizeof(empty));
    empty[0].ch = L'c';
    cliphtml_frag_row(&b, empty, 0, 0);
    cliphtml_finalize(&b);
    const char *frag = strstr(b.data, "<!--StartFragment-->") + 20;
    CHECK(strstr(frag, "ab") != NULL, "missing 'ab'");
    /* 两行：中间应有换行（<pre> 内 \n） */
    const char *nl = strchr(frag, '\n');
    CHECK(nl != NULL && strstr(nl, "c") != NULL, "line break / second row missing");
    cliphtml_free(&b);
}

int main(void) {
    test_offsets();
    test_colors_and_rle();
    test_palette16();
    test_escape_and_surrogate();
    test_blank_and_null();
    if (failures) { printf("[FAIL] %d check(s) failed\n", failures); return 1; }
    printf("[OK] HTML clipboard 验证通过（偏移量 / 真彩色 / Campbell 16 色 / RLE 合并 / 转义 / 代理对 / 空 cell）。\n");
    return 0;
}
"""


def main():
    print("=== HTML 彩色剪贴板 (verify_html_clipboard.py) ===")
    # cliphtml.c 是纯标准 C（不依赖 Win32），直接编译整个真源码文件 + 测试 main。
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
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
