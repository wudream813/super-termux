#!/usr/bin/env python3
"""verify_copy_cursor.py

v1.8.23 回归：复制模式里【光标本身】永远停在整字（宽字符主格）上——无论鼠标
点选/拖动，还是键盘上/下/左/右移动，都不许停在汉字/全角/emoji 的次格（半个字）
中间。纯坐标整字化由 src/screen.c 的 copy_cursor_to_lead() 完成：落在次格则退
到其主格，落在主格或窄字符上则原样返回，夹紧到 [0, ncols-1]。

两部分：
  1. 从【真实 src/screen.c】抽取 cell_is_wide_trail() + copy_cursor_to_lead()
     与真实 utf8.o（is_wide_cp）链接，断言光标列整字化正确；
     变异：令 copy_cursor_to_lead 恒返回 x（旧行为=光标可停半个字），用例即失败。
  2. 源码接线断言：input.c 的鼠标快速复制两角、普通左键拖选、上下移动、进入
     复制模式都调用了 copy_cursor_to_lead / copy_snap_cursor_to_char。
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
INPUT_C = os.path.join(ROOT, "src", "input.c")
SRC = os.path.join(ROOT, "src")
INC = os.path.join(ROOT, "include")

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef unsigned short WCHAR;
typedef unsigned short WORD;
/* 真实 CHAR_INFO 布局（4 字节/单元格）；函数必须按 CHAR_INFO 步长索引。 */
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

    /* 光标落在宽字符【次格 col3】-> 必须整字化到主格 col2（不能停半字中间） */
    ck("光标在汉字次格(3)->整字化到主格(2)", copy_cursor_to_lead(line1,n,3), 2);
    /* 光标落在主格 col2 -> 保持 2（本就整字） */
    ck("光标在汉字主格(2)->保持2", copy_cursor_to_lead(line1,n,2), 2);
    /* 窄字符/空格列原样不动 */
    ck("光标在ASCII(0)->保持0", copy_cursor_to_lead(line1,n,0), 0);
    ck("光标在空格(1)->保持1", copy_cursor_to_lead(line1,n,1), 1);
    ck("光标在x(4)->保持4", copy_cursor_to_lead(line1,n,4), 4);

    /* emoji 代理对 😀：col1 高0xD83D 主格，col2 低0xDE00 次格 */
    CHAR_INFO line2[16]; memset(line2,0,sizeof line2);
    line2[0]= (CHAR_INFO)CI('A'); line2[1]= (CHAR_INFO)CI(0xD83D);
    line2[2]= (CHAR_INFO)CI(0xDE00); line2[3]= (CHAR_INFO)CI('B');
    ck("光标在emoji次格(2)->整字化到主格(1)", copy_cursor_to_lead(line2,n,2), 1);
    ck("光标在emoji主格(1)->保持1", copy_cursor_to_lead(line2,n,1), 1);
    ck("光标在emoji后B(3)->保持3", copy_cursor_to_lead(line2,n,3), 3);

    /* 连续两个汉字 "中文"：col0 中主 col1 中次 col2 文主 col3 文次 col4 'z' */
    CHAR_INFO line3[8] = { CI(0x4E2D),CI(0),CI(0x6587),CI(0),CI('z'),CI(0),CI(0),CI(0) };
    ck("光标在中次格(1)->中主(0)", copy_cursor_to_lead(line3,8,1), 0);
    ck("光标在文次格(3)->文主(2)", copy_cursor_to_lead(line3,8,3), 2);
    ck("光标在文主(2)->保持2", copy_cursor_to_lead(line3,8,2), 2);

    /* 边界夹紧 */
    ck("光标负列->0", copy_cursor_to_lead(line1,n,-1), 0);
    ck("光标越界(ncols)->末列15", copy_cursor_to_lead(line1,n,100), 15);
    ck("空行(NULL)->原样3", copy_cursor_to_lead(NULL,n,3), 3);

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nALL COPY-CURSOR-TO-LEAD CHECKS PASSED\n");
    return 0;
}
"""


def extract(src, name):
    idx = src.find("\n" + name)
    if idx < 0:
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


def build_and_run(func_text, label):
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
        open(hc, "w").write(HARNESS.replace("%(func)s", func_text))
        p = subprocess.run(["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
                            "-Wall", "-Wextra", "-Werror", "-I", INC, hc, uo, "-o", bp],
                           capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stdout); print(p.stderr); raise SystemExit("编译失败: " + label)
        r = subprocess.run([bp], capture_output=True, text=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        print(r.stdout)
        if r.stderr:
            print(r.stderr)
        return r.returncode


def main():
    src = open(SCREEN_C, encoding="utf-8").read()
    func = extract(src, "cell_is_wide_trail") + "\n" + extract(src, "copy_cursor_to_lead")

    # 1) 正常源码：应全部通过
    rc = build_and_run(func, "normal")
    if rc != 0:
        raise SystemExit(1)

    # 2) 变异：检测到次格却忽略主格、仍返回 x（=旧行为，光标可停半个字），用例必须失败
    mutated = func.replace(
        "    if (cell_is_wide_trail(line, x, &lead)) return lead;",
        "    if (cell_is_wide_trail(line, x, &lead)) { /*MUTANT*/ }")
    if mutated == func:
        raise SystemExit("变异未生效（找不到替换点）")
    rc = build_and_run(mutated, "mutant")
    if rc == 0:
        raise SystemExit("变异居然通过——测试未能捕获『光标停在半个汉字』的回归！")
    print("[ok] 变异(恒返回x) 被测试正确捕获")

    # 3) input.c 接线断言：所有会移动复制光标的入口都做了整字化
    isrc = open(INPUT_C, encoding="utf-8").read()
    wiring = [
        "copy_cursor_to_lead",   # 整字化纯函数被调用
        "copy_snap_cursor_to_char(p, s)",  # 上下移动后整字化
    ]
    for tok in wiring:
        if tok not in isrc:
            raise SystemExit("input.c 缺少接线: " + tok)
    # 上下移动必须各调用一次整字化（k/K 与 j/J 两个分支）
    if isrc.count("copy_snap_cursor_to_char(p, s);") < 2:
        raise SystemExit("VK_UP / VK_DOWN 两分支未都调用 copy_snap_cursor_to_char")
    # 鼠标普通拖选与快速复制两角：光标点用 copy_cursor_to_lead 吸主格（显示）。
    if isrc.count("copy_cursor_to_lead(") < 3:
        raise SystemExit("鼠标点选/入口 的光标 未经过 copy_cursor_to_lead")
    # v1.8.26：选区【端点】必须与光标分离，存原始鼠标列（g_copy_end_x），
    # 交给渲染/复制 snap 按方向整字扩展，否则向右拖会把点到的汉字漏掉（偏左一格）。
    if "g_copy_end_x" not in isrc:
        raise SystemExit("input.c 缺少选区端点 g_copy_end_x（端点/光标未分离）")
    # 第二角（终点）必须记原始点击列，而不是吸到主格的 g_copy_cx
    if "g_copy_end_x = click_x;" not in isrc:
        raise SystemExit("鼠标第二角终点未记原始列（g_copy_end_x = click_x 缺失）")
    # 普通拖选（g_mouse_selecting）不得再对端点强制 copy_cursor_to_lead（保留
    # 原始列，snap 按方向扩展）。取拖选段：从 g_mouse_sel_sx 首次赋值往前一点。
    drag_idx = isrc.find("g_mouse_sel_sx = cur_x;")
    drag_head = isrc.rfind("FROM_LEFT_1ST_BUTTON_PRESSED", 0, drag_idx)
    drag_seg = isrc[drag_head:drag_idx + 40]
    if "copy_cursor_to_lead" in drag_seg:
        raise SystemExit("普通拖选端点不应强制吸主格（会导致选区偏左一格）")
    # yank 必须用选区端点 g_copy_end_x，而不是光标 g_copy_cx
    yank_seg = isrc[isrc.find("static void copy_mode_yank("):isrc.find("static void copy_mode_yank(") + 600]
    if "g_copy_end_x" not in yank_seg:
        raise SystemExit("copy_mode_yank 未用 g_copy_end_x 作为复制端点")
    print("[ok] 光标吸主格显示、选区端点 g_copy_end_x 记原始列（向右拖不漏字/不偏左）")

    print("[OK] verify_copy_cursor passed")


if __name__ == "__main__":
    main()
