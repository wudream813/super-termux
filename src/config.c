#include "config.h"
/* platform.h 两侧都要：它提供 TERMUX_DEFAULT_SHELL_* / TERMUX_PATH_SEP* /
 * plat_user_home() / plat_default_shell()，这些宏和声明在 Windows 上同样需要
 * （展开出来就是原来的 cmd.exe 与反斜杠，行为不变）。 */
#include "platform.h"
#ifndef _WIN32
#include <string.h>        /* strrchr */
#endif

ChooserItem g_chooser_items[MAX_CHOOSER_ITEMS];
int g_chooser_item_count = 0;

int g_settings_nav = 0;
int g_settings_field = 0;
int g_settings_table_sel = 0;
int g_default_startup = 0;
int g_scrollback_lines = SCROLL_BUF_LINES;
int g_mouse_enabled = 1;
int g_copy_move_deselect = 1;
int g_confirm_on_exit = 0;
int g_confirm_on_close = 0;
int g_search_case_sensitive = 0;
/* v2.3.0：`session = on` ⇒ 退出时把会话写进 termux.session、下次启动灌回来。
 * v2.3.2 起默认开：默认关 = 用户没手动开过就永远看不到效果，
 * 而「看不到效果」和「功能坏了」在现场是一模一样的。 */
int g_session_persist = 1;
/* 图形直通这两个全局的定义在 src/vt.c —— 状态机在那儿，而 tests/ 里好几个 harness
 * 只链 vt.c 不链 config.c（cascade_probe、以及它派生的那几个回归构建）。定义放这儿
 * 它们就链得上；放 config.c 则每个 harness 都要自己补一份，早晚漏一个。 */
/* v2.3.6：Windows 侧 ConPTY 要不要带 PSEUDOCONSOLE_PASSTHROUGH_MODE(0x8)。
 * 这里只存文本（auto | on | off），解释在 conpty_loader.c 的纯函数里：config.c 两侧
 * 都编译，而 loader 只有 Windows 有 —— 让 config 直接调它会链不动。main.c 读完 ini
 * 之后把这段文本交给 conpty_set_passthrough()。留空 = auto。 */
static char g_passthrough_val[12] = {0};
const char *conpty_passthrough_text(void) {
    return g_passthrough_val[0] ? g_passthrough_val : "auto";
}
/* v2.1.7/8：终端只能整格重绘、没有半透明，所以「淡入」只能靠几帧之间把写给终端的颜色
 * 整体向页面底色混合来模拟 —— 时长也就只能是帧的倍数（动画期间约 8~15ms 一帧，实测
 * 110ms 出 7 档）。默认 110ms：够看出方向，又短到不会让人觉得要点一下等一下。
 * v2.1.8 把这一段同时用于三件事：设置页切页、浮层与悬停气泡/toast 的淡入、以及切换标签
 * 页时整页的左右滑入（滑 = 动画帧里逐行把 CUP 的起始列平移再裁掉溢出）。
 * ini 里 `anim = off | short | normal`，也可以直接写毫秒数（上限 600）；`off` 时这三件事
 * 一帧都不做，输出与没有这个功能时逐字节相同。 */
int g_anim_ms = 110;
int g_settings_show_presets = 0;
int g_preset_sel = 0;

int g_settings_theme_sel = 0;
int g_settings_pane_sel = -1;        /* -1 = 方案行（最上面，最快上手）；槽位见 THEME_PANE_* */
int g_settings_pane_scheme = 0;
int g_settings_show_pane_schemes = 0;
int g_settings_pane_scroll = 0;
int g_settings_appear_scroll = 0;
int g_settings_manage_scroll = 0;        /* v2.1.4：条目管理页纵向滚动 */
int g_settings_hscroll[6] = {0};
int g_settings_bar_drag = 0;        /* v2.1.5：正在拖哪根滚动条（0 = 没拖） */       /* v2.1.4：右栏横向滚动量（每页一格，不写进 ini） */
int g_settings_sidebar_scroll = 0;   /* v2.1.2：侧栏菜单项列表滚动量（不写进 ini） */
int g_settings_behavior_scroll = 0;
int g_settings_detail_scroll = 0;
int g_settings_startup_scroll = 0;
int g_settings_keys_sel = 0;
int g_settings_keys_scroll = 0;
int g_settings_behavior_sel = 0;
int g_key_capture_active = 0;
char g_hex_edit_buf[8] = {0};
int g_hex_edit_len = 0, g_hex_edit_active = 0, g_hex_edit_role = -1;

/* 侧栏顺序：启动 → 各菜单项 → 条目管理 → 外观 → 键位 → 行为 → 窗格配色。
 * v2.1.4：多一页「条目管理」（[M]），新建/预设库/每行 ↑↓改删 都收进去。
 * Tab 循环走的就是这两个函数（input.c 里 ↑/↓ 换页唯一入口），漏一格就跳不到那页 ——
 * 原先 case 只到 3、default 回落 BEHAVIOR，窗格配色页只能靠热键 [W] 进。 */
int settings_nav_order_count(void) { return g_chooser_item_count + 5; }

int settings_nav_at(int idx) {
    if (idx <= 0) return SETTINGS_NAV_STARTUP;
    if (idx <= g_chooser_item_count) return idx;              /* 菜单项详情 */
    switch (idx - g_chooser_item_count) {
        case 1: return SETTINGS_NAV_ITEMS;
        case 2: return SETTINGS_NAV_APPEARANCE;
        case 3: return SETTINGS_NAV_KEYS;
        case 4: return SETTINGS_NAV_BEHAVIOR;
        default: return SETTINGS_NAV_PANE;
    }
}

int settings_nav_index_of(int nav) {
    if (nav == SETTINGS_NAV_ITEMS) return g_chooser_item_count + 1;
    if (nav == SETTINGS_NAV_APPEARANCE) return g_chooser_item_count + 2;
    if (nav == SETTINGS_NAV_KEYS) return g_chooser_item_count + 3;
    if (nav == SETTINGS_NAV_BEHAVIOR) return g_chooser_item_count + 4;
    if (nav == SETTINGS_NAV_PANE) return g_chooser_item_count + 5;
    if (nav >= 1 && nav <= g_chooser_item_count) return nav;
    return 0;
}

char g_edit_name[sizeof(g_chooser_items[0].name)] = {0};
int g_edit_name_len = 0, g_edit_name_pos = 0;
char g_edit_cmd[256] = {0};
int g_edit_cmd_len = 0, g_edit_cmd_pos = 0;
char g_edit_dir[256] = {0};
int g_edit_color = 0;
int g_edit_dir_len = 0, g_edit_dir_pos = 0;

#ifdef _WIN32
const ChooserItem g_presets[] = {
    {"cmd", "cmd.exe", "", 0},
    {"PowerShell", "powershell.exe", "", 0},
    {"Pwsh", "pwsh.exe", "", 0},
    {"WSL", "wsl.exe", "", 0},
    {"Git Bash", "bash.exe", "", 0},
    {"Python", "python -i", "", 0},
    {"Node.js", "node", "", 0},
    {"自定义命令行", ":custom", "", 0},
};
#else
/* POSIX 上没有 cmd.exe / powershell.exe；预设换成登录 shell 与常见解释器。
 * 「自定义命令行」必须留着，它是设置页里唯一能自填命令的入口。 */
const ChooserItem g_presets[] = {
    {"Bash", "bash -l", "", 0},
    {"Sh", "/bin/sh", "", 0},
    {"Zsh", "zsh -l", "", 0},
    {"Fish", "fish", "", 0},
    {"Python", "python3 -i", "", 0},
    {"Node.js", "node", "", 0},
    {"自定义命令行", ":custom", "", 0},
};
#endif
const int g_preset_count = (int)(sizeof(g_presets) / sizeof(g_presets[0]));

void init_default_config(void) {
    g_default_startup = 0;
    g_scrollback_lines = SCROLL_BUF_LINES;
    g_mouse_enabled = 1;
    g_copy_move_deselect = 1;
    g_confirm_on_exit = 0;
    g_confirm_on_close = 0;
    g_search_case_sensitive = 0;
    g_session_persist = 1;              /* v2.3.2：默认开（ini 里写 session = off 可关） */
    g_graphics_relay = 1;               /* v2.3.6：图形协议直通，默认开 */
    g_graphics_max_kb = 32768;
    g_passthrough_val[0] = 0;
    theme_init();
    keymap_init();
#ifdef _WIN32
    g_chooser_item_count = 3;
    snprintf(g_chooser_items[0].name, sizeof(g_chooser_items[0].name), "cmd");
    snprintf(g_chooser_items[0].cmd, sizeof(g_chooser_items[0].cmd), "cmd.exe");
    g_chooser_items[0].workdir[0] = 0;
    g_chooser_items[0].color = 0;

    snprintf(g_chooser_items[1].name, sizeof(g_chooser_items[1].name), "PowerShell");
    snprintf(g_chooser_items[1].cmd, sizeof(g_chooser_items[1].cmd), "powershell.exe");
    g_chooser_items[1].workdir[0] = 0;
    g_chooser_items[1].color = 0;
#else
    /* 默认只放一项：$SHELL。名字取 basename，标签栏才会显示 "bash" 而不是
     * 一长串全路径；cmd 存全路径，免得 PATH 里找不到。 */
    g_chooser_item_count = 2;
    {
        char sh_u8[256] = {0};
        const char *base;
        WideCharToMultiByte(CP_UTF8, 0, plat_default_shell(), -1,
                            sh_u8, (int)sizeof(sh_u8) - 1, NULL, NULL);
        base = strrchr(sh_u8, '/');
        base = (base && base[1]) ? base + 1 : sh_u8;
        /* name 只有 32 字节而 basename 可达 255：显式按容量截，-O1 的
         * -Wformat-truncation 才认（`make lint-o1` 门禁要求零警告）。 */
        snprintf(g_chooser_items[0].name, sizeof(g_chooser_items[0].name), "%.*s",
                 (int)sizeof(g_chooser_items[0].name) - 1, base);
        snprintf(g_chooser_items[0].cmd, sizeof(g_chooser_items[0].cmd), "%s", sh_u8);
        g_chooser_items[0].workdir[0] = 0;
        g_chooser_items[0].color = 0;
    }
#endif

    /* 「自定义命令行」永远是最后一项，Windows 上是下标 2，POSIX 上是下标 1。 */
    snprintf(g_chooser_items[g_chooser_item_count - 1].name,
             sizeof(g_chooser_items[g_chooser_item_count - 1].name), "自定义命令行");
    snprintf(g_chooser_items[g_chooser_item_count - 1].cmd,
             sizeof(g_chooser_items[g_chooser_item_count - 1].cmd), ":custom");
    g_chooser_items[g_chooser_item_count - 1].workdir[0] = 0;
    g_chooser_items[g_chooser_item_count - 1].color = 0;
}

enum { SEC_COMPAT = 0, SEC_GENERAL, SEC_MENU, SEC_THEME, SEC_KEYS, SEC_IGNORE };

int config_parse_bool(const char *val, int fallback) {
    if (!val) return fallback;
    while (*val == ' ' || *val == '\t') val++;
    if (_strnicmp(val, "true", 4) == 0 || _strnicmp(val, "yes", 3) == 0 ||
        _strnicmp(val, "on", 2) == 0 || *val == '1') return 1;
    if (_strnicmp(val, "false", 5) == 0 || _strnicmp(val, "no", 2) == 0 ||
        _strnicmp(val, "off", 3) == 0 || *val == '0') return 0;
    return fallback;
}

/* 写回 ini 时优先用三个语义值；被手动改成别的毫秒数就原样写数字。 */
static char g_anim_buf[24];
static const char *anim_ini_text(void) {
    if (g_anim_ms == 0) return "off";
    if (g_anim_ms == 60) return "short";
    if (g_anim_ms == 110) return "normal";
    snprintf(g_anim_buf, sizeof(g_anim_buf), "%d", g_anim_ms);
    return g_anim_buf;
}

/* [general] 段的键；返回 1 表示这一行已被消费。 */
static int apply_general_key(const char *key, const char *val) {
    if (_stricmp(key, "default_startup") == 0) { g_default_startup = atoi(val); return 1; }
    if (_stricmp(key, "theme") == 0)           { theme_set_by_name(val); return 1; }
    if (_stricmp(key, "prefix") == 0)          { keymap_set_prefix(val); return 1; }
    if (_stricmp(key, "scrollback") == 0) {
        int n = atoi(val);
        if (n < 200) n = 200;
        if (n > 500000) n = 500000;
        g_scrollback_lines = n;
        return 1;
    }
    if (_stricmp(key, "mouse") == 0)           { g_mouse_enabled = config_parse_bool(val, 1); return 1; }
    if (_stricmp(key, "copy_move_deselect") == 0) { g_copy_move_deselect = config_parse_bool(val, 1); return 1; }
    if (_stricmp(key, "confirm_on_exit") == 0) { g_confirm_on_exit = config_parse_bool(val, 0); return 1; }
    if (_stricmp(key, "confirm_on_close") == 0) { g_confirm_on_close = config_parse_bool(val, 0); return 1; }
    if (_stricmp(key, "search_case_sensitive") == 0) { g_search_case_sensitive = config_parse_bool(val, 0); return 1; }
    if (_stricmp(key, "graphics") == 0) { g_graphics_relay = config_parse_bool(val, 1); return 1; }
    if (_stricmp(key, "conpty_passthrough") == 0) {
        /* 先夹长度再拷：snprintf("%s") 写进 12 字节的缓冲会被 -Wformat-truncation 骂
         * （lint-o1 那一道门禁就是为这类「编得过、跑起来截断」而设的）。 */
        size_t n = strlen(val);
        if (n >= sizeof(g_passthrough_val)) n = sizeof(g_passthrough_val) - 1;
        memcpy(g_passthrough_val, val, n);
        g_passthrough_val[n] = 0;
        return 1;
    }
    if (_stricmp(key, "graphics_max") == 0) {
        /* KB 为单位；下限 4KB（再小就没意义了），上限 262144KB=256MB（别让手滑把内存吃光）。 */
        int n = atoi(val);
        if (n < 4) n = 4;
        if (n > 262144) n = 262144;
        g_graphics_max_kb = n;
        return 1;
    }
    if (_stricmp(key, "session") == 0)  { g_session_persist = config_parse_bool(val, 1); return 1; }   /* 值写坏了当开：这行存在就说明想开 */
    if (_stricmp(key, "anim") == 0) {
        /* off/none 与 0 都是关；写个认不出来的单词时不要把它当成 0 关掉动画，
         * 按默认走 —— 用户手打 ini 打错字是常事，静默关掉功能最难查。 */
        if (!_stricmp(val, "off") || !_stricmp(val, "none")) g_anim_ms = 0;
        else if (!_stricmp(val, "short")) g_anim_ms = 60;
        else if (!_stricmp(val, "normal")) g_anim_ms = 110;
        else if (val[0] >= '0' && val[0] <= '9') {
            int n = atoi(val);
            if (n > 600) n = 600;            /* 再长就不是「过渡」是「等待」了 */
            g_anim_ms = n;
        } else g_anim_ms = 110;
        return 1;
    }
    return 0;
}

/* v2.3.0：ini 与会话快照共用这一支定位（原先是 resolve_ini_path 内部写死的一串）。
 * 「exe 旁边那份优先，否则用用户主目录下的点文件」—— 两处各写一遍迟早会漂。 */
void config_sibling_path(WCHAR *out, int out_len, int for_write,
                         const WCHAR *next_to_exe, const WCHAR *in_home) {
    WCHAR exe_path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    WCHAR *last_bs = wcsrchr(exe_path, TERMUX_PATH_SEP);
    if (last_bs) {
        *last_bs = 0;
        _snwprintf(out, out_len - 1, L"%s" TERMUX_PATH_SEP_S L"%s", exe_path, next_to_exe);
    } else {
        wcsncpy(out, next_to_exe, out_len - 1);
    }
    if (for_write) return;
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return;
    const WCHAR *prof = plat_user_home();
    if (prof) {
        WCHAR alt[MAX_PATH] = {0};
        _snwprintf(alt, MAX_PATH - 1, L"%s" TERMUX_PATH_SEP_S L"%s", prof, in_home);
        if (GetFileAttributesW(alt) != INVALID_FILE_ATTRIBUTES)
            wcsncpy(out, alt, out_len - 1);
    }
}

static void resolve_ini_path(WCHAR *out, int out_len, int for_write) {
    config_sibling_path(out, out_len, for_write, L"termux.ini", L".termux.ini");
}

static void trim_tail(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && ((unsigned char)s[n - 1] <= ' ')) s[--n] = 0;
}

void load_config(void) {
    init_default_config();

    WCHAR ini_path[MAX_PATH] = {0};
    resolve_ini_path(ini_path, MAX_PATH, 0);

    FILE *f = _wfopen(ini_path, L"rb");
    if (!f) {
        save_config();
        theme_apply();
        return;
    }

    char line[512];
    int parsed_count = 0;
    int section = SEC_COMPAT;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';' || *p == '\r' || *p == '\n') continue;

        if (*p == '[') {
            char name[32] = {0};
            char *close = strchr(p, ']');
            if (close) {
                int len = (int)(close - p - 1);
                if (len > (int)sizeof(name) - 1) len = (int)sizeof(name) - 1;
                if (len > 0) memcpy(name, p + 1, len);
            }
            if (_stricmp(name, "general") == 0 || _stricmp(name, "settings") == 0) section = SEC_GENERAL;
            else if (_stricmp(name, "menu") == 0) section = SEC_MENU;
            else if (_stricmp(name, "theme") == 0) section = SEC_THEME;
            else if (_stricmp(name, "keys") == 0) section = SEC_KEYS;
            else section = SEC_IGNORE;
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p;
        while (*key == ' ' || *key == '\t') key++;
        trim_tail(key);

        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        trim_tail(val);

        if (section == SEC_IGNORE) continue;
        if (section == SEC_THEME) { if (!theme_set_pane_hex(key, val)) theme_set_role_hex(key, val); continue; }
        if (section == SEC_KEYS) {
            if (_stricmp(key, "prefix") == 0) keymap_set_prefix(val);
            else keymap_bind(key, val);
            continue;
        }
        if (section == SEC_GENERAL) { apply_general_key(key, val); continue; }
        /* SEC_MENU 与无段落的老配置：先认 general 键，再按菜单项解析 */
        if (section == SEC_COMPAT && apply_general_key(key, val)) continue;
        if (section == SEC_MENU && _stricmp(key, "default_startup") == 0) { g_default_startup = atoi(val); continue; }

        char *comma1 = strchr(val, ',');
        if (!comma1) continue;
        *comma1 = 0;
        char *name = val;
        char *cmd = comma1 + 1;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        char default_workdir[1] = {0};
        char *workdir = default_workdir;
        char *comma2 = strchr(cmd, ',');
        if (comma2) {
            *comma2 = 0;
            workdir = comma2 + 1;
            while (*workdir == ' ' || *workdir == '\t') workdir++;
        }
        /* v1.8.9: 目录之后还能再跟一个颜色字段，两种写法都认：
         *   1 = 名称, cmd.exe, D:\\work, color=3
         *   1 = 名称, cmd.exe, , 3
         * 没写就是 0（跟随默认蓝色）。 */
        int item_color = 0;
        char *comma3 = comma2 ? strchr(workdir, ',') : NULL;
        if (comma3) {
            *comma3 = 0;
            char *ctext = comma3 + 1;
            while (*ctext == ' ' || *ctext == '\t') ctext++;
            trim_tail(ctext);
            if (_strnicmp(ctext, "color", 5) == 0) {
                ctext += 5;
                while (*ctext == ' ' || *ctext == '=' || *ctext == '\t') ctext++;
            }
            item_color = atoi(ctext);
        }

        trim_tail(name);
        trim_tail(cmd);
        trim_tail(workdir);
        if (_strnicmp(workdir, "color", 5) == 0) {   /* 省略了目录，直接写 color=N */
            const char *ctext = workdir + 5;
            while (*ctext == ' ' || *ctext == '=' || *ctext == '\t') ctext++;
            item_color = atoi(ctext);
            workdir[0] = 0;
        }
        if (item_color < 0 || item_color > 8) item_color = 0;

        if (name[0] && cmd[0] && parsed_count < MAX_CHOOSER_ITEMS) {
            snprintf(g_chooser_items[parsed_count].name, sizeof(g_chooser_items[0].name), "%s", name);
            snprintf(g_chooser_items[parsed_count].cmd, sizeof(g_chooser_items[0].cmd), "%s", cmd);
            snprintf(g_chooser_items[parsed_count].workdir, sizeof(g_chooser_items[0].workdir), "%s", workdir);
            g_chooser_items[parsed_count].color = item_color;
            parsed_count++;
        }
    }
    fclose(f);

    if (parsed_count > 0) g_chooser_item_count = parsed_count;

    /* v2.3.2 的开发/CI 把手（与 main.c 那个 TERMUX_DUMP 同族）：TERMUX_NO_SESSION=1 ⇒
     * 本进程一律不读不写会话快照。为什么判据需要它：`session` 从这一版起默认开，而
     * 跑判据的脚本多半直接拿仓库里那支 exe —— exe 旁边只要留下一份 termux.session，
     * 下一轮启动就会把它灌回屏上，渲染类判据（如 lastcol 的「满宽行原样发到宿主」）
     * 会因为多出来的两行而分块位置改变、红得莫名其妙。产品路径不受影响。 */
    {
        const char *ns = getenv("TERMUX_NO_SESSION");
        if (ns && *ns && *ns != '0') g_session_persist = 0;
    }
    theme_apply();
}

void save_config(void) {
    WCHAR ini_path[MAX_PATH] = {0};
    resolve_ini_path(ini_path, MAX_PATH, 1);

    FILE *f = _wfopen(ini_path, L"wb");
    if (!f) {
        const WCHAR *prof = plat_user_home();
        if (prof) {
            WCHAR user_ini[MAX_PATH] = {0};
            _snwprintf(user_ini, MAX_PATH - 1, L"%s" TERMUX_PATH_SEP_S L".termux.ini", prof);
            f = _wfopen(user_ini, L"wb");
        }
    }
    if (!f) return;

    char buf[1024];
    int len;

    const char *header =
        "# super-termux 配置文件 (UTF-8)\r\n"
        "# [general] 全局行为 / [theme] 配色 / [keys] 键位 / [menu] 新建菜单\r\n"
        "\r\n"
        "[general]\r\n"
        "# theme: github-dark | one-dark | nord | gruvbox-dark | dracula\r\n"
        "# prefix: 前缀键，C- = Ctrl，M- = Alt，S- = Shift，例如 C-a\r\n"
        "# anim: 设置页过渡动画 off | short | normal（也可写毫秒数，上限 600）\r\n"
        "# session: on ⇒ 退出时把会话（各窗格已滚出去的历史 + 标签与分屏布局，含颜色）写进（默认 on，off 可关）\r\n"
        "#          termux.session，下次启动灌回来（关窗口也存；进程本身不保留，见 README）\r\n"
        "# graphics: on | off —— 图形协议直通（sixel / kitty / iTerm2 图片原样交给宿主终端）\r\n"
        "# graphics_max: 单条序列的大小上限（KB，4..262144，默认 32768）；超过就整段丢弃\r\n"
        "# conpty_passthrough: auto | on | off —— 仅 Windows。auto=只在 Win11 22H2+ 给 ConPTY\r\n"
        "#          加 PSEUDOCONSOLE_PASSTHROUGH_MODE（不加它 conhost 会自己吃掉 sixel/kitty）\r\n";
    fwrite(header, 1, strlen(header), f);

    len = snprintf(buf, sizeof(buf),
        "theme = %s\r\n"
        "prefix = %s\r\n"
        "scrollback = %d\r\n"
        "mouse = %s\r\n"
        "copy_move_deselect = %s\r\n"
        "confirm_on_exit = %s\r\n"
        "confirm_on_close = %s\r\n"
        "search_case_sensitive = %s\r\n"
        "session = %s\r\n"
        "graphics = %s\r\n"
        "graphics_max = %d\r\n"
        "conpty_passthrough = %s\r\n"
        "anim = %s\r\n"
        "default_startup = %d\r\n\r\n",
        theme_name(), keymap_prefix_text(), g_scrollback_lines,
        g_mouse_enabled ? "true" : "false",
        g_copy_move_deselect ? "true" : "false",
        g_confirm_on_exit ? "true" : "false",
        g_confirm_on_close ? "true" : "false",
        g_search_case_sensitive ? "true" : "false",
        g_session_persist ? "true" : "false",
        g_graphics_relay ? "on" : "off",
        g_graphics_max_kb,
        conpty_passthrough_text(),
        anim_ini_text(),
        g_default_startup);
    if (len > 0) fwrite(buf, 1, len, f);

    const char *theme_hdr =
        "[theme]\r\n"
        "# 覆盖单个语义色，取消注释即可生效（16 个角色见 README）\r\n";
    fwrite(theme_hdr, 1, strlen(theme_hdr), f);
    if (theme_has_overrides()) {
        for (int i = 0; i < TH_ROLE_COUNT; i++) {
            char hex[16];
            theme_get_override(i, hex, sizeof(hex));
            if (!hex[0]) continue;
            len = snprintf(buf, sizeof(buf), "%s = %s\r\n", theme_role_name(i), hex);
            if (len > 0) fwrite(buf, 1, len, f);
        }
    } else {
        const char *sample = "# accent = #58a6ff\r\n# background = #0d1117\r\n";
        fwrite(sample, 1, strlen(sample), f);
    }
    /* 窗格 palette：上面的 background 只管 termux 自己的 UI；cmd 里的文字是
     * 16 色索引，要改它的底色 / 字色 / 16 色得用这组（像 Windows Terminal 的
     * color scheme）。一项都不设 = 原样透传给宿主终端。 */
    const char *pane_hdr =
        "# 窗格 16 色 palette：改 cmd / shell 里文字的默认前后景与 16 个索引色\r\n"
        "# （pane_foreground / pane_background / pane_black … pane_bright_white）\r\n"
        "# 例：浅色窗格  pane_background = #ffffff  pane_foreground = #24292f\r\n"
        "# 滚动条：pane_scrollbar = 滑块色，pane_scrollbar_track = 轨道底色（浅色窗格建议设，否则和背景同色）\r\n";
    fwrite(pane_hdr, 1, strlen(pane_hdr), f);
    for (int i = 0; i < THEME_PANE_SLOTS; i++) {
        char hex[16];
        theme_pane_get(i, hex, sizeof(hex));
        if (!hex[0]) continue;
        len = snprintf(buf, sizeof(buf), "%s = %s\r\n", theme_pane_slot_name(i), hex);
        if (len > 0) fwrite(buf, 1, len, f);
    }
    fwrite("\r\n", 1, 2, f);

    const char *keys_hdr =
        "[keys]\r\n"
        "# 动作名 = 前缀之后要按的键，例如: new-pane = c\r\n"
        "# 键后面加 noprefix 表示不用按前缀，直接触发，例如: next-pane = M-n noprefix\r\n";
    fwrite(keys_hdr, 1, strlen(keys_hdr), f);
    if (keymap_has_user_bindings()) {
        for (int i = 0; i < keymap_user_binding_count(); i++) {
            len = snprintf(buf, sizeof(buf), "%s = %s%s\r\n",
                           keymap_user_binding_action(i), keymap_user_binding_key(i),
                           keymap_user_binding_no_prefix(i) ? " noprefix" : "");
            if (len > 0) fwrite(buf, 1, len, f);
        }
    } else {
        const char *sample =
            "# command-palette = :\r\n"
            "# new-pane = c\r\n"
            "# close-pane = x\r\n"
            "# next-theme = T\r\n";
        fwrite(sample, 1, strlen(sample), f);
    }
    fwrite("\r\n", 1, 2, f);

    const char *menu_hdr =
        "[menu]\r\n"
        "# 序号 = 菜单显示名称, 启动命令行, 启动目录(可选), color=颜色(可选 1-8)\r\n"
        "# 特殊命令 \":custom\" 表示打开自定义命令行输入框\r\n"
        "# color 省略或 0 表示跟随默认蓝色\r\n";
    fwrite(menu_hdr, 1, strlen(menu_hdr), f);
    for (int i = 0; i < g_chooser_item_count; i++) {
        int color = g_chooser_items[i].color;
        if (color < 0 || color > 8) color = 0;
        char color_suffix[24] = {0};
        if (color > 0) snprintf(color_suffix, sizeof(color_suffix), ", color=%d", color);
        if (g_chooser_items[i].workdir[0]) {
            len = snprintf(buf, sizeof(buf), "%d = %s, %s, %s%s\r\n", i + 1,
                           g_chooser_items[i].name, g_chooser_items[i].cmd,
                           g_chooser_items[i].workdir, color_suffix);
        } else if (color > 0) {
            len = snprintf(buf, sizeof(buf), "%d = %s, %s, %s\r\n", i + 1,
                           g_chooser_items[i].name, g_chooser_items[i].cmd, color_suffix + 2);
        } else {
            len = snprintf(buf, sizeof(buf), "%d = %s, %s\r\n", i + 1,
                           g_chooser_items[i].name, g_chooser_items[i].cmd);
        }
        if (len > 0) fwrite(buf, 1, len, f);
    }
    fclose(f);
}

void open_config_file(void) {
    WCHAR ini_path[MAX_PATH] = {0};
    /* load_config() 会在可写时于 exe 同目录生成配置；只读安装目录时回退到
     * USERPROFILE，这里沿用同一套查找顺序。 */
    resolve_ini_path(ini_path, MAX_PATH, 0);
    ShellExecuteW(NULL, L"open", ini_path, NULL, NULL, SW_SHOWNORMAL);
}

void load_item_to_editor(int idx) {
    if (idx < 0 || idx >= g_chooser_item_count) return;
    snprintf(g_edit_name, sizeof(g_edit_name), "%s", g_chooser_items[idx].name);
    g_edit_name_len = (int)strlen(g_edit_name);
    g_edit_name_pos = g_edit_name_len;

    snprintf(g_edit_cmd, sizeof(g_edit_cmd), "%s", g_chooser_items[idx].cmd);
    g_edit_cmd_len = (int)strlen(g_edit_cmd);
    g_edit_cmd_pos = g_edit_cmd_len;

    snprintf(g_edit_dir, sizeof(g_edit_dir), "%s", g_chooser_items[idx].workdir);
    g_edit_dir_len = (int)strlen(g_edit_dir);
    g_edit_dir_pos = g_edit_dir_len;

    g_edit_color = g_chooser_items[idx].color;
    if (g_edit_color < 0 || g_edit_color > 8) g_edit_color = 0;

    g_settings_field = 0;
}

void save_editor_to_item(int idx) {
    if (idx < 0 || idx >= g_chooser_item_count) return;
    if (g_edit_name_len > 0) {
        snprintf(g_chooser_items[idx].name, sizeof(g_chooser_items[0].name), "%s", g_edit_name);
    }
    if (g_edit_cmd_len > 0) {
        snprintf(g_chooser_items[idx].cmd, sizeof(g_chooser_items[0].cmd), "%s", g_edit_cmd);
    }
    snprintf(g_chooser_items[idx].workdir, sizeof(g_chooser_items[0].workdir), "%s", g_edit_dir);
    g_chooser_items[idx].color = (g_edit_color >= 0 && g_edit_color <= 8) ? g_edit_color : 0;
    save_config();
}
