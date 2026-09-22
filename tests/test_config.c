/* super-termux 配置体系单元测试（主题引擎 + 键位表）
 *
 * 这些模块不依赖任何 Win32 调用，因此可以用 tests/stub 里的最小 windows.h
 * 替身在 Linux 上原生编译、直接执行断言：
 *
 *     gcc -Wall -Wextra -Itests/stub -Iinclude src/theme.c src/keymap.c \
 *         tests/test_config.c -o /tmp/test_config && /tmp/test_config
 */
#include "theme.h"
#include "keymap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_checks = 0;
static int g_failed = 0;

static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        g_failed++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failed++;
        printf("  [FAIL] %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

/* ------------------------------------------------------------------ 主题 */

static void test_theme_identity(void) {
    printf("theme: 默认主题必须是 identity（一个字节都不改）\n");
    theme_init();
    char buf[] = "\x1b[048;2;033;038;045m│\x1b[038;2;230;237;243m x";
    char copy[sizeof(buf)];
    memcpy(copy, buf, sizeof(buf));
    theme_remap(buf, (int)strlen(buf));
    check(memcmp(buf, copy, sizeof(buf)) == 0, "github-dark 下输出保持不变");
    check_str(theme_name(), "github-dark", "默认主题名");
}

static void test_theme_remap(void) {
    printf("theme: 换主题后 UI 色被替换、pane 内容色不受影响\n");
    theme_init();
    check(theme_set_by_name("nord") == 1, "nord 主题存在");
    check(theme_set_by_name("no-such-theme") == 0, "未知主题名被拒绝");
    theme_apply();

    char buf[128];
    /* 前半段是 UI 面板底色（零填充），后半段是 pane 内容色（%d 输出，无前导零） */
    snprintf(buf, sizeof(buf), "\x1b[048;2;033;038;045mUI\x1b[38;2;33;38;45mPANE");
    int len = (int)strlen(buf);
    theme_remap(buf, len);
    check((int)strlen(buf) == len, "重映射保持长度不变");
    check(strstr(buf, "\x1b[048;2;033;038;045m") == NULL, "UI 底色已被替换");
    check(strstr(buf, "\x1b[38;2;33;38;45m") != NULL, "pane 内容色原样保留");

    int r, g, b;
    theme_role_rgb(TH_BG2, &r, &g, &b);
    char want[32];
    snprintf(want, sizeof(want), "\x1b[048;2;%03d;%03d;%03dm", r, g, b);
    check(strstr(buf, want) != NULL, "UI 底色替换为 nord 的 panel 角色色");

    /* 带 ;1（加粗）后缀的形式也要命中 */
    snprintf(buf, sizeof(buf), "\x1b[038;2;217;119;054;1mX");
    theme_remap(buf, (int)strlen(buf));
    check(strstr(buf, "217;119;054") == NULL, "带 ;1 后缀的序列同样被替换");

    theme_init();
}

static void test_theme_override(void) {
    printf("theme: [theme] 段的单色覆盖\n");
    theme_init();
    check(theme_set_role_hex("accent", "#ff8800") == 1, "接受 #rrggbb");
    check(theme_set_role_hex("accent", "ff8800") == 1, "接受不带 # 的写法");
    check(theme_set_role_hex("accent", "#xyz") == 0, "拒绝非法十六进制");
    check(theme_set_role_hex("not-a-role", "#ffffff") == 0, "拒绝未知角色名");
    theme_apply();

    int r, g, b;
    theme_role_rgb(TH_ACCENT, &r, &g, &b);
    check(r == 0xff && g == 0x88 && b == 0x00, "覆盖值生效");
    check(theme_has_overrides() == 1, "覆盖标记置位");

    char hex[16];
    theme_get_override(TH_ACCENT, hex, sizeof(hex));
    check_str(hex, "#ff8800", "覆盖值可原样写回 ini");

    /* 覆盖了 accent，默认主题也不再是 identity */
    char buf[64];
    snprintf(buf, sizeof(buf), "\x1b[048;2;031;111;235mX");
    theme_remap(buf, (int)strlen(buf));
    check(strstr(buf, "031;111;235") == NULL, "覆盖后 accent 底色被替换");

    theme_init();
    check(theme_has_overrides() == 0, "theme_init 清空覆盖");
}

static void test_theme_roles(void) {
    printf("theme: 角色名 <-> 索引\n");
    check(theme_role_index("panel") == TH_BG2, "panel -> TH_BG2");
    check(theme_role_index("PANEL") == TH_BG2, "角色名大小写不敏感");
    check(theme_role_index("nope") == -1, "未知角色名返回 -1");
    for (int i = 0; i < TH_ROLE_COUNT; i++)
        check(theme_role_index(theme_role_name(i)) == i, "角色名往返一致");
    check(theme_count() >= 5, "至少内置 5 套主题");
    for (int i = 0; i < theme_count(); i++)
        check(theme_set_by_name(theme_name_at(i)) == 1, "内置主题都能按名切换");
    theme_init();
}

/* WCAG 相对亮度与对比度：换主题后界面必须依然“看得清” */
static double srgb_channel(int v) {
    double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(int r, int g, int b) {
    return 0.2126 * srgb_channel(r) + 0.7152 * srgb_channel(g) + 0.0722 * srgb_channel(b);
}

static double contrast_roles(int fg_role, int bg_role) {
    int fr, fg, fb, br, bg2, bb;
    theme_role_rgb(fg_role, &fr, &fg, &fb);
    theme_role_rgb(bg_role, &br, &bg2, &bb);
    double l1 = luminance(fr, fg, fb), l2 = luminance(br, bg2, bb);
    if (l1 < l2) { double t = l1; l1 = l2; l2 = t; }
    return (l1 + 0.05) / (l2 + 0.05);
}

static void check_contrast(const char *theme, int fg_role, int bg_role, double min, const char *what) {
    double c = contrast_roles(fg_role, bg_role);
    g_checks++;
    if (c < min) {
        g_failed++;
        printf("  [FAIL] %s: %s 对比度 %.2f < %.2f\n", theme, what, c, min);
    }
}

static void test_theme_contrast(void) {
    printf("theme: 每套内置主题的关键配色对比度\n");
    for (int i = 0; i < theme_count(); i++) {
        theme_init();
        theme_set_by_name(theme_name_at(i));
        theme_apply();
        const char *name = theme_name_at(i);
        /* 正文 / 次要文字压在面板与标签栏底色上 */
        check_contrast(name, TH_FG, TH_BG2, 7.0, "正文 vs 面板底");
        check_contrast(name, TH_FG, TH_BG1, 7.0, "正文 vs 标签栏底");
        check_contrast(name, TH_FG_DIM, TH_BG2, 3.2, "次要文字 vs 面板底");
        check_contrast(name, TH_FG_DIM, TH_BG1, 3.2, "次要文字 vs 标签栏底");
        /* 活动标签：白字压强调色 */
        check_contrast(name, TH_WHITE, TH_ACCENT, 3.2, "白字 vs 强调色");
        /* 深色字压在亮色按钮上（[*] [+] 等） */
        check_contrast(name, TH_BG0, TH_CYAN, 5.0, "深色字 vs 浅蓝按钮");
        check_contrast(name, TH_BG0, TH_GREEN, 4.5, "深色字 vs 绿色按钮");
        check_contrast(name, TH_BG0, TH_YELLOW, 5.0, "深色字 vs 琥珀按钮");
        check_contrast(name, TH_BG0, TH_ORANGE, 4.0, "深色字 vs 橙色按钮");
        /* 白字压危险色 / 选区 */
        check_contrast(name, TH_WHITE, TH_RED, 3.0, "白字 vs 红色");
        check_contrast(name, TH_WHITE, TH_SELECTION, 5.0, "白字 vs 选区底");
        /* 面板与标签栏必须能分层 */
        check_contrast(name, TH_BG2, TH_BG1, 1.12, "面板底 vs 标签栏底");
    }
    theme_init();
}

/* ------------------------------------------------------------------ 键位 */

#define CTRL_ONLY  (LEFT_CTRL_PRESSED)
#define ALT_ONLY   (LEFT_ALT_PRESSED)

static void test_keymap_defaults(void) {
    printf("keymap: 默认键位复刻 v1.8.3 行为\n");
    keymap_init();
    int arg = 0;

    check(keymap_lookup('C', 0, 'c', &arg) == ACT_NEW_PANE, "c -> new-pane");
    check(keymap_lookup('N', 0, 'n', &arg) == ACT_NEXT_PANE, "n -> next-pane");
    check(keymap_lookup('P', 0, 'p', &arg) == ACT_PREV_PANE, "p -> prev-pane");
    check(keymap_lookup('X', 0, 'x', &arg) == ACT_CLOSE_PANE, "x -> close-pane");
    check(keymap_lookup('D', 0, 'd', &arg) == ACT_QUIT, "d -> quit");
    check(keymap_lookup('S', 0, 's', &arg) == ACT_SETTINGS, "s -> settings");
    check(keymap_lookup('R', 0, 'r', &arg) == ACT_RELOAD_CONFIG, "r -> reload-config");
    check(keymap_lookup('T', 0, 't', &arg) == ACT_TAB_COLOR_NEXT, "t -> tab-color-next");
    check(keymap_lookup('T', SHIFT_PRESSED, 'T', &arg) == ACT_TAB_COLOR_PREV, "Shift+t -> tab-color-prev");
    check(keymap_lookup(0, 0, ':', &arg) == ACT_COMMAND_PALETTE, ": -> command-palette");
    check(keymap_lookup(0, 0, 0xFF1A, &arg) == ACT_COMMAND_PALETTE, "全角冒号同样打开命令面板");
    check(keymap_lookup(0, 0, '/', &arg) == ACT_SEARCH, "/ -> search");
    check(keymap_lookup(0, 0, '?', &arg) == ACT_HELP, "? -> help");
    check(keymap_lookup(0, 0, 'h', &arg) == ACT_HELP, "h -> help");
    check(keymap_lookup(0, 0, '[', &arg) == ACT_COPY_MODE, "[ -> copy-mode");
    check(keymap_lookup(0, 0, '+', &arg) == ACT_NEW_PANE_MENU, "+ -> new-pane-menu");
    check(keymap_lookup(VK_ADD, 0, 0, &arg) == ACT_NEW_PANE_MENU, "小键盘 + -> new-pane-menu");

    /* ---- 中文输入法（全角标点）：2026-09-20 用户报「ctrl+b 加中文时的 _ 没反应」。
     * 输入法把 Shift+标点 变成 VK_PACKET + 全角字符，只按虚拟键码匹配的绑定会漏。
     * 判据必须覆盖【VK_PACKET + 全角字符】这个真实形态，不能只喂 vk=0。 ---- */
    check(keymap_lookup(VK_OEM_MINUS, SHIFT_PRESSED, '_', &arg) == ACT_SPLIT_HORIZONTAL,
          "_ (VK_OEM_MINUS+Shift) -> split-horizontal");
    check(keymap_lookup(0, 0, '_', &arg) == ACT_SPLIT_HORIZONTAL, "_ (纯字符) -> split-horizontal");
    check(keymap_lookup(VK_PACKET, 0, 0xFF3F, &arg) == ACT_SPLIT_HORIZONTAL,
          "中文输入法 VK_PACKET + 全角下划线 ＿ -> split-horizontal");
    check(keymap_lookup(VK_PACKET, SHIFT_PRESSED, 0xFF3F, &arg) == ACT_SPLIT_HORIZONTAL,
          "同上，带 Shift 状态也要命中");
    check(keymap_lookup(VK_PACKET, 0, 0xFF0D, &arg) == ACT_SPLIT_VERTICAL,
          "全角减号 － -> split-vertical（前缀 - 左右分屏）");
    check(keymap_lookup(VK_PACKET, 0, 0xFF5C, &arg) == ACT_SPLIT_VERTICAL,
          "全角竖线 ｜ -> split-vertical");
    check(keymap_lookup(VK_PACKET, 0, 0xFF0F, &arg) == ACT_SEARCH, "全角斜杠 ／ -> search");
    check(keymap_lookup(VK_PACKET, 0, 0xFF1F, &arg) == ACT_HELP, "全角问号 ？ -> help");
    check(keymap_lookup(VK_PACKET, 0, 0xFF3B, &arg) == ACT_COPY_MODE, "全角方括号 ［ -> copy-mode");
    check(keymap_lookup(VK_PACKET, 0, 0xFF0B, &arg) == ACT_NEW_PANE_MENU, "全角加号 ＋ -> new-pane-menu");
    check(keymap_lookup(0, 0, 0xFF1A, &arg) == ACT_COMMAND_PALETTE,
          "全角冒号 ：-> command-palette（折叠后与 CHR(':') 同一条）");
    /* 折叠只影响键位匹配：未绑定的全角字符仍然返回 ACT_NONE，不会被误吞。 */
    check(keymap_lookup(VK_PACKET, 0, 0xFF21, &arg) == ACT_NONE, "全角 Ａ 未绑定 -> ACT_NONE");

    /* v1.8.45：按编号跳转 pane（主键盘/小键盘数字）已整体移除，统一走 w 打开
     * 「切换 panel」可视化面板。 */
    check(keymap_lookup('W', 0, 'w', &arg) == ACT_SWITCH_PANEL_PALETTE, "w -> switch-panel 面板");
    check(keymap_lookup('3', 0, '3', &arg) == ACT_NONE, "主键盘 3 不再直接跳转");
    check(keymap_lookup(VK_NUMPAD7, 0, 0, &arg) == ACT_NONE, "小键盘 7 不再按编号跳转");
    check(keymap_lookup(VK_F5, 0, 0, &arg) == ACT_NONE, "未绑定键返回 ACT_NONE");
}

static void test_keymap_prefix(void) {
    printf("keymap: 前缀键可配置\n");
    keymap_init();
    check(keymap_is_prefix('B', CTRL_ONLY, 0x02) == 1, "默认 Ctrl+B 是前缀");
    check(keymap_is_prefix(0, 0, 0x02) == 1, "只送出控制字符时也识别");
    check(keymap_is_prefix('A', CTRL_ONLY, 0x01) == 0, "Ctrl+A 不是默认前缀");
    check(keymap_prefix_char() == 0x02, "前缀控制字符 = 0x02");

    check(keymap_set_prefix("C-a") == 1, "prefix = C-a 解析成功");
    check(keymap_is_prefix('A', CTRL_ONLY, 0x01) == 1, "Ctrl+A 成为前缀");
    check(keymap_is_prefix('B', CTRL_ONLY, 0x02) == 0, "Ctrl+B 不再是前缀");
    check(keymap_prefix_char() == 0x01, "前缀控制字符随之变为 0x01");
    check(keymap_set_prefix("") == 0, "空前缀被拒绝");
    keymap_init();
}

static void test_keymap_parse(void) {
    printf("keymap: 键位字符串解析\n");
    KeySpec k;
    check(keymap_parse_key("C-b", &k) && k.ctrl && !k.alt && k.vk == 'B', "C-b");
    check(keymap_parse_key("M-x", &k) && k.alt && !k.ctrl && k.vk == 'X', "M-x (Alt)");
    check(keymap_parse_key("A-x", &k) && k.alt, "A-x 等价于 M-x");
    check(keymap_parse_key("S-t", &k) && k.shift && !k.shift_any && k.vk == 'T', "S-t");
    check(keymap_parse_key("t", &k) && k.shift_any && k.vk == 'T', "无修饰字母忽略 Shift 状态");
    check(keymap_parse_key("F5", &k) && k.vk == VK_F1 + 4, "F5");
    check(keymap_parse_key("space", &k) && k.vk == VK_SPACE, "space");
    check(keymap_parse_key("PgUp", &k) && k.vk == VK_PRIOR, "命名键大小写不敏感");
    check(keymap_parse_key(":", &k) && k.ch == ':', "标点按字符匹配");
    check(keymap_parse_key("", &k) == 0, "空串失败");
    check(keymap_parse_key("nosuchkey", &k) == 0, "未知键名失败");
}

static void test_keymap_rebind(void) {
    printf("keymap: [keys] 覆盖\n");
    keymap_init();
    int arg = 0;
    check(keymap_bind("new-pane", "k") == 1, "new-pane = k 绑定成功");
    check(keymap_lookup('K', 0, 'k', &arg) == ACT_NEW_PANE, "新键位生效");
    check(keymap_lookup('C', 0, 'c', &arg) == ACT_NONE, "该动作的默认键位被顶掉");
    check(keymap_lookup('X', 0, 'x', &arg) == ACT_CLOSE_PANE, "其它动作的默认键位不受影响");
    check(keymap_bind("no-such-action", "k") == 0, "未知动作名被拒绝");
    check(keymap_bind("new-pane", "nosuchkey") == 0, "非法键位被拒绝");

    check(keymap_user_binding_count() == 1, "用户绑定计数");
    check_str(keymap_user_binding_action(0), "new-pane", "回写动作名");
    check_str(keymap_user_binding_key(0), "k", "回写键位文本");

    keymap_init();
    check(keymap_has_user_bindings() == 0, "keymap_init 清空用户绑定");
}

static void test_keymap_describe(void) {
    printf("keymap: 帮助页显示的键位跟随配置\n");
    keymap_init();
    char buf[48];
    keymap_describe(ACT_NEW_PANE, buf, sizeof(buf));
    check_str(buf, "Ctrl+B c", "默认 new-pane 描述");
    keymap_describe(ACT_COMMAND_PALETTE, buf, sizeof(buf));
    check_str(buf, "Ctrl+B :", "默认 command-palette 描述");
    keymap_describe(ACT_TAB_COLOR_PREV, buf, sizeof(buf));
    check_str(buf, "Ctrl+B Shift+T", "Shift 组合的描述");

    /* v1.8.38：Tab/Space/Enter 等控制字符必须输出字面键名，不能把原始 0x09
     * 控制符塞进描述串（命令面板按 utf8 宽度补 pad 会错位，右侧多一个空格）。 */
    keymap_describe(ACT_SPLIT_NEXT, buf, sizeof(buf));
    check_str(buf, "Ctrl+B Tab", "split-next(\\t) 描述为字面 Tab");
    {
        int has_ht = 0;
        for (size_t bi = 0; bi < strlen(buf); bi++) if ((unsigned char)buf[bi] == 0x09) has_ht = 1;
        check(!has_ht, "split-next 描述串不含原始 HT(0x09) 控制符");
    }
    keymap_describe(ACT_SPLIT_PREV, buf, sizeof(buf));
    check_str(buf, "Ctrl+B Shift+tab", "split-prev(Shift+Tab) 描述");

    /* v1.8.39：可打印 OEM 键按 Shift 状态显示实际符号，Shift+- 是 _（上下分屏），
     * 之前落到 "?" 显示成 Ctrl+B Shift+?。 */
    keymap_describe(ACT_SPLIT_HORIZONTAL, buf, sizeof(buf));
    check_str(buf, "Ctrl+B _", "split-horizontal(Shift+-) 描述为 _");
    keymap_describe(ACT_SPLIT_VERTICAL, buf, sizeof(buf));
    check_str(buf, "Ctrl+B -", "split-vertical(-) 描述为 -");

    keymap_describe(ACT_SEND_PREFIX, buf, sizeof(buf));
    check_str(buf, "Ctrl+B Ctrl+B", "未绑定时 send-prefix 显示为连按两次前缀");
    keymap_bind("send-prefix", "q");
    keymap_describe(ACT_SEND_PREFIX, buf, sizeof(buf));
    check_str(buf, "Ctrl+B q", "绑定后 send-prefix 显示真实键位");
    int sp_arg = 0;
    check(keymap_lookup('Q', 0, 'q', &sp_arg) == ACT_SEND_PREFIX, "绑定后的键真的触发 send-prefix");
    keymap_unbind("send-prefix");
    keymap_describe(ACT_SEND_PREFIX, buf, sizeof(buf));
    check_str(buf, "Ctrl+B Ctrl+B", "复位后回到连按两次前缀");

    keymap_set_prefix("C-a");
    keymap_bind("new-pane", "F2");
    keymap_describe(ACT_NEW_PANE, buf, sizeof(buf));
    check_str(buf, "Ctrl+A F2", "改了 prefix 与键位后描述同步");
    keymap_init();

    /* v2.0.4 (审计 BUG-12)：spec_text 最长能产出 "Ctrl+Alt+Shift+backspace"
     * = 24 字节，而 keymap_describe 内部原来用 prefix[24]/key[24]，差 1 字节
     * 装不下 NUL，帮助页显示成不存在的键名 "Ctrl+Alt+Shift+backspac"。
     * 设置页录键不追加 S-（走不到），但手写 termux.ini `prefix = C-M-S-backspace`
     * 直通 keymap_set_prefix，用户照 ini 注释就能写出来。 */
    check(keymap_set_prefix("C-M-S-backspace") != 0, "C-M-S-backspace 是合法前缀");
    keymap_bind("send-prefix", "C-M-S-backspace");
    char big[128];
    keymap_prefix_describe(big, sizeof(big));
    check_str(big, "Ctrl+Alt+Shift+backspace", "最长前缀名 24 字节完整");
    keymap_describe(ACT_SEND_PREFIX, big, sizeof(big));
    check_str(big, "Ctrl+Alt+Shift+backspace Ctrl+Alt+Shift+backspace",
              "describe 内部缓冲不能截断最长前缀 / 最长键名");
    keymap_unbind("send-prefix");
    keymap_describe(ACT_SEND_PREFIX, big, sizeof(big));
    check_str(big, "Ctrl+Alt+Shift+backspace Ctrl+Alt+Shift+backspace",
              "未绑定时的前缀两连同样不截断");
    keymap_init();
}

static void test_keymap_capture(void) {
    printf("keymap: 图形化设置页的键位录制与复位\n");
    keymap_init();
    char text[24];

    check(keymap_key_text_from_event('K', 0, 'k', text, sizeof(text)) == 1 && strcmp(text, "k") == 0,
          "普通字母录成 \"k\"");
    check(keymap_key_text_from_event('B', LEFT_CTRL_PRESSED, 2, text, sizeof(text)) == 1 && strcmp(text, "C-b") == 0,
          "Ctrl+B 录成 \"C-b\"");
    check(keymap_key_text_from_event('W', LEFT_ALT_PRESSED, 'w', text, sizeof(text)) == 1 && strcmp(text, "M-w") == 0,
          "Alt+W 录成 \"M-w\"");
    check(keymap_key_text_from_event('T', SHIFT_PRESSED, 'T', text, sizeof(text)) == 1 && strcmp(text, "S-t") == 0,
          "Shift+T 录成 \"S-t\"");
    check(keymap_key_text_from_event(VK_F2, 0, 0, text, sizeof(text)) == 1 && strcmp(text, "F2") == 0,
          "F2 录成 \"F2\"");
    check(keymap_key_text_from_event(VK_PRIOR, 0, 0, text, sizeof(text)) == 1 && strcmp(text, "pgup") == 0,
          "PgUp 录成命名键");
    check(keymap_key_text_from_event(VK_SHIFT, SHIFT_PRESSED, 0, text, sizeof(text)) == 0,
          "纯修饰键不能作为键位");
    check(keymap_key_text_from_event(VK_CONTROL, LEFT_CTRL_PRESSED, 0, text, sizeof(text)) == 0,
          "Ctrl 单独按下不能作为键位");

    /* 录制到的文本必须能被 keymap_bind 直接接受（设置页就是这么用的） */
    int arg = 0;
    keymap_key_text_from_event(VK_F2, 0, 0, text, sizeof(text));
    check(keymap_bind("new-pane", text) == 1, "录制结果可直接绑定");
    check(keymap_lookup(VK_F2, 0, 0, &arg) == ACT_NEW_PANE, "绑定后 F2 生效");
    check(keymap_action_is_overridden(ACT_NEW_PANE) == 1, "设置页可据此显示“自定义”标记");

    check(keymap_unbind("new-pane") == 1, "复位按钮解除绑定");
    check(keymap_lookup(VK_F2, 0, 0, &arg) == ACT_NONE, "复位后 F2 失效");
    check(keymap_lookup('C', 0, 'c', &arg) == ACT_NEW_PANE, "复位后默认键位恢复");
    check(keymap_action_is_overridden(ACT_NEW_PANE) == 0, "自定义标记同时清除");
    check(keymap_unbind("no-such-action") == 0, "复位未知动作返回 0");

    check(keymap_action_count() >= 17, "动作表可供设置页遍历");
    check(keymap_action_at(0) != ACT_NONE && keymap_action_at(-1) == ACT_NONE, "动作遍历边界安全");
    keymap_init();
}

static void test_theme_clear(void) {
    printf("theme: 设置页的复位按钮\n");
    theme_init();
    theme_set_role_hex("accent", "#ff0000");
    theme_set_role_hex("panel", "#001122");
    theme_apply();
    check(theme_role_is_overridden(TH_ACCENT) == 1, "单项覆盖状态可查询");

    theme_clear_role_override(TH_ACCENT);
    theme_apply();
    check(theme_role_is_overridden(TH_ACCENT) == 0, "单项复位");
    check(theme_role_is_overridden(TH_BG2) == 1, "其它覆盖项不受影响");

    theme_clear_overrides();
    theme_apply();
    check(theme_has_overrides() == 0, "一键清除全部自定义颜色");

    char buf[64];
    snprintf(buf, sizeof(buf), "\x1b[048;2;033;038;045mX");
    theme_remap(buf, (int)strlen(buf));
    check(strstr(buf, "033;038;045") != NULL, "清除后回到默认主题的 identity 行为");
    theme_init();
}

static void test_keymap_actions(void) {
    printf("keymap: 动作名表完整\n");
    /* 遍历【已登记】的动作（keymap_action_at 只返回 g_actions 表里的动作；
     * 已移除的 ACT_SELECT_PANE 等占位枚举不在表内，跳过）。 */
    for (int i = 0; i < keymap_action_count(); i++) {
        int a = keymap_action_at(i);
        const char *name = keymap_action_name(a);
        check(name && name[0] != 0, "每个动作都有名字");
        check(keymap_action_id(name) == a, "动作名往返一致");
        check(keymap_action_label(a)[0] != 0, "每个动作都有中文描述");
    }
    check(keymap_action_id("bogus") == ACT_NONE, "未知动作名返回 ACT_NONE");
    check(keymap_action_id("select-pane") == ACT_NONE, "已移除的 select-pane 不再解析");
}

static void test_keymap_noprefix(void) {
    printf("keymap: 直接键（noprefix）与前缀可读描述\n");
    keymap_init();
    char buf[48];
    keymap_prefix_describe(buf, sizeof(buf));
    check_str(buf, "Ctrl+B", "前缀描述带 Ctrl 全名");
    check(keymap_prefix_is_default() == 1, "默认前缀被识别为默认值");
    keymap_set_prefix("C-a");
    keymap_prefix_describe(buf, sizeof(buf));
    check_str(buf, "Ctrl+A", "改前缀后描述同步");
    check(keymap_prefix_is_default() == 0, "非默认前缀被识别");
    keymap_init();

    int arg = 0;
    check(keymap_action_uses_prefix(ACT_NEW_PANE) == 1, "默认动作走前缀");
    check(keymap_lookup('C', 0, 'c', &arg) == ACT_NEW_PANE, "前缀态下 c 命中");
    check(keymap_lookup_direct('C', 0, 'c', &arg) == ACT_NONE, "前缀动作不会被直接键命中");

    check(keymap_set_action_prefix(ACT_NEW_PANE, 0) == 1, "把 new-pane 改成直接键");
    check(keymap_action_uses_prefix(ACT_NEW_PANE) == 0, "标记生效");
    check(keymap_lookup('C', 0, 'c', &arg) == ACT_NONE, "直接键不再出现在前缀表里");
    check(keymap_lookup_direct('C', 0, 'c', &arg) == ACT_NEW_PANE, "直接键由 lookup_direct 命中");
    keymap_describe(ACT_NEW_PANE, buf, sizeof(buf));
    check_str(buf, "c", "直接键的描述不带前缀段");

    check(keymap_set_action_prefix(ACT_NEW_PANE, 1) == 1, "改回前缀键");
    check(keymap_lookup('C', 0, 'c', &arg) == ACT_NEW_PANE, "改回后前缀态恢复命中");
    check(keymap_lookup_direct('C', 0, 'c', &arg) == ACT_NONE, "改回后不再直接命中");

    keymap_init();
    check(keymap_bind("copy-mode", "F8 noprefix") == 1, "[keys] 里 \"F8 noprefix\" 解析成功");
    check(keymap_lookup_direct(VK_F1 + 7, 0, 0, &arg) == ACT_COPY_MODE, "F8 直接触发复制模式");
    check(keymap_lookup(VK_F1 + 7, 0, 0, &arg) == ACT_NONE, "F8 不再需要前缀");
    check(keymap_user_binding_count() == 1, "写回一条用户绑定");
    check(keymap_user_binding_no_prefix(0) == 1, "该绑定被标记为 noprefix");
    check_str(keymap_user_binding_key(0), "F8", "回写键位文本不含 noprefix 后缀");
    check(keymap_bind("next-pane", "F9 direct") == 1, "direct 同义词可用");
    check(keymap_lookup_direct(VK_F1 + 8, 0, 0, &arg) == ACT_NEXT_PANE, "direct 绑定生效");
    keymap_init();
    check(keymap_action_uses_prefix(ACT_COPY_MODE) == 1, "keymap_init 复位直接键标记");
}

int main(void) {
    test_theme_identity();
    test_theme_remap();
    test_theme_override();
    test_theme_roles();
    test_theme_contrast();
    test_keymap_defaults();
    test_keymap_prefix();
    test_keymap_parse();
    test_keymap_rebind();
    test_keymap_describe();
    test_keymap_capture();
    test_theme_clear();
    test_keymap_actions();
    test_keymap_noprefix();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
