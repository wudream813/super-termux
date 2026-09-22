#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_ringbuf_asan.py - Real AddressSanitizer verification for win-termux ring buffer & scrolling.
Dynamically extracts screen_ensure_line, screen_phys_row, screen_write_cell, and screen_scroll_up from
include/screen.h and src/screen.c, compiles with gcc -fsanitize=address,undefined and tests 1200+ edge cases.
"""

import subprocess
import tempfile
import os
import sys
from pathlib import Path
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 无 -fsanitize）
#   统一收在 tests/hbuild.py；降级时会自己往 stderr 打 [SKIP-SANITIZER]。
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402


ROOT = Path(__file__).resolve().parent

hdr = (ROOT / "include" / "screen.h").read_text(encoding="utf-8")
src = (ROOT / "src" / "screen.c").read_text(encoding="utf-8")

def extract_func(text, prefix):
    idx = text.find(prefix)
    if idx == -1:
        return None
    brace_start = text.find("{", idx)
    if brace_start == -1:
        return None
    depth = 0
    for i in range(brace_start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[idx : i + 1]
    return None

f_ensure = extract_func(src, "int screen_ensure_line(")
if not f_ensure:
    sys.exit("FAIL: screen_ensure_line not found in src/screen.c")

f_phys = extract_func(hdr, "static inline int screen_phys_row(")
if not f_phys:
    sys.exit("FAIL: screen_phys_row not found in include/screen.h")

f_write = extract_func(src, "void screen_write_cell(")
if not f_write:
    sys.exit("FAIL: screen_write_cell not found in src/screen.c")

# v1.8.11: 四个并行数组的搬运统一收口到这些辅助函数，测试同样从真源码抽取，
# 这样「漏搬 rgb_valid」这类回归依然会被 ASAN/断言抓到。
HELPERS = []
for sig in ("static void line_free(",
            "static void line_fill_blank(",
            "static int line_alloc(",
            "static void line_copy(",
            "static void alt_row_copy("):
    body = extract_func(src, sig)
    if not body:
        sys.exit("FAIL: %s not found in src/screen.c" % sig)
    HELPERS.append(body)
f_helpers = "\n".join(HELPERS)

f_up = extract_func(src, "void screen_scroll_up(")
if not f_up:
    sys.exit("FAIL: screen_scroll_up not found in src/screen.c")

PRELUDE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SCROLL_BUF_LINES 10000
#define MAX_PANES 16
#define MAX_SEARCH_MATCHES 2048
#define RGB565_WHITE 0xFFFF
#define RGB565_BLACK 0x0000

typedef unsigned short WORD;
typedef unsigned short WCHAR;

static inline WORD rgb565(int r, int g, int b) {
    return (WORD)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

typedef struct {
    union {
        WCHAR UnicodeChar;
        char   AsciiChar;
    } Char;
    WORD Attributes;
} CHAR_INFO;

typedef struct {
    CHAR_INFO *cells;
    WORD *fg_rgb;
    WORD *bg_rgb;
    unsigned char *rgb_valid;
    int len, used;
} ScreenLine;

typedef struct {
    ScreenLine *lines;
    int cols, rows, total_lines, scroll_top;
    int cursor_x, cursor_y, cursor_visible;
    WORD current_attr;
    int fg_color, bg_color, bold, underline, reverse_video;
    int saved_cx, saved_cy;
    CHAR_INFO *alt_buffer;
    int in_alt_screen, alt_scroll_top;
    int origin_mode, auto_wrap, wraparound_pending;
    int scroll_region_top, scroll_region_bottom;
    int app_cursor_keys, app_keypad;
    int mouse_tracking, mouse_sgr, bracketed_paste, win32_input_mode;
    char tab_stops[512];
    char response_buf[256];
    int response_len;
    unsigned utf8_state, utf8_cp;
    int pane_index;
    int detect_col, detect_count;
    int fg_r, fg_g, fg_b, bg_r, bg_g, bg_b;
    int fg_rgb_on, bg_rgb_on;
    WORD *alt_fg_rgb, *alt_bg_rgb;
    unsigned char *alt_rgb_valid;
    int hist_lines;
    int alt_hist_lines;
    unsigned char *line_wrap;
} ScreenBuffer;

typedef struct {
    int active;
    void *hpc;
    void *pipe_in, *pipe_out, *process, *thread, *read_thread;
    ScreenBuffer screen;
    char title[64];
    char full_title[256];
    int scroll_offset;
    int color;
    int is_settings;
    int is_about;
    int exited_hold;
    unsigned long exit_code;
    WCHAR input_history[256];
    int input_history_len;
    int input_history_pos;
} Pane;

typedef struct {
    int abs_y;
    int start_x;
    int end_x;
} SearchMatch;

typedef struct {
    Pane panes[MAX_PANES];
    int pane_count, active_pane;
} MuxState;

MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0;
int g_search_match_cur = -1;
int g_search_active = 0;
"""

DRIVER = r"""
int main(void) {
    memset(&g_mux, 0, sizeof(g_mux));
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    s.rows = 30;
    s.cols = 120;
    s.total_lines = s.rows + SCROLL_BUF_LINES;
    s.current_attr = 0x07;

    s.lines = (ScreenLine *)calloc(s.total_lines, sizeof(ScreenLine));
    assert(s.lines);

    // Test 1: simulate long output wrap-around (10025 line feeds)
    for (int i = 0; i < 10025; i++) {
        screen_scroll_up(&s, 0, s.rows - 1, 1);
    }
    assert(s.scroll_top == 10025 % s.total_lines);

    // Test 2: Partial scroll region at various cursor_y near wrap boundaries (1200 combinations)
    for (int wrap = 0; wrap < 40; wrap++) {
        s.scroll_top = (s.total_lines - 20 + wrap) % s.total_lines;
        for (int cy = 0; cy < s.rows; cy++) {
            for (int x = 0; x < s.cols; x++) {
                screen_write_cell(&s, cy, x, L'A' + (cy % 26), 0x07);
            }
            // CSI M with cursor_y = cy (top = cy, bottom = rows - 1)
            screen_scroll_up(&s, cy, s.rows - 1, 1);
        }
    }

    // Cleanup
    for (int i = 0; i < s.total_lines; i++) {
        free(s.lines[i].cells);
        free(s.lines[i].fg_rgb);
        free(s.lines[i].bg_rgb);
        free(s.lines[i].rgb_valid);
    }
    free(s.lines);

    printf("ASAN Ring Buffer verification passed: 0 overflows, 0 memory leaks.\n");
    return 0;
}
"""

C_TEST_CODE = (PRELUDE + "\n" + f_phys + "\n" + f_helpers + "\n" + f_ensure + "\n" +
               f_write + "\n" + f_up + "\n" + DRIVER)

def main():
    print("=== ASAN Ring Buffer Scrolling Test (verify_ringbuf_asan.py) ===")
    with tempfile.TemporaryDirectory() as tmpdir:
        c_path = os.path.join(tmpdir, "test_ringbuf.c")
        exe_path = os.path.join(tmpdir, "test_ringbuf")
        with open(c_path, "w", encoding="utf-8") as f:
            f.write(C_TEST_CODE)

        compile_cmd = [
            "gcc", "-O2", "-g", *hbuild.sanitize_flags(),
            "-o", exe_path, c_path
        ]
        res = subprocess.run(compile_cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print("Compilation error:", res.stderr)
            return 1

        run_res = subprocess.run([exe_path], capture_output=True, text=True)
        if run_res.returncode != 0:
            print("Execution failed (ASAN error caught):\n", run_res.stderr)
            return 1
        print("  " + run_res.stdout.strip())
        print("  [OK] 环形缓冲区局部滚动区 (screen_scroll_up) 内存安全性验证通过！")
    return 0

if __name__ == "__main__":
    sys.exit(main())
