#!/usr/bin/env python3
"""verify_copy_step.py

v1.8.20 回归：复制模式移动光标（键盘 ←/→ 与鼠标端点）一次跨过整个宽字符，
绝不停在中文/全角/emoji 的次格（占位）上。

从【真实 src/screen.c】抽取 copy_step_char()，构造一行 WCHAR 断言：
  - 向右：站在宽字符主格 -> 跳 2 列；次格 -> 跳 1 列；ASCII -> 1 列；
  - 向左：站在次格（占位0/低代理）-> 退 2 列；左邻是宽主格 -> 退 2；否则 1；
  - 边界夹紧（首列/末列不越界）。
变异：把 copy_step_char 改成每次只走 1 格（旧行为），用例立即失败。
"""
import os
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 无 -fsanitize）
#   统一收在 tests/hbuild.py；降级时会自己往 stderr 打 [SKIP-SANITIZER]。
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402

import subprocess
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
SCREEN_C = os.path.join(ROOT, "src", "screen.c")
SRC = os.path.join(ROOT, "src")
INC = os.path.join(ROOT, "include")

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef unsigned short WCHAR;
typedef unsigned short WORD;
/* 真实 CHAR_INFO 布局：Char(WCHAR) + Attributes(WORD) = 4 字节/单元格。
 * 这些吸附/步进函数必须按 CHAR_INFO 步长索引；若误取 &Char 当 WCHAR*（2 字节
 * 步长）就会读到相邻单元格的 Attributes、列号错位（历史 bug：→ 要按两下）。 */
typedef struct { union { WCHAR UnicodeChar; char AsciiChar; } Char; WORD Attributes; } CHAR_INFO;
#define LCC(line, i) ((line)[(i)].Char.UnicodeChar)
#define CI(ch) { { (WCHAR)(ch) }, 0x07 }
static int cell_is_wide_trail(const CHAR_INFO *line, int k, int *lead);
int is_wide_cp(unsigned int cp);
%(func)s

static int failures = 0;
static void ck(const char *n, int got, int want) {
    if (got != want) { printf("[FAIL] %s: got %d want %d\n", n, got, want); failures++; }
    else             { printf("[ok]   %s: %d\n", n, got); }
}

int main(void) {
    /* "a 保 x"：col0 'a', col1 ' ', col2 保(主), col3 保占位0(次), col4 'x' */
    CHAR_INFO line1[16] = { CI('a'),CI(' '),CI(0x4FDD),CI(0),CI('x'),
                            CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0),CI(0) };
    int n = 16;

    /* 向右：从主格 col2 跨到 col4（跳过次格 col3） */
    ck("右:站在中文主格(2)->跳到4", copy_step_char(line1,n,2,+1), 4);
    /* 向右：从 ASCII col0 -> col1 */
    ck("右:ASCII(0)->1", copy_step_char(line1,n,0,+1), 1);
    /* 向右：从空格 col1 -> col2（进入主格） */
    ck("右:空格(1)->2", copy_step_char(line1,n,1,+1), 2);
    /* 向右：从次格 col3 -> col4（次格不是主格，只走 1） */
    ck("右:次格(3)->4", copy_step_char(line1,n,3,+1), 4);
    /* 向右：末列不越界 */
    ck("右:末列(15)夹紧", copy_step_char(line1,n,15,+1), 15);

    /* 向左：站在次格 col3 -> 退到主格 col2（跨过整个汉字） */
    ck("左:站在次格(3)->吸到保主格(2)", copy_step_char(line1,n,3,-1), 2);
    /* 向左：站在 'x' col4，左邻(col3)是占位 0、再左 col2 是中文主格：
       从 x 向左应跨过整个汉字到 col2（主格） */
    ck("左:从x(4)->汉字主格(2)", copy_step_char(line1,n,4,-1), 2);
    /* 向左：ASCII col0 -> 0（不越界） */
    ck("左:col0夹紧0", copy_step_char(line1,n,0,-1), 0);
    /* 向左：col1(空格) 左邻 col0 'a' 非宽 -> 退到 0 */
    ck("左:空格(1)->0", copy_step_char(line1,n,1,-1), 0);

    /* emoji 代理对：😀 U+1F600 = 高0xD83D(col1) 低0xDE00(col2)，col0 'A' col3 'B' */
    CHAR_INFO line2[16]; memset(line2,0,sizeof line2);
    line2[0]= (CHAR_INFO)CI('A'); line2[1]= (CHAR_INFO)CI(0xD83D);
    line2[2]= (CHAR_INFO)CI(0xDE00); line2[3]= (CHAR_INFO)CI('B');
    ck("右:emoji主格(1)->跳到3", copy_step_char(line2,n,1,+1), 3);
    ck("右:emoji次格(2)->3", copy_step_char(line2,n,2,+1), 3);
    ck("左:emoji次格(2)->emoji主格(1)", copy_step_char(line2,n,2,-1), 1);
    ck("左:从B(3)->emoji主格(1)", copy_step_char(line2,n,3,-1), 1);

    /* 连续两个汉字 "中文"：col0 中主 col1 中次 col2 文主 col3 文次 col4 'z' */
    CHAR_INFO line3[8] = { CI(0x4E2D),CI(0),CI(0x6587),CI(0),CI('z'),CI(0),CI(0),CI(0) };
    ck("右:中主(0)->文主(2)", copy_step_char(line3,8,0,+1), 2);
    ck("右:文主(2)->z(4)", copy_step_char(line3,8,2,+1), 4);
    ck("左:中次(1)->退到0", copy_step_char(line3,8,1,-1), 0);
    ck("左:文次(3)->文主格(2)", copy_step_char(line3,8,3,-1), 2);

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nALL COPY-STEP-CHAR CHECKS PASSED\n");
    return 0;
}
"""


def extract(src, name):
    idx = src.find(name)
    if idx < 0:
        raise SystemExit("找不到 " + name)
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
    func = extract(src, "cell_is_wide_trail") + "\n" + extract(src, "copy_step_char")
    h = HARNESS.replace("%(func)s", func)
    with tempfile.TemporaryDirectory() as td:
        stub = os.path.join(td, "stub")
        os.makedirs(stub)
        open(os.path.join(stub, "windows.h"), "w").write(
            "#pragma once\n#include <stddef.h>\n#include <wchar.h>\n"
            "typedef unsigned short WORD; typedef wchar_t WCHAR;\n"
            "typedef unsigned long DWORD; typedef void* HANDLE; typedef int BOOL;\n")
        for hdr in ["shellapi.h", "process.h", "windowsx.h"]:
            open(os.path.join(stub, hdr), "w").write('#pragma once\n#include "windows.h"\n')
        uo = os.path.join(td, "utf8.o")
        p = subprocess.run(["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
                            "-I", stub, "-I", INC, "-c", os.path.join(SRC, "utf8.c"), "-o", uo],
                           capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stderr); raise SystemExit("utf8 编译失败")
        hc = os.path.join(td, "h.c"); bp = os.path.join(td, "h")
        open(hc, "w").write(h)
        p = subprocess.run(["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
                            "-Wall", "-Wextra", "-Werror", "-I", INC, hc, uo, "-o", bp],
                           capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stdout); print(p.stderr); raise SystemExit("编译失败")
        r = subprocess.run([bp], capture_output=True, text=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr); raise SystemExit(1)
    print("[OK] verify_copy_step passed")


if __name__ == "__main__":
    main()
