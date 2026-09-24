#ifndef WIN_TERMUX_CONFIG_H
#define WIN_TERMUX_CONFIG_H

#include "common.h"
#include "types.h"
#include "theme.h"
#include "keymap.h"

extern ChooserItem g_chooser_items[MAX_CHOOSER_ITEMS];
extern int g_chooser_item_count;

/* 侧栏导航：0 = 启动页，1..N = 菜单项详情页，下面三个是新增的分类页 */
#define SETTINGS_NAV_STARTUP    0
#define SETTINGS_NAV_APPEARANCE 100
#define SETTINGS_NAV_KEYS       101
#define SETTINGS_NAV_BEHAVIOR   102
#define SETTINGS_NAV_PANE       103   /* v2.0.7：窗格配色（pane_*） */

extern int g_settings_nav;
extern int g_settings_field;
extern int g_settings_table_sel;
extern int g_default_startup;

/* [general] 段 */
extern int g_scrollback_lines;    /* 每个 pane 的滚动历史行数 */
extern int g_mouse_enabled;       /* 是否启用鼠标（标签点击 / 拖选 / 滚轮） */
extern int g_copy_move_deselect;  /* 复制模式无 Shift/Alt 移动时丢弃当前高亮（1=丢弃） */
extern int g_confirm_on_exit;        /* 退出 termux 前是否二次确认 */
extern int g_confirm_on_close;       /* 关闭窗格 / 标签前是否二次确认 */
extern int g_search_case_sensitive;  /* 搜索是否锁定大小写（区分大小写） */
extern int g_settings_show_presets;
extern int g_preset_sel;

/* 外观页 / 键位页 / 行为页的光标与编辑状态 */
extern int g_settings_pane_sel;     /* 窗格配色页：当前选中的 pane_* 槽位；-1 = 方案行 */
extern int g_settings_pane_scheme;  /* 窗格配色页：方案行当前指向的预设下标 */
extern int g_settings_show_pane_schemes;  /* v2.1.0：方案选择浮层（预设方案列表）是否打开 */
extern int g_settings_pane_scroll;  /* 窗格配色页：单列模式下的行滚动（矮终端） */
extern int g_settings_appear_scroll;    /* v2.1.0：外观页纵向滚动（终端太矮时） */
extern int g_settings_sidebar_scroll;     /* v2.1.2：设置页侧栏菜单项列表的滚动量 */
extern int g_settings_behavior_scroll;  /* v2.1.0：行为页纵向滚动 */
extern int g_settings_detail_scroll;    /* v2.1.0：菜单项详细配置页纵向滚动 */
extern int g_settings_startup_scroll;    /* v2.1.0：默认启动项页纵向滚动 */
extern int g_settings_theme_sel;    /* 0..theme_count-1 = 主题行；之后 = 语义色行 */
extern int g_settings_keys_sel;     /* 0 = 前缀键行；1..N = 动作行 */
extern int g_settings_keys_scroll;
extern int g_settings_behavior_sel;
extern int g_key_capture_active;    /* 1 = 正在等待用户按下新键位 */
extern char g_hex_edit_buf[8];      /* 语义色十六进制输入 */
extern int g_hex_edit_len, g_hex_edit_active, g_hex_edit_role;

extern char g_edit_name[sizeof(g_chooser_items[0].name)];
extern int g_edit_name_len, g_edit_name_pos;
extern char g_edit_cmd[256];
extern int g_edit_cmd_len, g_edit_cmd_pos;
extern char g_edit_dir[256];
extern int g_edit_color;   /* 菜单项编辑器里的启动默认颜色 0-8 */
extern int g_edit_dir_len, g_edit_dir_pos;

extern const ChooserItem g_presets[];
extern const int g_preset_count;

void init_default_config(void);
void load_config(void);
void save_config(void);
int config_parse_bool(const char *val, int fallback);

/* 侧栏导航顺序（含新增分类页），供键盘 Ctrl+↑/↓ 循环使用 */
int settings_nav_order_count(void);
int settings_nav_at(int idx);
int settings_nav_index_of(int nav);
void open_config_file(void);
void load_item_to_editor(int idx);
void save_editor_to_item(int idx);

#endif // WIN_TERMUX_CONFIG_H
