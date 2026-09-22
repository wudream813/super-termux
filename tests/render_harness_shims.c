/* ---------------------------------------------------------------------------
 * 渲染 harness 的替身层。
 *
 * 作用：让 render.c 及其依赖能在 Linux 下链接运行，而不去碰真实的 ConPTY、
 * 窗格进程和 Windows 控制台。三部分内容：
 *
 *  1) Windows 控制台替身的状态定义（声明在 tests/stub/windows.h）。
 *  2) main.c 里的全局变量。harness 不链 main.c（它自带 main()），但这些全局
 *     被 render.c / input.c 引用，所以按 src/main.c 原样抄一份定义。
 *  3) 宿主回调与 pane.c 的替身。签名逐条照 include/types.h、include/pane.h 抄，
 *     避免 conflicting types。
 *
 * host_write 走 WriteConsoleA，而 stub 的 WriteConsoleA 把字节攒进 g_stub_out，
 * 所以捕获 g_stub_out 就等于捕获整帧渲染输出。
 * ------------------------------------------------------------------------- */
#include "common.h"
#include "types.h"
#include "input.h"
#include "config.h"
#include "pane.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>

/* ---- 1) stub 状态的定义点 ---- */
int g_stub_cols = 80;
int g_stub_rows = 25;
INPUT_RECORD *g_stub_events = 0;
int g_stub_event_count = 0;
int g_stub_event_pos = 0;
char *g_stub_out = 0;
int g_stub_out_len = 0;
int g_stub_out_cap = 0;

/* dump_render_output 的落盘开关：设了 TERMUX_DUMP 就写 render_dump.log，
 * 格式与生产一致，便于和真机日志对照。 */
static FILE *g_dump_fp = 0;
static int g_dump_checked = 0;

/* ---- 2) main.c 的全局变量（不链 main.c，故在此定义） ---- */
MuxState g_mux;
int g_pop_anchor_x = -1;
int g_mouse_x = -1, g_mouse_y = -1;
int g_mouse_prev_in_tabbar = 0;
char g_toast_msg[96] = {0};
DWORD64 g_toast_until = 0;
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
int g_sb_drag_pane = -1;   /* 同步义务：新增全局要同时改 src/main.c 与这里 */
int g_copy_mode = 0;
int g_copy_sel_active = 0;
int g_copy_cx = 0, g_copy_cy = 0;
int g_copy_end_x = 0;
int g_copy_anchor_x = 0, g_copy_anchor_abs_y = 0;
int g_copy_block = 0;
int g_copy_quick = 0;
int g_ui_mode_pane = -1;
int g_mouse_selecting = 0;
int g_mouse_sel_sx = 0, g_mouse_sel_s_abs_y = 0;
int g_mouse_sel_ex = 0, g_mouse_sel_e_abs_y = 0;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0;
int g_search_match_cur = -1;
int g_search_mode = 0;
int g_search_active = 0;
char g_search_buf[64] = {0};
int g_search_len = 0, g_search_pos = 0;
/* 与 src/main.c 的全局一一对应：input.c 里新加的全局必须同步补到这里，否则
 * verify_clip_behavior.sh 链接会报 undefined reference（2026-09-20 g_search_dirty
 * 就漏过一次）。 */
int g_search_dirty = 0;

/* ---- 3) 宿主回调 ---- */
void host_write(const char *data, int len) {
    while (len > 0) {
        DWORD written = 0;
        if (!WriteConsoleA(g_mux.hOut, data, (DWORD)len, &written, NULL) || written == 0) break;
        data += written;
        len -= (int)written;
    }
}

void toast_show(const char *msg, unsigned int ms) { (void)ms;
    if (msg) snprintf(g_toast_msg, sizeof g_toast_msg, "%s", msg);
}

void dump_render_output(const char *out, int len, int pcols, int prows,
                        int hcols, int hrows) {
    if (!g_dump_checked) {
        g_dump_checked = 1;
        if (getenv("TERMUX_DUMP")) g_dump_fp = fopen("render_dump.log", "wb");
    }
    if (g_dump_fp && out && len > 0) {
        fprintf(g_dump_fp, "FRAME pcols=%d prows=%d hcols=%d hrows=%d len=%d\n",
                pcols, prows, hcols, hrows, len);
        fwrite(out, 1, (size_t)len, g_dump_fp);
        fputc('\n', g_dump_fp);
        fflush(g_dump_fp);
    }
}

void dump_delta_output(const char *data, int delta_len, int full_len) {
    (void)data; (void)delta_len; (void)full_len;
}

void log_mouse_event(const char *tag, const MOUSE_EVENT_RECORD *me) {
    (void)tag; (void)me;
}

/* ---- pane.c 的替身：渲染 harness 不需要真 ConPTY ---- */
int  create_pane(void) { return -1; }
int  create_pane_shell(const WCHAR *s) { (void)s; return -1; }
int  create_pane_shell_with_dir(const WCHAR *s, const WCHAR *d) { (void)s; (void)d; return -1; }
int  create_pane_from_item(int i) { (void)i; return -1; }
int  create_about_pane(void) { return -1; }
int  open_settings_pane(void) { return -1; }
void close_pane(int i) { (void)i; }
/* 原来是空实现。bug #28 的判据要观察「拖动中焦点有没有被切走」，空实现会让那个
 * bug 在也测不出来（假绿）—— 所以改成忠实的：与 src/pane.c 的 switch_pane 一样
 * 真的改 g_mux.active_pane。省掉了注册单叶子分屏树那一段（harness 里 pane 已经
 * 在树里了）。 */
void switch_pane(int idx) {
    if (idx < 0 || idx >= g_mux.pane_count || !g_mux.panes[idx].active) return;
    if (idx != g_mux.active_pane) ui_modes_cancel();
    g_mux.active_pane = idx;
    g_mux.panes[idx].scroll_offset = 0;
    g_mux.needs_redraw = 1;
}
int  find_next_active_pane(int c) { (void)c; return -1; }
void pane_mark_dead(int i) { (void)i; }
void reap_dead_panes(void) {}
void write_to_pane(const char *d, int l) { (void)d; (void)l; }
void write_to_pane_internal(Pane *p, const char *d, int l) { (void)p; (void)d; (void)l; }
void get_system_version_string(char *o, int m) { if (m > 0) o[0] = 0; }
/* 故意空实现：拖动中生产路径也不会调它（bug #12 的 freeze_model 决定）。
 * 若哪天 harness 需要模拟"松手后同步"，在这里补真的 reflow 调用。 */
void pane_resize_to(int i, int c, int r) { (void)i; (void)c; (void)r; }

/* POSIX 侧 config.c 的默认启动项会问平台层「默认 shell 是什么」。harness 不链
 * platform_posix.c，所以这里镜像一份替身。移植约定：凡是新增的平台层符号，
 * 都必须在这份 shims 里补上，否则 verify_clip_behavior / verify_sb_drag 会在
 * 链接期报 undefined reference。 */
#ifndef _WIN32
const WCHAR *plat_default_shell(void) { return L"/bin/sh"; }
#endif

/* config.c 的 resolve_ini_path() 在 exe 同目录找不到 ini 时会回退到用户主目录。
 * harness 里返回 NULL —— 等价于 _wgetenv(L"USERPROFILE") 拿不到值，于是测试
 * 不会往磁盘上写任何配置文件。
 *
 * ★ 这个必须【两个平台都定义】，不能放进上面的 #ifndef _WIN32 里。
 *   它原来在里面，于是 mingw 交叉编译 render_harness / sb_drag_harness 时
 *   config.c 找不到它：undefined reference to `plat_user_home'。
 *   （harness 两个平台都不链 src/platform_win.c / platform_posix.c，
 *     所以两边的替身都得由这份 shims 提供。）
 *   plat_default_shell 才是真的 POSIX-only —— Windows 侧默认 shell 来自
 *   platform.h 的 TERMUX_DEFAULT_SHELL_W 宏，不走平台层函数。 */
const WCHAR *plat_user_home(void) { return NULL; }
