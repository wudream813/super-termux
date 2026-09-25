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
/* 窗格配色页（v2.0.7）：18 个 pane_* 槽位，两列各 9 行 */
/* 窗格配色页：第 5 行是方案行（[方案] ‹ Campbell › …），槽位表从第 7 行起。
 * 每项宽 = 1 + 16 标签 + 1 + 2 色块 + 1 + 8 值 = 29，加列间距 = 32。
 * 右侧区域装不下两列（< 2*32）时自动改成单列 20 行（窄终端不再被截掉右列）。 */
#define SETTINGS_PANE_SCHEME_ROW 5
#define SETTINGS_PANE_ROW0      7
#define SETTINGS_PANE_ROWS      10
#define SETTINGS_PANE_COL_W     32
#define SETTINGS_PANE_ITEM_W    29
#define SETTINGS_PANE_VALUE_OFF 22   /* 值段（'#'）在 col+VALUE_OFF-1（1 基）：1+16+1+2+1 = 21 列前缀 */
#define SETTINGS_KEYS_ROW0      6
#define SETTINGS_BEHAVIOR_ROW0  6
#define SETTINGS_BEHAVIOR_TOGGLES 5   /* mouse / copy_move_deselect / confirm_on_exit / confirm_on_close / search_case_sensitive */
/* 相对 main_left 的按钮列偏移，渲染时用绝对定位写出，鼠标按同样的偏移命中。
 * v1.8.44：说明列加宽到 36（最长中文说明约 32 列）、动作名列 20、键位列 18，
 * 按钮相应右移；命中与渲染共用同一常量。 */
#define SETTINGS_KEYS_PREFIX_COL 79   /* [前缀] / [直接] 切换（宽终端；窄终端见 settings_keys_*_col 函数） */

/* v1.8.9: 菜单项的「启动默认颜色」选择条。
 * 第 0 格是「默认」(宽 6)，其后 8 格分别是标签色 1-8 (每格宽 3)，格子彼此相连，
 * 渲染与鼠标命中共用同一套几何。 */
#define ITEM_COLOR_DEFAULT_W 6
#define ITEM_COLOR_SWATCH_W  3
#define ITEM_COLOR_ROW_W     (ITEM_COLOR_DEFAULT_W + 8 * ITEM_COLOR_SWATCH_W)
/* col / left 均为 1-based 终端列；未命中返回 -1，命中返回 0(默认) 或 1-8。 */
/* v2.1.1：本帧实际画出的色块格数（1..8）——渲染与鼠标命中同源。 */
extern int g_item_color_max;
int item_color_hit(int left, int col);
void render_item_color_row(char *out, int bs, int *posp, int row, int left, int color, int focused);
/* v2.1.1：host_cols 版（行尾剩余宽度决定「+N」/提示）；上面那个只是传 g_mux.host_cols 的包装。 */
void render_item_color_row_w(char *out, int bs, int *posp, int row, int left, int color, int focused,
                             int host_cols);
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
    PALETTE_ACTION_OPEN_PANE_PALETTE,
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

/* ===========================================================================
 * v2.1.0：矮终端纵向滚动 + 窄屏截断的悬停气泡
 *
 * 两件事都是「屏幕不够就把内容丢掉」的后遗症：太矮时外观页 13 行之后的语义色区、
 * 行为页末尾、菜单项详细配置的按钮和提示行全都画到屏幕外；侧栏更糟——[A]/[K]/
 * [B]/[W] 四个入口被挤掉，鼠标根本进不去子页。现在：
 *   - 侧栏按 host_rows 自适应（先省表头与分隔行，再省 [P] 行，菜单项列表截断），
 *     四个入口与 [Ctrl+S] 永远在屏内；
 *   - 外观 / 行为 / 菜单项详细配置三页各自持有一个滚动量，行号一律由
 *     settings_*_row_view() 换算，渲染与命中判定同源；
 *   - 横向装不下的文字截到边界并补「...」，鼠标停在上面时以浮层气泡给出全文。
 * ========================================================================= */
#define SETTINGS_TIP_MAX  24           /* 每帧登记的截断行上限 */
#define SETTINGS_TIP_TEXT 512
typedef struct {
    int row;                            /* 1 基终端行 */
    int col;                            /* 该行文字起始列（1 基） */
    int len;                            /* 全文显示宽度（列） */
    char full[SETTINGS_TIP_TEXT];
} SettingsTip;
void settings_tip_reset(void);
int  settings_tip_count(void);
const SettingsTip *settings_tip_at(int row, int col);   /* 命中返回登记项，否则 NULL */
/* 有滚动时在行右端画 (first-last/total) 位置指示 */
/* ---- v2.1.4：设置页右栏横向滚动（Shift+滚轮 / 触控板横滚）---- */
typedef struct { const char *sgr; const char *text; int pad_to_col; } SettingsSeg;
int  settings_hscroll_slot(int nav);
int  settings_hscroll(void);                     /* 当前页的横向滚动量（列） */
int  settings_canvas_w(int host_cols, int main_left);
void settings_hscroll_clamp(int host_rows, int host_cols, int main_left, int content_w);
void settings_hscroll_reveal(int host_rows, int host_cols, int main_left, int content_w,
                             int col, int w);
void settings_hscroll_by(int delta);
/* lpin/rpin = 视口左/右两端【钉住不随横滚移动】的列数（行首序号、行尾按钮）。 */
void settings_hline2(char *out, int bs, int *posp, int row, int main_left, int host_cols,
                     int content_w, const SettingsSeg *seg, int nseg, int tip_row_len,
                     int lpin, int rpin);
#define settings_hline(o, bs, p, row, ml, hc, cw, sg, ns, tip)     settings_hline2((o), (bs), (p), (row), (ml), (hc), (cw), (sg), (ns), (tip), 0, 0)
void settings_menu_table_geom(int host_rows, int host_cols, int main_left,
                              int *cw_out, int *name_w, int *cmd_w, int *btn_col,
                              int *show_ud, int *show_btn);
/* v2.1.4：窄终端表格的单一口径（画布列宽 + 视口按钮位置）；返回 1 = 开了横向滚动。 */
int  settings_menu_table_calc(int host_rows, int host_cols, int main_left,
                        int *name_w, int *cmd_w, int *btn_col,
                        int *show_ud, int *show_btn);
void settings_h_wheel(int delta);
void settings_hscroll_follow(int host_rows, int host_cols, int main_left,
                             int col, int width);
/* v2.1.4：启动项页与条目管理页共用的表。 */
typedef int (*SettingsRowView)(int host_rows, int natural);
typedef struct {
    char *out; int bs; int *pos;
    int host_rows, host_cols, main_left;
    int sel;                        /* 聚焦行（-1 = 无） */
    SettingsRowView row_view;       /* 该页的行号换算 */
    int h_on, h_sc;                 /* 横滚是否生效 / 当前滚动量 */
    int cw, nw, cw2, bc, ud, btn;   /* 表几何（画布口径） */
    int show_ops;                  /* v2.1.4：0 = 只读表（启动项页），不画 ▶ 与 [↑][↓][改][删] */
} MenuRowCtx;
void render_menu_rows(MenuRowCtx *rc);

void settings_hscroll_mark(char *out, int bs, int *posp, int row, int right_col,
                           int h, int vw, int content_w, int host_rows);

void settings_scroll_mark(char *out, int bs, int *posp, int row, int right_col,
                          int first_vis, int last_vis, int total, int host_rows);
void settings_page_mark(char *out, int bs, int *posp, int row, int right_col, int host_rows,
                        int first, int last, int *scroll);

void settings_startup_radio_spans(int host_cols, int main_left,
                                  int *opt0_on, int *opt0_w, int *opt1_on, int *opt1_w);
int  settings_page_row(int host_rows, int natural, int first, int last,
                       int *scroll, int sel_natural);
int  settings_page_natural_at(int host_rows, int row, int first, int last, int *scroll, int sel_natural);
int  settings_appearance_row_view(int host_rows, int natural);
int  settings_appearance_sel_natural(int host_rows);
int  settings_appearance_natural_at(int host_rows, int row);
int  settings_behavior_row_view(int host_rows, int natural);
int  settings_behavior_sel_natural(int host_rows);
int  settings_behavior_natural_at(int host_rows, int row);
int  settings_detail_row_view(int host_rows, int natural);
int  settings_startup_row_view(int host_rows, int natural);
int  settings_manage_row_view(int host_rows, int natural);   /* v2.1.4：条目管理页 */
int  settings_manage_natural_at(int host_rows, int row);
int  settings_startup_natural_at(int host_rows, int row);
int  settings_detail_natural_at(int host_rows, int row);
typedef struct {
    int hdr, nav_label, sep1, start, sep2;
    int items_row0, items_cap, items;     /* v2.1.4：items = 「[M] 条目管理」那一行 */
    int app, keys, beh, pane, save;
    int compact, nav_tight;   /* nav_tight = 矮终端把底部那一排再挤紧一档（沿用上一版 hide_presets 的触发条件） */
    int items_scroll;                          /* v2.1.2：菜单项列表滚动量（已夹好） */
} SettingsSidebarGeom;
void settings_sidebar_geom(int host_rows, int item_count, SettingsSidebarGeom *g);
/* v2.1.2：只问「这一屏侧栏能放几行菜单项」。 */
int  settings_sidebar_geom_cap(int host_rows, int item_count);
/* v2.1.2：把侧栏窗口夹到「选中项可见」。只在选中项变化时调用（不能每帧调，
 * 否则滚轮被拽回原位）。 */
void settings_sidebar_clamp_sel(int host_rows, int item_count);
/* v2.1.2：侧栏列表当前该被看见的行（0 = 无）。渲染夹窗口时用。 */
int  settings_sidebar_sel_natural(void);
/* v2.1.2：侧栏宽度单一来源（渲染 / 命中 / 光标都调它，窄终端下不再各算一套）。 */
int  settings_host_sidebar_w(int host_cols);
int settings_theme_row(int idx);
int settings_role_row(int role);
int settings_role_col(int main_left, int role);
/* host_cols 决定单列/双列（见 SETTINGS_PANE_* 注释）。 */
int settings_pane_two_cols(int host_cols, int main_left);
int settings_pane_rows_per_col(int host_cols, int main_left);
/* 可见行数（矮终端 + 单列时不够 20 行，会按 g_settings_pane_scroll 滚动；返回 -1 = 该项当前不可见） */
int settings_pane_visible_rows(int host_rows, int host_cols, int main_left);
void settings_pane_clamp_scroll(int host_rows, int host_cols, int main_left);
int settings_pane_row(int host_cols, int main_left, int slot);
int settings_pane_col(int host_cols, int main_left, int slot);
int settings_pane_hint_row(int host_rows, int host_cols, int main_left);
/* v2.0.9：键位页列宽随可用宽度收缩（窄终端先砍「说明」列，再压「动作名」列），
 * 保证 [前缀]/[改]/[复位] 三个按钮始终留在屏幕内（以前 60 列时它们被裁到屏幕外，
 * 点不到也看不到）。render 绘制与 input 命中判定共用同一套。 */
int settings_keys_name_w(int host_cols, int main_left);
int settings_keys_desc_w(int host_cols, int main_left);
int settings_keys_combo_w(int host_cols, int main_left);
int settings_keys_prefix_col(int host_cols, int main_left);
int settings_keys_edit_col(int host_cols, int main_left);
int settings_keys_reset_col(int host_cols, int main_left);
int settings_keys_show_reset(int host_cols, int main_left);
/* v2.1.0：窗格配色页顶部的「预设方案」行按 Enter / 点击打开的方案列表浮层。 */
void pane_scheme_picker_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h);
/* v2.1.1：十六进制颜色编辑浮层。以前编辑框是「嵌在表行里」的，终端一窄
 * （值段起点在屏幕外）就整段被裁掉——看不见色、也看不见自己敲了哪几位。
 * 改成居中的小浮层：色块 + 完整 6 位十六进制 + 提示，任何宽度都不截断。 */
void hex_edit_popup_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h);
#define HEX_EDIT_POPUP_MIN 20    /* 能摆下「色块 + 8 位值」的最小框宽 */
#define HEX_EDIT_POPUP_HEX_OFF 6   /* 值段（'#'）在 left + 该偏移（1 基）：│ + 两空格 + 色块 2 + 空格 */
void render_hex_edit_popup(char *out, int bs, int *posp, int host_rows, int host_cols);
/* v2.1.1：滚轮滚动设置页右侧内容（渲染与命中同用一套滚动量）。 */
void settings_wheel_scroll(int delta);
/* v2.1.1：详情页「启动默认颜色」色块条，宽度不够时少画几格（渲染与命中同源）。 */
int settings_detail_color_w(int host_cols, int main_left);
void render_pane_scheme_picker(char *out, int bs, int *posp, int host_rows, int host_cols);
int  pane_scheme_picker_swatches(int pw);
/* v2.0.9：菜单项管理页（启动页下半部分）每行的 [↑][↓][改][删] 按钮列同样随宽度收缩：
 * 先压「启动命令行」列（30 → 8），极窄时只留 [改][删]（调序用 Ctrl+↑/↓）。
 * 渲染与鼠标命中共用。 */
int settings_menu_cmd_w(int host_cols, int main_left);
int settings_menu_name_w(int host_cols, int main_left);         /* 显示名称列宽（窄时收窄） */
int settings_menu_pre_w(int host_cols, int main_left);           /* 行首（▶ + [n]）占几列 */
int settings_menu_show_btn(int host_cols, int main_left);       /* 0 = 连按钮区都放不下 */
int settings_menu_header(char *buf, int bs, int host_cols, int main_left);
int settings_menu_btn_col(int host_cols, int main_left);
int settings_menu_show_ud(int host_cols, int main_left);   /* 是否画 [↑][↓] */
int settings_pane_order_slot(int pos);
int settings_pane_order_pos(int slot);
int settings_sidebar_pane_row(void);
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
