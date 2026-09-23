#include "theme.h"

/* ---------------------------------------------------------------------------
 * 内置主题
 * 每个主题只需要给出 16 个语义角色色，其余 30 多个界面用色由参考表按
 * 「角色 + 底色 + 混合比例」自动派生。
 * ------------------------------------------------------------------------- */

#define C(r, g, b) {(unsigned char)(r), (unsigned char)(g), (unsigned char)(b)}

const ThemeDef g_builtin_themes[] = {
    {"github-dark", {
        C( 13,  17,  23), C( 22,  27,  34), C( 33,  38,  45), C(230, 237, 243),
        C(139, 148, 158), C(255, 255, 255), C( 31, 111, 235), C(121, 192, 255),
        C( 63, 185,  80), C( 31, 136,  61), C(248,  81,  73), C(217, 119,  54),
        C(210, 153,  34), C(137,  87, 229), C(205,  93, 173), C( 38,  75, 110),
    }},
    {"one-dark", {
        C( 22,  25,  30), C( 33,  37,  43), C( 47,  53,  63), C(220, 226, 236),
        C(150, 160, 176), C(255, 255, 255), C( 61, 141, 224), C(121, 202, 255),
        C(126, 196,  84), C( 76, 140,  56), C(232,  86,  96), C(226, 145,  70),
        C(240, 195, 100), C(180, 110, 226), C(230, 110, 180), C( 45,  74, 110),
    }},
    {"nord", {
        C( 30,  35,  44), C( 42,  49,  61), C( 55,  63,  78), C(229, 233, 240),
        C(154, 165, 186), C(255, 255, 255), C( 74, 123, 184), C(136, 198, 220),
        C(150, 196, 118), C( 90, 130,  76), C(214,  98, 108), C(219, 138, 100),
        C(238, 206, 130), C(184, 138, 200), C(214, 138, 196), C( 48,  76, 112),
    }},
    {"gruvbox-dark", {
        C( 22,  24,  25), C( 33,  33,  33), C( 52,  49,  46), C(240, 226, 190),
        C(180, 165, 140), C(255, 249, 220), C( 60, 140, 145), C(126, 178, 160),
        C(160, 175,  50), C(104, 116,  20), C(234,  70,  60), C(232, 110,  30),
        C(226, 168,  40), C(206, 120, 158), C(226, 146, 172), C( 46,  72,  92),
    }},
    {"dracula", {
        C( 22,  23,  30), C( 32,  34,  44), C( 52,  55,  72), C(248, 248, 242),
        C(150, 160, 200), C(255, 255, 255), C(150, 110, 236), C(130, 226, 248),
        C( 80, 224, 118), C( 44, 150,  84), C(248,  90,  90), C(248, 168,  90),
        C(238, 226, 130), C(180, 132, 248), C(248, 120, 190), C( 56,  70, 108),
    }},
};
const int g_builtin_theme_count = (int)(sizeof(g_builtin_themes) / sizeof(g_builtin_themes[0]));

/* ---------------------------------------------------------------------------
 * 参考色板：render.c / utf8.c / pane.c 中出现的每一个 UI 颜色
 * out = base + mix% * (role - base)
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned char r, g, b;      /* github-dark 下的原始值（即字面量中的值） */
    unsigned char role, base, mix;
} ThemeRef;

static const ThemeRef g_theme_refs[] = {
    { 13,  17,  23, TH_BG0,        TH_BG0,   100},
    { 22,  27,  34, TH_BG1,        TH_BG1,   100},
    { 33,  38,  45, TH_BG2,        TH_BG2,   100},
    { 27,  33,  44, TH_BG2,        TH_BG1,    80},
    {230, 237, 243, TH_FG,         TH_FG,    100},
    {139, 148, 158, TH_FG_DIM,     TH_FG_DIM, 100},
    {110, 118, 129, TH_FG_DIM,     TH_BG1,    78},
    { 48,  54,  61, TH_FG_DIM,     TH_BG2,    16},
    {255, 255, 255, TH_WHITE,      TH_WHITE, 100},
    { 31, 111, 235, TH_ACCENT,     TH_ACCENT, 100},
    { 22,  62, 128, TH_ACCENT,     TH_BG1,    45},
    { 38,  50,  68, TH_ACCENT,     TH_BG2,    15},
    { 38,  60,  88, TH_ACCENT,     TH_BG2,    25},
    { 48,  75, 110, TH_ACCENT,     TH_BG2,    35},
    {121, 192, 255, TH_CYAN,       TH_CYAN,  100},
    { 52,  96, 128, TH_CYAN,       TH_BG1,    40},
    { 45,  55,  72, TH_CYAN,       TH_BG2,    12},
    {140, 205, 255, TH_CYAN,       TH_WHITE,  85},
    {225, 235, 250, TH_CYAN,       TH_WHITE,  20},
    { 88, 166, 255, TH_CYAN,       TH_ACCENT, 60},
    { 63, 185,  80, TH_GREEN,      TH_GREEN, 100},
    { 36,  99,  49, TH_GREEN,      TH_BG1,    50},
    { 31, 136,  61, TH_GREEN_DARK, TH_GREEN_DARK, 100},
    { 24,  80,  48, TH_GREEN_DARK, TH_BG1,    50},
    {248,  81,  73, TH_RED,        TH_RED,   100},
    {217, 119,  54, TH_ORANGE,     TH_ORANGE, 100},
    {112,  66,  34, TH_ORANGE,     TH_BG1,    46},
    {210, 153,  34, TH_YELLOW,     TH_YELLOW, 100},
    {110,  82,  30, TH_YELLOW,     TH_BG1,    47},
    {137,  87, 229, TH_PURPLE,     TH_PURPLE, 100},
    { 74,  48, 122, TH_PURPLE,     TH_BG1,    45},
    {163, 113, 247, TH_PURPLE,     TH_WHITE,  78},
    {205,  93, 173, TH_PINK,       TH_PINK,  100},
    {104,  50,  90, TH_PINK,       TH_BG1,    45},
};
static const int g_theme_ref_count = (int)(sizeof(g_theme_refs) / sizeof(g_theme_refs[0]));

static const char *const g_role_names[TH_ROLE_COUNT] = {
    "background", "tabbar", "panel", "foreground",
    "foreground_dim", "white", "accent", "cyan",
    "green", "green_dark", "red", "orange",
    "yellow", "purple", "pink", "selection"
};

/* ---------------------------------------------------------------------------
 * 运行时状态
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned char from_r, from_g, from_b;
    char to[12];                 /* "RRR;GGG;BBB" 定长 11 字节，等长替换 */
} ThemeMapEntry;

static int g_theme_idx = 0;
static ThemeRGB g_roles[TH_ROLE_COUNT];
static unsigned char g_override_set[TH_ROLE_COUNT];
static ThemeRGB g_override_val[TH_ROLE_COUNT];
static unsigned char g_pane_set[THEME_PANE_SLOTS];
static ThemeRGB g_pane_val[THEME_PANE_SLOTS];
static ThemeMapEntry g_map[sizeof(g_theme_refs) / sizeof(g_theme_refs[0])];
static int g_map_count = 0;
static int g_identity = 1;

static unsigned char blend_ch(int role_v, int base_v, int mix) {
    int v = base_v + (role_v - base_v) * mix / 100;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (unsigned char)v;
}

void theme_init(void) {
    g_theme_idx = 0;
    memset(g_override_set, 0, sizeof(g_override_set));
    memset(g_override_val, 0, sizeof(g_override_val));
    memset(g_pane_set, 0, sizeof(g_pane_set));
    theme_apply();
}

int theme_set_by_name(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < g_builtin_theme_count; i++) {
        if (_stricmp(name, g_builtin_themes[i].name) == 0) {
            g_theme_idx = i;
            return 1;
        }
    }
    return 0;
}

int theme_role_index(const char *role_name) {
    if (!role_name) return -1;
    for (int i = 0; i < TH_ROLE_COUNT; i++)
        if (_stricmp(role_name, g_role_names[i]) == 0) return i;
    return -1;
}

const char *theme_role_name(int role) {
    if (role < 0 || role >= TH_ROLE_COUNT) return "";
    return g_role_names[role];
}

static int hex_nib(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int theme_set_role_hex(const char *role_name, const char *hex) {
    int role = theme_role_index(role_name);
    if (role < 0 || !hex) return 0;
    while (*hex == ' ' || *hex == '\t' || *hex == '#') hex++;
    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = hex_nib((unsigned char)hex[i]);
        if (v[i] < 0) return 0;
    }
    /* 第 7 个字符必须是结束或空白，避免 "#1234567" 这类脏值被接受 */
    if (hex[6] && hex[6] != ' ' && hex[6] != '\t' && hex[6] != '\r' && hex[6] != '\n') return 0;
    g_override_set[role] = 1;
    g_override_val[role].r = (unsigned char)(v[0] * 16 + v[1]);
    g_override_val[role].g = (unsigned char)(v[2] * 16 + v[3]);
    g_override_val[role].b = (unsigned char)(v[4] * 16 + v[5]);
    return 1;
}

void theme_apply(void) {
    const ThemeDef *def = &g_builtin_themes[g_theme_idx];
    for (int i = 0; i < TH_ROLE_COUNT; i++)
        g_roles[i] = g_override_set[i] ? g_override_val[i] : def->role[i];

    g_map_count = 0;
    g_identity = 1;
    for (int i = 0; i < g_theme_ref_count; i++) {
        const ThemeRef *ref = &g_theme_refs[i];
        const ThemeRGB *role = &g_roles[ref->role];
        const ThemeRGB *base = &g_roles[ref->base];
        unsigned char r = blend_ch(role->r, base->r, ref->mix);
        unsigned char g = blend_ch(role->g, base->g, ref->mix);
        unsigned char b = blend_ch(role->b, base->b, ref->mix);
        if (r != ref->r || g != ref->g || b != ref->b) g_identity = 0;
        ThemeMapEntry *e = &g_map[g_map_count++];
        e->from_r = ref->r; e->from_g = ref->g; e->from_b = ref->b;
        snprintf(e->to, sizeof(e->to), "%03u;%03u;%03u",
                 (unsigned)r, (unsigned)g, (unsigned)b);
    }
}

void theme_role_rgb(int role, int *r, int *g, int *b) {
    if (role < 0 || role >= TH_ROLE_COUNT) { if (r) *r = 0; if (g) *g = 0; if (b) *b = 0; return; }
    if (r) *r = g_roles[role].r;
    if (g) *g = g_roles[role].g;
    if (b) *b = g_roles[role].b;
}

WORD theme_role_rgb565(int role) {
    int r, g, b;
    theme_role_rgb(role, &r, &g, &b);
    return rgb565(r, g, b);
}

static int digits3(const char *p) {
    return p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9';
}

static int num3(const char *p) {
    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

/* 等长就地替换：只命中 "038;2;RRR;GGG;BBB" / "048;2;RRR;GGG;BBB" 形式。
 * pane 内容里的颜色由 %d 输出（"38;2;121;192;255"），不带前导零的 038/048
 * 前缀，因此永远不会被误改。 */
void theme_remap(char *buf, int len) {
    if (g_identity || !buf || len < 17) return;
    for (int i = 0; i + 17 <= len; i++) {
        if (buf[i] != '0') continue;
        if (buf[i + 1] != '3' && buf[i + 1] != '4') continue;
        if (buf[i + 2] != '8' || buf[i + 3] != ';' || buf[i + 4] != '2' || buf[i + 5] != ';') continue;
        char *p = buf + i + 6;
        if (!digits3(p) || p[3] != ';' || !digits3(p + 4) || p[7] != ';' || !digits3(p + 8)) continue;
        char term = p[11];
        if (term != 'm' && term != ';') continue;
        int r = num3(p), g = num3(p + 4), b = num3(p + 8);
        for (int k = 0; k < g_map_count; k++) {
            if (g_map[k].from_r == r && g_map[k].from_g == g && g_map[k].from_b == b) {
                memcpy(p, g_map[k].to, 11);
                break;
            }
        }
        i += 16;
    }
}

const char *theme_name(void) { return g_builtin_themes[g_theme_idx].name; }
int theme_index(void) { return g_theme_idx; }
int theme_count(void) { return g_builtin_theme_count; }
const char *theme_name_at(int idx) {
    if (idx < 0 || idx >= g_builtin_theme_count) return "";
    return g_builtin_themes[idx].name;
}

int theme_role_is_overridden(int role) {
    if (role < 0 || role >= TH_ROLE_COUNT) return 0;
    return g_override_set[role] ? 1 : 0;
}

void theme_clear_overrides(void) {
    memset(g_override_set, 0, sizeof(g_override_set));
    memset(g_override_val, 0, sizeof(g_override_val));
    memset(g_pane_set, 0, sizeof(g_pane_set));
}

/* ---- 窗格 palette（见 theme.h）---- */
static const char *const g_pane_slot_names[THEME_PANE_SLOTS] = {
    "pane_black", "pane_red", "pane_green", "pane_yellow",
    "pane_blue", "pane_magenta", "pane_cyan", "pane_white",
    "pane_bright_black", "pane_bright_red", "pane_bright_green", "pane_bright_yellow",
    "pane_bright_blue", "pane_bright_magenta", "pane_bright_cyan", "pane_bright_white",
    "pane_foreground", "pane_background",
};

int theme_pane_slot_index(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < THEME_PANE_SLOTS; i++)
        if (_stricmp(name, g_pane_slot_names[i]) == 0) return i;
    return -1;
}

const char *theme_pane_slot_name(int slot) {
    if (slot < 0 || slot >= THEME_PANE_SLOTS) return "";
    return g_pane_slot_names[slot];
}

static int parse_hex6(const char *hex, ThemeRGB *out) {
    if (!hex) return 0;
    while (*hex == ' ' || *hex == '\t' || *hex == '#') hex++;
    int v[6];
    for (int i = 0; i < 6; i++) { v[i] = hex_nib((unsigned char)hex[i]); if (v[i] < 0) return 0; }
    if (hex[6] && hex[6] != ' ' && hex[6] != '\t' && hex[6] != '\r' && hex[6] != '\n') return 0;
    out->r = (unsigned char)(v[0] * 16 + v[1]);
    out->g = (unsigned char)(v[2] * 16 + v[3]);
    out->b = (unsigned char)(v[4] * 16 + v[5]);
    return 1;
}

int theme_set_pane_hex(const char *name, const char *hex) {
    int slot = theme_pane_slot_index(name);
    if (slot < 0) return 0;
    ThemeRGB c;
    if (!parse_hex6(hex, &c)) return 0;
    g_pane_val[slot] = c;
    g_pane_set[slot] = 1;
    return 1;
}

int theme_pane_rgb(int slot, int *r, int *g, int *b) {
    if (slot < 0 || slot >= THEME_PANE_SLOTS || !g_pane_set[slot]) return 0;
    if (r) *r = g_pane_val[slot].r;
    if (g) *g = g_pane_val[slot].g;
    if (b) *b = g_pane_val[slot].b;
    return 1;
}

int theme_pane_any(void) {
    for (int i = 0; i < THEME_PANE_SLOTS; i++) if (g_pane_set[i]) return 1;
    return 0;
}

void theme_pane_get(int slot, char *hex_out, int cap) {
    if (!hex_out || cap <= 0) return;
    hex_out[0] = 0;
    if (slot < 0 || slot >= THEME_PANE_SLOTS || !g_pane_set[slot]) return;
    snprintf(hex_out, cap, "#%02x%02x%02x", g_pane_val[slot].r, g_pane_val[slot].g, g_pane_val[slot].b);
}

void theme_clear_role_override(int role) {
    if (role < 0 || role >= TH_ROLE_COUNT) return;
    g_override_set[role] = 0;
}

int theme_has_overrides(void) {
    for (int i = 0; i < TH_ROLE_COUNT; i++) if (g_override_set[i]) return 1;
    return 0;
}

void theme_get_override(int role, char *out_hex, int out_size) {
    if (!out_hex || out_size <= 0) return;
    out_hex[0] = 0;
    if (role < 0 || role >= TH_ROLE_COUNT || !g_override_set[role]) return;
    snprintf(out_hex, out_size, "#%02x%02x%02x",
             g_override_val[role].r, g_override_val[role].g, g_override_val[role].b);
}
