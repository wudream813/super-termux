#ifndef WIN_TERMUX_PANE_H
#define WIN_TERMUX_PANE_H

#include "common.h"
#include "types.h"
#include "screen.h"
#include "utf8.h"
#include "config.h"
#include "vt.h"
#include "theme.h"

int create_pane(void);
int create_pane_sized(int cols, int rows);
int create_pane_shell_sized(const WCHAR *shell, int cols, int rows);
int create_pane_shell(const WCHAR *shell);
int create_pane_shell_with_dir(const WCHAR *shell, const WCHAR *workdir);
int create_pane_from_item(int idx);
int create_about_pane(void);
int open_settings_pane(void);
void close_pane(int idx);
void switch_pane(int idx);
int find_next_active_pane(int cur);
void pane_mark_dead(int idx);
void reap_dead_panes(void);
unsigned __stdcall pane_read_thread(void *arg);
void write_to_pane(const char *data, int len);
void write_to_pane_internal(Pane *pane, const char *data, int len);
void get_system_version_string(char *out, int max_len);
/* 分屏：把某 pane 的屏幕缓冲与 ConPTY 调整到给定宽高（列/行）。 */
void pane_resize_to(int idx, int cols, int rows);

#endif // WIN_TERMUX_PANE_H
