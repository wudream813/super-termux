#ifndef WIN_TERMUX_RENDER_H
#define WIN_TERMUX_RENDER_H

#include "common.h"
#include "types.h"
#include "screen.h"
#include "utf8.h"
#include "config.h"
#include "theme.h"
#include "keymap.h"

#define SETTINGS_SIDEBAR_W 22

/* 设置页三个新分类的固定行号（渲染与鼠标命中共用） */
#define SETTINGS_THEME_ROW0     5
#define SETTINGS_ROLE_ROW0      13
#define SETTINGS_ROLE_ROWS      8
#define SETTINGS_ROLE_COL_W     34
#define SETTINGS_KEYS_ROW0      6
#define SETTINGS_BEHAVIOR_ROW0  6
#define SETTINGS_BEHAVIOR_TOGGLES 5   /* mouse / copy_move_deselect / confirm_on_exit / confirm_on_close / search_case_sensitive */
/* 相对 main_left 的按钮列偏移，渲染时用绝对定位写出，鼠标按同样的偏移命中。
 * v1.8.44：说明列加宽到 36（最长中文说明约 32 列）、动作名列 20、键位列 18，
 * 按钮相应右移；命中与渲染共用同一常量。 */
#define SETTINGS_KEYS_PREFIX_COL 79   /* [前缀] / [直接] 切换 */

/* v1.8.9: 菜单项的「启动默认颜色」选择条。
 * 第 0 格是「默认」(宽 6)，其后 8 格分别是标签色 1-8 (每格宽 3)，格子彼此相连，
 * 渲染与鼠标命中共用同一套几何。 */
#define ITEM_COLOR_DEFAULT_W 6
#define ITEM_COLOR_SWATCH_W  3
#define ITEM_COLOR_ROW_W     (ITEM_COLOR_DEFAULT_W + 8 * ITEM_COLOR_SWATCH_W)
/* col / left 均为 1-based 终端列；未命中返回 -1，命中返回 0(默认) 或 1-8。 */
int item_color_hit(int left, int col);
void render_item_color_row(char *out, int bs, int *posp, int row, int left, int color, int focused);
#define SETTINGS_KEYS_EDIT_COL  87
#define SETTINGS_KEYS_RESET_COL 92
#define SETTINGS_SB_MINUS_COL   22
#define SETTINGS_SB_PLUS_COL    33
#define RENAME_W 30
#define RENAME_H 3
#define CMD_BOX_W 38
#define CMD_BOX_H 4
#define CTX_W 24
#define CTX_H 4
#define CP_SWATCH_W 3   /* coloured cells per swatch; the 4th cell is a gap */
#define CP_W 20
#define CP_H 4

/* Command palette pages.  palette_mode remains a boolean so the rest of the
 * input/render pipeline can keep treating the palette as a modal overlay. */
enum {
    PALETTE_PAGE_ROOT = 0,
    PALETTE_PAGE_OPERATIONS,
    PALETTE_PAGE_SETTINGS,
    PALETTE_PAGE_NEW_TERMINAL,
    PALETTE_PAGE_SWITCH_PANEL,
    PALETTE_PAGE_DEFAULT_STARTUP,
    PALETTE_PAGE_ADD_PANEL,
    PALETTE_PAGE_MENU_SETTINGS,
    PALETTE_PAGE_PANEL_EDITOR
};

enum {
    PALETTE_FOCUS_INPUT = 0,
    PALETTE_FOCUS_LIST = 1
};

typedef enum {
    PALETTE_ACTION_NONE = 0,
    PALETTE_ACTION_OPEN_OPERATIONS,
    PALETTE_ACTION_OPEN_SETTINGS,
    PALETTE_ACTION_OPEN_NEW_TERMINAL,
    PALETTE_ACTION_START_CUSTOM,
    PALETTE_ACTION_RENAME,
    PALETTE_ACTION_COLOR,
    PALETTE_ACTION_SEARCH,
    PALETTE_ACTION_SWITCH_PANEL,
    PALETTE_ACTION_COPY_MODE,
    PALETTE_ACTION_RELOAD,
    PALETTE_ACTION_GRAPHICAL_SETTINGS,
    PALETTE_ACTION_CLOSE_PANEL,
    PALETTE_ACTION_QUIT,
    PALETTE_ACTION_DEFAULT_STARTUP,
    PALETTE_ACTION_OPEN_INI,
    PALETTE_ACTION_ADD_PANEL,
    PALETTE_ACTION_MENU_SETTINGS,
    PALETTE_ACTION_OPEN_ABOUT,
    PALETTE_ACTION_EDIT_PANEL,
    PALETTE_ACTION_SELECT_TERMINAL,
    PALETTE_ACTION_SELECT_PANEL,
    PALETTE_ACTION_SELECT_DEFAULT,
    PALETTE_ACTION_NEXT_THEME,
    PALETTE_ACTION_OPEN_APPEARANCE,
    PALETTE_ACTION_OPEN_KEYS,
    PALETTE_ACTION_OPEN_BEHAVIOR,
    PALETTE_ACTION_SPLIT_VERTICAL,    /* 分屏：左右切分 */
    PALETTE_ACTION_SPLIT_HORIZONTAL,  /* 分屏：上下切分 */
    PALETTE_ACTION_SPLIT_NEXT,        /* 分屏：切换到下一个窗格 */
    PALETTE_ACTION_SPLIT_CLOSE,       /* 分屏：关闭当前窗格 */
    PALETTE_ACTION_SPLIT_ZOOM         /* 分屏：当前窗格全屏缩放 / 还原 */
} PaletteAction;

typedef struct {
    const char *id;
    const char *title;
    const char *desc;
    const char *shortcut;
    PaletteAction action;
    int value;
    int number;
    int color;
} PaletteItemInfo;

void update_host_title(void);
void render_screen(void);
void draw_tab_bar(char *out, int bs, int *posp);
void render_help_content(char *out, int bs, int *posp, int host_rows, int host_cols);
void render_chooser(char *out, int bs, int *posp, int host_rows, int host_cols);
void chooser_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h);
/* Popup left edge in ANSI's 1-based column space.  Mouse anchors remain 0-based. */
int popup_left_1based(int anchor0, int width, int host_cols);
void render_custom_cmd_box(char *out, int bs, int *posp, int host_rows, int host_cols);
void render_rename_box(char *out, int bs, int *posp, int host_rows, int host_cols);
void render_ctx_menu(char *out, int bs, int *posp, int host_rows, int host_cols);
void render_color_picker(char *out, int bs, int *posp, int host_rows, int host_cols);
void render_settings_presets(char *out, int bs, int *posp, int host_rows, int host_cols);
void presets_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h, int *max_nw, int *max_cw);
void render_settings_panel(char *out, int bs, int *posp, int host_rows, int host_cols);
void settings_sidebar_extra_rows(int *appearance_r, int *keys_r, int *behavior_r);
int settings_theme_row(int idx);
int settings_role_row(int role);
int settings_role_col(int main_left, int role);
int settings_keys_rows(void);
int settings_keys_visible(int host_rows);
int settings_keys_row_at(int host_rows, int entry);
int settings_keys_entry_at(int host_rows, int row);
void render_search_box(char *out, int bs, int *posp, int host_rows, int host_cols);
/* 搜索输入框（右上角紧凑框）的几何，渲染与光标共用。 */
void search_box_layout(int host_cols, int *row, int *left, int *input_col, int *input_w);
/* 搜索框里「Aa / aa」大小写标记的鼠标命中：r、c 为 1 基终端行列。返回 1 表示
 * 鼠标正落在大小写标记上（用于 hover 高亮与点击切换区分大小写）。 */
int search_box_case_hit(int host_cols, int r, int c);
/* 大小写标记当前是否被鼠标悬停（渲染用，0 基 g_mouse_x/y）。 */
int search_box_case_hovered(int host_cols);
void render_confirm_exit(char *out, int bs, int *posp, int host_rows, int host_cols);
/* 通用确认弹窗。kind=0 退出 termux（标题「退出确认」），kind=1 关闭窗格/标签。 */
void render_confirm_dialog(char *out, int bs, int *posp, int host_rows, int host_cols, int kind);
/* 顶栏右侧状态徽章（复制模式 / 搜索）。折叠时只有徽章本体与按钮，鼠标悬停
 * 才向左展开提示文字，所以按钮列不会随提示出现而漂移。 */
typedef struct {
    int kind;                 /* 0 = 无, 1 = 复制模式, 2 = 搜索 */
    int row;                  /* 1-based ANSI 行 */
    int start, end;           /* 折叠区间，1-based，end 独占 */
    int prev_s, prev_e;       /* 搜索：上一个按钮 */
    int next_s, next_e;       /* 搜索：下一个按钮 */
    int close_s, close_e;     /* 搜索：关闭按钮 */
} StatusBadge;

int status_badge_layout(int host_cols, StatusBadge *out);
int status_badge_hovered(const StatusBadge *b);
void render_status_badge(char *out, int bs, int *posp, int host_cols);

void confirm_exit_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h);
void confirm_exit_button_geom(int host_rows, int host_cols, int *row,
                              int *yes_start, int *yes_end,
                              int *no_start, int *no_end);
void render_command_palette(char *out, int bs, int *posp, int host_rows, int host_cols);
void palette_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h);
int palette_visible_rows(int host_rows);
int palette_item_count(int page);
int palette_filter_cmds(int page, int *out_indices, int max_out, const char *query);
int palette_item_info(int page, int item_index, PaletteItemInfo *out);
void palette_editor_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h, int *input_w);
/* 滚动条可见性门槛（列数）。渲染（整屏路径 + 分屏窗格路径）与鼠标命中测试
 * 【必须共用这一个判据】，否则会出现「窗格窄到不画滚动条、却仍能拖它」——
 * 用户看到的就是「滚动条没了」（2026-09-20 报：窗格 <=6 列时消失）。
 * cols 传该窗格用来画滚动条的那一列所属的宽度（分屏 = pane 的 cols，
 * 单窗格 = host_cols）；in_alt_screen 传 s->in_alt_screen。 */
int render_sb_cols_ok(int cols, int in_alt_screen);

/* 滚动条要【让开】的那一行（0 基 pane 行号），-1 = 不用让。
 * 滚动条是覆盖 pane 右缘内列画的；VT 的自动换行是延迟的（在最后一列写完一个字，
 * 光标要等下一个字才换行），所以光标停在最后一列时正好压在滚动条那一格上 ——
 * 刚敲的字被滚动条的空格盖掉，光标看着像卡住不动（2026-09-20 用户报）。
 * 光标正落在右缘列时那一行不画滚动条，让刚敲的字始终可见。 */
int render_sb_spare_row(int cursor_visible, int cursor_x, int cursor_y, int cols);

void render_cleanup(void);

#endif // WIN_TERMUX_RENDER_H
