#!/usr/bin/env python3
"""verify_copy_snap.py

v1.8.19 回归：选区端点必须吸附到完整字符，避免选中半个宽字符（中文/emoji）。

宽字符占两列：
  - BMP 宽字符（中文/全角/假名）：主格写字、次格写 0 占位；
  - non-BMP emoji：主格高代理、次格低代理。
端点落在宽字符中间时：
  - 右端点在主格 -> 右扩 1（snap_right_to_char）；
  - 左端点在次格 -> 左退 1（snap_left_to_char）。

从【真实 src/screen.c】抽取两个函数，构造一行 WCHAR 做边界断言。
变异：把 snap_*_to_char 改成恒返回原 x（不吸附），用例立即失败。
"""
import os
import subprocess
import sys
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 无 -fsanitize）
#   统一收在 tests/hbuild.py；降级时会自己往 stderr 打 [SKIP-SANITIZER]。
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402

import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
SCREEN_C = os.path.join(ROOT, "src", "screen.c")
SRC = os.path.join(ROOT, "src")
INC = os.path.join(ROOT, "include")

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef unsigned short WORD; typedef unsigned short WCHAR;
typedef unsigned int UINT; typedef void* HANDLE; typedef int BOOL;
/* 真实 CHAR_INFO 布局（4 字节/单元格）；函数必须按 CHAR_INFO 步长索引，
 * 否则 &Char 当 WCHAR* 会读到相邻单元格的 Attributes（→ 要按两下的历史 bug）。 */
typedef struct { union { WCHAR UnicodeChar; char AsciiChar; } Char; WORD Attributes; } CHAR_INFO;
#define LCC(line, i) ((line)[(i)].Char.UnicodeChar)
#define CI(ch) { { (WCHAR)(ch) }, 0x07 }
int is_wide_cp(unsigned int cp);
%(funcs)s

static int failures = 0;
static void ck(const char *n, int got, int want) {
    if (got != want) { printf("[FAIL] %s: got %d want %d\n", n, got, want); failures++; }
    else             { printf("[ok]   %s: %d\n", n, got); }
}

int main(void) {
    /* "ab 保 x"：
       col 0 'a', 1 'b', 2 ' ', 3 保(0x4FDD 主格), 4 保占位0(次格),
       5 'x'。中文占 3、4 两列。 */
    CHAR_INFO line1[16] = { CI('a'),CI('b'),CI(' '),CI(0x4FDD),CI(0),CI('x'),
                            CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0) };
    int n = 16;

    /* 右端点：在中文主格 col3 -> 扩到 col4（包含占位） */
    ck("右:端点在中文主格(3)->扩到4", snap_right_to_char(line1,n,3), 4);
    /* 右端点在中文次格 col4 -> 已是次格，不再扩（次格不宽）-> 保持 4 */
    ck("右:端点在中文次格(4)->不扩", snap_right_to_char(line1,n,4), 4);
    /* 右端点在 ASCII col1 -> 不变 */
    ck("右:端点在ASCII(1)->不变", snap_right_to_char(line1,n,1), 1);
    /* 右端点在空格 col2 -> 不变 */
    ck("右:端点在空格(2)->不变", snap_right_to_char(line1,n,2), 2);

    /* 左端点：落在中文次格 col4 -> 左退到主格 col3 */
    ck("左:端点在中文次格(4)->退到3", snap_left_to_char(line1,n,4), 3);
    /* 左端点在中文主格 col3 -> 不变 */
    ck("左:端点在中文主格(3)->不变", snap_left_to_char(line1,n,3), 3);
    /* 左端点在 ASCII col5('x') -> 不变 */
    ck("左:端点在ASCII x(5)->不变", snap_left_to_char(line1,n,5), 5);
    /* 左端点在 col0 -> 保持 0（不能负） */
    ck("左:端点col0->0", snap_left_to_char(line1,n,0), 0);

    /* emoji 代理对：😀 U+1F600 = 高代理 0xD83D, 低代理 0xDE00，占 col1 主、col2 次 */
    CHAR_INFO line2[16]; memset(line2,0,sizeof line2);
    line2[0]= (CHAR_INFO)CI('A'); line2[1]= (CHAR_INFO)CI(0xD83D);
    line2[2]= (CHAR_INFO)CI(0xDE00); line2[3]= (CHAR_INFO)CI('B');
    ck("右:端点在emoji高代理(1)->扩到2", snap_right_to_char(line2,n,1), 2);
    ck("右:端点在emoji低代理(2)->不扩", snap_right_to_char(line2,n,2), 2);
    ck("左:端点在emoji低代理(2)->退到1", snap_left_to_char(line2,n,2), 1);
    ck("左:端点在emoji高代理(1)->不变", snap_left_to_char(line2,n,1), 1);

    /* 全角字符 '＠' U+FF20 宽 */
    CHAR_INFO line3[8] = { CI('x'), CI(0xFF20), CI(0), CI('y'), CI(0),CI(0),CI(0),CI(0) };
    ck("右:全角主格(1)->扩到2", snap_right_to_char(line3,8,1), 2);
    ck("左:全角次格(2)->退到1", snap_left_to_char(line3,8,2), 1);

    /* 越界夹紧：宽主格在最后一列、无次格可扩 -> 不越界 */
    CHAR_INFO line4[4] = { CI('a'),CI('b'),CI('c'),CI(0x4FDD) };
    ck("右:末列宽主格无次格->不越界(3)", snap_right_to_char(line4,4,3), 3);

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nALL SNAP-TO-CHAR CHECKS PASSED\n");
    return 0;
}
"""


def extract_func(src, name):
    idx = src.find(name)
    if idx < 0:
        raise SystemExit("找不到 " + name)
    # 回退到行首（int name( ）
    ls = src.rfind("\n", 0, idx) + 1
    brace = src.find("{", idx)
    depth = 0
    i = brace
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[ls:i + 1]
        i += 1
    raise SystemExit("花括号不配对 " + name)


def main():
    src = open(SCREEN_C, encoding="utf-8").read()
    f1 = extract_func(src, "snap_right_to_char")
    f2 = extract_func(src, "snap_left_to_char")
    harness = HARNESS.replace("%(funcs)s", f1 + "\n" + f2)

    with tempfile.TemporaryDirectory() as td:
        stub = os.path.join(td, "stub")
        os.makedirs(stub)
        winh = os.path.join(stub, "windows.h")
        open(winh, "w").write(
            "#pragma once\n#include <stddef.h>\n#include <wchar.h>\n"
            "typedef unsigned short WORD; typedef unsigned long DWORD; typedef long LONG;\n"
            "typedef short SHORT; typedef wchar_t WCHAR; typedef unsigned char BYTE;\n"
            "typedef unsigned int UINT; typedef int INT; typedef void* HANDLE; typedef int BOOL;\n"
            "#define TRUE 1\n#define FALSE 0\n#ifndef NULL\n#define NULL 0\n#endif\n")
        for h in ["shellapi.h", "process.h", "windowsx.h"]:
            open(os.path.join(stub, h), "w").write('#pragma once\n#include "windows.h"\n')

        hc = os.path.join(td, "h.c")
        binp = os.path.join(td, "h")
        open(hc, "w", encoding="utf-8").write(harness)
        uo = os.path.join(td, "utf8.o")
        p = subprocess.run(["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
                            "-I", stub, "-I", INC, "-c",
                            os.path.join(SRC, "utf8.c"), "-o", uo],
                           capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stderr); raise SystemExit("utf8 编译失败")
        p = subprocess.run(["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
                            "-Wall", "-Wextra", "-Werror", "-I", INC,
                            hc, uo, "-o", binp],
                           capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stdout); print(p.stderr); raise SystemExit("编译失败")
        r = subprocess.run([binp], capture_output=True, text=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr); raise SystemExit(1)
    # v1.8.24：渲染高亮在【活屏】(vo==0) 也必须整字吸附（旧实现活屏 ar=-1 漏吸附）。
    # v1.8.25：取行必须返回 CHAR_INFO*（真实 4 字节步长）；旧的返回
    # &cells[0].Char.UnicodeChar（WCHAR*，2 字节步长）会读到相邻单元格的
    # Attributes、列号错位，导致键盘 → 要按两下才跨过一个汉字。
    render_c = open(os.path.join(ROOT, "src", "render.c"), encoding="utf-8").read()
    input_c = open(os.path.join(ROOT, "src", "input.c"), encoding="utf-8").read()
    if "static const CHAR_INFO *render_sel_line(ScreenBuffer *s, int row, int vo)" not in render_c:
        raise SystemExit("render.c 缺少 CHAR_INFO* render_sel_line()（活屏选区整字取行）")
    if "snap_sel_row(s, y, vo," not in render_c:
        raise SystemExit("snap_sel_row 未按 (s, y, vo, ...) 传屏幕行+滚动偏移（活屏无法吸附）")
    # 旧的错误取行法（返回 &...Char.UnicodeChar 当 WCHAR*，2 字节步长错位）不得残留
    for tag, src in (("render.c", render_c), ("input.c", input_c)):
        for bad in ("return &s->lines[pr].cells[0].Char.UnicodeChar",
                    "return &s->lines[ar].cells[0].Char.UnicodeChar",
                    "return &s->alt_buffer[(size_t)row * s->cols].Char.UnicodeChar",
                    "= &s->alt_buffer[(size_t)abs_y * s->cols].Char.UnicodeChar",
                    "= &s->lines[pr].cells[0].Char.UnicodeChar"):
            if bad in src:
                raise SystemExit(tag + " 仍把整行当 WCHAR* 取（2字节步长错位）: " + bad)
    # 三个整字函数的签名必须是 CHAR_INFO*
    screen_h = open(os.path.join(ROOT, "include", "screen.h"), encoding="utf-8").read()
    for fn in ("snap_right_to_char", "snap_left_to_char", "copy_step_char", "copy_cursor_to_lead"):
        if (fn + "(const WCHAR *") in screen_h:
            raise SystemExit("screen.h " + fn + " 仍是 WCHAR* 参数（应为 CHAR_INFO*）")
    print("[ok] 整字函数按 CHAR_INFO 真实步长索引；活屏/回滚/alt 高亮均整字吸附")

    print("[OK] verify_copy_snap passed")


if __name__ == "__main__":
    main()
