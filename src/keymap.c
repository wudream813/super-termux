#include "keymap.h"
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * 动作表
 * ------------------------------------------------------------------------- */
typedef struct {
    int action;
    const char *name;
    const char *label;
} ActionInfo;

static const ActionInfo g_actions[] = {
    {ACT_SEND_PREFIX,     "send-prefix",     "发送前缀键本身"},
    {ACT_COMMAND_PALETTE, "command-palette", "打开命令面板"},
    {ACT_NEW_PANE,        "new-pane",        "新建默认 pane"},
    {ACT_NEW_PANE_MENU,   "new-pane-menu",   "新建 pane 菜单"},
    {ACT_COPY_MODE,       "copy-mode",       "进入复制模式"},
    {ACT_SEARCH,          "search",          "搜索滚动历史"},
    {ACT_SETTINGS,        "settings",        "打开图形化设置"},
    {ACT_RELOAD_CONFIG,   "reload-config",   "热重载配置文件"},
    {ACT_HELP,            "help",            "打开 / 关闭帮助"},
    {ACT_NEXT_PANE,       "next-pane",       "下一个 pane"},
    {ACT_PREV_PANE,       "prev-pane",       "上一个 pane"},
    {ACT_CLOSE_PANE,      "close-pane",      "关闭当前 pane"},
    {ACT_QUIT,            "quit",            "退出 termux"},
    {ACT_TAB_COLOR_NEXT,  "tab-color-next",  "下一个标签颜色"},
    {ACT_TAB_COLOR_PREV,  "tab-color-prev",  "上一个标签颜色"},
    {ACT_SWITCH_PANEL_PALETTE, "switch-panel", "切换 panel"},
    {ACT_NEXT_THEME,      "next-theme",      "切换下一个主题"},
    {ACT_SPLIT_HORIZONTAL, "split-horizontal", "分屏：上下切分"},
    {ACT_SPLIT_VERTICAL,   "split-vertical",   "分屏：左右切分"},
    {ACT_SPLIT_NEXT,       "split-next",       "分屏：下一个窗格"},
    {ACT_SPLIT_PREV,       "split-prev",       "分屏：上一个窗格"},
    {ACT_SPLIT_CLOSE,      "split-close",      "分屏：关闭当前窗格"},
    {ACT_SPLIT_ZOOM,       "split-zoom",       "分屏：当前窗格全屏缩放/还原"},
    {ACT_SPLIT_UP,         "split-up",         "分屏：切换到上方窗格"},
    {ACT_SPLIT_DOWN,       "split-down",       "分屏：切换到下方窗格"},
    {ACT_SPLIT_LEFT,       "split-left",       "分屏：切换到左方窗格"},
    {ACT_SPLIT_RIGHT,      "split-right",      "分屏：切换到右方窗格"},
    {ACT_SPLIT_RESIZE_UP,  "split-resize-up",  "分屏：把分隔线上移"},
    {ACT_SPLIT_RESIZE_DOWN,"split-resize-down","分屏：把分隔线下移"},
    {ACT_SPLIT_RESIZE_LEFT,"split-resize-left","分屏：把分隔线左移"},
    {ACT_SPLIT_RESIZE_RIGHT,"split-resize-right","分屏：把分隔线右移"},
};
static const int g_action_count = (int)(sizeof(g_actions) / sizeof(g_actions[0]));

/* ---------------------------------------------------------------------------
 * 默认键位（完整复刻 v1.8.3 的行为，含各种键盘布局兜底）
 * ------------------------------------------------------------------------- */
#define VKEY(v, sh)     {(WORD)(v), 0, 0, 0, (unsigned char)(sh), 0}
#define VKEY_ANY(v)     {(WORD)(v), 0, 0, 0, 0, 1}
#define CHR(c)          {0, (WCHAR)(c), 0, 0, 0, 1}
#define VKEY_SHIFT(v)   {(WORD)(v), 0, 0, 0, 1, 0}

static const KeyBinding g_default_bindings[] = {
    /* 新建 pane 菜单：'+' / 小键盘 + / Shift+= */
    {CHR('+'),                     ACT_NEW_PANE_MENU,   0},
    {VKEY_ANY(VK_ADD),             ACT_NEW_PANE_MENU,   0},
    {VKEY_SHIFT(VK_OEM_PLUS),      ACT_NEW_PANE_MENU,   0},
    /* 命令面板：':'（含中文全角冒号）/ Shift+; */
    {CHR(':'),                     ACT_COMMAND_PALETTE, 0},
    {CHR(0xFF1A),                  ACT_COMMAND_PALETTE, 0},
    {VKEY_SHIFT(VK_OEM_1),         ACT_COMMAND_PALETTE, 0},
    /* 历史搜索：'/' */
    {CHR('/'),                     ACT_SEARCH,          0},
    {VKEY(VK_OEM_2, 0),            ACT_SEARCH,          0},
    /* 帮助：'?' / 'h' */
    {CHR('?'),                     ACT_HELP,            0},
    {CHR('h'),                     ACT_HELP,            0},
    {CHR('H'),                     ACT_HELP,            0},
    {VKEY_SHIFT(VK_OEM_2),         ACT_HELP,            0},
    /* 复制模式：'[' */
    {CHR('['),                     ACT_COPY_MODE,       0},
    {VKEY_ANY(VK_OEM_4),           ACT_COPY_MODE,       0},
    /* 其余单字母动作 */
    {VKEY_ANY('R'),                ACT_RELOAD_CONFIG,   0},
    {VKEY_ANY('C'),                ACT_NEW_PANE,        0},
    {VKEY_ANY('N'),                ACT_NEXT_PANE,       0},
    {VKEY_ANY('P'),                ACT_PREV_PANE,       0},
    {VKEY_ANY('X'),                ACT_CLOSE_PANE,      0},
    {VKEY_ANY('D'),                ACT_QUIT,            0},
    {VKEY_ANY('S'),                ACT_SETTINGS,        0},
    {VKEY_SHIFT('T'),              ACT_TAB_COLOR_PREV,  0},
    {VKEY('T', 0),                 ACT_TAB_COLOR_NEXT,  0},
    /* 打开「切换 panel」命令面板（tmux 惯例前缀 w = choose-window）。
     * 主键盘数字 1-9/0 直接跳转的绑定已移除，统一改走这个可视化切换面板。 */
    {VKEY_ANY('W'),                ACT_SWITCH_PANEL_PALETTE, 0},
    /* pane 跳转：小键盘数字「按编号跳转」已移除，统一走前缀 w 的可视化「切换 panel」。 */
    /* ---- 分屏（前缀键之后） ---- */
    {VKEY_SHIFT(VK_OEM_MINUS),     ACT_SPLIT_HORIZONTAL, 0},  /* 前缀 _ ：上下分屏 */
    /* 同一条再挂一个【纯字符】绑定：中文输入法把 Shift+- 变成 VK_PACKET + 全角
     * ＿(U+FF3F)，上面那条按虚拟键码匹配的抓不到。CHR() 不看 vk、也不要求
     * shift 状态，配合 spec_match 的全角折叠，半角 _ 和全角 ＿ 都能命中。
     * （2026-09-20 用户报「ctrl+b 加中文时的 _ 没反应」。） */
    {CHR('_'),                     ACT_SPLIT_HORIZONTAL, 0},
    {CHR('-'),                     ACT_SPLIT_VERTICAL,   0},  /* 前缀 - ：左右分屏 */
    {CHR('|'),                     ACT_SPLIT_VERTICAL,   0},  /* 前缀 | 兜底 */
    {CHR('\t'),                    ACT_SPLIT_NEXT,       0},  /* 前缀 Tab */
    {VKEY_SHIFT(VK_TAB),           ACT_SPLIT_PREV,       0},
    /* 关闭分屏窗格用 q（quit-pane 语义），避免与关标签页的 x 冲突。 */
    {VKEY_ANY('Q'),                ACT_SPLIT_CLOSE,      0},
    /* z = 当前窗格全屏缩放 / 还原（z 未被占用）。 */
    {VKEY_ANY('Z'),                ACT_SPLIT_ZOOM,       0},
    /* 方向键切换窗格；Shift+方向 拖动分隔线。 */
    {VKEY(VK_UP, 0),               ACT_SPLIT_UP,         0},
    {VKEY(VK_DOWN, 0),             ACT_SPLIT_DOWN,       0},
    {VKEY(VK_LEFT, 0),             ACT_SPLIT_LEFT,       0},
    {VKEY(VK_RIGHT, 0),            ACT_SPLIT_RIGHT,      0},
    {VKEY_SHIFT(VK_UP),            ACT_SPLIT_RESIZE_UP,  0},
    {VKEY_SHIFT(VK_DOWN),          ACT_SPLIT_RESIZE_DOWN,0},
    {VKEY_SHIFT(VK_LEFT),          ACT_SPLIT_RESIZE_LEFT,0},
    {VKEY_SHIFT(VK_RIGHT),         ACT_SPLIT_RESIZE_RIGHT,0},
};
static const int g_default_count = (int)(sizeof(g_default_bindings) / sizeof(g_default_bindings[0]));

/* ---------------------------------------------------------------------------
 * 运行时状态
 * ------------------------------------------------------------------------- */
typedef struct {
    KeyBinding bind;
    char key_text[24];
    unsigned char no_prefix;   /* 1 = 这个键不需要先按前缀，直接触发 */
} UserBinding;

static UserBinding g_user[KEYMAP_MAX_USER_BINDINGS];
static int g_user_count = 0;
static unsigned char g_action_overridden[ACT_COUNT];
static KeySpec g_prefix;
static char g_prefix_text[24];

/* 命名键 */
typedef struct { const char *name; WORD vk; } NamedKey;
static const NamedKey g_named_keys[] = {
    {"space", VK_SPACE}, {"tab", VK_TAB}, {"enter", VK_RETURN}, {"return", VK_RETURN},
    {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE}, {"backspace", VK_BACK}, {"bs", VK_BACK},
    {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
    {"home", VK_HOME}, {"end", VK_END}, {"pgup", VK_PRIOR}, {"pageup", VK_PRIOR},
    {"pgdn", VK_NEXT}, {"pagedown", VK_NEXT}, {"ins", VK_INSERT}, {"insert", VK_INSERT},
    {"del", VK_DELETE}, {"delete", VK_DELETE},
};
static const int g_named_key_count = (int)(sizeof(g_named_keys) / sizeof(g_named_keys[0]));

void keymap_init(void) {
    g_user_count = 0;
    memset(g_action_overridden, 0, sizeof(g_action_overridden));
    keymap_set_prefix("C-b");
}

int keymap_action_id(const char *name) {
    if (!name) return ACT_NONE;
    for (int i = 0; i < g_action_count; i++)
        if (_stricmp(name, g_actions[i].name) == 0) return g_actions[i].action;
    return ACT_NONE;
}

const char *keymap_action_name(int action) {
    for (int i = 0; i < g_action_count; i++)
        if (g_actions[i].action == action) return g_actions[i].name;
    return "";
}

const char *keymap_action_label(int action) {
    for (int i = 0; i < g_action_count; i++)
        if (g_actions[i].action == action) return g_actions[i].label;
    return "";
}

int keymap_parse_key(const char *text, KeySpec *out) {
    if (!text || !out) return 0;
    KeySpec spec;
    memset(&spec, 0, sizeof(spec));

    char buf[32];
    int n = 0;
    while (*text == ' ' || *text == '\t') text++;
    while (text[n] && text[n] != ' ' && text[n] != '\t' && text[n] != '\r' && text[n] != '\n' &&
           n < (int)sizeof(buf) - 1)
        { buf[n] = text[n]; n++; }
    buf[n] = 0;
    if (n == 0) return 0;

    char *p = buf;
    int explicit_shift = 0;
    /* 前缀修饰符：C- (Ctrl)、M-/A- (Alt)、S- (Shift)。单独的 "-" 表示减号键。 */
    while (p[0] && p[1] == '-' && p[2]) {
        char m = (char)toupper((unsigned char)p[0]);
        if (m == 'C') spec.ctrl = 1;
        else if (m == 'M' || m == 'A') spec.alt = 1;
        else if (m == 'S') { spec.shift = 1; explicit_shift = 1; }
        else break;
        p += 2;
    }
    if (!*p) return 0;

    int plen = (int)strlen(p);
    if (plen == 1) {
        unsigned char c = (unsigned char)p[0];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            spec.vk = (WORD)toupper(c);
            if (!explicit_shift) spec.shift_any = 1;
        } else if (c >= '0' && c <= '9') {
            spec.vk = (WORD)c;
            if (!explicit_shift) spec.shift_any = 1;
        } else {
            spec.ch = (WCHAR)c;
            spec.shift_any = 1;
        }
        *out = spec;
        return 1;
    }

    if ((p[0] == 'f' || p[0] == 'F') && plen <= 3) {
        int num = atoi(p + 1);
        if (num >= 1 && num <= 24) {
            spec.vk = (WORD)(VK_F1 + num - 1);
            if (!explicit_shift) spec.shift_any = 1;
            *out = spec;
            return 1;
        }
    }

    for (int i = 0; i < g_named_key_count; i++) {
        if (_stricmp(p, g_named_keys[i].name) == 0) {
            spec.vk = g_named_keys[i].vk;
            if (!explicit_shift) spec.shift_any = 1;
            *out = spec;
            return 1;
        }
    }
    return 0;
}

int keymap_bind(const char *action_name, const char *key_text) {
    int action = keymap_action_id(action_name);
    if (action == ACT_NONE || action >= ACT_COUNT) return 0;
    if (g_user_count >= KEYMAP_MAX_USER_BINDINGS) return 0;

    /* 用户自定义键位：「动作名 = 键位」。pane 切换统一走 switch-panel（前缀 w
     * 可视化面板），已无带参数的 select-pane。 */
    KeySpec spec;
    if (!keymap_parse_key(key_text, &spec)) return 0;

    /* "M-n noprefix" / "F5 direct"：键后面的可选标记表示这个动作不走前缀。 */
    int no_prefix = 0;
    char key_only[24] = {0};
    {
        const char *t = key_text;
        while (*t == ' ' || *t == '\t') t++;
        int n = 0;
        while (t[n] && t[n] != ' ' && t[n] != '\t' && n < (int)sizeof(key_only) - 1) {
            key_only[n] = t[n];
            n++;
        }
        key_only[n] = 0;
        const char *rest = t + n;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (_strnicmp(rest, "noprefix", 8) == 0 || _strnicmp(rest, "no-prefix", 9) == 0 ||
            _strnicmp(rest, "direct", 6) == 0)
            no_prefix = 1;
    }

    UserBinding *ub = &g_user[g_user_count++];
    ub->bind.key = spec;
    ub->bind.action = action;
    ub->bind.arg = 0;
    ub->no_prefix = (unsigned char)no_prefix;
    snprintf(ub->key_text, sizeof(ub->key_text), "%s", key_only);
    g_action_overridden[action] = 1;
    return 1;
}

int keymap_set_prefix(const char *key_text) {
    KeySpec spec;
    if (!keymap_parse_key(key_text, &spec)) return 0;
    g_prefix = spec;
    snprintf(g_prefix_text, sizeof(g_prefix_text), "%s", key_text);
    return 1;
}

const char *keymap_prefix_text(void) { return g_prefix_text; }

static void spec_text(const KeySpec *s, char *out, int out_size);

void keymap_prefix_describe(char *out, int out_size) {
    if (!out || out_size <= 0) return;
    spec_text(&g_prefix, out, out_size);
}

int keymap_prefix_is_default(void) { return strcmp(g_prefix_text, "C-b") == 0; }

char keymap_prefix_char(void) {
    if (g_prefix.ctrl && g_prefix.vk >= 'A' && g_prefix.vk <= 'Z')
        return (char)(g_prefix.vk - 'A' + 1);
    return 0;
}

/* 全角 ASCII（U+FF01..U+FF5E）折成对应半角（U+0021..U+007E）。
 *
 * 中文输入法开全角标点时，Shift+- 送上来的是全角下划线 U+FF3F，而且往往是
 * VK_PACKET(0xE7) + 只有 UnicodeChar 有值 —— 走虚拟键码的绑定（VKEY_SHIFT
 * (VK_OEM_MINUS)）根本匹配不到，于是「Ctrl+B 然后 _」上下分屏在中文输入法下
 * 没反应（2026-09-20 用户报）。命令面板那条早就为同一件事单独加过 CHR(0xFF1A)
 * 全角冒号；这里改成统一折叠，所有 CHR() 绑定一次性都认全角孪生字符。
 *
 * 只影响【键位匹配】，不影响发给 pane 的字节。 */
static WCHAR keymap_fold_wide(WCHAR uc) {
    if (uc >= 0xFF01 && uc <= 0xFF5E) return (WCHAR)(uc - 0xFF01 + 0x21);
    return uc;
}

static int spec_match(const KeySpec *s, WORD vk, DWORD ctrl, WCHAR uc) {
    int is_ctrl = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    int is_alt = (ctrl & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    int is_shift = (ctrl & SHIFT_PRESSED) != 0;
    if (s->ctrl != (unsigned char)is_ctrl) return 0;
    if (s->alt != (unsigned char)is_alt) return 0;
    if (!s->shift_any && s->shift != (unsigned char)is_shift) return 0;
    if (s->vk) return s->vk == vk;
    if (s->ch) return uc != 0 && s->ch == keymap_fold_wide(uc);
    return 0;
}

int keymap_is_prefix(WORD vk, DWORD ctrl, WCHAR uc) {
    if (spec_match(&g_prefix, vk, ctrl, uc)) return 1;
    /* Ctrl+字母 在部分输入法/布局下只会送出控制字符 */
    char pc = keymap_prefix_char();
    if (pc && uc == (WCHAR)(unsigned char)pc) return 1;
    return 0;
}

int keymap_lookup(WORD vk, DWORD ctrl, WCHAR uc, int *arg) {
    if (arg) *arg = 0;
    for (int i = 0; i < g_user_count; i++) {
        if (g_user[i].no_prefix) continue;   /* 直接键不再挂在前缀下面 */
        if (spec_match(&g_user[i].bind.key, vk, ctrl, uc)) {
            if (arg) *arg = g_user[i].bind.arg;
            return g_user[i].bind.action;
        }
    }
    for (int i = 0; i < g_default_count; i++) {
        const KeyBinding *b = &g_default_bindings[i];
        if (g_action_overridden[b->action]) continue;
        if (spec_match(&b->key, vk, ctrl, uc)) {
            if (arg) *arg = b->arg;
            return b->action;
        }
    }
    return ACT_NONE;
}

int keymap_lookup_direct(WORD vk, DWORD ctrl, WCHAR uc, int *arg) {
    if (arg) *arg = 0;
    for (int i = 0; i < g_user_count; i++) {
        if (!g_user[i].no_prefix) continue;
        if (spec_match(&g_user[i].bind.key, vk, ctrl, uc)) {
            if (arg) *arg = g_user[i].bind.arg;
            return g_user[i].bind.action;
        }
    }
    return ACT_NONE;
}

int keymap_action_uses_prefix(int action) {
    for (int i = 0; i < g_user_count; i++)
        if (g_user[i].bind.action == action) return g_user[i].no_prefix ? 0 : 1;
    return 1;   /* 默认键位一律走前缀 */
}

/* 把一个 KeySpec 写回 ini 语法（"C-b" / "F5" / "t"），用于把默认键位固化成
 * 用户绑定 —— 切换“是否使用前缀”时必须先有一条用户绑定可以打标记。 */
static int spec_ini_text(const KeySpec *s, char *out, int out_size) {
    char key[12] = {0};
    if (s->ch) {
        if (s->ch < 128) snprintf(key, sizeof(key), "%c", (char)s->ch);
        else return 0;
    } else if (s->vk >= VK_F1 && s->vk <= VK_F24) {
        snprintf(key, sizeof(key), "F%d", s->vk - VK_F1 + 1);
    } else if ((s->vk >= 'A' && s->vk <= 'Z') || (s->vk >= '0' && s->vk <= '9')) {
        char c = (char)s->vk;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        snprintf(key, sizeof(key), "%c", c);
    } else {
        for (int i = 0; i < g_named_key_count; i++)
            if (g_named_keys[i].vk == s->vk) { snprintf(key, sizeof(key), "%s", g_named_keys[i].name); break; }
    }
    if (!key[0]) return 0;
    snprintf(out, out_size, "%s%s%s%s",
             s->ctrl ? "C-" : "", s->alt ? "M-" : "",
             (s->shift && !s->shift_any) ? "S-" : "", key);
    return 1;
}

int keymap_set_action_prefix(int action, int use_prefix) {
    if (action <= ACT_NONE || action >= ACT_COUNT) return 0;
    for (int i = 0; i < g_user_count; i++) {
        if (g_user[i].bind.action != action) continue;
        g_user[i].no_prefix = use_prefix ? 0 : 1;
        return 1;
    }
    /* 还在用默认键位：先固化成用户绑定，再打标记。 */
    const KeySpec *found = NULL;
    for (int i = 0; i < g_default_count && !found; i++)
        if (g_default_bindings[i].action == action) found = &g_default_bindings[i].key;
    if (!found) return 0;
    char text[24];
    if (!spec_ini_text(found, text, sizeof(text))) return 0;
    if (!keymap_bind(keymap_action_name(action), text)) return 0;
    g_user[g_user_count - 1].no_prefix = use_prefix ? 0 : 1;
    return 1;
}

static void spec_text(const KeySpec *s, char *out, int out_size) {
    char keyname[24];
    int oem_sym = 0;   /* 1 = keyname 是可打印 OEM 符号（_ : ? 等），Shift 已体现在符号里 */
    if (s->ch) {
        /* Tab 等控制字符必须用字面名（"Tab"），直接 %c 会输出原始控制符 0x09，
         * 命令面板按 utf8 宽度补 pad 时错位，表现为「切换窗格右侧多一个空格」。 */
        if (s->ch == '\t')                 snprintf(keyname, sizeof(keyname), "%s", "Tab");
        else if (s->ch == ' ')             snprintf(keyname, sizeof(keyname), "%s", "Space");
        else if (s->ch == '\r' || s->ch == '\n') snprintf(keyname, sizeof(keyname), "%s", "Enter");
        else if (s->ch < 128)              snprintf(keyname, sizeof(keyname), "%c", (char)s->ch);
        else                               snprintf(keyname, sizeof(keyname), "%s", "键");
    } else if (s->vk >= VK_F1 && s->vk <= VK_F24) {
        snprintf(keyname, sizeof(keyname), "F%d", s->vk - VK_F1 + 1);
    } else if ((s->vk >= 'A' && s->vk <= 'Z') || (s->vk >= '0' && s->vk <= '9')) {
        char c = (char)s->vk;
        /* Ctrl/Alt 组合按惯例大写显示（Ctrl+B），单键则小写（c） */
        if (c >= 'A' && c <= 'Z' && !s->shift && !s->ctrl && !s->alt) c = (char)(c - 'A' + 'a');
        snprintf(keyname, sizeof(keyname), "%c", c);
    } else {
        const char *nm = "?";
        for (int i = 0; i < g_named_key_count; i++)
            if (g_named_keys[i].vk == s->vk) { nm = g_named_keys[i].name; break; }
        /* 可打印 OEM 键（- = ; / [ 等）按 Shift 状态显示实际打出的符号，
         * 否则 Shift+- 这类绑定会落到 "?"（命令面板里 Ctrl+B Shift+? 误导）。 */
        if (nm[0] == '?' && nm[1] == 0) {
            static const struct { WORD vk; char plain; char shifted; } oem[] = {
                {VK_OEM_MINUS, '-', '_'}, {VK_OEM_PLUS,  '=', '+'},
                {VK_OEM_1,     ';', ':'}, {VK_OEM_2,     '/', '?'},
                {VK_OEM_3,     '`', '~'}, {VK_OEM_4,     '[', '{'},
                {VK_OEM_5,     '\\','|'}, {VK_OEM_6,     ']', '}'},
                {VK_OEM_7,     '\'','"'}, {VK_OEM_COMMA, ',', '<'},
                {VK_OEM_PERIOD,'.', '>'},
            };
            for (int i = 0; i < (int)(sizeof(oem)/sizeof(oem[0])); i++) {
                if (oem[i].vk == s->vk) {
                    keyname[0] = s->shift ? oem[i].shifted : oem[i].plain;
                    keyname[1] = 0;
                    oem_sym = 1;
                    nm = NULL;
                    break;
                }
            }
        }
        if (nm) snprintf(keyname, sizeof(keyname), "%s", nm);
    }
    /* 可打印 OEM 符号（_ : ? + 等）本身已隐含 Shift 状态，不再重复加 "Shift+"：
     * Shift+- 显示为 "_"（Ctrl+B _），而不是累赘的 "Shift+_"。命名键/字母
     * （Tab/方向/T）仍显式标注 Shift+（如 Shift+Tab、Shift+T）。 */
    snprintf(out, out_size, "%s%s%s%s",
             s->ctrl ? "Ctrl+" : "", s->alt ? "Alt+" : "",
             (s->shift && !s->shift_any && !oem_sym) ? "Shift+" : "", keyname);
}

void keymap_describe(int action, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    out[0] = 0;
    /* send-prefix 默认没有独立键位（连按两次前缀即可），但用户可以在
     * [keys] / 设置页里给它绑一个键，这时必须显示真实键位而不是前缀两连。 */
    const KeySpec *found = NULL;
    for (int i = 0; i < g_user_count && !found; i++)
        if (g_user[i].bind.action == action) found = &g_user[i].bind.key;
    for (int i = 0; i < g_default_count && !found; i++)
        if (g_default_bindings[i].action == action && !g_action_overridden[action])
            found = &g_default_bindings[i].key;

    /* spec_text 最长产出 "Ctrl+Alt+Shift+backspace" = 24 字节 + NUL；原来的 [24]
     * 差 1 字节，帮助页显示成 "…backspac"（审计 BUG-12，unittest 有验红用例）。 */
    char prefix[40], key[40];
    spec_text(&g_prefix, prefix, sizeof(prefix));
    if (!found) {
        if (action != ACT_SEND_PREFIX) return;
        snprintf(key, sizeof(key), "%s", prefix);   /* 连按两次前缀 */
    } else {
        spec_text(found, key, sizeof(key));
    }
    /* 标记为直接键的动作不显示前缀段，否则会教用户按一个没用的键。 */
    if (!keymap_action_uses_prefix(action)) snprintf(out, out_size, "%s", key);
    else snprintf(out, out_size, "%s %s", prefix, key);
}

int keymap_action_count(void) { return g_action_count; }

int keymap_action_at(int idx) {
    if (idx < 0 || idx >= g_action_count) return ACT_NONE;
    return g_actions[idx].action;
}

int keymap_action_is_overridden(int action) {
    if (action <= ACT_NONE || action >= ACT_COUNT) return 0;
    return g_action_overridden[action] ? 1 : 0;
}

int keymap_unbind(const char *action_name) {
    int action = keymap_action_id(action_name);
    if (action == ACT_NONE) return 0;
    int w = 0;
    for (int i = 0; i < g_user_count; i++) {
        if (g_user[i].bind.action == action) continue;
        if (w != i) g_user[w] = g_user[i];
        w++;
    }
    int removed = g_user_count - w;
    g_user_count = w;
    g_action_overridden[action] = 0;
    return removed > 0;
}

int keymap_key_text_from_event(WORD vk, DWORD ctrl, WCHAR uc, char *out, int out_size) {
    if (!out || out_size < 8) return 0;
    /* 纯修饰键与锁定键不能作为绑定 */
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
        vk == 0xA0 || vk == 0xA1 || vk == 0xA2 || vk == 0xA3 || vk == 0xA4 || vk == 0xA5 ||
        vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) return 0;

    int is_ctrl = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    int is_alt = (ctrl & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    int is_shift = (ctrl & SHIFT_PRESSED) != 0;

    char mods[8] = {0};
    int m = 0;
    if (is_ctrl) { mods[m++] = 'C'; mods[m++] = '-'; }
    if (is_alt)  { mods[m++] = 'M'; mods[m++] = '-'; }

    char key[24] = {0};
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(key, sizeof(key), "F%d", vk - VK_F1 + 1);
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(key, sizeof(key), "%c", (char)('0' + (vk - VK_NUMPAD0)));
    } else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        char c = (char)vk;
        if (c >= 'A' && c <= 'Z') {
            if (is_shift && !is_ctrl && !is_alt) { mods[m++] = 'S'; mods[m++] = '-'; }
            c = (char)(c - 'A' + 'a');
        }
        snprintf(key, sizeof(key), "%c", c);
    } else {
        for (int i = 0; i < g_named_key_count; i++) {
            if (g_named_keys[i].vk == vk) { snprintf(key, sizeof(key), "%s", g_named_keys[i].name); break; }
        }
        if (!key[0] && uc >= 0x20 && uc < 0x7F) snprintf(key, sizeof(key), "%c", (char)uc);
    }
    if (!key[0]) return 0;
    mods[m] = 0;
    snprintf(out, out_size, "%s%s", mods, key);

    /* 回读一次，确保生成的文本一定能被解析回来 */
    KeySpec probe;
    return keymap_parse_key(out, &probe);
}

int keymap_has_user_bindings(void) { return g_user_count > 0; }
int keymap_user_binding_count(void) { return g_user_count; }

const char *keymap_user_binding_action(int idx) {
    if (idx < 0 || idx >= g_user_count) return "";
    return keymap_action_name(g_user[idx].bind.action);
}

const char *keymap_user_binding_key(int idx) {
    if (idx < 0 || idx >= g_user_count) return "";
    return g_user[idx].key_text;
}

int keymap_user_binding_no_prefix(int idx) {
    if (idx < 0 || idx >= g_user_count) return 0;
    return g_user[idx].no_prefix ? 1 : 0;
}
