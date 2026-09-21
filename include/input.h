#ifndef WIN_TERMUX_INPUT_H
#define WIN_TERMUX_INPUT_H

#include "common.h"
#include "types.h"
#include "screen.h"
#include "utf8.h"
#include "config.h"
#include "pane.h"
#include "render.h"

void handle_key(KEY_EVENT_RECORD *ke);

/* 复制 / 搜索模态的归属管理：进入时记下 pane，切走时收回。 */
void ui_modes_claim(void);
void ui_modes_cancel(void);
void ui_modes_sync_pane(void);
void handle_mouse(MOUSE_EVENT_RECORD *me);
int  split_drag_active(void);   /* 分屏边框拖动中：pane_resize_to 据此推迟 ConPTY resize */
void action_execute(int action, int arg, DWORD ctrl);
void handle_prefix(WORD vk, DWORD ctrl, WCHAR uc);
void handle_settings_key(KEY_EVENT_RECORD *ke);
void handle_settings_mouse(MOUSE_EVENT_RECORD *me);
/* Returns 1 when the key was NOT consumed and copy mode has been left, so the
 * caller should keep processing it (Shift/Alt 点选会话里的“其它键退出并发送”). */
int handle_copy_mode_key(KEY_EVENT_RECORD *ke);
void handle_search_key(KEY_EVENT_RECORD *ke);
void handle_palette_key(KEY_EVENT_RECORD *ke);
void handle_palette_mouse(MOUSE_EVENT_RECORD *me);
void open_command_palette(void);
void execute_palette_command(int item_index);
void copy_range_to_clipboard(Pane *p, int sx, int sy_abs, int ex, int ey_abs);
/* block = 1 复制矩形区域（每行取同一段列），block = 0 复制连续文本流。 */
void copy_selection_to_clipboard(Pane *p, int sx, int sy_abs, int ex, int ey_abs, int block, int halfopen);
void execute_search(void);
void search_preview_live(void);
/* 终端输出新数据后重算搜索匹配（bug #24）。与 search_preview_live 的区别：
 * 会把用户正停留的那条匹配找回来，浏览位置不跳。 */
void search_refresh_live(void);
/* pane 读线程在收到新输出后调用：只在搜索真的开着时置脏标记。 */
void search_mark_dirty(void);
/* 重扫之后把「原来停留的那条」找回来：同一列、行号最接近。返回 index，找不到 -1。
 * ms 为新匹配表、n 为其长度、want_abs_y / want_x 为重扫前停留项的坐标。 */
int search_relocate_cur(const SearchMatch *ms, int n, int want_abs_y, int want_x);
void search_jump_next(void);
void search_jump_prev(void);
void do_scroll(int d);

#endif // WIN_TERMUX_INPUT_H
