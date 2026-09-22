/* ---------------------------------------------------------------------------
 * globals.c —— 平台无关的全局状态与诊断输出。
 *
 * 原来这些都在 src/main.c 里。移植到 Linux / macOS 时 main 要按平台分成两份
 * （控制台 I/O 完全不同：ReadConsoleInputW vs termios + 转义序列解析），但这些
 * 全局和 dump 函数两边一模一样，所以抽出来共用，免得复制一份再各自漂移。
 * ------------------------------------------------------------------------- */
#include "common.h"
#include "types.h"
#include "screen.h"
#include "config.h"
#include "input.h"

MuxState g_mux;
int g_pop_anchor_x = -1;
int g_mouse_x = -1, g_mouse_y = -1;
int g_mouse_prev_in_tabbar = 0;
char g_toast_msg[96] = {0};
DWORD64 g_toast_until = 0;
void toast_show(const char *msg, unsigned int ms) {
    if (!msg) { g_toast_until = 0; return; }
    strncpy(g_toast_msg, msg, sizeof(g_toast_msg) - 1);
    g_toast_msg[sizeof(g_toast_msg) - 1] = 0;
    g_toast_until = GetTickCount64() + ms;
    g_mux.needs_redraw = 1;
}
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
int g_hover_settings_cmd_idx = -1;
DWORD64 g_hover_settings_cmd_start = 0;
int g_hover_settings_cmd_active = 0;

int g_sb_dragging = 0;
int g_sb_grab_offset = 0;
int g_sb_drag_pane = -1;

// Copy Mode & Selection
int g_copy_mode = 0;
int g_copy_sel_active = 0;
int g_copy_cx = 0, g_copy_cy = 0;
int g_copy_end_x = 0;   /* 选区端点列：键盘=光标主格；鼠标=原始点击列（交给渲染/复制按方向整字扩展） */
int g_copy_anchor_x = 0, g_copy_anchor_abs_y = 0;
int g_copy_block = 0;
int g_copy_quick = 0;
int g_ui_mode_pane = -1;
int g_mouse_selecting = 0;
int g_mouse_sel_sx = 0, g_mouse_sel_s_abs_y = 0;
int g_mouse_sel_ex = 0, g_mouse_sel_e_abs_y = 0;

// Scrollback History Search
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0;
int g_search_match_cur = -1;
int g_search_mode = 0;
int g_search_active = 0;
char g_search_buf[64] = {0};
int g_search_len = 0, g_search_pos = 0;
int g_search_dirty = 0;

int g_dump_enabled = 0;

/* TERMUX_DUMP 下的进度打点，写进 mouse_dump.log。
 *
 * 原来这是 src/main.c 里的 static，但 src/input.c 也要用（记录 ConPTY 送来的
 * 每个 KEY_EVENT），所以挪到这里 —— globals.c 是 Windows 和 POSIX 两个 main
 * 共用的，定义一份两边都能链上。
 *
 * 为什么值得常驻：CI 的 ConPTY 冒烟测试是目前【唯一】能在真 Windows 上跑
 * termux.exe 的手段，而每一轮要 3~4 分钟。让程序自己把走到哪一步、收到什么键
 * 写下来，比一轮一轮猜便宜得多。只在设置了 TERMUX_DUMP 时生效，
 * 正式使用完全无影响（g_dump_enabled 为 0 时直接 return）。 */
void dump_mark(const char *fmt, ...) {
    if (!g_dump_enabled) return;
    FILE *f = fopen("mouse_dump.log", "ab");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static int g_mouse_log_moves = 0;



void dump_pane_bytes(int idx, const char *data, int len) {
    if (!g_dump_enabled || len <= 0) return;
    FILE *f = fopen("termux_dump.log", "ab");
    if (!f) return;
    fprintf(f, "[pane %d len %d]\n", idx, len);
    fwrite(data, 1, (size_t)len, f);
    fputc('\n', f);
    fclose(f);
}

void dump_render_output(const char *data, int len, int mcols, int mrows, int hcols, int hrows) {
    if (!g_dump_enabled || len <= 0) return;
    FILE *f = fopen("render_dump.log", "ab");
    if (!f) return;
    fprintf(f, "[render len %d model %dx%d host %dx%d]\n", len, mcols, mrows, hcols, hrows);
    fwrite(data, 1, (size_t)len, f);
    fputc('\n', f);
    fclose(f);
}

/* v1.8.15 诊断：实际通过帧差分发给宿主终端的增量字节（整帧 vs 增量对照）。
 * 仅在 TERMUX_DUMP 环境变量存在时写 render_delta.log，用于排查脏区渲染问题。 */
void dump_delta_output(const char *data, int delta_len, int full_len) {
    if (!g_dump_enabled || delta_len <= 0) return;
    static int s_frame = 0;
    s_frame++;
    /* 采样：每 20 帧记一次，避免日志暴涨；增量帧（delta<full）始终记，
     * 因为脏区问题只出现在增量路径。 */
    if (delta_len >= full_len && (s_frame % 20) != 0) return;
    FILE *f = fopen("render_delta.log", "ab");
    if (!f) return;
    fprintf(f, "[delta %d / full %d]\n", delta_len, full_len);
    fwrite(data, 1, (size_t)delta_len, f);
    fputc('\n', f);
    fclose(f);
}

void log_mouse_event(const char *tag, const MOUSE_EVENT_RECORD *me) {
    if (!g_dump_enabled) return;
    unsigned btn = (unsigned)me->dwButtonState;
    int is_press = (btn & (FROM_LEFT_1ST_BUTTON_PRESSED | FROM_LEFT_2ND_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED)) &&
                   (me->dwEventFlags == 0 || me->dwEventFlags == DOUBLE_CLICK);
    int is_release = (btn & 0x7) == 0 && me->dwEventFlags == 0;
    if (me->dwEventFlags == MOUSE_MOVED) {
        if (++g_mouse_log_moves < 20) return;
        g_mouse_log_moves = 0;
    }
    if (me->dwEventFlags == MOUSE_WHEELED || me->dwEventFlags == MOUSE_HWHEELED) return;
    FILE *f = fopen("mouse_dump.log", "ab");
    if (!f) return;
    fprintf(f, "[v8.54] %s pos=%d,%d flags=%u btn=0x%X ctrl=0x%X%s | chooser=%d ctx=%d rename=%d help=%d pop_anchor=%d mouse=%d,%d tab_count=%d\n",
            tag, (int)me->dwMousePosition.X, (int)me->dwMousePosition.Y,
            (unsigned)me->dwEventFlags, btn, (unsigned)me->dwControlKeyState,
            is_press ? " PRESS" : (is_release ? " RELEASE" : ""),
            g_mux.chooser_mode, g_mux.ctx_mode, g_mux.rename_mode, g_mux.help_mode,
            g_pop_anchor_x, g_mouse_x, g_mouse_y, g_mux.tab_count);
    if (is_press) {
        for (int i = 0; i < g_mux.tab_count; i++) {
            PaneTabInfo *t = &g_mux.tab_info[i];
            fprintf(f, "  tab[%d] pane=%d cols[%d,%d) close[%d,%d)\n", i, t->pane_idx,
                    t->start_col, t->end_col, t->close_start, t->close_end);
        }
    }
    fclose(f);
}
