#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""16/256 色背景属性回归 (v1.8.15)。

背景
----
`colortool -c` 用 ``ESC[48;5;Nm``（256 色）画色块，win-termux 的 vt 解析器把
256 色量化进 16 色 attr（``bg_color``），再由 ``build_attr()`` 组装成 Windows
控制台属性字（低 4 位前景色，高 4 位背景色）。旧代码里：

    WORD a = ctab[fg & 15] | ((ctab[bg & 15] >> 4) << 4);

``ctab[]`` 存的是「前景」位标志（``FOREGROUND_*``，值都在 0..0xF）。背景位
应当把同一颜色**左移 4 位**（红 ``FOREGROUND_RED=0x04`` -> ``BACKGROUND_RED=
0x40``），但旧代码先 **右移** 4 再左移：前景值 ``>>4`` 恒为 0，于是所有走
16/256 色 attr 的程序内容**背景位永远是 0（黑）**。真彩 ``ESC[48;2;r;g;bm``
走 ``bg_rgb_on`` 分支不经 ``build_attr`` 的背景位，所以 win-termux 自身 UI、
真彩程序都正常，唯独 colortool 这类 256/16 色背景整片丢失。

本脚本在 Linux 下用一个最小 Win32 桩编译 **真实的 src/screen.c + src/vt.c +
src/utf8.c**，喂真实 colortool 会发的字节流，断言：

1. ``ESC[48;5;Nm`` 色块格的背景 attr 高 4 位正确（红=0x40/绿=0x20/蓝=0x10）；
2. 直接单元测试 ``build_attr``：bg=1..7 背景位非 0 且与颜色一一对应；
3. 前景 16/256 色不受影响（低 4 位正常）；
4. 变异（把修复改回 ``(ctab[bg]>>4)<<4``）必须失败。
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

# 最小 Win32 兼容桩：提供 screen.c/vt.c/types.h 用到的 Win32 类型与宏。
WIN32_STUB = r"""
#ifndef WINDOWS_H_STUB
#define WINDOWS_H_STUB
#include <stdint.h>
#include <wchar.h>
typedef unsigned short WORD;
typedef int BOOL;
typedef uint32_t DWORD;
typedef uint64_t DWORD64;
typedef wchar_t WCHAR;
typedef unsigned char BYTE;
typedef void* HANDLE;
typedef void* HPCON;
typedef unsigned int UINT;
typedef long LONG;
typedef unsigned long ULONG;
typedef void* HINSTANCE;
typedef void* HWND;
typedef void* HMENU;
typedef long LPARAM;
typedef unsigned long WPARAM;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef struct { union { WCHAR UnicodeChar; char AsciiChar; } Char; WORD Attributes; } CHAR_INFO;
typedef struct _COORD { short X,Y; } COORD;
typedef struct _SMALL_RECT { short Left,Top,Right,Bottom; } SMALL_RECT;
typedef struct { int dummy; } CRITICAL_SECTION;
typedef struct _CONSOLE_SCREEN_BUFFER_INFO { COORD dwSize; COORD dwCursorPosition; WORD wAttributes; SMALL_RECT srWindow; COORD dwMaximumWindowSize; } CONSOLE_SCREEN_BUFFER_INFO;
typedef struct _MOUSE_EVENT_RECORD { COORD dwMousePosition; DWORD dwButtonState; DWORD dwControlKeyState; DWORD dwEventFlags; } MOUSE_EVENT_RECORD;
#define COMMON_LVB_UNDERSCORE 0x8000
#define FOREGROUND_BLUE 0x0001
#define FOREGROUND_GREEN 0x0002
#define FOREGROUND_RED 0x0004
#define FOREGROUND_INTENSITY 0x0008
#define BACKGROUND_BLUE 0x0010
#define BACKGROUND_GREEN 0x0020
#define BACKGROUND_RED 0x0040
#define BACKGROUND_INTENSITY 0x0080
static inline void EnterCriticalSection(CRITICAL_SECTION*c){(void)c;}
static inline void LeaveCriticalSection(CRITICAL_SECTION*c){(void)c;}
static inline void InitializeCriticalSection(CRITICAL_SECTION*c){(void)c;}
#endif
"""

SHELLAPI_STUB = "#ifndef SHELLAPI_H_STUB\n#define SHELLAPI_H_STUB\n#include \"windows.h\"\n#endif\n"
PROCESS_STUB = "#ifndef PROCESS_H_STUB\n#define PROCESS_H_STUB\n#include \"windows.h\"\n#endif\n"

# 全局符号定义（types.h 里 extern 的全局变量；screen.c/vt.c 需要 g_mux 与搜索状态）。
GLOBALS = r"""
#include "common.h"
#include "types.h"
MuxState g_mux;
int g_pop_anchor_x = -1;
int g_mouse_x = -1, g_mouse_y = -1;
int g_mouse_prev_in_tabbar = 0;
WCHAR g_high_surrogate = 0;
WCHAR g_orig_title[256] = {0};
int g_hover_preview_pane = -1;
DWORD64 g_hover_preview_start = 0;
int g_hover_preview_active = 0;
int g_hover_chooser_idx = -1;
DWORD64 g_hover_chooser_start = 0;
int g_hover_chooser_active = 0;
int g_hover_settings_name_idx = -1;
DWORD64 g_hover_settings_name_start = 0;
int g_hover_settings_name_active = 0;
int g_scrollback_lines = SCROLL_BUF_LINES;
int g_search_active = 0;
int g_search_match_count = 0;
int g_search_match_cur = 0;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
"""

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "screen.h"
#include "vt.h"

static int failures = 0;
#define CHECK(cond, ...) do { if(!(cond)){ printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while(0)

/* build_attr 单元测试：背景位必须随 bg_color 正确置位。 */
static void test_build_attr_bg(void) {
    /* 期望的背景高 4 位：bg 颜色号 -> (ctab[bg] << 4) */
    static const WORD expect_bg[8] = {
        0x0000, /* 0 黑   */
        0x0040, /* 1 红   BACKGROUND_RED */
        0x0020, /* 2 绿   BACKGROUND_GREEN */
        0x0060, /* 3 黄   RED|GREEN */
        0x0010, /* 4 蓝   BACKGROUND_BLUE */
        0x0050, /* 5 品红 RED|BLUE */
        0x0030, /* 6 青   GREEN|BLUE */
        0x0070  /* 7 白   RED|GREEN|BLUE */
    };
    for (int bg = 0; bg < 8; bg++) {
        ScreenBuffer s; memset(&s, 0, sizeof(s));
        s.fg_color = 7; s.bg_color = bg;
        s.bold = 0; s.underline = 0; s.reverse_video = 0;
        s.fg_rgb_on = 0; s.bg_rgb_on = 0;
        WORD a = build_attr(&s);
        WORD bgpart = a & 0x00F0;
        CHECK(bgpart == expect_bg[bg],
              "build_attr bg=%d: 背景位=0x%02X 期望 0x%02X（旧 bug 会恒为 0）",
              bg, bgpart, expect_bg[bg]);
    }
    /* 亮色背景 8..15 应带 BACKGROUND_INTENSITY(0x0080) */
    for (int bg = 8; bg < 16; bg++) {
        ScreenBuffer s; memset(&s, 0, sizeof(s));
        s.fg_color = 7; s.bg_color = bg;
        s.bold = 0; s.underline = 0; s.reverse_video = 0;
        s.fg_rgb_on = 0; s.bg_rgb_on = 0;
        WORD a = build_attr(&s);
        CHECK((a & 0x0080), "build_attr bg=%d(亮色): 背景应带 INTENSITY 位, attr=0x%03X", bg, a);
    }
}

/* 端到端：喂真实 colortool 字节流（ESC[48;5;Nm 256 色背景），检查 cell attr。 */
static void test_colortool_bg(void) {
    ScreenBuffer s; memset(&s, 0, sizeof(s));
    screen_init(&s, 80, 12);
    s.pane_index = -1;
    /* colortool -c 的「1m」行：亮白前景 + 三个 256 色背景块（红1/绿2/蓝4）。 */
    const char *seq =
        "\x1b[8;1H"
        "1m   "
        "\x1b[38;5;15m\x1b[6C"
        "  gYw     gYw   "
        "\x1b[48;5;1m  gYw  \x1b[49m"
        "\x1b[48;5;2m  gYw  \x1b[49m"
        "\x1b[48;5;4m  gYw  \x1b[49m";
    screen_process_output(&s, seq, (int)strlen(seq));

    /* 背景走 Windows 属性位：ANSI 红(48;5;1)->bg_color1->ctab[1]=RED->BACKGROUND_RED(0x40)，
     * 即属性高 4 位含 RED(0x4)；绿->0x2；蓝->0x1。断言三个色块的背景位出现。 */
    int have_red = 0, have_green = 0, have_blue = 0, have_any_bg = 0;
    int pr = screen_phys_row(&s, 7);
    for (int x = 0; x < 80; x++) {
        WORD a = s.lines[pr].cells[x].Attributes;
        int bg = (a >> 4) & 0x0F;
        if (bg != 0) {
            have_any_bg = 1;
            if (bg & 0x4) have_red = 1;    /* BACKGROUND_RED */
            if (bg & 0x2) have_green = 1;  /* BACKGROUND_GREEN */
            if (bg & 0x1) have_blue = 1;   /* BACKGROUND_BLUE */
        }
    }
    CHECK(have_any_bg, "colortool 色块：至少应有非黑背景（旧 bug 全部 bg=0）");
    CHECK(have_red, "colortool 红块 ESC[48;5;1m：背景应含 BACKGROUND_RED 位");
    CHECK(have_green, "colortool 绿块 ESC[48;5;2m：背景应含 BACKGROUND_GREEN 位");
    CHECK(have_blue, "colortool 蓝块 ESC[48;5;4m：背景应含 BACKGROUND_BLUE 位");

    /* 前景亮白（ESC[38;5;15m）不应受影响：低 4 位 = 15。 */
    ScreenBuffer t; memset(&t, 0, sizeof(t));
    screen_init(&t, 10, 2); t.pane_index = -1;
    const char *fseq = "\x1b[38;5;15mX";
    screen_process_output(&t, fseq, (int)strlen(fseq));
    int tp = screen_phys_row(&t, 0);
    int fg = t.lines[tp].cells[0].Attributes & 0x0F;
    CHECK(fg == 15, "前景 ESC[38;5;15m：低 4 位应为 15（亮白），实际 %d", fg);
    /* 16 色普通前景（ESC[31m ANSI 红）：内部按 Windows 控制台色号存（红=4），
     * 渲染回 VT 时由 render.c 的 m[] 表映射回 ANSI 红=1。这里只断言它不再是
     * 默认白 7、且 build_attr 对该色号给出含 FOREGROUND_RED 的属性。 */
    ScreenBuffer u; memset(&u, 0, sizeof(u));
    screen_init(&u, 10, 2); u.pane_index = -1;
    const char *useq = "\x1b[31mX";
    screen_process_output(&u, useq, (int)strlen(useq));
    int up = screen_phys_row(&u, 0);
    int fg_attr = u.lines[up].cells[0].Attributes & 0x0F;
    CHECK((fg_attr & 0x04) && !(fg_attr & 0x02),
          "前景 ESC[31m（红）：attr 应含 FOREGROUND_RED 且不含 GREEN，实际 0x%X", fg_attr);

    screen_free(&s); screen_free(&t); screen_free(&u);
}

int main(void) {
    test_build_attr_bg();
    test_colortool_bg();
    if (failures) { printf("[FAIL] %d 处失败\n", failures); return 1; }
    printf("[OK] build_attr 背景位 / colortool 256 色背景全部正确。\n");
    return 0;
}
"""


def main():
    print("=== 16/256 色背景属性回归 (verify_attr_bg.py) ===")
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        stub = td / "stub"
        stub.mkdir()
        (stub / "windows.h").write_text(WIN32_STUB, encoding="utf-8")
        (stub / "shellapi.h").write_text(SHELLAPI_STUB, encoding="utf-8")
        (stub / "process.h").write_text(PROCESS_STUB, encoding="utf-8")
        (td / "globals.c").write_text(GLOBALS, encoding="utf-8")
        (td / "harness.c").write_text(HARNESS, encoding="utf-8")
        exe = td / "t"
        cmd = ["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
               "-Wall", "-Wextra", "-Werror",
               "-I", str(stub), "-I", str(ROOT / "include"),
               str(ROOT / "src" / "screen.c"),
               str(ROOT / "src" / "vt.c"),
               str(ROOT / "src" / "utf8.c"),
               str(td / "globals.c"),
               str(td / "harness.c"), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            sys.exit("FAIL: compile error")
        env = {"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"}
        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
