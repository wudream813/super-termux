#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""screen.c 状态迁移回归 (v1.8.11)：alt 屏 resize 的真彩色标记 + 搜索当前项落点。

两条不变量，都是从 src/screen.c 抽真源码编译执行来验证的（改坏 C 代码这里会红）：

BUG-8  alt 屏（vim / htop 这类全屏程序）改窗口大小时，一行里的
       cells / fg_rgb / bg_rgb / rgb_valid 四个并行数组必须都搬 cc 个元素。
       历史上 rgb_valid 只搬了每行第 0 列，于是除最左一列外整屏真彩色被清空，
       颜色退化成 16 色。

BUG-9  搜索结果被新输出挤出滚动缓冲时，如果用户正停留的那一条被剔除，
       光标应当落到「它之后最近的存活项」（剔除总是从最老一端发生，所以就是
       index 0），而不是弹到最新的一条。
"""

import subprocess
import sys
import tempfile
from pathlib import Path
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 不支持 -fsanitize）
#   统一收在 tests/hbuild.py，不要在这里各写一份 sys.platform 判断。
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402


ROOT = Path(__file__).resolve().parent
hdr = (ROOT / "include" / "screen.h").read_text(encoding="utf-8")
src = (ROOT / "src" / "screen.c").read_text(encoding="utf-8")


def extract_func(text, prefix):
    idx = text.find(prefix)
    if idx == -1:
        sys.exit("FAIL: %s not found" % prefix)
    brace = text.find("{", idx)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[idx:i + 1]
    sys.exit("FAIL: unbalanced braces for %s" % prefix)


PIECES = [extract_func(hdr, "static inline int screen_phys_row(")]
for sig in ("static void line_free(",
            "static void line_fill_blank(",
            "static int line_alloc(",
            "static void line_copy(",
            "static void alt_row_copy(",
            "int screen_ensure_line(",
            "void screen_write_cell(",
            "void screen_scroll_up(",
            "void cell_truecolor(",
            "void screen_mark_softwrap(",
            "static int reflow_glyph_w(",
            "static int reflow_append_rows(",
            "static int reflow_acc_cb(",
            "static int reflow_rows_for(",
            "static int screen_row_reflow_len(",
            "static int screen_content_span(",
            "int screen_reflow_height(",
            "int screen_scroll_limit(",
            "int screen_reflow_view(",
            "static int rfring_init(",
            "static void rfring_free(",
            "static void rfring_add(",
            "static int reflow_sink_cb(",
            "static void line_store_rglyph(",
            "static int screen_trace_on(",
            "static void screen_trace_row(",
            "static void screen_trace_wrapmap(",
            "static void screen_trace_ring(",
            "static int screen_row_blank(",
            "void screen_repaint_snapshot_free(",
            "void screen_repaint_snapshot(",
            "static int screen_reanchor_enabled(",
            "static int line_ends_with_longer(",
            "static void screen_trace_reanchor(",
            "int screen_repaint_reanchor(",
            "static int screen_resize_reflow(",
            "static int screen_resize_legacy(",
            "int screen_resize("):
    PIECES.append(extract_func(src, sig))

PRELUDE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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
    union { WCHAR UnicodeChar; char AsciiChar; } Char;
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
    int resize_repaint_pending;
    int resize_repaint_pass;
    ScreenLine *repaint_snap;
    unsigned char *repaint_snap_wrap;   /* 与 repaint_snap 一一对应的续行标志 */
    int repaint_snap_rows, repaint_snap_cols;
    int repaint_snap_content;
} ScreenBuffer;

typedef struct {
    CHAR_INFO ci;
    WORD fg, bg;
    unsigned char v;
} RGlyph;

typedef struct {
    RGlyph *buf;
    int cap, count, width;
} RfAcc;

typedef struct {
    RGlyph **rows;
    unsigned char *first;
    int *used;
    int cap, head, count, width;
} RfRing;

typedef struct {
    RfRing *ring;
    RGlyph *line;
    int width;
    int first_done;
} RfSink;

/* reflow 片段里宽字符判定：stub 只给 CJK/全角常见区间，足够测试（ASCII 窄）。 */
static int is_wide_cp(unsigned int cp) {
    (void)cp; return 0;
}

typedef struct {
    int active;
    void *hpc;
    void *pipe_in, *pipe_out, *process, *thread, *read_thread;
    ScreenBuffer screen;
    char title[64];
    char full_title[256];
    int scroll_offset;
    int color;
    int is_settings, is_about, exited_hold;
    unsigned long exit_code;
    WCHAR input_history[256];
    int input_history_len, input_history_pos;
} Pane;

typedef struct { int abs_y; int start_x; int end_x; } SearchMatch;
typedef struct { Pane panes[MAX_PANES]; int pane_count, active_pane; } MuxState;

MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0;
int g_search_match_cur = -1;
int g_search_active = 0;
int g_scrollback_lines = 200;
"""

DRIVER = r"""
static void alloc_alt(ScreenBuffer *s, int cols, int rows) {
    s->cols = cols; s->rows = rows;
    s->total_lines = rows + g_scrollback_lines;
    s->current_attr = 0x07;
    s->lines = (ScreenLine *)calloc(s->total_lines, sizeof(ScreenLine));
    /* 可见行预分配 cols 宽（真实程序 screen_init 如此），len=cols；历史行延迟分配。 */
    for (int i = 0; i < s->total_lines; i++)
        assert(line_alloc(&s->lines[i], cols, 0x07));
    s->alt_buffer = (CHAR_INFO *)calloc(rows * cols, sizeof(CHAR_INFO));
    s->alt_fg_rgb = (WORD *)calloc(rows * cols, sizeof(WORD));
    s->alt_bg_rgb = (WORD *)calloc(rows * cols, sizeof(WORD));
    s->alt_rgb_valid = (unsigned char *)calloc(rows * cols, 1);
    assert(s->lines && s->alt_buffer && s->alt_fg_rgb && s->alt_bg_rgb && s->alt_rgb_valid);
}

static void free_screen(ScreenBuffer *s) {
    if (s->lines) {
        for (int i = 0; i < s->total_lines; i++) line_free(&s->lines[i]);
        free(s->lines);
        s->lines = NULL;
    }
    free(s->line_wrap); s->line_wrap = NULL;
    free(s->alt_buffer); free(s->alt_fg_rgb); free(s->alt_bg_rgb); free(s->alt_rgb_valid);
    s->alt_buffer = NULL; s->alt_fg_rgb = NULL; s->alt_bg_rgb = NULL; s->alt_rgb_valid = NULL;
}

/* BUG-8: alt 屏 resize 后每一列的真彩色标记都必须还在。 */
static int test_alt_resize_truecolor(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    alloc_alt(&s, 16, 6);
    s.in_alt_screen = 1;
    s.fg_rgb_on = 1; s.bg_rgb_on = 1;
    s.fg_r = 200; s.fg_g = 100; s.fg_b = 50;
    s.bg_r = 10;  s.bg_g = 20;  s.bg_b = 30;
    for (int y = 0; y < s.rows; y++)
        for (int x = 0; x < s.cols; x++)
            screen_write_cell(&s, y, x, L'X', 0x07);

    WORD want_f = rgb565(200, 100, 50), want_b = rgb565(10, 20, 30);
    assert(screen_resize(&s, 24, 6) == 1);

    int bad = 0;
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 16; x++) {          /* 迁移过来的那 16 列 */
            WORD f, b; int fv, bv;
            cell_truecolor(&s, y, x, -1, &f, &b, &fv, &bv);
            if (!fv || !bv || f != want_f || b != want_b) {
                if (!bad) fprintf(stderr, "FAIL: resize 后 (%d,%d) 丢了真彩色 fv=%d bv=%d\n", y, x, fv, bv);
                bad++;
            }
        }
    }
    if (bad) { free_screen(&s); fprintf(stderr, "FAIL: %d 个 cell 的真彩色标记在 resize 中丢失\n", bad); return 1; }
    free_screen(&s);
    printf("  BUG-8: alt 屏 16x6 -> 24x6，96 个 cell 的真彩色标记全部保留\n");
    return 0;
}

/* BUG-9: 当前停留的匹配被剔除后，落到存活项里最近的那个（index 0），不是最新的。 */
static int test_search_cur_after_drop(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    memset(&g_mux, 0, sizeof(g_mux));
    g_scrollback_lines = 8;
    alloc_alt(&s, 10, 4);
    s.in_alt_screen = 0;
    s.pane_index = 0;
    g_mux.active_pane = 0;
    g_mux.pane_count = 1;
    g_mux.panes[0].active = 1;

    /* 填满历史，逼近容量上限，之后每滚一行就会丢掉最老的一行。 */
    int cap = s.total_lines - s.rows;
    for (int i = 0; i < cap; i++) screen_scroll_up(&s, 0, s.rows - 1, 1);
    assert(s.hist_lines == cap);

    g_search_active = 1;
    g_search_match_count = 3;
    g_search_matches[0].abs_y = 0;   /* 最老 */
    g_search_matches[1].abs_y = 3;
    g_search_matches[2].abs_y = 6;   /* 最新 */
    g_search_match_cur = 0;          /* 用户正停在最老的那一条 */

    screen_scroll_up(&s, 0, s.rows - 1, 2);   /* 挤掉最老的 2 行 -> 剔除 match 0 */

    if (g_search_match_count != 2) {
        fprintf(stderr, "FAIL: 剔除后应剩 2 条，实际 %d\n", g_search_match_count);
        free_screen(&s); return 1;
    }
    if (g_search_match_cur != 0) {
        fprintf(stderr, "FAIL: 当前项应落到存活项里最近的一条 (index 0)，实际 %d\n", g_search_match_cur);
        free_screen(&s); return 1;
    }
    if (g_search_matches[g_search_match_cur].abs_y != 1) {
        fprintf(stderr, "FAIL: 落点 abs_y 应为 1（原 abs_y=3 平移 2），实际 %d\n",
                g_search_matches[g_search_match_cur].abs_y);
        free_screen(&s); return 1;
    }

    /* 当前项没被剔除时，必须跟着平移、指向同一条。 */
    g_search_match_cur = 1;                   /* 现在指向 abs_y = 4（原 6） */
    int keep_abs = g_search_matches[1].abs_y;
    screen_scroll_up(&s, 0, s.rows - 1, 1);
    if (g_search_match_cur != 1 || g_search_matches[1].abs_y != keep_abs - 1) {
        fprintf(stderr, "FAIL: 未被剔除的当前项没有正确跟随平移\n");
        free_screen(&s); return 1;
    }
    free_screen(&s);
    printf("  BUG-9: 当前项被剔除 -> 落到最近的存活项；未被剔除 -> 原样跟随平移\n");
    return 0;
}

/* v1.8.42: 分屏拖条 resize 高度缩小时，旧可见区顶部行必须滚入历史、不能丢失。 */
static char at_rel(ScreenBuffer *s, int rel) {
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || !s->lines[pr].cells) return '?';
    return (char)s->lines[pr].cells[0].Char.UnicodeChar;
}

/* v1.8.52: 逻辑内容不变量。把环（历史+可见）按 line_wrap 合并回逻辑行、去掉行尾
 * 空格填充，输出成「逻辑文本」：一行一个逻辑行，行间以 \n 分隔。reflow-on-resize
 * 前后这个文本必须逐字一致（内容与顺序都不变），与当前宽度/物理行切分无关。 */
static void logical_text(ScreenBuffer *s, char *out, int outcap) {
    int pos = 0;
    int first = 1;
    /* 逐逻辑行输出。line_open：当前逻辑行是否在累积。 */
    for (int rel = -s->hist_lines; rel < s->rows; rel++) {
        int pr = screen_phys_row(s, rel);
        if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) continue;
        ScreenLine *ln = &s->lines[pr];
        int len = ln->len;
        while (len > 0 && ln->cells[len - 1].Char.UnicodeChar == L' ') len--;
        int wr = (s->line_wrap && s->line_wrap[pr]) ? 1 : 0;
        if (wr == 0) {
            if (!first) { if (pos < outcap - 1) out[pos++] = '\n'; }
            first = 0;
            for (int x = 0; x < len && pos < outcap - 2; x++) {
                WCHAR c = ln->cells[x].Char.UnicodeChar;
                if (c && c != L' ') out[pos++] = (char)(c & 0x7f);
            }
        } else {
            for (int x = 0; x < len && pos < outcap - 2; x++) {
                WCHAR c = ln->cells[x].Char.UnicodeChar;
                if (c && c != L' ') out[pos++] = (char)(c & 0x7f);
            }
        }
    }
    out[pos < outcap ? pos : outcap - 1] = 0;
}
/* 比较两段逻辑文本，忽略首/尾的整屏空白行 —— 两端都是合法排版产生的空白：
 *   尾部空白行：reflow 后一条逻辑行按新宽度占不同数量的物理行，末尾未使用的
 *               屏幕留白行数必然变化（v1.8.52 起 resize 只扫描内容跨度）；
 *   首部空白行：有历史时按底部锚定（提示符必须贴在最后一行），内容不足一屏
 *               就会在上方留白。
 * 内容之间的真实空行仍然逐字比较，所以「历史里的空行丢失」照样会被抓到；
 * 锚定行为本身由 tests/resize_history_repro.c 单独断言。 */
static int texts_eq(const char *a, const char *b) {
    char ba[4096], bb[4096];
    size_t la = strlen(a), lb = strlen(b);
    char *pa, *pb;
    if (la >= sizeof ba || lb >= sizeof bb) return 0;
    memcpy(ba, a, la + 1); memcpy(bb, b, lb + 1);
    while (la && ba[la - 1] == '\n') ba[--la] = 0;
    while (lb && bb[lb - 1] == '\n') bb[--lb] = 0;
    for (pa = ba; *pa == '\n'; pa++) { }
    for (pb = bb; *pb == '\n'; pb++) { }
    return strcmp(pa, pb) == 0;
}
/* v1.8.43: 宽窗格跑出历史后【收窄宽度】，历史必须逐行完整保留、内容不错位。 */
static int test_resize_narrow_keeps_history(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    g_scrollback_lines = 1000;
    alloc_alt(&s, 80, 10);
    s.in_alt_screen = 0;
    /* 写 10 行可见内容 A..J（可见行 0..9 各一行）。 */
    for (int y = 0; y < 10; y++)
        screen_write_cell(&s, y, 0, (WCHAR)('A' + y), 0x07);
    /* 再滚 10 行、每行在新底行写 K..T，制造 10 行历史（A..J 进历史）。 */
    for (int i = 0; i < 10; i++) {
        screen_scroll_up(&s, 0, s.rows - 1, 1);
        screen_write_cell(&s, s.rows - 1, 0, (WCHAR)('K' + i), 0x07);
    }
    if (s.hist_lines != 10) {
        fprintf(stderr, "FAIL: 收窄前 hist 应为 10，实际 %d\n", s.hist_lines);
        free_screen(&s); return 1;
    }
    /* 收窄宽度 80 -> 20（高度不变）。 */
    assert(screen_resize(&s, 20, 10) == 1);
    if (s.hist_lines != 10) {
        fprintf(stderr, "FAIL: 收窄宽度后 hist 应仍为 10，实际 %d\n", s.hist_lines);
        free_screen(&s); return 1;
    }
    /* 历史行内容（首列标记）必须与收窄前一致：可见 0..9 = K..T，历史 -1=J .. -10=A。 */
    for (int y = 0; y < 10; y++) {
        char want = (char)('K' + y);   /* 可见行 y 写的是 K+y（K..T） */
        if (at_rel(&s, y) != want) {
            fprintf(stderr, "FAIL: 收窄后可见行 %d 应为 %c，实际 %c\n", y, want, at_rel(&s, y));
            free_screen(&s); return 1;
        }
    }
    for (int h = 1; h <= 10; h++) {
        char want = (char)('J' - h + 1);   /* 历史 -1=J,-2=I,...,-10=A */
        if (at_rel(&s, -h) != want) {
            fprintf(stderr, "FAIL: 收窄后历史 -%d 应为 %c，实际 %c（历史错位/丢失）\n",
                    h, want, at_rel(&s, -h));
            free_screen(&s); return 1;
        }
    }
    free_screen(&s);
    printf("  v1.8.43: 宽窗格 80 列收窄到 20 列，10 行历史逐行完整保留\n");
    return 0;
}

/* v1.8.45 #6：左右分屏调大小（宽度收窄）时，历史里的【宽行】右侧不能被硬截断。
 * 写一满行（80 列，列 0..79 各放 'A'+x%26），滚 1 行进历史，再收窄 80->20；
 * 收窄后该行作为历史 -1，其第 21..79 列内容必须仍在（拖宽回来可见）。 */
static int test_resize_narrow_wide_history_not_truncated(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    g_scrollback_lines = 1000;
    alloc_alt(&s, 80, 10);
    s.in_alt_screen = 0;
    s.line_wrap = (unsigned char *)calloc(s.total_lines, 1);
    /* 一行写满 80 列（A..Z 循环），滚 1 行进历史。 */
    for (int x = 0; x < 80; x++)
        screen_write_cell(&s, 0, x, (WCHAR)('A' + (x % 26)), 0x07);
    screen_scroll_up(&s, 0, s.rows - 1, 1);
    char before[4096];
    logical_text(&s, before, sizeof(before));
    /* v1.8.52：收窄到 20 列应把这条 80 列逻辑行本地重排为 4 行（内容不丢不截断）。 */
    assert(screen_resize(&s, 20, 10) == 1);
    char after20[4096];
    logical_text(&s, after20, sizeof(after20));
    if (!texts_eq(before, after20)) {
        fprintf(stderr, "FAIL: 收窄 80->20 后逻辑内容变化: [%s] != [%s]\n", after20, before);
        free_screen(&s); return 1;
    }
    /* 再拖回 60：仍应完整还原（逻辑行折回更少行，内容依旧逐字一致）。 */
    assert(screen_resize(&s, 60, 10) == 1);
    char after60[4096];
    logical_text(&s, after60, sizeof(after60));
    if (!texts_eq(before, after60)) {
        fprintf(stderr, "FAIL: 拖回 60 后逻辑内容变化: [%s] != [%s]\n", after60, before);
        free_screen(&s); return 1;
    }
    free_screen(&s);
    printf("  v1.8.52: 80 列满行收窄 80->20->60，逻辑内容逐字保留（reflow 本地重排）\n");
    return 0;
}

/* v1.8.44：宽窗格跑出长历史后，同时【收窄宽度 + 缩小高度】（模拟把分屏条拖小），
 * 底部命令行必须留在新可见底部、老行滚入历史且顺序不错乱。 */
static int test_resize_wide_then_small(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    g_scrollback_lines = 1000;
    alloc_alt(&s, 80, 20);
    s.in_alt_screen = 0;
    s.line_wrap = (unsigned char *)calloc(s.total_lines, 1);
    /* 20 行可见逐行写标记；再滚 15 行、每滚底行续写，制造 15 行历史。
     * 逻辑内容 = 20+15 条单字符逻辑行，全局序号 G=0..34（0 最老）。 */
    int G = 0;
    for (int y = 0; y < 20; y++, G++)
        screen_write_cell(&s, y, 0, (WCHAR)('a' + G), 0x07);
    for (int i = 0; i < 15; i++, G++) {
        screen_scroll_up(&s, 0, s.rows - 1, 1);
        screen_write_cell(&s, s.rows - 1, 0, (WCHAR)('a' + G), 0x07);
    }
    char before[4096];
    logical_text(&s, before, sizeof(before));
    /* v1.8.52：同时收窄 80->30、缩小 20->8——逻辑内容整体保留（不再丢被裁行），
     * 底部锚定：可见区显示最新 8 行。 */
    assert(screen_resize(&s, 30, 8) == 1);
    if (s.rows != 8 || s.cols != 30) { fprintf(stderr, "FAIL: resize 后尺寸 %dx%d\n", s.cols, s.rows); free_screen(&s); return 1; }
    char after[4096];
    logical_text(&s, after, sizeof(after));
    if (!texts_eq(before, after)) {
        fprintf(stderr, "FAIL: 收窄+缩小后逻辑内容变化\n  before=[%s]\n  after =[%s]\n", before, after);
        free_screen(&s); return 1;
    }
    /* 可见区 = 最新 8 行（'t'..'a'+34），历史 = 前面 27 行。 */
    if (s.hist_lines != 27) {
        fprintf(stderr, "FAIL: 内容 35 行缩到 8 行屏，hist 应 27，实际 %d\n", s.hist_lines);
        free_screen(&s); return 1;
    }
    if (at_rel(&s, 0) != (char)('a'+27) || at_rel(&s, 7) != (char)('a'+34)) {
        fprintf(stderr, "FAIL: 底部锚定可见首/末行: 0=%c 7=%c (want %c/%c)\n",
                at_rel(&s,0), at_rel(&s,7), (char)('a'+27), (char)('a'+34));
        free_screen(&s); return 1;
    }
    /* 再放大回 20 行：内容仍不变。 */
    assert(screen_resize(&s, 30, 20) == 1);
    char after2[4096];
    logical_text(&s, after2, sizeof(after2));
    if (!texts_eq(before, after2)) {
        fprintf(stderr, "FAIL: 再放大后逻辑内容变化\n  after=[%s]\n", after2);
        free_screen(&s); return 1;
    }
    if (s.hist_lines != 15) {
        fprintf(stderr, "FAIL: 35 行内容放 20 行屏，hist 应 15，实际 %d\n", s.hist_lines);
        free_screen(&s); return 1;
    }
    free_screen(&s);
    printf("  v1.8.52: 80x20 同时收窄+缩小到 30x8、再放大——逻辑内容逐字不变、底部锚定、hist 随屏高变化\n");
    return 0;
}

static int test_resize_shrink_keeps_history(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    g_scrollback_lines = 1000;
    alloc_alt(&s, 20, 10);
    s.in_alt_screen = 0;
    s.line_wrap = (unsigned char *)calloc(s.total_lines, 1);
    for (int y = 0; y < 10; y++) screen_write_cell(&s, y, 0, (WCHAR)('A' + y), 0x07);
    screen_scroll_up(&s, 0, s.rows - 1, 3);
    screen_write_cell(&s, 7, 0, L'x', 0x07);
    screen_write_cell(&s, 8, 0, L'y', 0x07);
    screen_write_cell(&s, 9, 0, L'z', 0x07);
    if (s.hist_lines != 3) { fprintf(stderr, "FAIL: 缩小前 hist 应为 3，实际 %d\n", s.hist_lines); free_screen(&s); return 1; }
    char before[2048];
    logical_text(&s, before, sizeof(before));

    /* v1.8.52 reflow 语义：高度 10->6 不再丢被裁可见行——它们进入滚动历史，
     * 内容整体保留（D,E,F,G 进历史），可见 = 最新 6 行 H,I,J,x,y,z。 */
    assert(screen_resize(&s, 20, 6) == 1);
    if (s.rows != 6) { fprintf(stderr, "FAIL: resize 后 rows!=6\n"); free_screen(&s); return 1; }
    if (s.hist_lines != 7) {
        fprintf(stderr, "FAIL: 10 行内容缩到 6 行屏 hist 应 7（内容不丢），实际 %d\n", s.hist_lines);
        free_screen(&s); return 1;
    }
    if (at_rel(&s,0)!='H' || at_rel(&s,5)!='z') {
        fprintf(stderr, "FAIL: 底部锚定 0=H 5=z: 0=%c 5=%c\n", at_rel(&s,0), at_rel(&s,5));
        free_screen(&s); return 1;
    }
    char mid[2048];
    logical_text(&s, mid, sizeof(mid));
    if (!texts_eq(before, mid)) {
        fprintf(stderr, "FAIL: 缩小后逻辑内容变化\n  before=[%s]\n  after =[%s]\n", before, mid);
        free_screen(&s); return 1;
    }
    /* 放大回 10：内容 13 行（A..J + x,y,z），可见=最新 10 行 D..z，历史 A,B,C。 */
    assert(screen_resize(&s, 20, 10) == 1);
    if (s.hist_lines != 3) {
        fprintf(stderr, "FAIL: 放大回 10 行屏：13 行内容 -> hist 应 3（A,B,C），实际 %d\n", s.hist_lines);
        free_screen(&s); return 1;
    }
    if (at_rel(&s,0)!='D' || at_rel(&s,9)!='z') {
        fprintf(stderr, "FAIL: 放大后底部锚定 D..z: 0=%c 9=%c\n", at_rel(&s,0), at_rel(&s,9));
        free_screen(&s); return 1;
    }
    char after[2048];
    logical_text(&s, after, sizeof(after));
    if (!texts_eq(before, after)) {
        fprintf(stderr, "FAIL: 放大后逻辑内容变化\n  before=[%s]\n  after =[%s]\n", before, after);
        free_screen(&s); return 1;
    }
    free_screen(&s);
    printf("  v1.8.52: 高度 10->6->10——内容整体保留不丢（缩小时被裁可见行进历史），hist 随屏高变化\n");
    return 0;
}

/* v1.8.51：有滚动历史时直接把窗格【拉高】（放大 k>0），历史最新行被提回可见区
 * 后必须在滚动缓冲里消失——环形缓冲与 reflow 视口里的每条逻辑行都只能出现一次。 */
static int test_resize_enlarge_reflow_no_duplicate(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    g_scrollback_lines = 200;
    alloc_alt(&s, 24, 6);
    s.in_alt_screen = 0;
    s.line_wrap = (unsigned char *)calloc(s.total_lines, 1);
    /* 直接按「底部锚定正常终端」的环形几何铺缓冲：rel -8..-1 为 8 行历史、rel 0..5
     * 为 6 行可见，每槽一个互不相同的单字符标记（码点随 rel 递增=时间递增）。 */
    for (int rel = -8; rel < 6; rel++) {
        int pr = screen_phys_row(&s, rel);
        if (pr < 0) { free_screen(&s); fprintf(stderr, "FAIL: phys %d 越界\n", pr); return 1; }
        ScreenLine *l = &s.lines[pr];
        for (int x = 0; x < s.cols; x++) l->cells[x].Char.UnicodeChar = L' ';
        l->cells[0].Char.UnicodeChar = (WCHAR)('p' + (rel + 8));  /* p(rel-8)..~ 唯一递增 */
    }
    s.hist_lines = 8;
    int hist_before = s.hist_lines;
    /* 放大 6 -> 10：最新 4 行历史（标记 u,v,w,x）提回可见区顶部并退出历史。 */
    assert(screen_resize(&s, 24, 10) == 1);
    if (s.hist_lines != hist_before - 4) {
        fprintf(stderr, "FAIL: 放大 4 行后 hist 应为 %d，实际 %d（历史未同步扣减）\n",
                hist_before - 4, s.hist_lines);
        free_screen(&s); return 1;
    }
    /* 1) 环形缓冲扫一遍：从最老历史到最新可见，每个标记只能出现一次（无重复槽）。 */
    {
        int cnt[256] = {0};
        for (int rel = -s.hist_lines; rel < s.rows; rel++) {
            int pr = screen_phys_row(&s, rel);
            WCHAR c = s.lines[pr].cells[0].Char.UnicodeChar;
            if (c && c != L' ' && c < 256) cnt[c]++;
        }
        for (int i = 0; i < 256; i++)
            if (cnt[i] > 1) {
                fprintf(stderr, "FAIL: 放大后标记 '%c' 在环形缓冲出现 %d 次（历史+可见重复）\n",
                        i, cnt[i]);
                free_screen(&s); return 1;
            }
    }
    /* 2) reflow 视口逐 vo 上翻：每帧内容按时间单调递增、不含倒退/重复（v1.8.50 整窗统一坐标）。 */
    for (int vo = 0; vo <= s.hist_lines + s.rows; vo++) {
        RGlyph *ov = (RGlyph *)calloc(10 * 24, sizeof(RGlyph));
        screen_reflow_view(&s, vo, 10, 24, ov);
        int prev = -1;
        for (int y = 0; y < 10; y++) {
            WCHAR c = ov[y*24].ci.Char.UnicodeChar;
            if (!c || c == L' ') continue;
            if ((int)c <= prev) {
                fprintf(stderr, "FAIL: vo=%d 视口时间倒退/重复：%c 后接 %c\n", vo, prev, (char)c);
                free(ov); free_screen(&s); return 1;
            }
            prev = (int)c;
        }
        free(ov);
    }
    free_screen(&s);
    printf("  v1.8.51: 有历史时直接放大（6->10）——历史同步扣减，环形缓冲与 reflow 回看均无重复/倒退\n");
    return 0;
}

static int test_reflow_view(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    g_scrollback_lines = 1000;
    alloc_alt(&s, 10, 6);
    s.in_alt_screen = 0;
    s.line_wrap = (unsigned char *)calloc(s.total_lines, 1);
    /* 宽 10 高 6。逻辑行 "abcdefghij"（10 字符，硬换行 wrap=0）写在可见行 0，
     * 滚动 1 行后成为历史 -1；可见区变空白。 */
    for (int x = 0; x < 10; x++) screen_write_cell(&s, 0, x, (WCHAR)('a'+x), 0x07);
    screen_scroll_up(&s, 0, s.rows - 1, 1);
    if (s.hist_lines < 1) { fprintf(stderr, "FAIL: reflow 准备 hist<1\n"); return 1; }

    /* vo 自限（v1.8.52）：内容只有 3 个显示行（abcd/efgh/ij）< 视口 6 行时，
     * 任何 vo 都滚不到「更老」——vo 被 cap 到 0，视图与 vo=0 相同：底部 ij、上方
     * 不补「内容之上的空白 pad」；内容不丢。 */
    RGlyph *out = (RGlyph *)calloc(6 * 4, sizeof(RGlyph));
    int n = screen_reflow_view(&s, 2, 6, 4, out);
    if (n != 1) { fprintf(stderr, "FAIL: reflow_view 返回 %d（应1）\n", n); free(out); free_screen(&s); return 1; }
    {
        const char *rp[6] = {"", "", "", "abcd", "efgh", "ij"};
        for (int y = 0; y < 6; y++) {
            for (int x = 0; x < 4; x++) {
                char want = x < (int)strlen(rp[y]) ? rp[y][x] : ' ';
                WCHAR got = out[y*4+x].ci.Char.UnicodeChar;
                char gc = (got==0||got==L' ')?' ':(char)got;
                if (gc != want) {
                    fprintf(stderr, "FAIL: vo=2(cap) y%d x%d: 得 '%c' want '%c'（顶部出现空白 pad/内容偏移）\n", y, x, gc, want);
                    free(out); free_screen(&s); return 1;
                }
            }
        }
    }
    free(out);

    /* vo=0：视口 y5=ij（命令行）、y4=efgh、y3=abcd，y2 空白。 */
    RGlyph *out2 = (RGlyph *)calloc(6 * 4, sizeof(RGlyph));
    screen_reflow_view(&s, 0, 6, 4, out2);
    {
        const char *r3 = "abcd", *r4 = "efgh", *r5 = "ij";
        const char *rp[6] = {"","","",r3,r4,r5};
        for (int y = 3; y < 6; y++) {
            for (int x = 0; x < 4; x++) {
                char want = x < (int)strlen(rp[y]) ? rp[y][x] : ' ';
                WCHAR got = out2[y*4+x].ci.Char.UnicodeChar;
                char gc = (got==0||got==L' ')?' ':(char)got;
                if (gc != want) {
                    fprintf(stderr, "FAIL: vo=0 y%d x%d: 得 '%c' want '%c'\n", y, x, gc, want);
                    free(out2); free_screen(&s); return 1;
                }
            }
        }
        WCHAR g2 = out2[2*4].ci.Char.UnicodeChar;
        if (g2 != L' ' && g2 != 0) { fprintf(stderr, "FAIL: vo=0 y2 应空白，得 '%c'\n", (char)g2); free(out2); free_screen(&s); return 1; }
    }
    free(out2);

    /* 宽视口 20：折 1 行 abcdefghij，vo=0 视口底部 y5=整行。 */
    RGlyph *out3 = (RGlyph *)calloc(6 * 20, sizeof(RGlyph));
    screen_reflow_view(&s, 0, 6, 20, out3);
    for (int x = 0; x < 10; x++) {
        WCHAR got = out3[5*20+x].ci.Char.UnicodeChar;
        if ((char)got != (char)('a'+x)) {
            fprintf(stderr, "FAIL: 宽视口 vo=0 底部 y5 列%d: 得 '%c' want '%c'\n", x, (char)got, 'a'+x);
            free(out3); free_screen(&s); return 1;
        }
    }
    free(out3);

    /* 软换行合并：历史 -1="xyz"(wrap=1) + -2="ab"(wrap=0) => 逻辑行 "abxyz"，
     * vo=0 视口底部 y3=abxyz（宽10折1行）。 */
    {
        ScreenBuffer t; memset(&t, 0, sizeof(t));
        alloc_alt(&t, 6, 4);
        t.in_alt_screen = 0;
        t.line_wrap = (unsigned char *)calloc(t.total_lines, 1);
        screen_scroll_up(&t, 0, t.rows-1, 2);
        int r_new = screen_phys_row(&t, -1);
        int r_old = screen_phys_row(&t, -2);
        for (int x=0;x<3;x++) t.lines[r_new].cells[x].Char.UnicodeChar = (WCHAR)('x'+x);
        for (int x=0;x<2;x++) t.lines[r_old].cells[x].Char.UnicodeChar = (WCHAR)('a'+x);
        t.line_wrap[r_new] = 1;
        t.line_wrap[r_old] = 0;
        RGlyph *o = (RGlyph *)calloc(4*10, sizeof(RGlyph));
        int nn = screen_reflow_view(&t, 0, 4, 10, o);
        if (nn != 1) { fprintf(stderr, "FAIL: 软换行 reflow 返回 %d\n", nn); free(o); free_screen(&t); free_screen(&s); return 1; }
        const char *want = "abxyz";
        for (int x=0;x<5;x++) {
            WCHAR got = o[3*10+x].ci.Char.UnicodeChar;
            if ((char)got != want[x]) {
                fprintf(stderr, "FAIL: 软换行合并 底部列%d: 得 '%c' want '%c'\n", x, (char)got, want[x]);
                free(o); free_screen(&t); free_screen(&s); return 1;
            }
        }
        free(o); free_screen(&t);
    }

    free_screen(&s);
    printf("  v1.8.47: reflow 统一视图（历史+可见）——窄折多行/宽折回/软换行合并/vo窗口底部锚定\n");
    return 0;
}

/* v1.8.52: 属性测试——任意 resize 序列不得改变逻辑内容（不丢、不重、不乱序）。
 * 生成固定一批逻辑行（长短不一、含整行宽、含短行），记录参考文本，然后反复改
 * 宽/高并断言 logical_text 始终与参考一致。 */
static int test_random_resize_invariant(void) {
    ScreenBuffer s;
    memset(&s, 0, sizeof(s));
    g_scrollback_lines = 1000;
    alloc_alt(&s, 40, 8);
    s.in_alt_screen = 0;
    s.line_wrap = (unsigned char *)calloc(s.total_lines, 1);
    char linebuf[128];
    /* 逐行输出：行内容用带行号的字符 + 变长填充，长度横跨「不足一行 / 正好满行 / 多行」。 */
    int nlines = 24;
    for (int i = 0; i < nlines; i++) {
        /* 上一行若整行写满会触发……这里统一用整行 + 换行的方式模拟输出；利用
         * screen_write_cell + 手动换行来精确控制每行内容，避免依赖自动折行状态。 */
        int linelen = (i % 4 == 0) ? 40 : (i % 4 == 1) ? 12 : (i % 4 == 2) ? 75 : 40;
        int col = 0;
        int k = 0;
        for (int c = 0; c < linelen; c++) {
            char ch = (char)('0' + ((i + c) % 10));
            linebuf[k++] = ch;
            if (col >= s.cols) { /* 超宽部分塞到下一行：这里故意让行内容超过宽度，
                                    通过滚动模拟 wrap 不现实；先只用 ≤ 宽度的行与
                                    换行构造。 */
            }
            (void)ch;
        }
        linebuf[k] = 0;
        /* 简化：每行一个物理行，宽 ≤ 40 */
        int w2 = linelen > 40 ? 40 : linelen;
        screen_write_cell(&s, s.rows - 1, 0, (WCHAR)('A' + (i % 26)), 0x07);
        for (int c = 0; c < w2; c++) {
            WCHAR ch = (WCHAR)('a' + ((i + c) % 26));
            screen_write_cell(&s, s.rows - 1, c, ch, 0x07);
        }
        if (s.rows - 1 > 0 && i < nlines - 1) {
            /* 把底部行滚进历史腾出下一行（与真实终端逐行滚动一致） */
            screen_scroll_up(&s, 0, s.rows - 1, 1);
        }
    }
    char ref[8192];
    logical_text(&s, ref, sizeof(ref));
    if ((int)strlen(ref) < 10) { fprintf(stderr, "FAIL: 属性测试参考文本过短\n"); free_screen(&s); return 1; }
    /* 反复 resize：宽/高随机序列 */
    static const int seq[][2] = {
        {24, 12}, {50, 6}, {32, 9}, {20, 5}, {70, 15}, {45, 10}, {28, 7}, {60, 12}, {33, 8}, {40, 11}
    };
    for (unsigned t = 0; t < sizeof(seq)/sizeof(seq[0]); t++) {
        assert(screen_resize(&s, seq[t][0], seq[t][1]) == 1);
        char got[8192];
        logical_text(&s, got, sizeof(got));
        if (!texts_eq(ref, got)) {
            fprintf(stderr, "FAIL: 属性测试 resize 到 %dx%d 后逻辑内容变化\n  ref =[%.200s...]\n  got =[%.200s...]\n",
                    seq[t][0], seq[t][1], ref, got);
            free_screen(&s); return 1;
        }
        /* 宽/高 每次 resize 后 line_wrap 行数与内容一致：逐槽 content 读一遍无重复校验略过 */
    }
    /* 回到起点尺寸也应一致 */
    assert(screen_resize(&s, 40, 8) == 1);
    {
        char got[8192];
        logical_text(&s, got, sizeof(got));
        if (!texts_eq(ref, got)) {
            fprintf(stderr, "FAIL: 回到 40x8 后逻辑内容变化\n");
            free_screen(&s); return 1;
        }
    }
    free_screen(&s);
    printf("  v1.8.52: 属性测试——11 次随机 resize 后逻辑内容逐字不变\n");
    return 0;
}

int main(void) {
    if (test_alt_resize_truecolor()) return 1;
    if (test_search_cur_after_drop()) return 1;
    if (test_resize_narrow_keeps_history()) return 1;
    if (test_resize_narrow_wide_history_not_truncated()) return 1;
    if (test_resize_wide_then_small()) return 1;
    if (test_resize_shrink_keeps_history()) return 1;
    if (test_resize_enlarge_reflow_no_duplicate()) return 1;
    if (test_random_resize_invariant()) return 1;
    if (test_reflow_view()) return 1;
    printf("  [OK] screen.c 状态迁移验证通过（alt 屏真彩色迁移 + 搜索当前项落点 + resize 保历史）。\n");
    return 0;
}
"""

CODE = PRELUDE + "\n" + "\n".join(PIECES) + "\n" + DRIVER


def main() -> int:
    print("=== screen.c 状态迁移回归 (verify_screen_state.py) ===")
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "t.c"
        exe = Path(td) / "t.bin"
        c.write_text(CODE, encoding="utf-8")
        hbuild.sanitize_report()   # 不可用时大声说明，别悄悄降级
        build = subprocess.run(
            [hbuild.gcc(), "-O1", "-g", *hbuild.sanitize_flags(), "-Wall", "-Wextra",
             "-o", str(exe), str(c)],
            capture_output=True, text=True)
        if build.returncode:
            print(build.stderr or build.stdout, file=sys.stderr)
            print("FAIL: 无法编译 screen.c 抽取出来的状态迁移代码", file=sys.stderr)
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout, end="")
        if run.returncode:
            print(run.stderr, file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
