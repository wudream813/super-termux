#ifndef WIN_TERMUX_THEME_H
#define WIN_TERMUX_THEME_H

#include "common.h"

/* ---------------------------------------------------------------------------
 * 主题引擎 (theme engine)
 *
 * 渲染层的所有 UI 配色都以 "零填充三位" 的真彩序列写在字符串字面量里，例如
 *   "\x1b[48;2;033;038;045m"
 * 而 pane 内容的颜色是用 %d 动态拼出来的（不带前导零）。这个差异让我们可以在
 * 输出前对整帧做一次「只命中 UI 配色」的等长就地替换 —— 既不用把 render.c 里
 * 300 多处字面量改成 %s，也绝不会误伤终端程序自己输出的颜色。
 *
 * 参考色板 (github-dark) 中的每个颜色都登记在 g_theme_refs 里，并映射为
 * 「语义角色 + 与底色的混合比例」，因此换主题只需要提供 16 个角色色即可。
 * 默认主题走 identity 快路径，一个字节都不会被改写。
 * ------------------------------------------------------------------------- */

enum {
    TH_BG0 = 0,      /* 最深背景 / 亮底上的文字色 */
    TH_BG1,          /* 标签栏背景 */
    TH_BG2,          /* 面板、弹窗背景 */
    TH_FG,           /* 主文字 */
    TH_FG_DIM,       /* 次要文字 */
    TH_WHITE,        /* 高亮文字 */
    TH_ACCENT,       /* 主强调色（活动标签、边框） */
    TH_CYAN,         /* 信息色 / 浅蓝 */
    TH_GREEN,
    TH_GREEN_DARK,
    TH_RED,
    TH_ORANGE,
    TH_YELLOW,
    TH_PURPLE,
    TH_PINK,
    TH_SELECTION,    /* 选区底色 */
    TH_ROLE_COUNT
};

typedef struct {
    unsigned char r, g, b;
} ThemeRGB;

typedef struct {
    const char *name;
    ThemeRGB role[TH_ROLE_COUNT];
} ThemeDef;

extern const ThemeDef g_builtin_themes[];
extern const int g_builtin_theme_count;

/* 初始化为默认主题（github-dark），清空所有 [theme] 覆盖项。 */
void theme_init(void);

/* 按名称切换内置主题；未知名称返回 0 并保持原主题。 */
int theme_set_by_name(const char *name);

/* [theme] 段的单项覆盖，如 theme_set_role_hex("accent", "#58a6ff")。 */
int theme_set_role_hex(const char *role_name, const char *hex);

/* 角色名 <-> 索引，供配置读写与验证脚本使用。 */
int theme_role_index(const char *role_name);
const char *theme_role_name(int role);

/* 重新计算替换表。修改主题或覆盖项后必须调用（load_config 内部已调用）。 */
void theme_apply(void);

/* 对渲染输出做等长就地重映射；默认主题下为空操作。 */
void theme_remap(char *buf, int len);

/* 取当前主题下某角色的实际颜色。 */
void theme_role_rgb(int role, int *r, int *g, int *b);
WORD theme_role_rgb565(int role);

const char *theme_name(void);
int theme_index(void);
int theme_count(void);
const char *theme_name_at(int idx);
/* 存在 [theme] 覆盖项时为 1（保存配置时需要原样写回）。 */
int theme_has_overrides(void);

/* ---- 窗格 16 色 palette（v2.0.6）----------------------------------------
 * 上面 16 个角色只管 termux 自己的 UI（标签栏 / 面板 / 边框）。窗格里 cmd 的
 * 普通文字是 16 色索引属性（0x07 = 灰字黑底），渲染时原样发 \x1b[37;40m，
 * 由宿主终端按它自己的 palette 画 —— 所以改 background 对 cmd 背景无效。
 * 这组配置像 Windows Terminal 的 color scheme：把窗格的默认前景 / 默认背景 /
 * 16 个索引色各映射到一个 RGB，渲染时改发真彩色。【一项都没设时完全透传】，
 * 现有行为零改变（identity 断言仍成立）。
 *   pane_foreground / pane_background     默认前后景（SGR 39 / 49 与 0x07）
 *   pane_black … pane_white               索引 0..7
 *   pane_bright_black … pane_bright_white 索引 8..15 */
#define THEME_PANE_FG   16
#define THEME_PANE_BG   17
/* v2.0.8：滚动条颜色。thumb = 滑块、track = 轨道底色。不设时用内置渐变
 * （深色系，浅色 pane_background 下会和背景融为一体——用户反馈）。 */
#define THEME_PANE_SB_THUMB 18
#define THEME_PANE_SB_TRACK 19
#define THEME_PANE_SLOTS 20
/* 名字 -> 槽位（0..15 索引色，16 fg，17 bg）；不是 pane_* 返回 -1。 */
/* v2.0.9：窗格配色预设方案（Campbell / One Half / Solarized / GitHub Light / Dracula / Nord）。
 * apply 会把全部 20 个槽位一次写满（滚动条滑块 = 亮黑，轨道 = 背景），之后仍可逐项改。 */
int  theme_pane_scheme_count(void);
const char *theme_pane_scheme_name(int i);
int  theme_pane_scheme_apply(int i);
/* 当前 20 个槽位与方案 i 完全一致时返回 1（设置页用来标 [●]）。 */
int  theme_pane_scheme_matches(int i);
int  theme_pane_slot_index(const char *name);
const char *theme_pane_slot_name(int slot);
int  theme_set_pane_hex(const char *name, const char *hex);
/* 该槽位有映射时返回 1 并填 RGB；否则 0（渲染方应原样发 16 色索引）。 */
int  theme_pane_rgb(int slot, int *r, int *g, int *b);
int  theme_pane_any(void);
void theme_pane_get(int slot, char *hex_out, int cap);   /* 无映射写空串 */
const char *theme_pane_slot_label(int slot);              /* 设置页显示用的中文名 */
void theme_clear_pane_slot(int slot);
void theme_clear_pane_all(void);
/* 没设映射时设置页也要给用户看个色块：返回 xterm 默认 16 色近似值 */
void theme_pane_fallback_rgb(int slot, int *r, int *g, int *b);
int theme_role_is_overridden(int role);
void theme_clear_overrides(void);
void theme_clear_role_override(int role);
void theme_get_override(int role, char *out_hex, int out_size);

#endif /* WIN_TERMUX_THEME_H */
