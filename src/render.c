#include "render.h"
#include "framediff.h"
#include "split.h"
#include "pane.h"
#include "input.h"   /* split_drag_active()：拖动分屏边框期间不夹取 scroll_offset */
#include <stdarg.h>   /* cell_diag 诊断（TERMUX_CELLDIAG） */


#define TB_BG        "\x1b[048;2;022;027;034m"
/* 终端内容区底色（TH_BG0 = 13,17,23）：与 ConPTY 里 cmd/powershell 等程序的
 * 默认黑底一致。分屏窗格铺底用这个色，未填充格/铺底与程序黑底就没有色差，
 * 不会在窗格之间冒出奇怪的深灰块；分隔线另用面板灰（022;027;034）以可辨。 */
#define TERM_BG      "\x1b[048;2;013;017;023m"
#define TAB_IN_BG    "\x1b[048;2;033;038;045m"
#define TAB_IN_FG    "\x1b[038;2;139;148;158m"
#define TAB_ACT_BG   "\x1b[048;2;031;111;235m"
#define TAB_ACT_FG   "\x1b[038;2;255;255;255m"
#define BRAND_BG     "\x1b[048;2;137;087;229m"
#define BRAND_BG_HV  "\x1b[048;2;163;113;247m"
#define X_RED        "\x1b[038;2;248;081;073m"
#define X_RED_BG     "\x1b[048;2;248;081;073m"
#define PLUS_GREEN   "\x1b[038;2;063;185;080m"
#define PLUS_GREEN_BG "\x1b[048;2;063;185;080m"
#define DARK_FG      "\x1b[038;2;013;017;023m"

static const char *const TAB_COLOR_BG[9] = {
    "\x1b[048;2;031;111;235m",   // 0 default: blue
    "\x1b[048;2;031;111;235m",   // 1 blue (default highlight)
    "\x1b[048;2;063;185;080m",    // 2 green
    "\x1b[048;2;210;153;034m",   // 3 amber
    "\x1b[048;2;137;087;229m",   // 4 purple
    "\x1b[048;2;031;136;061m",    // 5 teal/green-dark
    "\x1b[048;2;121;192;255m",  // 6 light blue
    "\x1b[048;2;217;119;054m",   // 7 orange
    "\x1b[048;2;205;093;173m",   // 8 pink
};

static const char *const TAB_COLOR_BG_DIM[9] = {
    "\x1b[048;2;022;062;128m",
    "\x1b[048;2;022;062;128m",   // 1 blue dim
    "\x1b[048;2;036;099;049m",
    "\x1b[048;2;110;082;030m",
    "\x1b[048;2;074;048;122m",
    "\x1b[048;2;024;080;048m",
    "\x1b[048;2;052;096;128m",
    "\x1b[048;2;112;066;034m",
    "\x1b[048;2;104;050;090m",
};

static char g_current_host_title[128] = {0};

void update_host_title(void) {
    if (!g_orig_title[0]) {
        GetConsoleTitleW(g_orig_title, 255);
    }
    const char *target = "termux";
    if (g_mux.active_pane >= 0 && g_mux.active_pane < g_mux.pane_count && g_mux.panes[g_mux.active_pane].active) {
        const char *t = g_mux.panes[g_mux.active_pane].title;
        if (t && *t) target = t;
    }
    if (strcmp(g_current_host_title, target) != 0) {
        strncpy(g_current_host_title, target, sizeof(g_current_host_title) - 1);
        g_current_host_title[sizeof(g_current_host_title) - 1] = 0;
        SetConsoleTitleA(g_current_host_title);
        char seq[256];
        int len = snprintf(seq, sizeof(seq), "\x1b]0;%s\x07", g_current_host_title);
        host_write(seq, len);
    }
}

void draw_tab_bar(char *out, int bs, int *posp) {
    int pos = *posp;
    g_mux.tab_count = 0; int col = 0;
    int popup_open = (g_mux.settings_mode || g_mux.chooser_mode || g_mux.ctx_mode || g_mux.rename_mode ||
                      g_mux.custom_cmd_mode || g_search_mode || g_mux.palette_mode);
    {
        const char *brand = " termux ";
        int blen = (int)strlen(brand);
        int bcols = utf8_cols(brand, blen);
        if (col + bcols + 4 <= g_mux.host_cols) {
            int bhover = (!popup_open && g_mouse_y == 0 &&
                          g_mouse_x >= col && g_mouse_x < col + bcols);
            g_mux.tab_info[g_mux.tab_count].start_col = col;
            g_mux.tab_info[g_mux.tab_count].end_col = col + bcols;
            g_mux.tab_info[g_mux.tab_count].pane_idx = -2;
            if (bhover)
                pos += snprintf(out + pos, bs - pos, BRAND_BG_HV "\x1b[1m%s\x1b[22m", brand);
            else
                pos += snprintf(out + pos, bs - pos, BRAND_BG "%s", brand);
            col += bcols;
            g_mux.tab_count++;
        }
    }
    /* 标签按「标签页锚点」枚举。单窗格标签画成 [cmd ×]；分屏标签把同组所有窗格
     * （含 is_split_child）包在【一对】外层方括号里，窗格段之间用空格分隔，形如
     * [cmd× cmd×]，点段切窗格、点 × 关该窗格。设置/关于 pane（v1.8.45 起）也作为
     * 独立标签显示（标题「设置」/「关于」），可点击切换、点 × 关闭。 */
    for (int anchor = 0; anchor < g_mux.pane_count; anchor++) {
        if (!g_mux.panes[anchor].active) continue;
        if (g_mux.panes[anchor].is_split_child) continue;   /* 子窗格由其锚点统一枚举 */
        /* 设置 / 关于 pane 也作为独立标签显示在标签栏（可点击切换、点 × 关闭）。 */
        int group[MAX_PANES];
        int gn = split_tab_panes(anchor, group, MAX_PANES);
        if (gn <= 0) continue;
        int ci = g_mux.panes[anchor].color;   /* 整组沿用锚点（标签）颜色 */
        if (ci < 0 || ci > 8) ci = 0;
        const char *actbg = TAB_COLOR_BG[ci];
        const char *dimbg = TAB_COLOR_BG_DIM[ci];
        /* 收集本组存活窗格。 */
        int alive[MAX_PANES], an = 0;
        for (int gi = 0; gi < gn; gi++) {
            int i = group[gi];
            if (i >= 0 && i < g_mux.pane_count && g_mux.panes[i].active) alive[an++] = i;
        }
        if (an < 1) continue;
        int multi = (an >= 2);
        /* 宽度预算：单窗格 = '[' + nm + '×]'（nm宽 + 2，标题与 × 间无空格）；
         * 分屏 = '[' + Σ(nm宽+1 含×) + (an-1 个段间空格) + ']'（段间空格）。 */
        int gw = 0;
        for (int gi = 0; gi < an; gi++) {
            int i = alive[gi];
            char nm[64]; format_tab_title(nm, sizeof(nm), g_mux.panes[i].title[0] ? g_mux.panes[i].title : "cmd");
            int w = utf8_cols(nm, (int)strlen(nm)) + 1;   /* nm + × */
            gw += w + (multi && gi + 1 < an ? 1 : 0);     /* 段间空格 */
        }
        gw += multi ? 2 : 2;   /* 两种都只有 [ 与 ] 两格（单窗格标题与×间不再加空格） */
        /* 标签栏右端要常驻 [+] 与 [*] 两个按钮（各 3 列）+ 间隔，先预留这段空间。
         * 当前标签组连同预留空间装不下时就停止加入后续标签——col 保持在已画完的
         * 位置，不能强行置成 host_cols，否则会把 [+]/[*] 的位置判定挤乱，表现为
         * 「标签太多时标签/按钮位置跳变」。装不下的标签这一帧不画（其 tab_info 不
         * 登记，点击自然落空）。 */
        int reserved = 1 + 3 + 1 + 3 + 1;   /* 间隔 + [+] + 间隔 + [*] + 余量 */
        if (col + gw + reserved > g_mux.host_cols) break;

        /* 左括号。单窗格且该窗格激活时，整个标签（含括号）用激活底色 + 白粗体；
         * 单窗格未激活用暗色底 + 灰；分屏组括号统一用暗色底 + 灰（只高亮活动段）。 */
        int single_act = (!multi && alive[0] == g_mux.active_pane);
        if (single_act)
            pos += snprintf(out + pos, bs - pos, "%s" TAB_ACT_FG "\x1b[1m[", actbg);
        else
            pos += snprintf(out + pos, bs - pos, "%s\x1b[038;2;139;148;158m[", dimbg);
        int group_start = col;
        col++;
        for (int gi = 0; gi < an; gi++) {
            int i = alive[gi];
            char nm[64]; format_tab_title(nm, sizeof(nm), g_mux.panes[i].title[0] ? g_mux.panes[i].title : "cmd");
            int nmc = utf8_cols(nm, (int)strlen(nm));
            int act = (i == g_mux.active_pane);
            int first = (gi == 0), last = (gi == an - 1);
            if (act)
                pos += snprintf(out + pos, bs - pos, "%s" TAB_ACT_FG "\x1b[1m", actbg);
            else
                pos += snprintf(out + pos, bs - pos, "%s\x1b[038;2;139;148;158m", dimbg);
            /* 段起点：首段含左括号列（点击左括号=切到该窗格），其余段从标题起。 */
            g_mux.tab_info[g_mux.tab_count].start_col = first ? group_start : col;
            pos += snprintf(out + pos, bs - pos, "%s", nm);
            col += nmc;
            /* 单窗格与分屏段内，标题与 × 之间都不留空格（单窗格 [cmd×]、分屏
             * [cmd× cmd×]）；分屏的空格只在段与段之间。 */
            (void)multi;
            /* × 热区：紧跟的一格。 */
            int hovering = (!popup_open && g_mouse_y == 0 && g_mouse_x == col);
            if (hovering)
                pos += snprintf(out + pos, bs - pos, X_RED_BG "\x1b[038;2;255;255;255m\xc3\x97");
            else
                pos += snprintf(out + pos, bs - pos, "%s" X_RED "\xc3\x97", act ? actbg : dimbg);
            g_mux.tab_info[g_mux.tab_count].pane_idx = i;
            g_mux.tab_info[g_mux.tab_count].close_start = col;
            g_mux.tab_info[g_mux.tab_count].close_end = col + 1;
            col++;
            /* 段间空格（分屏且不是最后一段）：底色延续，点击归到下一段——该空格列
             * 不算进本段 end_col，下段 start_col 会落在空格列上。 */
            if (multi && !last) {
                pos += snprintf(out + pos, bs - pos, "%s ", dimbg);
                col++;
                g_mux.tab_info[g_mux.tab_count].end_col = col - 1;  /* 本段止于 ×，空格归下段 */
            } else {
                g_mux.tab_info[g_mux.tab_count].end_col = col;      /* 末段止于 ×（右括号随后并入） */
            }
            g_mux.tab_count++;
        }
        /* 右括号：并入最后一段（点右括号=切到最后一个窗格）。单窗格激活时括号
         * 也用激活底色 + 白粗体（整框高亮）；否则暗色底 + 灰。 */
        if (single_act)
            pos += snprintf(out + pos, bs - pos, "%s" TAB_ACT_FG "\x1b[1m]", actbg);
        else
            pos += snprintf(out + pos, bs - pos, "%s\x1b[038;2;139;148;158m]", dimbg);
        col++;
        g_mux.tab_info[g_mux.tab_count - 1].end_col = col;
    }
            g_mux.tab_info[g_mux.tab_count].close_start = 0;
    if (col < g_mux.host_cols - 4) { pos += snprintf(out + pos, bs - pos, TB_BG " "); col++; }
    if (col + 3 <= g_mux.host_cols - 4) {
        g_mux.tab_info[g_mux.tab_count].start_col = col;
        g_mux.tab_info[g_mux.tab_count].end_col = col + 3;
        g_mux.tab_info[g_mux.tab_count].pane_idx = -1;
        int phover = (!popup_open && g_mouse_y == 0 &&
                      g_mouse_x >= col && g_mouse_x < col + 3);
        if (phover)
            pos += snprintf(out + pos, bs - pos, PLUS_GREEN_BG DARK_FG "[+]\x1b[0m");
        else
            pos += snprintf(out + pos, bs - pos, PLUS_GREEN "[+]");
        col += 3;
        g_mux.tab_count++;
    }
    pos += snprintf(out + pos, bs - pos, TB_BG);
    while (col < g_mux.host_cols - 3 && pos < bs - 8) { out[pos++] = ' '; col++; }

    if (col + 3 <= g_mux.host_cols) {
        g_mux.tab_info[g_mux.tab_count].start_col = col;
        g_mux.tab_info[g_mux.tab_count].end_col = col + 3;
        g_mux.tab_info[g_mux.tab_count].pane_idx = -3;
        int shover = (!popup_open && g_mouse_y == 0 && g_mouse_x >= col && g_mouse_x < col + 3);
        if (shover)
            pos += snprintf(out + pos, bs - pos, "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m[*]\x1b[0m");
        else
            pos += snprintf(out + pos, bs - pos, TAB_IN_BG "\x1b[038;2;121;192;255m[*]\x1b[0m");
        col += 3;
        g_mux.tab_count++;
    }
    pos += snprintf(out + pos, bs - pos, TB_BG);
    while (col < g_mux.host_cols && pos < bs - 4) { out[pos++] = ' '; col++; }
    pos += snprintf(out + pos, bs - pos, "\x1b[0m");
    *posp = pos;
}

int popup_left_1based(int anchor0, int width, int host_cols) {
    if (width < 1) width = 1;
    if (host_cols < 1) host_cols = 1;

    /* anchor0 is a Windows mouse column (0-based); all drawing and hit
     * testing after this point use ANSI columns (1-based). */
    int anchor = (anchor0 >= 0) ? anchor0 + 1 : 1;
    int left = anchor;
    if (left + width - 1 > host_cols) left = anchor - width + 1;
    if (left < 1) left = 1;
    return left;
}

void chooser_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h) {
    (void)host_rows;
    int mcw = 0;
    for (int i = 0; i < g_chooser_item_count; i++) {
        int w15 = utf8_cols(g_chooser_items[i].name, (int)strlen(g_chooser_items[i].name));
        if (w15 > 15) w15 = 15;
        if (w15 > mcw) mcw = w15;
    }
    int tagw = (g_chooser_item_count >= 10) ? 4 : 3;
    int cw = 1 + 2 + tagw + 1 + mcw + 2;
    if (cw < 20) cw = 20;
    if (cw > host_cols) cw = host_cols;
    int ch = g_chooser_item_count + 4;
    if (w) *w = cw;
    if (h) *h = ch;
    *top = 2;
    *left = popup_left_1based(g_pop_anchor_x >= 0 ? g_pop_anchor_x : g_mouse_x, cw, host_cols);
}

void render_chooser(char *out, int bs, int *posp, int host_rows, int host_cols) {
    int top, left, cw, ch;
    chooser_geom(host_rows, host_cols, &top, &left, &cw, &ch);
    int pos = *posp;

    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;031;111;235m┌─ 新建 pane ", top, left);
    int used = 2 + 11;
    while (used < cw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80';
        used++;
    }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");

    for (int i = 0; i < g_chooser_item_count; i++) {
        int r = top + 1 + i;
        char disp_name[64] = {0};
        format_name15_display(disp_name, sizeof(disp_name), g_chooser_items[i].name);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m  \x1b[038;2;210;153;034m[%d]\x1b[0m \x1b[038;2;230;237;243m%s\x1b[0m",
                        r, left, i + 1, disp_name);
        char chooser_tag[16];
        snprintf(chooser_tag, sizeof(chooser_tag), "[%d]", i + 1);
        int item_used = 1 + 2 + utf8_cols(chooser_tag, (int)strlen(chooser_tag)) + 1 +
                        utf8_cols(disp_name, (int)strlen(disp_name));
        while (item_used < cw - 1 && pos < bs - 8) { out[pos++] = ' '; item_used++; }
        pos += snprintf(out + pos, bs - pos, "\x1b[048;2;033;038;045m│\x1b[0m");
    }

    int about_r = top + 1 + g_chooser_item_count;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m  \x1b[038;2;217;119;054;1m[A]\x1b[0m \x1b[038;2;217;119;054m关于 (About)\x1b[0m", about_r, left);
    int about_used = 1 + 2 + 3 + 1 + 12;
    while (about_used < cw - 1 && pos < bs - 8) { out[pos++] = ' '; about_used++; }
    pos += snprintf(out + pos, bs - pos, "\x1b[048;2;033;038;045m│\x1b[0m");

    int esc_r = top + 2 + g_chooser_item_count;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m  \x1b[038;2;139;148;158mEsc 取消\x1b[0m", esc_r, left);
    int esc_used = 1 + 2 + 8;
    while (esc_used < cw - 1 && pos < bs - 8) { out[pos++] = ' '; esc_used++; }
    pos += snprintf(out + pos, bs - pos, "\x1b[048;2;033;038;045m│\x1b[0m");

    int bot_r = top + 3 + g_chooser_item_count;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m└", bot_r, left);
    used = 1;
    while (used < cw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80';
        used++;
    }
    pos += snprintf(out + pos, bs - pos, "┘\x1b[0m");
    *posp = pos;
}

void render_custom_cmd_box(char *out, int bs, int *posp, int host_rows, int host_cols) {
    (void)host_rows;
    int pos = *posp;
    int top = 2;
    int ax = (g_pop_anchor_x >= 0) ? g_pop_anchor_x : g_mouse_x;
    int left = popup_left_1based(ax, CMD_BOX_W, host_cols);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;031;111;235m┌─ 自定义命令行 ─────────────────────┐\x1b[0m", top, left);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m" TB_BG " ", top + 1, left);
    render_scrollable_input(out, bs, &pos, g_mux.custom_cmd_buf, g_mux.custom_cmd_len, g_mux.custom_cmd_pos, CMD_BOX_W - 3, TB_BG, NULL);
    pos += snprintf(out + pos, bs - pos, "\x1b[048;2;033;038;045m│\x1b[0m");
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[038;2;139;148;158m", top + 2, left);
    const char *cmd_hint = "  [Enter=启动  Esc=取消]";
    int cmd_hint_cols = 1 + utf8_cols(cmd_hint, (int)strlen(cmd_hint));
    pos += snprintf(out + pos, bs - pos, "%s", cmd_hint);
    while (cmd_hint_cols < CMD_BOX_W - 1 && pos < bs - 8) {
        out[pos++] = ' ';
        cmd_hint_cols++;
    }
    pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m└────────────────────────────────────┘\x1b[0m", top + 3, left);
    *posp = pos;
}

void render_rename_box(char *out, int bs, int *posp, int host_rows, int host_cols) {
    (void)host_rows;
    int pos = *posp;
    int top = 2;
    int ax = (g_pop_anchor_x >= 0) ? g_pop_anchor_x : g_mouse_x;
    int left = popup_left_1based(ax, RENAME_W, host_cols);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;031;111;235m┌─ 重命名标签 ───────────────┐\x1b[0m", top, left);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m" TB_BG " ", top + 1, left);
    render_scrollable_input(out, bs, &pos, g_mux.rename_buf, g_mux.rename_len, g_mux.rename_pos, RENAME_W - 3, TB_BG, NULL);
    pos += snprintf(out + pos, bs - pos, "\x1b[048;2;033;038;045m│\x1b[0m");
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m└────────────────────────────┘\x1b[0m", top + 2, left);
    *posp = pos;
}

void render_ctx_menu(char *out, int bs, int *posp, int host_rows, int host_cols) {
    (void)host_rows;
    int pos = *posp;
    int top = 2;
    int ax = (g_pop_anchor_x >= 0) ? g_pop_anchor_x : g_mouse_x;
    int left = popup_left_1based(ax, CTX_W, host_cols);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;031;111;235m┌─ 标签操作 ───────────┐\x1b[0m", top, left);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m  \x1b[038;2;210;153;034m[1]\x1b[0m \x1b[038;2;230;237;243m修改颜色        \x1b[0m\x1b[048;2;033;038;045m│\x1b[0m", top + 1, left);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m  \x1b[038;2;210;153;034m[2]\x1b[0m \x1b[038;2;230;237;243m重命名标签      \x1b[0m\x1b[048;2;033;038;045m│\x1b[0m", top + 2, left);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m└──────────────────────┘\x1b[0m", top + 3, left);
    *posp = pos;
}

/* ---------------------------------------------------------------------------
 * v1.8.9: 菜单项的「启动默认颜色」选择条
 * 第 0 格「默认」宽 6，其后 8 个色块每格宽 3，格子相连。渲染与命中同源。
 * v2.1.1：整条宽 30，窄终端的右侧区域放不下时，后几格会被裁到屏幕外、鼠标也点不到
 * （用户报「过窄时，颜色编辑被截断」）。改成按可用宽度决定画几格，命中用同一个数。
 * ------------------------------------------------------------------------- */
int g_item_color_max = 8;      /* 本帧实际画出的色块格数（1..8），命中判定共用 */

int item_color_hit(int left, int col) {
    int off = col - left;
    int roww = ITEM_COLOR_DEFAULT_W + g_item_color_max * ITEM_COLOR_SWATCH_W;
    if (off < 0 || off >= roww) return -1;
    if (off < ITEM_COLOR_DEFAULT_W) return 0;
    int idx = 1 + (off - ITEM_COLOR_DEFAULT_W) / ITEM_COLOR_SWATCH_W;
    return (idx <= g_item_color_max) ? idx : -1;
}

/* 右侧区域放得下多少格（渲染与 input 命中同源）。
 * 画不满 8 格时要给行尾的「+N」留 2 列，否则它会被 ?7l 截掉，用户就不知道
 * 还有剩余色块可取。 */
int settings_detail_color_w(int host_cols, int main_left) {
    int avail = host_cols - main_left + 1;
    int n = (avail - ITEM_COLOR_DEFAULT_W) / ITEM_COLOR_SWATCH_W;
    if (n < 8) n = (avail - ITEM_COLOR_DEFAULT_W - 2) / ITEM_COLOR_SWATCH_W;
    if (n > 8) n = 8;
    if (n < 1) n = 1;
    return n;
}

void render_item_color_row(char *out, int bs, int *posp, int row, int left, int color, int focused) {
    render_item_color_row_w(out, bs, posp, row, left, color, focused, g_mux.host_cols);
}

/* v2.1.1：行尾还能剩几列（host_cols）决定要不要打「+N」/提示；独立可测（不藏全局）。 */
void render_item_color_row_w(char *out, int bs, int *posp, int row, int left, int color, int focused,
                             int host_cols) {
    int pos = *posp;
    if (color < 0 || color > 8) color = 0;
    int hover = -1;
    if (g_mouse_y + 1 == row) hover = item_color_hit(left, g_mouse_x + 1);

    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH", row, left);
    if (g_item_color_max < 1) g_item_color_max = 1;
    if (g_item_color_max > 8) g_item_color_max = 8;
    for (int i = 0; i <= g_item_color_max; i++) {
        int sel = (i == color);
        int hot = (i == hover);
        const char *bg;
        const char *fg;
        if (i == 0) {
            bg = sel ? "\x1b[048;2;038;060;088m" : (hot ? "\x1b[048;2;033;038;045m" : "\x1b[048;2;022;027;034m");
            fg = sel ? "\x1b[038;2;255;255;255;1m" : "\x1b[038;2;139;148;158m";
            pos += snprintf(out + pos, bs - pos, "%s%s 默认 \x1b[0m", bg, fg);
            continue;
        }
        bg = (sel || hot) ? TAB_COLOR_BG[i] : TAB_COLOR_BG_DIM[i];
        fg = (sel || hot) ? "\x1b[038;2;013;017;023;1m" : "\x1b[038;2;139;148;158m";
        pos += snprintf(out + pos, bs - pos, "%s%s%c%d%c\x1b[0m", bg, fg,
                        sel ? '[' : ' ', i, sel ? ']' : ' ');
    }
    /* 焦点在这一行时给个箭头，键盘用户才知道 ←/→ 会作用到哪里。
     * 窄到只画了几格时给「+N」：剩下的色块用方向键/数字键仍可取。 */
    {
        int used = ITEM_COLOR_DEFAULT_W + g_item_color_max * ITEM_COLOR_SWATCH_W;
        int room = host_cols - (left + used) + 1;        /* 行尾还剩几列 */
        int cut = (g_item_color_max < 8);                 /* 有色块没画出来 */
        const char *tail;
        /* 有色块没画出来 ⇒ 行尾必须留个「+N」（贴着屏幕边也要写，宁可挤掉别的提示）；
         * 放得下时提示与占位串等宽（14 列），保证整条行宽不随选中格变化。 */
        if (cut) tail = (room >= 14) ? (focused ? "+N 左右键取剩余" : "     +N    ") : "+N";
        else if (room >= 14) tail = focused ? "左右键选颜色" : "            ";
        else tail = "";
        pos += snprintf(out + pos, bs - pos, "  %s%s\x1b[0m",
                        focused ? "\x1b[038;2;121;192;255;1m" : "\x1b[038;2;110;118;129m", tail);
    }
    *posp = pos;
}

static void palette_hline(char *out, int bs, int *posp, int row, int left, int width,
                          const char *prefix, const char *suffix);

static void render_color_picker_cell(char *out, int bs, int *posp,
                                     const char *bg, const char *fg, char ch) {
    int pos = *posp;
    pos += snprintf(out + pos, bs - pos, "%s%s%c", bg, fg, ch);
    *posp = pos;
}

static void render_color_picker_row(char *out, int bs, int *posp, int row,
                                    int left, int base_color) {
    int pos = *posp;
    const char *panel_bg = "\x1b[048;2;033;038;045m";
    const char *normal_fg = "\x1b[038;2;013;017;023;1m";
    const char *hover_fg = "\x1b[038;2;255;255;255;1m";
    /* A swatch is a miniature tab: hovering it shows the "active tab" look
     * (full-strength colour, bold white label), everything else uses the
     * inactive-tab look (dimmed colour, muted grey label). */
    const char *idle_fg = "\x1b[038;2;139;148;158m";
    int mouse_row = row - 1; /* rendered row is ANSI 1-based */

    pos += snprintf(out + pos, bs - pos,
                    "\x1b[%d;%dH%s│", row, left, panel_bg);
    render_color_picker_cell(out, bs, &pos, panel_bg, normal_fg, ' ');

    for (int i = 0; i < 4; i++) {
        int color = base_color + i;
        int hovered = (g_mouse_y == mouse_row &&
                       g_mouse_x >= left + 1 + i * 4 &&
                       g_mouse_x < left + 5 + i * 4);
        const char *swatch_bg = hovered ? TAB_COLOR_BG[color] : TAB_COLOR_BG_DIM[color];
        for (int w = 0; w < 4; w++) {
            /* CP_SWATCH_W coloured cells then one panel-background gap, so the
             * swatches read as separate tabs instead of one long ribbon. */
            if (w >= CP_SWATCH_W) {
                render_color_picker_cell(out, bs, &pos, panel_bg, normal_fg, ' ');
                continue;
            }
            char ch = (w == 1) ? (char)('0' + color) : ' ';
            const char *fg = (w == 1) ? (hovered ? hover_fg : idle_fg) : normal_fg;
            render_color_picker_cell(out, bs, &pos, swatch_bg, fg, ch);
        }
    }

    /* Every remaining interior cell is explicitly panel background.  This
     * prevents the last swatch's background from leaking into the padding and
     * makes the visible row state unambiguous one cell at a time. */
    int interior_cols = 2 + 4 * 4;
    while (interior_cols < CP_W - 1) {
        render_color_picker_cell(out, bs, &pos, panel_bg, normal_fg, ' ');
        interior_cols++;
    }
    pos += snprintf(out + pos, bs - pos, "%s│\x1b[0m", panel_bg);
    *posp = pos;
}

void render_color_picker(char *out, int bs, int *posp, int host_rows, int host_cols) {
    (void)host_rows;
    int pos = *posp;
    int top = 2;
    int ax = (g_pop_anchor_x >= 0) ? g_pop_anchor_x : g_mouse_x;
    int left = popup_left_1based(ax, CP_W, host_cols);
    /* The frame is drawn to CP_W instead of a hard-coded ruler so the panel
     * always hugs the swatches; it used to be padded out with dead space. */
    const char *hdr = "┌─ 选择颜色 ";
    int cols = utf8_cols(hdr, (int)strlen(hdr));
    pos += snprintf(out + pos, bs - pos,
                    "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;031;111;235m%s",
                    top, left, hdr);
    while (cols < CP_W - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80'; cols++;
    }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");
    render_color_picker_row(out, bs, &pos, top + 1, left, 1);
    render_color_picker_row(out, bs, &pos, top + 2, left, 5);
    palette_hline(out, bs, &pos, top + 3, left, CP_W, "└", "┘");
    *posp = pos;
}

void presets_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h, int *max_nw, int *max_cw) {
    (void)host_rows;
    int mnw = 0, mcw = 0;
    for (int i = 0; i < g_preset_count; i++) {
        int nw = utf8_cols(g_presets[i].name, (int)strlen(g_presets[i].name));
        int cw = utf8_cols(g_presets[i].cmd, (int)strlen(g_presets[i].cmd));
        if (nw > mnw) mnw = nw;
        if (cw > mcw) mcw = cw;
    }
    if (mnw < 6) mnw = 6;
    if (mcw < 8) mcw = 8;
    const char *hdr_full = "┌─ 常用命令行预设 (按数字/回车选择) ┐";
    int min_hdr = utf8_cols(hdr_full, (int)strlen(hdr_full));
    int pw = 1 + 2 + 4 + mnw + 1 + mcw + 2;
    if (pw < min_hdr + 2) pw = min_hdr + 2;
    if (pw > host_cols) pw = host_cols;
    int ph = g_preset_count + 3;
    if (w) *w = pw;
    if (h) *h = ph;
    if (max_nw) *max_nw = mnw;
    if (max_cw) *max_cw = mcw;
    *top = 3;
    /* Unlike the tab bar, this popup is emitted with an ANSI cursor
     * position, so its left edge is 1-based. */
    *left = (host_cols - pw) / 2 + 1;
    if (*left < 1) *left = 1;
}

void render_settings_presets(char *out, int bs, int *posp, int host_rows, int host_cols) {
    int top, left, pw, ph, mnw, mcw;
    presets_geom(host_rows, host_cols, &top, &left, &pw, &ph, &mnw, &mcw);
    int pos = *posp;

    const char *hdr_text = "┌─ 常用命令行预设 (按数字/回车选择) ";
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;031;136;061;1m%s", top, left, hdr_text);
    int cols = utf8_cols(hdr_text, (int)strlen(hdr_text));
    while (cols < pw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80';
        cols++;
    }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");

    for (int i = 0; i < g_preset_count; i++) {
        int r = top + 1 + i;
        int row_hover = (g_mouse_y == r - 1 && g_mouse_x >= left - 1 && g_mouse_x < left - 1 + pw);
        int is_sel = (i == g_preset_sel);
        const char *bg = (row_hover || is_sel) ? "\x1b[048;2;045;055;072m" : "\x1b[048;2;022;027;034m";
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s  \x1b[038;2;210;153;034m[%d]\x1b[0m%s \x1b[038;2;230;237;243;1m",
                        r, left, bg, i + 1, bg);
        char preset_tag[16];
        snprintf(preset_tag, sizeof(preset_tag), "[%d]", i + 1);
        /* │(1) + 两空格(2) + [n](tag宽) + tag 后分隔空格(1)；漏算这个空格会让
         * pad_to_right_border 多补一格、整行右边框右突 1 列（右侧不对齐）。 */
        cols = 1 + 2 + utf8_cols(preset_tag, (int)strlen(preset_tag)) + 1;
        append_padded_utf8(out, bs, &pos, &cols, g_presets[i].name, mnw);
        pos += snprintf(out + pos, bs - pos, "%s \x1b[038;2;139;148;158m", bg);
        cols += 1;
        append_padded_utf8(out, bs, &pos, &cols, g_presets[i].cmd, mcw);
        pos += snprintf(out + pos, bs - pos, "%s", bg);
        pad_to_right_border(out, bs, &pos, &cols, pw);
    }

    int esc_r = top + 1 + g_preset_count;
    int h_esc = (g_mouse_y == esc_r - 1 && g_mouse_x >= left - 1 + 2 && g_mouse_x <= left - 1 + 14);
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[048;2;022;027;034m  ", esc_r, left);
    cols = 1 + 2;
    if (h_esc)
        pos += snprintf(out + pos, bs - pos, "\x1b[048;2;217;119;054m\x1b[038;2;255;255;255;1m [Esc] 取消 \x1b[0m\x1b[048;2;022;027;034m");
    else
        pos += snprintf(out + pos, bs - pos, "\x1b[048;2;033;038;045m\x1b[038;2;139;148;158m [Esc] 取消 \x1b[0m\x1b[048;2;022;027;034m");
    cols += 12;
    pad_to_right_border(out, bs, &pos, &cols, pw);

    int bot_r = top + 2 + g_preset_count;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m└", bot_r, left);
    cols = 1;
    while (cols < pw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80';
        cols++;
    }
    pos += snprintf(out + pos, bs - pos, "┘\x1b[0m");
    *posp = pos;
}

/* ---------------------------------------------------------------------------
 * 设置页新增的三个分类：外观 / 键位 / 行为
 * 渲染与鼠标命中共用同一套几何函数，避免两边写死行号后走偏。
 * ------------------------------------------------------------------------- */

/* v2.1.0：侧栏自适应。原布局固定要 13+菜单项数 行，终端一矮 [A]/[K]/[B]/[W] 四个
 * 入口就被挤到屏幕外（用户报「终端太矮时设置显示不全」——连子页都进不去）。
 * 规则：① 先省掉「导航选项」表头与两条分隔行（省 3 行）；② 再省 [P] 快速预设库行
 * （P 键仍可）；③ 菜单项列表变成一个可滚动的窗口（v2.1.2：原先是「只画前 N 个」，
 * 第 N+1 项起永远看不见，用户报「导航选项要可以滚动，而不是省略成 添加(共4项)」）。
 * 四个入口和 [Ctrl+S] 保存行永远留在屏内。 */
int settings_sidebar_sel_natural(void);
int settings_sidebar_geom_cap(int host_rows, int item_count) {
    SettingsSidebarGeom g;
    settings_sidebar_geom(host_rows, item_count, &g);
    return g.items_cap;      /* 复用同一份几何，免得两处各写一遍算错的 cap */
}

void settings_sidebar_clamp_sel(int host_rows, int item_count) {
    SettingsSidebarGeom g;
    settings_sidebar_geom(host_rows, item_count, &g);
    int sel = settings_sidebar_sel_natural();
    if (sel < 1 || g.items_cap <= 0) return;
    if (g_settings_sidebar_scroll > sel - 1) g_settings_sidebar_scroll = sel - 1;
    if (g_settings_sidebar_scroll < sel - g.items_cap) g_settings_sidebar_scroll = sel - g.items_cap;
}

void settings_sidebar_geom(int host_rows, int item_count, SettingsSidebarGeom *g) {
    memset(g, 0, sizeof(*g));
    /* v2.1.4：侧栏底部由「[+] 添加新条目 + [P] 快速预设库」两条合并成一条
     * 「[M] 条目管理」（新建 / 预设库 / ↑↓ / 改 / 删 都在那一页里）。
     * want 仍然按 13 + item_count 算：少掉的那一行让给条目窗口，
     * [A]/[K]/[B]/[W] 的行位与上一版逐字节一致 —— 右栏那些「行宽要跟侧栏错开」
     * 的排版（如行为页 scrollback 行的 [-]/[+]）全靠 g.app 这个门槛定位，动不得。 */
    int want = 13 + item_count;          /* [W] 落在 host_rows-1 所需的最小高度 */
    g->hdr = 2;
    if (host_rows < want) { g->compact = 1; want -= 3; }
    /* v2.1.4：侧栏少了一条按钮，但【行预算的档位】沿用上一版的 hide_presets 触发器：
     * 它决定 fixed_below（⇒ 条目窗口大小与 [A] 所在行），而右栏的
     * settings_right_row_limit 是拿 g.app 当「哪些行要给侧栏让位」的门槛用的
     * （行为页 scrollback 行的 [-]/[+] 就靠它）。省掉这一档 ⇒ 56x18 实测 [+] 被裁掉。 */
    if (host_rows < want) { g->nav_tight = 1; want -= 1; }
    g->nav_label = g->compact ? 0 : 3;
    g->sep1      = g->compact ? 0 : 4;
    g->start     = g->compact ? 3 : 5;
    g->sep2      = g->compact ? 0 : 6;
    g->items_row0 = g->start + (g->compact ? 1 : 2);
    /* 行预算与上一版同式（[+] [P] + [A][K][B][W] = 6 行；窄档省掉 [P] ⇒ 5 行）：
     * 条目窗口大小、[A]/[K]/[B]/[W] 的行位、右栏用来判「这一行要不要给侧栏让位」的
     * g.app 门槛都必须逐字节不变（56x18 实测：门槛一上移，行为页 scrollback 行的
     * [-]/[+] 就被当成「会和侧栏相交」裁掉了）。[+] [P] 合并成 [M] 省下的那一行，
     * 加在底部那一排的【起点】上（见下面 nav0），窗口行位因此不变。 */
    int fixed_below = g->nav_tight ? 5 : 6;
    int cap = host_rows - 1 - fixed_below - g->items_row0 + 1;
    if (cap < 0) cap = 0;
    g->items_cap = cap;
    /* v2.1.2：菜单项列表变成「窗口」而不是「截断」。原先只画前 cap 个，第 cap+1 个
     * 起就永远看不见（用户报「设置中导航选项要可以滚动，而不是省略成 添加(共4项)」）；
     * 现在窗口随选中项夹动（和右侧窗格同一套规则），滚轮/↑/↓ 都能把它挪。 */
    int maxs = item_count - cap;
    if (maxs < 0) maxs = 0;
    if (g_settings_sidebar_scroll > maxs) g_settings_sidebar_scroll = maxs;
    if (g_settings_sidebar_scroll < 0) g_settings_sidebar_scroll = 0;
    /* 选中项【不能】每帧来夹窗口 —— 那样窗口永远被拽回「第 1 项可见」，滚轮滚不动
     * （v2.1.2 第一版就踩了这个坑）。夹窗口只在「选中项刚变化」时由
     * settings_sidebar_clamp_sel() 主动做一次。 */
    g->items_scroll = g_settings_sidebar_scroll;
    /* 窗口开不了这么大时按条目数收窄：右侧/下方的 [+] [P] [A]… 必须紧贴窗口末行，
     * 否则条目少于窗口容量时侧栏会空出一段（v2.1.2 一度写成用未收窄的 cap ⇒ 2 项时
     * [+] 掉到第 17 行）。 */
    if (g->items_cap > item_count) g->items_cap = item_count;
    if (cap > item_count) cap = item_count;
    g->items = g->items_row0 + cap;
    int nav0 = g->items + (g->nav_tight ? 1 : 2);   /* [M] 那一行；非窄档再让出上一版 [P] 的一行 */
    g->app = nav0; g->keys = nav0 + 1; g->beh = nav0 + 2; g->pane = nav0 + 3;
    g->save = host_rows;
}

/* 侧栏列表里当前该被看见的那一行：1..item_count = 第 n 个菜单项，0 = 无选中。
 * 「启动 (Startup)」页的 ▶ 在右侧表格里（g_settings_table_sel），夹窗口时也要跟着它，
 * 否则右侧换行、侧栏不动，第 4/5 项就永远看不到。 */
int settings_sidebar_sel_natural(void) {
    /* 只认「第 n 个菜单项的详情页」（1..N）；100+ 的页号（含 104 条目管理）不算选中项。 */
    if (g_settings_nav >= 1 && g_settings_nav <= g_chooser_item_count) return g_settings_nav;
    return 0;
}

/* v2.1.3：右侧窗格（启动项表格 / 外观 / 键位 / 行为 / 详细配置）过去一律按「可见带 =
 * 3..host_rows」算行窗，完全不看左侧栏底部那几行固定入口（[+] [P] [A][K][B][W] 与末行
 * [Ctrl+S]）。条目一多、终端一窄，右侧第 15 行就会画到侧栏 [A] 那一行上 —— 40 列实测
 * 左右两栏互相盖住、分隔线 │ 整段消失（用户报「40 列时左右窗格互盖」）。
 * 这里给出右侧窗格真正的底行：让开侧栏最后三行（[B] [W] 与保存行之间那行还能画分隔线）。 */
/* 启动项页第 5 行的两个单选项：返回它们在「行内」的实际占位。
 * 段宽 = 标签本身的显示列数（" [●] 默认终端 (Terminal) " = 18 / " [○] 内置帮助 (Help) " = 15），
 * 是否画出来则取决于右栏行宽（host_cols - main_left，与 settings_line_begin 同口径）。 */
void settings_startup_radio_spans(int host_cols, int main_left,
                                  int *opt0_on, int *opt0_w, int *opt1_on, int *opt1_w) {
    static const char *L0 = " [●] 默认终端 (Terminal) ";
    static const char *L1 = " [○] 内置帮助 (Help) ";
    int w0 = utf8_cols(L0, (int)strlen(L0)), w1 = utf8_cols(L1, (int)strlen(L1));
    int row_w = host_cols - main_left - 1;    /* 末列留给折行余量，与 settings_line_begin 一致 */
    if (opt0_w) *opt0_w = w0;
    if (opt1_w) *opt1_w = w1;
    if (opt0_on) *opt0_on = (row_w >= w0) ? 1 : 0;
    if (opt1_on) *opt1_on = (*opt0_on && row_w >= w0 + 3 + w1) ? 1 : 0;
}

#define SETTINGS_RICH_COLS 121           /* v2.1.4：算「富布局要多宽」时的假定终端宽（与 settings_canvas_w 的上限同口径） */
#define SETTINGS_WIDE_COLS 60             /* v2.1.4：宏上移到本文件靠前处（横滚滚动条要用同一口径） */
static int menu_pre_w(int host_cols, int main_left);   /* v2.1.4：定义在后面，横滚算画布宽要先用 */
static int settings_right_row_limit(int host_rows, int host_cols, int main_left, int row);
static int settings_mark_left(int host_rows, int right_col, int row, int tl);   /* v2.1.4 */
/* v2.1.4：>0 时 menu_geom 改用这个「视口可用列数」，并把按钮钉到它的右端。
 * 只在 settings_menu_table_geom() 算窄视口那一档时临时置位。 */
static int g_menu_narrow_av = 0;
/* 横滚那一轮的「行首」宽度仍按画布口径给（行首在横滚下是钉住的，不该跟着视口变档）。 */
static int g_menu_narrow_pre = -1;
static void menu_geom(int host_cols, int main_left, int *name_w, int *cmd_w,
                      int *btn_col, int *show_ud, int *show_btn);
static SettingsTip g_tips[SETTINGS_TIP_MAX];
static int g_tip_n;

/* ---- v2.1.4：右栏横向滚动（太窄时表格的名称列/命令行列被裁掉 ⇒ 可以左右滚）----
 * 只有「横向真的藏了内容」的地方才滚：启动项页与条目管理页的表格（名称列 + 命令行列 +
 * 按钮按虚拟画布宽度排版，视口只画其中一段）。详情页的输入框自己有
 * render_scrollable_input() 的光标跟随窗口，说明行截断后有悬停气泡，都不需要整页横滚。 */
int settings_hscroll_slot(int nav) {
    switch (nav) {
        case SETTINGS_NAV_STARTUP: return 0;
        case SETTINGS_NAV_ITEMS: return 1;
        case SETTINGS_NAV_APPEARANCE: return 2;
        case SETTINGS_NAV_KEYS: return 3;
        case SETTINGS_NAV_BEHAVIOR: return 4;
        default: return 5;                      /* 窗格页 / 各菜单项详情页 */
    }
}
int settings_hscroll(void) {
    int v = g_settings_hscroll[settings_hscroll_slot(g_settings_nav)];
    return v > 0 ? v : 0;
}
/* 虚拟画布宽度：视口放不下整张表时，按这个宽度排版，视口只画 [h, h+vw) 那一段。
 * 只有 6 列以上的差距才值得开横滚（否则滚两下就到底，白白让行变短）。 */
int settings_canvas_w(int host_cols, int main_left) {
    int vw = host_cols - main_left - 1;
    if (vw < 1) vw = 1;
    int pre = menu_pre_w(host_cols, main_left);
    /* 只在「表格真的被裁掉东西」时开横滚：宽终端（>= SETTINGS_WIDE_COLS）本来就按
     * host_cols 排满，横滚不会多带一个字 ⇒ 画布 = 视口，80/100/120 列的排版与
     * v2.1.3 逐字节相同；窄终端（40~56 列）行名被裁成「日…」，才把画布撑到富布局
     * 宽度（名称 12 + 命令行 30 + [↑][↓][改][删] 16），被挤掉的列用 Shift+滚轮看。 */
    if (host_cols >= SETTINGS_WIDE_COLS) return vw;
    int nw = 0, cw = 0, bc = 0, ud = 0, btn = 0;
    menu_geom(SETTINGS_RICH_COLS, main_left, &nw, &cw, &bc, &ud, &btn);
    (void)bc;
    /* 富布局 = 行首 + 名称 + 命令行 + 按钮，一列都不裁。窄终端画布就按它撑开（被挤掉的
     * 列用 Shift+滚轮看）；连富布局都排不出来（名称/命令行/按钮被 menu_geom 丢了）时
     * 退回视口宽 —— 有多少画多少，不开横滚。 */
    if (!btn || !cw || !nw) return vw;
    int rich = pre + nw + 2 + cw + 2 + (10 + (ud ? 6 : 0));
    if (rich > 120) rich = 120;
    return (rich > vw) ? rich : vw;
}
void settings_hscroll_clamp(int host_rows, int host_cols, int main_left, int content_w) {
    (void)host_rows;
    int *v = &g_settings_hscroll[settings_hscroll_slot(g_settings_nav)];
    int vw = host_cols - main_left - 1;
    if (vw < 1) vw = 1;
    int max = content_w - vw;
    if (max < 0) max = 0;
    if (*v > max) *v = max;
    if (*v < 0) *v = 0;
}
/* 聚焦自动滚：把 [col, col+w) 这段滚进视口。 */
void settings_hscroll_reveal(int host_rows, int host_cols, int main_left, int content_w,
                             int col, int w) {
    int *v = &g_settings_hscroll[settings_hscroll_slot(g_settings_nav)];
    int vw = host_cols - main_left - 1;
    if (vw < 1) vw = 1;
    if (col < *v) *v = col;
    if (col + w > *v + vw) *v = col + w - vw;
    settings_hscroll_clamp(host_rows, host_cols, main_left, content_w);
}
/* 表格几何（虚拟画布口径）：启动项页与条目管理页共用，命中判定同源。 */
void settings_menu_table_geom(int host_rows, int host_cols, int main_left,
                              int *cw_out, int *name_w, int *cmd_w, int *btn_col,
                              int *show_ud, int *show_btn) {
    (void)host_rows;
    int h_on = settings_menu_table_calc(host_rows, host_cols, main_left,
                                       name_w, cmd_w, btn_col, show_ud, show_btn);
    if (cw_out) *cw_out = settings_canvas_w(host_cols, main_left);
    (void)h_on;
}

/* 单一口径：返回 1 = 这张表开了横向滚动（画布比视口宽，列宽按画布算、按钮钉视口右端）；
 * 返回 0 = 没开（列宽就按 host_cols 那一档，与 v2.1.3 完全一致）。 */
int settings_menu_table_calc(int host_rows, int host_cols, int main_left,
                             int *name_w, int *cmd_w, int *btn_col,
                             int *show_ud, int *show_btn) {
    (void)host_rows;
    int vw = host_cols - main_left - 1;
    if (vw < 1) vw = 1;
    int cw = settings_canvas_w(host_cols, main_left);
    if (cw <= vw) {
        menu_geom(host_cols, main_left, name_w, cmd_w, btn_col, show_ud, show_btn);
        return 0;
    }
    /* 没开横滚（画布 = 视口）时，行宽按视口那一档算就是最终宽度。 */
    /* 先按【画布】定住列的定义（名称列 / 命令行列多宽），再按【视口】把按钮挪到右端 ——
     * 两轮之间列的定义不能各算各的，否则表头和表体对不齐。 */
    menu_geom(cw + main_left + 1, main_left, name_w, cmd_w, btn_col, show_ud, show_btn);
    g_menu_narrow_av = vw;
    g_menu_narrow_pre = menu_pre_w(cw + main_left + 1, main_left);
    menu_geom(main_left + 1 + vw, main_left, name_w, cmd_w, btn_col, show_ud, show_btn);
    g_menu_narrow_av = 0;
    g_menu_narrow_pre = -1;
    return 1;
}

/* 横向滚动条：画在表格行右端（与纵向的 (a-b/total) 同一条线，只是换成 « »）。 */
/* 启动项页：聚焦行的名称列滚进视口（只管左侧名称；按钮列仍由 Shift+滚轮去找，
 * 否则一进来就被拽到最右边，反而看不到名字）。纵向的自动跟随在 settings_page_row 里。 */
static void settings_startup_hfollow(int host_rows) {
    /* 「自动滚到聚焦位置」：只在【选中行刚变化】的那一次动手，且只把聚焦行的行首
     * （序号 + 名称）拽进视口 —— 聚焦已经看得见就什么都不做。
     * 每帧都夹会把用户 Shift+滚轮滚出来的位置一次次拽回去（v2.1.4 第一版就这样）。 */
    static int last_sel = -999, last_rows = -1, last_cols = -1;
    int changed = (last_sel != g_settings_table_sel || last_rows != host_rows
                   || last_cols != g_mux.host_cols);
    last_sel = g_settings_table_sel; last_rows = host_rows; last_cols = g_mux.host_cols;
    if (!changed) return;
    int sbw = settings_host_sidebar_w(g_mux.host_cols);
    int main_left = sbw + 3;
    int nw = 0, nc = 0, nb = 0, nu = 0, nt = 0;
    if (!settings_menu_table_calc(g_mux.host_rows, g_mux.host_cols, main_left,
                                  &nw, &nc, &nb, &nu, &nt)) return;
    int cw = settings_canvas_w(g_mux.host_cols, main_left);
    int *v = &g_settings_hscroll[settings_hscroll_slot(g_settings_nav)];
    if (*v <= 0) return;          /* 一开始就停在最左：序号/名称本来就看得见，不拽 */
    int pre = settings_menu_pre_w(cw + main_left + 1, main_left);
    if (nw < 1) nw = 1;
    /* 只在聚焦行首真的掉出视口左端时才滚回来。 */
    if (pre - 1 < *v) *v = pre - 1;
    settings_hscroll_clamp(host_rows, g_mux.host_cols, main_left, cw);
}

void settings_hscroll_mark(char *out, int bs, int *posp, int row, int right_col,
                           int h, int vw, int content_w, int host_rows) {
    if (content_w <= vw) return;
    char tag[40];
    if (h <= 0) snprintf(tag, sizeof(tag), "\xc2\xbb\xc2\xbb\xc2\xbb");
    else if (h >= content_w - vw) snprintf(tag, sizeof(tag), "\xc2\xab\xc2\xab\xc2\xab");
    else snprintf(tag, sizeof(tag), "\xc2\xab%d/%d\xc2\xbb", h + 1, content_w - vw + 1);
    int tl = (int)strlen(tag);
    int mleft = settings_mark_left(host_rows, right_col, row, tl);  /* 与预留算法同源（v2.1.4） */
    if (!mleft) return;
    int pos = *posp;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;110;118;129m%s\x1b[0m",
                    row, mleft, tag);
    *posp = pos;
}

void settings_hscroll_by(int delta) {
    int *v = &g_settings_hscroll[settings_hscroll_slot(g_settings_nav)];
    *v += delta;
    if (*v < 0) *v = 0;
    g_mux.needs_redraw = 1;
}
/* Shift+滚轮：右栏横向滚动。只在「横向真的藏了内容」的页（启动项表 / 条目管理表）生效，
 * 别的页没有可滚的东西，直接不做，免得用户以为坏了。 */
void settings_h_wheel(int delta) {
    int nav = g_settings_nav;
    if (nav != SETTINGS_NAV_STARTUP && nav != SETTINGS_NAV_ITEMS) return;
    settings_hscroll_by((delta > 0 ? -3 : 3));
}
/* 聚焦行变了（滚轮 / ↑↓ / 点击）⇒ 把聚焦行滚进视口。 */
void settings_hscroll_follow(int host_rows, int host_cols, int main_left,
                             int col, int width) {
    int cw = settings_canvas_w(host_cols, main_left);
    if (cw <= host_cols - main_left - 1) return;
    settings_hscroll_reveal(host_rows, host_cols, main_left, cw, col, width);
}

/* 按显示列把文本裁到 maxw 列（放不下就补「...」）。maxw<=0 ⇒ 输出空串。 */
static void fit_cols(char *dst, int dstbs, const char *src, int maxw) {
    if (dstbs <= 0) return;
    dst[0] = 0;
    if (maxw <= 0) return;
    int len = (int)strlen(src), i = 0, used = 0, k = 0;
    while (i < len) {
        int adv = 0;
        unsigned cp = utf8_decode_cp(src + i, len - i, &adv);
        if (adv <= 0) break;
        int w = is_zero_width_cp(cp) ? 0 : (is_wide_cp(cp) ? 2 : 1);
        if (used + w > maxw) break;
        if (k + adv >= dstbs - 4) break;
        for (int q = 0; q < adv; q++) dst[k++] = src[i + q];
        used += w; i += adv;
    }
    dst[k] = 0;
    if (i < len) {                                  /* 还有剩 ⇒ 收三列给省略号 */
        while (k > 0 && used > maxw - 3) {
            int b = k - 1, adv2 = 0;
            while (b > 0 && ((unsigned char)dst[b] & 0xC0) == 0x80) b--;
            unsigned cp2 = utf8_decode_cp(dst + b, k - b, &adv2);
            int cw = is_zero_width_cp(cp2) ? 0 : (is_wide_cp(cp2) ? 2 : 1);
            k = b; used -= cw;
        }
        snprintf(dst + k, dstbs - k, "%s", "...");
    }
}

/* 取 src 的第 [from,to) 个显示列（转义序列整段保留；落在宽字符中间就整字丢掉）。
 * 注意：按【内容列】计数，SGR 之类的转义序列不占列、也不许被切成半截
 * （半截转义会把 "[0m" 当正文画出来）。 */
static void hcut(const char *src, char *dst, int dstbs, int from, int to) {
    if (dstbs <= 0) return;
    dst[0] = 0;
    int len = (int)strlen(src), i = 0, used = 0, k = 0;
    while (i < len) {
        if (src[i] == '\x1b') {
            int e = i + 1;
            if (e < len && src[e] == '[') {
                e++;
                while (e < len && !(src[e] >= 'm' && src[e] <= '~')) e++;
                if (e < len) e++;
            } else { e = i + 1; }
            if (used > from && used < to) {          /* 只在可见段内保留样式 */
                int n = e - i;
                if (k + n < dstbs - 1) { for (int q = 0; q < n; q++) dst[k++] = src[i + q]; }
            }
            i = e;
            continue;
        }
        int adv = 0;
        unsigned cp = utf8_decode_cp(src + i, len - i, &adv);
        if (adv <= 0) { i++; continue; }
        int w = is_zero_width_cp(cp) ? 0 : (is_wide_cp(cp) ? 2 : 1);
        int nc = used + w;
        if (used >= to) break;
        if (used >= from && nc <= to) {
            if (k + adv < dstbs - 1) { for (int q = 0; q < adv; q++) dst[k++] = src[i + q]; }
        }
        used = nc; i += adv;
    }
    dst[k] = 0;
    int rest = dstbs - k - 1;
    if (rest > 0 && to - from > (to < used ? to : used)) { }
}


/* 横向可滚的一行：段按【画布列】排到 content_w，再按 hscroll 取视口那一段画出来。
 * 装不下的部分交给行尾的「...」+ 悬停气泡（沿用 settings_tip_*）。 */
#define SETTINGS_HMARK_W 10      /* v2.1.4：右端 «n/m» 横向滚动标记占的列数（含 1 列间隔） */
/* 行末那一截标记（纵向 (a-b/N) 或横向 «n/m»）从第几列开始写（1 基终端列）；放不下返回 0。
 * 写标记的两支函数与「正文先让几列」的算法必须共用这里，否则标记会压在正文最后一个宽字符的
 * 右半格上：vterm 会把那个宽字符挤成半角 ⇒ 整行显示宽度反而多出 1 列（32/56/58 列实测）。 */
static int settings_mark_left(int host_rows, int right_col, int row, int tl) {
    if (right_col < SETTINGS_WIDE_COLS) {
        int ml = settings_host_sidebar_w(right_col) + 3;
        int lim = settings_right_row_limit(host_rows, right_col, ml, row);
        if (lim < right_col - ml && lim > 0) right_col = ml - 1 + lim;
    }
    if (right_col - tl < 1) return 0;
    return right_col - tl + 1;
}
void settings_hline2(char *out, int bs, int *posp, int row, int main_left, int host_cols,
                     int content_w, const SettingsSeg *seg, int nseg, int tip_row_len,
                     int lpin, int rpin) {
    if (row < 0 || nseg <= 0) return;
    int vw = host_cols - main_left - 1;            /* 视口里能写的列数（末列留给分隔线） */
    if (vw < 1) return;
    if (content_w < vw) content_w = vw;
    if (lpin + rpin > vw - 1) { lpin = 0; rpin = 0; }   /* 两端钉不住就整体滚 */
    char full[1024];
    int pos = 0, col = 0;
    full[0] = 0;
    for (int i = 0; i < nseg; i++) {
        if (!seg[i].sgr) continue;                 /* 空段 = 占位（例如丢了按钮） */
        pos += snprintf(full + pos, sizeof(full) - pos, "%s", seg[i].sgr);
        int before = col;
        if (seg[i].text) pos += snprintf(full + pos, sizeof(full) - pos, "%s", seg[i].text);
        col = before + utf8_cols(seg[i].text ? seg[i].text : "",
                                 seg[i].text ? (int)strlen(seg[i].text) : 0);
        if (seg[i].pad_to_col > col && pos < (int)sizeof(full) - 8) {
            while (col < seg[i].pad_to_col && pos < (int)sizeof(full) - 2) { full[pos++] = ' '; col++; }
        }
    }
    full[pos] = 0;
    if (col > content_w) content_w = col;          /* 实际更宽就按实际的算 */
    int mid = vw - lpin - rpin;
    int h = settings_hscroll();
    if (mid > 0) { if (h > content_w - vw + rpin) h = content_w - vw + rpin; }
    else { if (h > content_w - vw) h = content_w - vw; }
    if (h < 0) h = 0;
    /* 视口 = [钉住的左端][滚动的中段][钉住的右端] */
    char left[256], middle[1024], right[256], vis[1560];
    left[0] = middle[0] = right[0] = 0;
    if (lpin > 0) hcut(full, left, sizeof(left), 0, lpin);
    if (mid > 0) hcut(full, middle, sizeof(middle), lpin + h, vw - rpin);
    if (rpin > 0) hcut(full, right, sizeof(right), vw - rpin, vw);   /* 右端已按视口列排布 */
    /* 三段拼成一行（vis 比三段之和还宽一点 ⇒ 不会触发 -Wformat-truncation）。 */
    vis[0] = 0;
    {
        int vp = 0;
        const char *parts[3] = { left, middle, right };
        for (int pi = 0; pi < 3; pi++) {
            int n = (int)strlen(parts[pi]);
            if (vp + n >= (int)sizeof(vis) - 1) break;
            memcpy(vis + vp, parts[pi], (size_t)n); vp += n;
        }
        vis[vp] = 0;
    }
    *posp += snprintf(out + *posp, bs - *posp, "\x1b[%d;%dH%s\x1b[0m", row, main_left, vis);
    if (col > vw && tip_row_len > 0 && g_tip_n < SETTINGS_TIP_MAX) {
        SettingsTip *t = &g_tips[g_tip_n++];
        t->row = row; t->col = main_left; t->len = tip_row_len;
        char plain[SETTINGS_TIP_TEXT];
        int k2 = 0;
        for (int i = 0; i < nseg && k2 < (int)sizeof(plain) - 8; i++) {
            if (!seg[i].text) continue;
            for (int q = 0; seg[i].text[q] && k2 < (int)sizeof(plain) - 8; q++) plain[k2++] = seg[i].text[q];
            plain[k2++] = ' ';
        }
        plain[k2] = 0;
        snprintf(t->full, sizeof(t->full), "%s", plain);
    }
}

/* 右侧窗格【逐行】限宽：把「本行若一直写到右边界会不会踩到左侧栏」折算成可用列数上限。
 * 返回 >=1 = 本行最多写这么多列；返回 <=0 = 一列都塞不下（整行不画）。
 *
 * v2.1.3 的教训：第一版是去夹右侧窗格「可见带的底行」（行窗整体抬高），结果把
 * tests/verify_pane_palette.py 的 G/I/K2/K3/M1/M3 六条打翻了 —— 那些断言要求 12 行
 * 终端滚到底仍能看见 background 行 / 最后一个键位动作，行窗一夹就没那么多行可滚。
 * 现在行窗、滚动量、可见条目数全部保持 v2.1.2 的算法，只在「窄到两栏会在同一行相交、
 * 且这一行确实是侧栏底部入口 [A][K][B][W]/保存行 占用的行」时，把该行可用宽度收到
 * 分隔线之前（塞不下就整行不画）。宽终端（host_cols >= SETTINGS_WIDE_COLS）一律不限，
 * ⇒ 80/100/120 列基准零改动。 */
/* SETTINGS_WIDE_COLS 已上移到本文件靠前处（v2.1.4） */
int settings_right_row_limit(int host_rows, int host_cols, int main_left, int row) {
    int full = host_cols - main_left - 1;      /* 与 settings_line_begin 同一口径（留 1 列防折行） */
    if (row <= 0 || host_cols >= SETTINGS_WIDE_COLS) return full;
    SettingsSidebarGeom g;
    settings_sidebar_geom(host_rows, g_chooser_item_count, &g);

    if (row < g.app) return full;               /* 还没到侧栏底部那一排 */
    int room = main_left - 3;                   /* │ 在 main_left-3 列 ⇒ 正文最多写到它之前 */
    return room < full ? room : full;
}

void settings_sidebar_extra_rows(int *appearance_r, int *keys_r, int *behavior_r) {
    SettingsSidebarGeom g;
    settings_sidebar_geom(g_mux.host_rows, g_chooser_item_count, &g);
    if (appearance_r) *appearance_r = g.app;
    if (keys_r) *keys_r = g.keys;
    if (behavior_r) *behavior_r = g.beh;
}

/* ---- 页内纵向滚动（渲染与命中判定同源）----
 * natural = 高终端下的行号；可见带为 [first, host_rows]，滚动量 *scroll 由本页持有，
 * 并被夹到「选中项所在行始终可见」。返回 -1 = 该行不在可见区（调用方跳过绘制）。 */
int settings_page_row(int host_rows, int natural, int first, int last, int *scroll, int sel_natural) {
    int vis = host_rows - first + 1;
    if (vis < 3) vis = 3;
    if (vis > last - first + 1) vis = last - first + 1;
    int maxs = (last - first + 1) - vis;
    if (maxs < 0) maxs = 0;
    if (sel_natural >= first && sel_natural <= last) {
        if (*scroll > sel_natural - first) *scroll = sel_natural - first;
        if (*scroll < sel_natural - first - vis + 1) *scroll = sel_natural - first - vis + 1;
    }
    if (*scroll > maxs) *scroll = maxs;
    if (*scroll < 0) *scroll = 0;
    if (natural < first || natural > last) return -1;
    int r = natural - *scroll;
    if (r < first || r > first + vis - 1) return -1;
    return r;
}

/* 反查：屏幕行 -> 自然行。先按「选中项始终可见」把滚动量夹一遍，保证 input 的命中
 * 判定与上一帧渲染用的是同一个滚动量（否则点一下会偏一行）。 */
int settings_page_natural_at(int host_rows, int row, int first, int last, int *scroll, int sel_natural) {
    settings_page_row(host_rows, first, first, last, scroll, sel_natural);
    int vis = host_rows - first + 1;
    if (vis < 3) vis = 3;
    if (vis > last - first + 1) vis = last - first + 1;
    if (row < first || row > first + vis - 1) return -1;
    int natural = row + *scroll;
    if (natural < first || natural > last) return -1;
    return natural;
}

void settings_scroll_mark(char *out, int bs, int *posp, int row, int right_col,
                          int first_vis, int last_vis, int total, int host_rows) {
    if (total <= last_vis - first_vis + 1 && first_vis <= 1) return;   /* 没滚动就不标 */
    char tag[32];
    snprintf(tag, sizeof(tag), "(%d-%d/%d)", first_vis, last_vis, total);
    int tl = (int)strlen(tag);
    /* v2.1.3：标记原先一律贴着 host_cols 写。窄终端（右侧正文与左侧栏会在同一行
     * 相交）⇒ 这一段直接踩在侧栏/分隔线上（14x30 实测：`(3-5/12)` 把 col 28..36
     * 的侧栏字符盖掉，并把 `)` 挤出屏幕触发折行）。收到侧栏之前。 */
    int mleft = settings_mark_left(host_rows, right_col, row, tl);  /* 与预留算法同源（v2.1.4） */
    if (!mleft) return;
    int pos = *posp;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;110;118;129m%s\x1b[0m",
                    row, mleft, tag);
    *posp = pos;
}

/* ---- v2.1.1：十六进制颜色编辑浮层（几何在文件后段，画框需要 append_swatch 等）----
 * 用户反馈「过窄时，颜色编辑被截断」：编辑框原先嵌在表行里，值段起点是
 * col + SETTINGS_PANE_VALUE_OFF - 1（窗格页）或 role_col + 21（外观页右列），
 * 终端一窄就整段跑到屏幕外——看不见色块，也看不见自己敲到哪一位。
 * 改成居中浮层：色块 + 完整 6 位 + 提示，宽度按需收缩，任何尺寸都不截断。 */
void hex_edit_popup_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h);
static int hex_edit_popup_w(int host_cols);
int settings_host_sidebar_w(int host_cols);

/* 浮层是否会盖在这一行上：会 ⇒ 表行不再重复画编辑框（避免同一个值出现两份）。
 * 另一个浮层（预设 / 方案列表）打开时 hex 编辑其实已被打断，此时仍画表行内的框。 */
static int hex_edit_popup_shown(int host_rows, int host_cols) {
    if (!g_hex_edit_active || g_settings_show_presets || g_settings_show_pane_schemes) return 0;
    if (hex_edit_popup_w(host_cols) < HEX_EDIT_POPUP_MIN || host_rows < 3) return 0;
    return 1;
}

/* 提示行右端的位置指示：仅当本页确实需要滚动时画。 */
/* v2.1.4：提示行右端那一截 (a-b/N) 是【另一支函数】贴着右端补上去的。窄终端上提示文字
 * 会被裁到行尾 ⇒ 标记正好压在最后一个【宽字符的右半格】上（56 列实测：「择」被挤成半角，
 * 整行显示宽度反而多出 1 列 ⇒ O/P 组判据红）。所以先按 settings_page_mark 的同一判据问一句
 * 「这一行会不会画标记」，会画就把它占的列从预算里扣掉。 */
int settings_mark_room(int host_rows, int host_cols, int main_left, int row,
                       int first, int last) {
    int tot = last - first + 1;
    int vis = host_rows - first + 1;
    if (vis < 3) vis = 3;
    if (vis > tot) vis = tot;
    if (tot <= vis) return 0;                       /* 页短到不用滚 ⇒ 不画标记 */
    (void)main_left;
    int mleft = settings_mark_left(host_rows, host_cols, row, 9);   /* "(3-23/24)" = 9 列 */
    if (!mleft) return 0;                           /* 连标记都放不下 ⇒ 它不会画 */
    return host_cols - mleft + 1;                   /* 标记宽度 + 1 列间隔 */
}
void settings_page_mark(char *out, int bs, int *posp, int row, int right_col, int host_rows,
                        int first, int last, int *scroll) {
    int tot = last - first + 1;
    int vis = host_rows - first + 1;
    if (vis < 3) vis = 3;
    if (vis > tot) vis = tot;
    if (tot <= vis) return;
    int top = first + *scroll;
    if (top + vis - 1 > last) top = last - vis + 1;
    if (top < first) top = first;
    settings_scroll_mark(out, bs, posp, row, right_col, top, top + vis - 1, tot, host_rows);
}

/* 外观页：自然行 3..22（标题/提示/5 个主题/语义色标题+说明/16 色两列 8 行/提示） */
#define SETTINGS_APPEAR_FIRST 3
#define SETTINGS_APPEAR_LAST  (SETTINGS_ROLE_ROW0 + SETTINGS_ROLE_ROWS + 1)
int settings_appearance_row_view(int host_rows, int natural) {
    return settings_page_row(host_rows, natural, SETTINGS_APPEAR_FIRST,
                             SETTINGS_APPEAR_LAST, &g_settings_appear_scroll,
                             settings_appearance_sel_natural(host_rows));
}
int settings_appearance_sel_natural(int host_rows) {
    (void)host_rows;
    int tc = theme_count();
    int sel = g_settings_theme_sel;
    if (sel < 0) return -1;
    if (sel < tc) return settings_theme_row(sel);
    if (sel < tc + TH_ROLE_COUNT) return settings_role_row(sel - tc);
    return -1;
}
int settings_appearance_natural_at(int host_rows, int row) {
    return settings_page_natural_at(host_rows, row, SETTINGS_APPEAR_FIRST,
                                    SETTINGS_APPEAR_LAST, &g_settings_appear_scroll,
                                    settings_appearance_sel_natural(host_rows));
}
/* 行为页：自然行 3..14（标题/提示/5 开关/scrollback/提示） */
#define SETTINGS_BEHAVIOR_LAST (SETTINGS_BEHAVIOR_ROW0 + SETTINGS_BEHAVIOR_TOGGLES + 3)
int settings_behavior_row_view(int host_rows, int natural) {
    return settings_page_row(host_rows, natural, 3, SETTINGS_BEHAVIOR_LAST,
                             &g_settings_behavior_scroll, settings_behavior_sel_natural(host_rows));
}
int settings_behavior_sel_natural(int host_rows) {
    (void)host_rows;
    if (g_settings_behavior_sel < 0) return -1;
    return SETTINGS_BEHAVIOR_ROW0 + g_settings_behavior_sel;
}
int settings_behavior_natural_at(int host_rows, int row) {
    return settings_page_natural_at(host_rows, row, 3, SETTINGS_BEHAVIOR_LAST,
                                     &g_settings_behavior_scroll, settings_behavior_sel_natural(host_rows));
}
/* 菜单项详细配置页：自然行 3..19（标题 + 4 组「标签+输入框」+ 颜色 + 按钮 + 提示） */
#define SETTINGS_DETAIL_FIRST 3
#define SETTINGS_DETAIL_LAST  19
int settings_detail_row_view(int host_rows, int natural) {
    int sel = -1;
    if (g_settings_field >= 0 && g_settings_field <= 3) sel = 5 + g_settings_field * 2;
    return settings_page_row(host_rows, natural, SETTINGS_DETAIL_FIRST,
                             SETTINGS_DETAIL_LAST, &g_settings_detail_scroll, sel);
}
/* 默认启动项页：自然行 3..(10+n+2)（标题/提示/两个选项/管理标题+说明/表头/n 行/按钮/提示） */
static int settings_startup_last(int host_rows) {
    (void)host_rows;
    return 12 + (g_chooser_item_count > 0 ? g_chooser_item_count : 1);
}
int settings_manage_row_view(int host_rows, int natural) {
    int sel = (g_settings_table_sel >= 0) ? 10 + g_settings_table_sel : -1;
    /* 横向：滚过界就夹回来（这一页没有「▶ 跟随」的必要——表很短）。 */
    int sbw = settings_host_sidebar_w(g_mux.host_cols);
    settings_hscroll_clamp(host_rows, g_mux.host_cols, sbw + 3,
                           settings_canvas_w(g_mux.host_cols, sbw + 3));   /* 与渲染同一个画布口径 */
    int last = SETTINGS_MANAGE_LAST;
    int total = 12 + g_chooser_item_count;         /* 到提示行为止 */
    if (total < last) last = total;
    int vis = host_rows - SETTINGS_MANAGE_FIRST + 1;
    if (vis > last - SETTINGS_MANAGE_FIRST + 1) vis = last - SETTINGS_MANAGE_FIRST + 1;
    int maxs = (last - SETTINGS_MANAGE_FIRST + 1) - vis;
    if (maxs < 0) maxs = 0;
    if (sel >= SETTINGS_MANAGE_FIRST && sel <= last) {
        if (g_settings_manage_scroll > sel - SETTINGS_MANAGE_FIRST)
            g_settings_manage_scroll = sel - SETTINGS_MANAGE_FIRST;
        if (g_settings_manage_scroll < sel - SETTINGS_MANAGE_FIRST - vis + 1)
            g_settings_manage_scroll = sel - SETTINGS_MANAGE_FIRST - vis + 1;
    }
    if (g_settings_manage_scroll > maxs) g_settings_manage_scroll = maxs;
    if (g_settings_manage_scroll < 0) g_settings_manage_scroll = 0;
    if (natural < SETTINGS_MANAGE_FIRST || natural > last) return -1;
    int r = natural - g_settings_manage_scroll;
    if (r < SETTINGS_MANAGE_FIRST || r > SETTINGS_MANAGE_FIRST + vis - 1) return -1;
    return r;
}

int settings_manage_natural_at(int host_rows, int row) {
    /* 与 settings_manage_row_view 互逆：屏幕行 → 自然行（命中判定用）。 */
    int vis = host_rows - SETTINGS_MANAGE_FIRST + 1;
    int total = 12 + g_chooser_item_count;
    int last = (total < SETTINGS_MANAGE_LAST) ? total : SETTINGS_MANAGE_LAST;
    if (vis > last - SETTINGS_MANAGE_FIRST + 1) vis = last - SETTINGS_MANAGE_FIRST + 1;
    if (row < SETTINGS_MANAGE_FIRST || row > SETTINGS_MANAGE_FIRST + vis - 1) return -1;
    int nat = row + g_settings_manage_scroll;
    if (nat < SETTINGS_MANAGE_FIRST || nat > last) return -1;
    return nat;
}

int settings_startup_row_view(int host_rows, int natural) {
    int sel = (g_settings_table_sel >= 0) ? 10 + g_settings_table_sel : -1;
    settings_startup_hfollow(host_rows);      /* v2.1.4：聚焦行横向自动滚进视口（只在选中变化时动） */
    return settings_page_row(host_rows, natural, 3, settings_startup_last(host_rows),
                             &g_settings_startup_scroll, sel);
}
int settings_startup_natural_at(int host_rows, int row) {
    return settings_page_natural_at(host_rows, row, 3, settings_startup_last(host_rows),
                                     &g_settings_startup_scroll,
                                     (g_settings_table_sel >= 0) ? 10 + g_settings_table_sel : -1);
}
int settings_detail_natural_at(int host_rows, int row) {
    int sel = -1;
    if (g_settings_field >= 0 && g_settings_field <= 3) sel = 5 + g_settings_field * 2;
    return settings_page_natural_at(host_rows, row, SETTINGS_DETAIL_FIRST,
                                    SETTINGS_DETAIL_LAST, &g_settings_detail_scroll, sel);
}

/* 截断气泡：跟随鼠标的小浮层，最多 3 行、按显示宽度折行。 */
static void render_settings_tooltip(char *out, int bs, int *posp, int host_rows, int host_cols) {
    if (g_mouse_x < 0 || g_mouse_y < 0) return;
    if (g_hex_edit_active || g_key_capture_active || g_settings_show_presets || g_settings_show_pane_schemes)
        return;
    const SettingsTip *t = settings_tip_at(g_mouse_y + 1, g_mouse_x + 1);
    if (!t || !t->full[0]) return;

    /* v2.1.2：框宽按【折行后每行的实际显示列】算。原先直接拿登记的 len（侧栏那条登记
     * 记的是「从起点到行尾」的列数，比正文短）当宽度 ⇒ 框比正文窄，右边框连同剩下的
     * 字一起被顶到行尾之外。 */
    int fullw = utf8_cols(t->full, (int)strlen(t->full));
    int bw = fullw + 4;                         /* │ 空格 内容 空格 │ */
    if (bw > host_cols) bw = host_cols;
    int cap = bw - 4;
    if (cap < 6) { cap = 6; bw = cap + 4; if (bw > host_cols) return; }

    /* 折行：逐字累计显示宽度，一行放不下就换行；最多 3 行，末尾补「...」 */
    int slen = (int)strlen(t->full);
    int lstart[4], llen[4], nl = 0;
    int i = 0;
    while (i < slen && nl < 3) {
        lstart[nl] = i;
        int col = 0, j = i;
        while (j < slen) {
            int adv = 0;
            unsigned cp = utf8_decode_cp(t->full + j, slen - j, &adv);
            if (adv <= 0) break;
            if (is_zero_width_cp(cp)) { j += adv; continue; }
            int cw = is_wide_cp(cp) ? 2 : 1;
            if (col + cw > cap) break;
            /* 末行要留出「...」的位置 */
            if (i + 0 >= 0 && j + adv < slen && nl == 2 && col + cw + 3 > cap) break;
            j += adv; col += cw;
        }
        if (j == i) j = i + 1;                 /* 永不空转 */
        llen[nl] = j - i;
        i = j;
        nl++;
    }
    int clipped = (i < slen);
    int bh = nl + 2;
    {
        int maxline = 0;
        for (int k = 0; k < nl; k++) {
            int lw = utf8_cols(t->full + lstart[k], llen[k]);
            if (k == nl - 1 && clipped) lw += 3;          /* 末行还要放「...」 */
            if (lw > maxline) maxline = lw;
        }
        if (maxline > 0 && maxline + 4 < bw) bw = maxline + 4;
        cap = bw - 4;
    }

    int top = g_mouse_y + 2;                   /* 鼠标行的下一行 */
    if (top + bh - 1 > host_rows) top = (g_mouse_y + 1) - bh;   /* 放不下就翻到上方 */
    if (top < 2) top = 2;
    int left = g_mouse_x + 1;
    if (left + bw - 1 > host_cols) left = host_cols - bw + 1;
    if (left < 1) left = 1;
    /* v2.1.2：气泡不许盖住来触发它的那一行 —— 鼠标在侧栏时（用户就是在那儿看被截断的
     * 菜单项名），把气泡整体推到分隔线右侧的正文区去。 */
    int sbw = settings_host_sidebar_w(host_cols);
    if (g_mouse_x < sbw && left < sbw + 3) {
        left = sbw + 3;
        if (left + bw - 1 > host_cols) left = host_cols - bw + 1;
        if (left < 1) left = 1;
    }

    const char *BG = "\x1b[048;2;022;027;034m";   /* 与其它浮层同色：不引入新用色 */
    const char *BR = "\x1b[038;2;121;192;255m";
    int pos = *posp;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s\x1b[0m┌", top, left, BR);
    for (int c = 1; c < bw - 1 && pos < bs - 3; c++) { out[pos++]='\xe2'; out[pos++]='\x94'; out[pos++]='\x80'; }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");
    for (int k = 0; k < nl; k++) {
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s│\x1b[0m%s ", top + 1 + k, left, BR, BG);
        int used = utf8_cols(t->full + lstart[k], llen[k]);
        for (int b = 0; b < llen[k] && pos < bs - 8; b++) out[pos++] = t->full[lstart[k] + b];
        int pad = cap - used;
        if (clipped && k == nl - 1) pad -= 3;
        for (int c = 0; c < pad; c++) { if (pos < bs - 1) out[pos++] = ' '; }
        if (clipped && k == nl - 1) { out[pos++]='.'; out[pos++]='.'; out[pos++]='.'; }
        pos += snprintf(out + pos, bs - pos, " %s\x1b[0m%s│\x1b[0m", BG, BR);
    }
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s\x1b[0m└", top + bh - 1, left, BR);
    for (int c = 1; c < bw - 1 && pos < bs - 3; c++) { out[pos++]='\xe2'; out[pos++]='\x94'; out[pos++]='\x80'; }
    pos += snprintf(out + pos, bs - pos, "┘\x1b[0m");
    *posp = pos;
}

int settings_theme_row(int idx) { return SETTINGS_THEME_ROW0 + idx; }

int settings_role_row(int role) { return SETTINGS_ROLE_ROW0 + (role % SETTINGS_ROLE_ROWS); }

int settings_role_col(int main_left, int role) {
    return role < SETTINGS_ROLE_ROWS ? main_left : main_left + SETTINGS_ROLE_COL_W;
}

/* 窗格配色页的排布：左列 = 默认背景 / 默认前景 + 索引 0..6，右列 = 索引 7..15。
 * 最常改的两项放最上面。 */
static const int g_pane_order[THEME_PANE_SLOTS] = {
    THEME_PANE_BG, THEME_PANE_FG, THEME_PANE_SB_THUMB, THEME_PANE_SB_TRACK, 0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};
int settings_pane_order_slot(int pos) { return (pos >= 0 && pos < THEME_PANE_SLOTS) ? g_pane_order[pos] : -1; }
int settings_pane_order_pos(int slot) { for (int i = 0; i < THEME_PANE_SLOTS; i++) if (g_pane_order[i] == slot) return i; return -1; }
int settings_pane_two_cols(int host_cols, int main_left) { return host_cols - main_left + 1 >= 2 * SETTINGS_PANE_COL_W; }
int settings_pane_rows_per_col(int host_cols, int main_left) { return settings_pane_two_cols(host_cols, main_left) ? SETTINGS_PANE_ROWS : THEME_PANE_SLOTS; }
int settings_pane_visible_rows(int host_rows, int host_cols, int main_left) {
    int per = settings_pane_rows_per_col(host_cols, main_left);
    int vis = host_rows - 1 - SETTINGS_PANE_ROW0 - 1;   /* 底行留状态；提示行至少 1 行 */
    if (vis < 3) vis = 3;
    return vis < per ? vis : per;
}
void settings_pane_clamp_scroll(int host_rows, int host_cols, int main_left) {
    int per = settings_pane_rows_per_col(host_cols, main_left);
    int vis = settings_pane_visible_rows(host_rows, host_cols, main_left);
    int p = settings_pane_order_pos(g_settings_pane_sel);
    if (p >= 0) {
        int line = p % per;
        if (g_settings_pane_scroll > line) g_settings_pane_scroll = line;
        if (g_settings_pane_scroll < line - vis + 1) g_settings_pane_scroll = line - vis + 1;
    }
    if (g_settings_pane_scroll > per - vis) g_settings_pane_scroll = per - vis;
    if (g_settings_pane_scroll < 0) g_settings_pane_scroll = 0;
}
int settings_pane_row(int host_cols, int main_left, int slot) {
    int p = settings_pane_order_pos(slot);
    int line = (p % settings_pane_rows_per_col(host_cols, main_left)) - g_settings_pane_scroll;
    return line < 0 ? -1 : SETTINGS_PANE_ROW0 + line;
}
int settings_pane_col(int host_cols, int main_left, int slot) {
    int p = settings_pane_order_pos(slot);
    return p < settings_pane_rows_per_col(host_cols, main_left) ? main_left : main_left + SETTINGS_PANE_COL_W;
}
int settings_pane_hint_row(int host_rows, int host_cols, int main_left) {
    return SETTINGS_PANE_ROW0 + settings_pane_visible_rows(host_rows, host_cols, main_left) + 1;
}
int settings_sidebar_pane_row(void) { int a, k, b; settings_sidebar_extra_rows(&a, &k, &b); return b + 1; }

int settings_keys_rows(void) { return 1 + keymap_action_count(); }

int settings_keys_visible(int host_rows) {
    int vis = host_rows - SETTINGS_KEYS_ROW0 - 1;
    if (vis < 3) vis = 3;
    if (vis > settings_keys_rows()) vis = settings_keys_rows();
    return vis;
}

/* 让选中行始终留在可视窗口里 */
static void settings_keys_clamp_scroll(int host_rows) {
    int vis = settings_keys_visible(host_rows);
    int total = settings_keys_rows();
    if (g_settings_keys_sel < 0) g_settings_keys_sel = 0;
    if (g_settings_keys_sel >= total) g_settings_keys_sel = total - 1;
    if (g_settings_keys_scroll > g_settings_keys_sel) g_settings_keys_scroll = g_settings_keys_sel;
    if (g_settings_keys_scroll < g_settings_keys_sel - vis + 1) g_settings_keys_scroll = g_settings_keys_sel - vis + 1;
    if (g_settings_keys_scroll > total - vis) g_settings_keys_scroll = total - vis;
    if (g_settings_keys_scroll < 0) g_settings_keys_scroll = 0;
}

int settings_keys_row_at(int host_rows, int entry) {
    settings_keys_clamp_scroll(host_rows);
    int rel = entry - g_settings_keys_scroll;
    if (rel < 0 || rel >= settings_keys_visible(host_rows)) return -1;
    return SETTINGS_KEYS_ROW0 + rel;
}

int settings_keys_entry_at(int host_rows, int row) {
    settings_keys_clamp_scroll(host_rows);
    int rel = row - SETTINGS_KEYS_ROW0;
    if (rel < 0 || rel >= settings_keys_visible(host_rows)) return -1;
    int entry = g_settings_keys_scroll + rel;
    return entry < settings_keys_rows() ? entry : -1;
}

/* 一个 2 格宽的实心色块，用真实 RGB 输出（不参与主题重映射） */
static void append_swatch(char *out, int bs, int *posp, int r, int g, int b) {
    *posp += snprintf(out + *posp, bs - *posp, "\x1b[48;2;%d;%d;%dm  \x1b[0m", r, g, b);
}

static const char *settings_row_style(int selected, int hovered) {
    if (selected) return hovered ? "\x1b[048;2;048;075;110m\x1b[038;2;255;255;255;1m"
                                 : "\x1b[048;2;038;060;088m\x1b[038;2;121;192;255;1m";
    return hovered ? "\x1b[048;2;033;038;045m\x1b[038;2;255;255;255;1m" : "\x1b[038;2;230;237;243m";
}

/* v2.0.9：设置页说明 / 提示行按右侧可用宽度裁剪（窄终端上不再折行盖到侧栏与底栏 —— 用户
 * 反馈「太窄会显示不了」）。用法：begin(行, 起点, 样式) → text(...)* → end()。
 * 裁剪只看显示列数（宽字符算 2），末尾不补空格。 */
static int g_sl_left = 0;   /* begin 时记住的剩余可用列 */
static int g_sl_host_rows = 0;   /* v2.1.3：本帧行数（逐行限宽判定用） */
static int g_sl_row = 0, g_sl_col0 = 1;   /* v2.1.0：当前行与行首列（截断气泡登记用） */
static int g_sl_hidden = 0;               /* 1 = 本行不可见，begin 已提前返回 */
/* v2.1.4：这两行上移到本文件靠前的位置（横向滚动行的气泡登记要用）。 */
void settings_tip_reset(void) { g_tip_n = 0; g_sl_row = 0; }
int settings_tip_count(void) { return g_tip_n; }
const SettingsTip *settings_tip_at(int row, int col) {
    /* v2.1.2：不再「同一行随便挑一个气泡」兜底。用户报「光标在导航选项中，可以触发
     * 旁边的悬停提示」：侧栏那一行和右侧窗格同一行，兜底把右侧被截断的行拽出来弹在
     * 侧栏上。现在必须鼠标确实停在被截断那一段的列区间内才弹。 */
    for (int i = 0; i < g_tip_n; i++) {
        const SettingsTip *t = &g_tips[i];
        if (t->row != row) continue;
        if (col >= t->col && col < t->col + t->len) return t;
    }
    return NULL;
}

static void settings_line_begin(char *out, int bs, int *posp, int row, int main_left, int host_cols, const char *sgr) {
    /* 调用方给 -1 = 这一行滚出了可见区：整行不发（text 因 g_sl_left=0 自动跳过，
     * end 只复位属性，不会留下任何字符）。 */
    if (row < 0) { g_sl_hidden = 1; g_sl_left = 0; g_sl_row = 0; return; }
    g_sl_hidden = 0;
    /* v2.1.3：预算原先正好等于「main_left..host_cols」的全部列数，于是段尾
     * append_padded_utf8 补的那一格空格正好落在屏幕最后一列 ⇒ 终端自动折行，
     * 把【下一行的行首】整段盖掉（40 列时侧栏 [A] 那行就是这么消失的）。留 1 列。
     * 留 1 列也顺带解决了「最后一段恰好写满预算 ⇒ 末尾补的空格踩到分隔线 │」。 */
    g_sl_row = row; g_sl_col0 = main_left;
    /* 预算 = min(右栏全宽, 逐行限宽)。
     * v2.1.3：段被裁短时，段尾那格补位空格必须记在【整段预算】上——原先记成
     * 「旧预算 - 段宽」等于凭空多写 1 列，落在屏幕最后一列就触发终端自动折行，
     * 把下一行行首（侧栏的 [A] / 分隔线 │）整段盖掉（40 列实测）。 */
    /* 与启动项表格的 menu_geom 同一口径：右侧正文只到 host_cols-1，最后一列必须留白，
     * 否则整行宽度 = 终端宽度，任何一格补位空格都会触发自动折行。 */
    int full = host_cols - main_left - 1;
    int lim = settings_right_row_limit(g_sl_host_rows, host_cols, main_left, row);
    g_sl_left = lim < full ? lim : full;
    if (g_sl_left < 0) g_sl_left = 0;
    if (g_sl_left <= 0) { g_sl_hidden = 1; g_sl_left = 0; return; }
    if (g_sl_left < 0) g_sl_left = 0;
    *posp += snprintf(out + *posp, bs - *posp, "\x1b[%d;%dH%s", row, main_left, sgr);
}
static void settings_line_sgr(char *out, int bs, int *posp, const char *sgr) {
    if (g_sl_hidden) return;
    *posp += snprintf(out + *posp, bs - *posp, "%s", sgr);
}
/* ellip=1（默认用法）：放不下就切到行尾补「...」并把全文登记进悬停气泡。
 * ellip=0：一行里有多段（如详情页的三个按钮）时用 —— 前面的段静默丢掉自己的尾部，
 * 最后一段才带「...」，否则每段都补省略号会看起来像被切了三刀。 */
static void settings_line_text_e(char *out, int bs, int *posp, const char *text, int ellip) {
    if (g_sl_left <= 0 || g_sl_hidden) return;
    int w = utf8_cols(text, (int)strlen(text));
    if (w <= g_sl_left) {
        *posp += snprintf(out + *posp, bs - *posp, "%s", text);
        g_sl_left -= w;   /* 原样写出，不补位 —— 补位会把按钮行铺到行尾（v2.1.3） */
    } else {
        /* 横向装不下：截到行尾并补「...」，同时把全文登记进气泡表，
         * 鼠标停在这一行上时以浮层给出完整内容。 */
        int budget = ellip ? g_sl_left - 3 : g_sl_left;
        if (budget < 1) budget = 1;
        int before = *posp;
        append_padded_utf8(out, bs, posp, NULL, text, budget);
        /* 去掉 append_padded_utf8 尾部补的空格 */
        int slen = (int)strlen(text);
        while (*posp > before + slen && *posp > before && out[*posp - 1] == ' ') (*posp)--;
        if (ellip && g_sl_left > 3 && *posp < bs - 4) { out[(*posp)++]='.'; out[(*posp)++]='.'; out[(*posp)++]='.'; }
        if (!ellip) { g_sl_left = 0; return; }   /* 预算已用完 ⇒ 后面的段自动写不进去 */
        g_sl_left = 0;
        if (g_sl_row > 0 && g_tip_n < SETTINGS_TIP_MAX) {
            SettingsTip *t = &g_tips[g_tip_n++];
            t->row = g_sl_row; t->col = g_sl_col0; t->len = w;
            snprintf(t->full, sizeof(t->full), "%s", text);
        }
    }
}
static void settings_line_text(char *out, int bs, int *posp, const char *text) {
    settings_line_text_e(out, bs, posp, text, 1);
}

static void settings_line_end(char *out, int bs, int *posp) {
    if (g_sl_hidden) return;
    *posp += snprintf(out + *posp, bs - *posp, "\x1b[0m");
}

static void render_settings_items(char *out, int bs, int *posp, int host_rows, int host_cols, int main_left) {
    /* v2.1.4「条目管理」页：新建 / 快速预设库 / 每一项的 ↑↓改删 都收在这里，
     * 「启动」页那张表只留「设为启动默认」的单选。表本身与启动项页同一支函数画。 */
    int pos = *posp;
    settings_line_begin(out, bs, &pos, settings_manage_row_view(host_rows, 3),
                        main_left, host_cols, "\x1b[038;2;121;192;255;1m");
        settings_line_text(out, bs, &pos, "\xe2\x96\xa0 \xe6\x9d\xa1\xe7\x9b\xae\xe7\xae\xa1\xe7\x90\x86 (Menu Items)");
        settings_line_end(out, bs, &pos);
    settings_line_begin(out, bs, &pos, settings_manage_row_view(host_rows, 4),
                        main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "\xe5\x90\xaf\xe5\x8a\xa8\xe8\x8f\x9c\xe5\x8d\x95\xe7\x9a\x84\xe9\xa1\xb9\xe7\x9b\xae\xef\xbc\x88\xe9\xa1\xba\xe5\xba\x8f\xe5\xb0\xb1\xe6\x98\xaf\xe5\x90\xaf\xe5\x8a\xa8\xe6\x97\xb6\xe7\x9a\x84\xe5\x88\x97\xe8\xa1\xa8\xe9\xa1\xba\xe5\xba\x8f\xef\xbc\x89\xef\xbc\x9a");
        settings_line_end(out, bs, &pos);

    /* 表头（与启动项页同一套几何 / 同一个横滚口径） */
    int g_cw = 0, g_nw = 0, g_cw2 = 0, g_bc = 0, g_ud = 0, g_btn = 0;
    settings_menu_table_geom(host_rows, host_cols, main_left, &g_cw, &g_nw, &g_cw2,
                             &g_bc, &g_ud, &g_btn);
    int h_on = (g_cw > host_cols - main_left - 1);
    int h_sc = h_on ? settings_hscroll() : 0;
    int hr = settings_manage_row_view(host_rows, 9);
    if (hr > 0) {
        if (!h_on) {
            settings_line_begin(out, bs, &pos, hr, main_left, host_cols, "\x1b[038;2;121;192;255;1m");
            char head[192];
            if (settings_menu_header(head, sizeof(head), host_cols, main_left) > 0)
                settings_line_text(out, bs, &pos, head);
            settings_line_end(out, bs, &pos);
        } else {
            int pre = settings_menu_pre_w(g_cw + main_left + 1, main_left);
            SettingsSeg sg[5]; int ns = 0;
            sg[ns].sgr = "\x1b[0m"; sg[ns].text = ""; sg[ns].pad_to_col = pre - 5; ns++;
            sg[ns].sgr = "\x1b[038;2;121;192;255;1m"; sg[ns].text = "\xe5\xba\x8f\xe5\x8f\xb7"; sg[ns].pad_to_col = pre; ns++;
            if (g_nw) { sg[ns].sgr = "\x1b[038;2;121;192;255;1m"; sg[ns].text = "\xe6\x98\xbe\xe7\xa4\xba\xe5\x90\x8d\xe7\xa7\xb0"; sg[ns].pad_to_col = pre + g_nw; ns++; }
            if (g_cw2) { sg[ns].sgr = "\x1b[038;2;121;192;255;1m"; sg[ns].text = "\xe5\x90\xaf\xe5\x8a\xa8\xe5\x91\xbd\xe4\xbb\xa4\xe8\xa1\x8c"; sg[ns].pad_to_col = g_bc; ns++; }
            if (g_btn) { sg[ns].sgr = "\x1b[0m\x1b[038;2;121;192;255;1m"; sg[ns].text = "\xe6\x93\x8d\xe4\xbd\x9c"; sg[ns].pad_to_col = 0; ns++; }
            /* 行尾那截 «n/m» 标记由 settings_hscroll_mark 单独贴着右端写 ⇒ 表头这一行
             * 先给它让出 10 列（否则标记会压在最后一个宽字符的右半格上，整行反而多出 1 列）。 */
            int mleft = settings_mark_left(host_rows, host_cols, hr, SETTINGS_HMARK_W - 1);
            settings_hline2(out, bs, &pos, hr, main_left, mleft ? mleft - 1 : host_cols,
                            g_cw, sg, ns, 40, 0, 0);
            settings_hscroll_mark(out, bs, &pos, hr, host_cols, h_sc,
                                  host_cols - main_left - 1, g_cw, host_rows);
        }
    }
    {
        MenuRowCtx rc; rc.out = out; rc.bs = bs; rc.pos = &pos;
        rc.host_rows = host_rows; rc.host_cols = host_cols; rc.main_left = main_left;
        rc.sel = g_settings_table_sel; rc.row_view = settings_manage_row_view;
        rc.h_on = h_on; rc.h_sc = h_sc; rc.cw = g_cw;
        rc.nw = g_nw; rc.cw2 = g_cw2; rc.bc = g_bc; rc.ud = g_ud; rc.btn = g_btn;
        rc.show_ops = 1;
        render_menu_rows(&rc);
    }

    /* 顶部动作条：[+] 新建条目 / [P] 快速预设库 */
    int btn_r = settings_manage_row_view(host_rows, 10 + g_chooser_item_count + 1);
    if (btn_r > 0) {
        int h_add = (g_mouse_y == btn_r - 1 && g_mouse_x >= main_left - 1 && g_mouse_x < main_left + 13);
        int h_pre = (g_mouse_y == btn_r - 1 && g_mouse_x >= main_left + 15 && g_mouse_x < main_left + 29);
        settings_line_begin(out, bs, &pos, btn_r, main_left, host_cols, "\x1b[0m");
        settings_line_sgr(out, bs, &pos, h_add ? "\x1b[048;2;063;185;080m\x1b[038;2;013;017;023;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;063;185;080;1m");
        settings_line_text_e(out, bs, &pos, " [+] \xe6\x96\xb0\xe5\xbb\xba\xe6\x9d\xa1\xe7\x9b\xae ", 0);
        settings_line_sgr(out, bs, &pos, "\x1b[0m");
        settings_line_sgr(out, bs, &pos, h_pre ? "\x1b[048;2;031;136;061m\x1b[038;2;255;255;255;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;031;136;061;1m");
        settings_line_text_e(out, bs, &pos, " [P] \xe9\xa2\x84\xe8\xae\xbe\xe5\xba\x93 ", 0);
        settings_line_sgr(out, bs, &pos, "\x1b[0m");
        settings_line_end(out, bs, &pos);
    }

    int hint_r = settings_manage_row_view(host_rows, 12 + g_chooser_item_count);
    settings_line_begin(out, bs, &pos, hint_r, main_left, host_cols, "\x1b[038;2;139;148;158m");
    {
        /* 提示行自己也得留神：这一页的提示比别页长，窄终端（如 50 列）放不下时
         * 与其让 settings_line_text 补「...」正好写满到最后一列（⇒ 自动折行盖掉下一行
         * 行首），不如直接换一条短的，整条都在预算里。 */
        const char *hint_long =
            "\xe6\x8f\x90\xe7\xa4\xba: \xe2\x86\x91/\xe2\x86\x93 \xe9\x80\x89\xe6\x8b\xa9, Ctrl+\xe2\x86\x91/\xe2\x86\x93 \xe8\xb0\x83\xe5\xba\x8f, Enter \xe7\xbc\x96\xe8\xbe\x91, X \xe5\x88\xa0\xe9\x99\xa4, + \xe6\x96\xb0\xe5\xbb\xba, P \xe9\xa2\x84\xe8\xae\xbe, Shift+\xe6\xbb\x9a\xe8\xbd\xae \xe6\xa8\xaa\xe6\xbb\x9a";
        const char *hint_short =
            "\xe6\x8f\x90\xe7\xa4\xba: \xe2\x86\x91/\xe2\x86\x93 \xe9\x80\x89\xe6\x8b\xa9, Ctrl+\xe2\x86\x91/\xe2\x86\x93 \xe8\xb0\x83\xe5\xba\x8f, Enter \xe7\xbc\x96\xe8\xbe\x91, X \xe5\x88\xa0\xe9\x99\xa4";
        int room = settings_mark_room(host_rows, host_cols, main_left, hint_r,
                                      SETTINGS_MANAGE_FIRST, SETTINGS_MANAGE_LAST);
        g_sl_left = (g_sl_left > room) ? (g_sl_left - room) : 0;   /* 0 ⇒ 整条提示不写 */
        const char *hint = utf8_cols(hint_long, (int)strlen(hint_long)) <= g_sl_left
                               ? hint_long : hint_short;
        settings_line_text(out, bs, &pos, hint);
    }
    settings_line_end(out, bs, &pos);
    settings_page_mark(out, bs, &pos, hint_r, host_cols, host_rows, SETTINGS_MANAGE_FIRST,
                       SETTINGS_MANAGE_LAST, &g_settings_manage_scroll);
    *posp = pos;
}

static void render_settings_appearance(char *out, int bs, int *posp, int host_rows, int host_cols, int main_left) {
    int pos = *posp;
    (void)host_cols;
    settings_line_begin(out, bs, &pos, settings_appearance_row_view(host_rows, 3),
                        main_left, host_cols, "\x1b[038;2;121;192;255;1m");
        settings_line_text(out, bs, &pos, "■ 配色主题 (Theme)");
        settings_line_end(out, bs, &pos);
    settings_line_begin(out, bs, &pos, settings_appearance_row_view(host_rows, 4),
                        main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "↑/↓ 选择，Enter/Space 立即应用并写入 termux.ini：");
        settings_line_end(out, bs, &pos);

    for (int i = 0; i < theme_count(); i++) {
        int row = settings_appearance_row_view(host_rows, settings_theme_row(i));
        if (row < 0) continue;   /* 滚出可见区 */
        int active = (i == theme_index());
        int selected = (g_settings_theme_sel == i);
        int hovered = (g_mouse_y == row - 1 && g_mouse_x >= main_left - 1 && g_mouse_x < main_left + 40);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s %s %-14s \x1b[0m",
                        row, main_left, settings_row_style(selected, hovered),
                        active ? "[●]" : "[○]", theme_name_at(i));
        /* 主题预览：强调色 / 绿 / 橙 / 紫 四个色块 */
        const ThemeDef *def = &g_builtin_themes[i];
        int preview[4] = {TH_ACCENT, TH_GREEN, TH_ORANGE, TH_PURPLE};
        for (int k = 0; k < 4; k++) {
            ThemeRGB c = def->role[preview[k]];
            append_swatch(out, bs, &pos, c.r, c.g, c.b);
        }
    }

    int title_r = settings_appearance_row_view(host_rows, SETTINGS_ROLE_ROW0 - 2);
    if (title_r < 0) title_r = 0;
    settings_line_begin(out, bs, &pos, title_r, main_left, host_cols, "\x1b[038;2;121;192;255;1m");
        settings_line_text(out, bs, &pos, "■ 语义颜色 (Palette)");
        settings_line_sgr(out, bs, &pos, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "   Enter 编辑十六进制, R 复位当前项, Ctrl+R 清除全部");
        settings_line_end(out, bs, &pos);
    settings_line_begin(out, bs, &pos, settings_appearance_row_view(host_rows, SETTINGS_ROLE_ROW0 - 1),
                        main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "界面里所有派生色都由这 16 个角色混合得出，改一个即可成套生效。窗格内 cmd 的底色/字色见 termux.ini 的 pane_*。");
        settings_line_end(out, bs, &pos);

    for (int role = 0; role < TH_ROLE_COUNT; role++) {
        int row = settings_appearance_row_view(host_rows, settings_role_row(role));
        int col = settings_role_col(main_left, role);
        if (row < 0) continue;   /* 滚出可见区 */
        int selected = (g_settings_theme_sel == theme_count() + role);
        /* hover 列区间与点击命中一致：c ∈ [col, col+COL_W-2]（g_mouse_x = c-1）。 */
        int hovered = (g_mouse_y == row - 1 && g_mouse_x >= col - 1 && g_mouse_x <= col + SETTINGS_ROLE_COL_W - 3);
        int r, g, b;
        theme_role_rgb(role, &r, &g, &b);

        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s %-15s\x1b[0m ",
                        row, col, settings_row_style(selected, hovered), theme_role_name(role));
        append_swatch(out, bs, &pos, r, g, b);

        if (g_hex_edit_active && g_hex_edit_role == role && hex_edit_popup_shown(host_rows, host_cols)) {
            /* v2.1.1：编辑框交给浮层（窄终端上这一行放不下值段，画出来也是被裁的） */
        } else if (g_hex_edit_active && g_hex_edit_role == role) {
            char shown[16];
            snprintf(shown, sizeof(shown), "#%s", g_hex_edit_buf);
            pos += snprintf(out + pos, bs - pos, " \x1b[048;2;038;060;088m\x1b[038;2;255;255;255;1m%-8s\x1b[0m", shown);
        } else {
            pos += snprintf(out + pos, bs - pos, " \x1b[038;2;139;148;158m#%02x%02x%02x\x1b[0m%s",
                            r, g, b, theme_role_is_overridden(role) ? "\x1b[038;2;210;153;034m*\x1b[0m" : " ");
        }
    }

    int hint_r = settings_appearance_row_view(host_rows, SETTINGS_ROLE_ROW0 + SETTINGS_ROLE_ROWS + 1);
    if (hint_r < 0) hint_r = host_rows;
    if (g_hex_edit_active) {
        pos += snprintf(out + pos, bs - pos,
            "\x1b[%d;%dH\x1b[038;2;210;153;034;1m正在编辑 %s：输入 6 位十六进制，Enter 确认，Esc 取消\x1b[0m",
            hint_r, main_left, theme_role_name(g_hex_edit_role));
    } else {
        settings_line_begin(out, bs, &pos, hint_r, main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "提示: ↑/↓ 选择, ←/→ 换列, Enter 应用/编辑, R 复位, Ctrl+R 清除全部自定义, Ctrl+S 保存, Esc 返回");
        settings_line_end(out, bs, &pos);
        settings_page_mark(out, bs, &pos, hint_r, host_cols, host_rows,
                           SETTINGS_APPEAR_FIRST, SETTINGS_APPEAR_LAST, &g_settings_appear_scroll);
    }
    *posp = pos;
}

/* v2.0.7：窗格配色页 —— cmd / shell 文字的默认前后景与 16 色（[theme] pane_*）。
 * 与外观页的 UI 角色分开：那 16 个只管 termux 自己的界面。
 * hex 编辑框复用外观页的 g_hex_edit_*：g_hex_edit_role >= 0 是 UI 角色，
 * < 0 是 pane 槽位（编码 -(slot+1)），两页不会同时开。 */
/* v2.1.0：不走 settings_line_begin 的行（自己发 CUP 的那些）在截断前把行/列登记上，
 * 否则气泡会挂到上一条 begin 的行号上（窗格页的值段就是这么串位的）。 */
static void settings_tip_arm(int row, int col0) { g_sl_row = row; g_sl_col0 = col0; }

static void append_clipped_utf8(char *out, int bs, int *posp, const char *s, int max_cols) {
    int saved = g_sl_left;
    g_sl_left = max_cols;
    settings_line_text(out, bs, posp, s);
    g_sl_left = saved;
}

/* v2.1.0：预设方案选择浮层 —— 用户反馈「窗格配色要可以选择」。原先方案行只显示
 * 一个名字、←/→ 逐个换，既看不见有哪些、也看不见长什么样。改成 Enter/点击弹出
 * 完整列表：每行带 背景/前景/红/绿 四个色块预览，↑/↓ 或数字选中、Enter 应用。
 * 视觉语言与「常用命令行预设」浮层一致（同一套边框 + [Esc] 取消行）。 */
void pane_scheme_picker_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h) {
    int n = theme_pane_scheme_count();
    int pw = 44;
    if (pw > host_cols) pw = host_cols;
    if (pw < 22) pw = 22;
    int ph = n + 3;                       /* 标题 + n 项 + [Esc] + 底边 */
    if (ph > host_rows) ph = host_rows;
    if (ph < 5) ph = 5;
    int t = (host_rows - ph) / 2 + 1;
    if (t < 1) t = 1;
    if (t + ph > host_rows + 1) t = host_rows + 1 - ph;
    if (t < 1) t = 1;
    int l = (host_cols - pw) / 2 + 1;
    if (l < 1) l = 1;
    if (l + pw > host_cols + 1) l = host_cols + 1 - pw;
    if (l < 1) l = 1;
    if (top) *top = t;
    if (left) *left = l;
    if (w) *w = pw;
    if (h) *h = ph;
}

/* 浮层里一行能放下的色块个数：名字列 16 格 + 两侧留白，其余给色块（每块 2 列）。 */
int pane_scheme_picker_swatches(int pw) {
    int avail = pw - 1 - 2 - 4 - 16 - 1 - 3;   /* │  两空格 [n] 空格 名字 空格 块… ● */
    if (avail < 0) avail = 0;
    return avail / 2;
}

void render_pane_scheme_picker(char *out, int bs, int *posp, int host_rows, int host_cols) {
    int pos = *posp;
    int top, left, pw, ph;
    pane_scheme_picker_geom(host_rows, host_cols, &top, &left, &pw, &ph);
    int n = theme_pane_scheme_count();
    int nsw = pane_scheme_picker_swatches(pw);
    int name_w = pw - 1 - 2 - 4 - 1 - 1 - nsw * 2 - 3;
    if (name_w < 6) name_w = 6;
    if (name_w > 16) name_w = 16;

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "┌─ 窗格配色方案 ↑/↓ 选择, Enter 应用 ");
    pos += snprintf(out + pos, bs - pos, "[%d;%dH[038;2;255;255;255m[048;2;031;136;061;1m%s", top, left, hdr);
    int cols = utf8_cols(hdr, (int)strlen(hdr));
    while (cols < pw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80';
        cols++;
    }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");

    for (int i = 0; i < n; i++) {
        int r = top + 1 + i;
        if (r > top + ph - 2) break;
        int row_hover = (g_mouse_y == r - 1 && g_mouse_x >= left - 1 && g_mouse_x < left - 1 + pw);
        int is_sel = (i == g_settings_pane_scheme);
        int applied = theme_pane_scheme_matches(i);
        const char *bg = (row_hover || is_sel) ? "\x1b[048;2;045;055;072m" : "\x1b[048;2;022;027;034m";
        const char *fg = is_sel ? "\x1b[038;2;255;255;255;1m" : "\x1b[038;2;230;237;243m";
        char tag[16];
        snprintf(tag, sizeof(tag), "[%d]", i + 1);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s  \x1b[038;2;210;153;034m%s\x1b[0m%s\x1b[0m",
                        r, left, bg, tag, bg);
        int ccols = 1 + 2 + utf8_cols(tag, (int)strlen(tag));
        /* 名字与色块之间的空格计入 name_w 之外，和预设浮层同一算法：tag 后 1 格分隔 */
        pos += snprintf(out + pos, bs - pos, "%s ", bg);
        ccols += 1;
        pos += snprintf(out + pos, bs - pos, "%s%s", fg, bg);
        append_padded_utf8(out, bs, &pos, &ccols, theme_pane_scheme_name(i), name_w);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m%s", bg);
        /* 注意：append_padded_utf8 已经把 cols 加成了「名字宽度 + 尾部补的空格」，
         * 这里不能再 +1，否则 pad_to_right_border 少补一格 ⇒ 右边框比上下边框短 1 列
         * （用户报「窗格选择的配色方案右侧没有对齐」）。 */
        ThemeRGB prev[THEME_PANE_SLOTS];
        theme_pane_scheme_preview(i, prev);
        for (int k = 0; k < nsw; k++) {
            /* 与窗格配色表同序：背景、前景、滑块、轨道，再是 16 个 ANSI 色 */
            int slot = settings_pane_order_slot(k);
            if (slot < 0 || slot >= THEME_PANE_SLOTS) slot = k;
            append_swatch(out, bs, &pos, prev[slot].r, prev[slot].g, prev[slot].b);
            ccols += 2;
        }
        pos += snprintf(out + pos, bs - pos, " %s%s\x1b[0m",
                        applied ? "\x1b[038;2;063;185;080;1m" : "\x1b[038;2;048;054;061m",
                        applied ? "●" : "○");
        ccols += 2;
        pad_to_right_border(out, bs, &pos, &ccols, pw);
    }

    int esc_r = top + 1 + n;
    if (esc_r <= top + ph - 2) {
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[048;2;022;027;034m  \x1b[048;2;033;038;045m\x1b[038;2;139;148;158m [Esc] 取消 \x1b[0m\x1b[048;2;022;027;034m", esc_r, left);
        int ccols = 1 + 2 + 12;
        pad_to_right_border(out, bs, &pos, &ccols, pw);
    }

    int bot_r = top + ph - 1;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m└", bot_r, left);
    int bcols = 1;
    while (bcols < pw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80';
        bcols++;
    }
    pos += snprintf(out + pos, bs - pos, "┘\x1b[0m");
    *posp = pos;
}

/* ---- v2.1.1：十六进制编辑浮层 --------------------------------------------
 * 只画「色块 + 完整 6 位 + 提示」三行，宽度按需收缩到终端宽；表行此时不重复画框。
 * 因为框宽 = min(需要, 终端宽) 且值段固定排在左 + 6，任何宽度下都不会截断。 */
#define HEX_EDIT_POPUP_W 50
/* 侧栏宽度单一来源：渲染 / 命中 / 光标 / 浮层都调它（浮层必须整体待在右侧正文区里，
 * 否则会切掉侧栏菜单，分隔线在 sb_w + 2）。v2.1.2 起 render_settings_panel 与
 * handle_settings_mouse 不再各自抄一遍公式。 */
int settings_host_sidebar_w(int host_cols) {
    int sb_w = SETTINGS_SIDEBAR_W;
    if (sb_w > host_cols / 2) sb_w = host_cols / 2;
    if (sb_w < 15) sb_w = 15;
    if (sb_w > host_cols) sb_w = host_cols;
    if (sb_w < 1) sb_w = 1;
    return sb_w;
}

/* v2.1.3：侧栏里的每一条（表头、分隔线、启动行）都必须停在分隔线之前 —— 原先这些都是
 * 写死的 21~22 列字符串，在 30 列终端（sb_w=15）会溢出到下一行开头，把侧栏自己的条目、
 * 甚至右侧正文顶掉。这两个小工具负责「按列数生成/裁剪」。 */
static void sidebar_fill(char *buf, int bs, const char *unit, int cols) {
    int uw = utf8_cols(unit, (int)strlen(unit));
    if (bs <= 0) return;
    buf[0] = 0;
    if (cols < 1 || uw < 1) return;
    int n = cols / uw, k = 0;
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(unit);
        if (k + len >= bs - 1) break;
        for (int j = 0; j < len; j++) buf[k++] = unit[j];
    }
    buf[k] = 0;
}
/* 把 text 裁到 cols 列以内写进 buf（按字符边界，不留半个 UTF-8 序列）。 */
static void sidebar_clip(char *buf, int bs, const char *text, int cols) {
    if (bs <= 0) return;
    buf[0] = 0;
    int k = 0, used = 0, i = 0, len = (int)strlen(text);
    (void)0;   /* 见函数尾：先按 cols 取字，再回头吃掉行尾补位用的空格 */
    while (i < len) {
        int adv = 0;
        unsigned cp = utf8_decode_cp(text + i, len - i, &adv);
        if (adv <= 0) break;
        int w = is_zero_width_cp(cp) ? 0 : (is_wide_cp(cp) ? 2 : 1);
        if (used + w > cols) break;
        if (k + adv >= bs - 1) break;
        for (int q = 0; q < adv; q++) buf[k++] = text[i + q];
        used += w; i += adv;
    }
    /* v2.1.3：那些定宽标签串尾部本来就有一串补齐空格（"  [K] 键位设置      "）。
     * 若连着它们一起截，sb_w=15 时只能留下 13 列正文 + 1 列补位 = 白丢一个汉字的
     * 位置。空格吃掉后正文可以一直排到 cols（cols 已给分隔线让开一列）。 */
    while (k > 0 && buf[k - 1] == ' ') k--;
    buf[k] = 0;
}

void hex_edit_popup_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h) {
    int ml = settings_host_sidebar_w(host_cols) + 3;      /* 正文起点 */
    int pw = hex_edit_popup_w(host_cols);
    if (pw < 1) pw = HEX_EDIT_POPUP_MIN;
    if (pw > host_cols) pw = host_cols;
    int ph = 3;
    if (ph > host_rows) ph = host_rows;
    int t = host_rows / 3;                    /* 上 1/3：盖住表头，不压正在编辑的行 */
    if (t < 1) t = 1;
    if (t + ph - 1 > host_rows) t = host_rows - ph + 1;
    if (t < 1) t = 1;
    int content = host_cols - ml + 1;
    int l = ml + (content - pw) / 2;
    if (l < ml) l = ml;
    if (l < 1) l = 1;
    if (pw > content && l > host_cols - pw + 1) l = host_cols - pw + 1;   /* 贴右墙 */
    if (l < 1) l = 1;
    if (l + pw > host_cols + 1) l = host_cols + 1 - pw;
    if (l < 1) l = 1;
    if (top) *top = t;
    if (left) *left = l;
    if (w) *w = pw;
    if (h) *h = ph;
}

static int hex_edit_popup_w(int host_cols) {
    int ml = settings_host_sidebar_w(host_cols) + 3;
    int pw = HEX_EDIT_POPUP_W;
    int content = host_cols - ml + 1;
    if (pw > content) pw = content;
    if (pw >= HEX_EDIT_POPUP_MIN) return pw;
    /* 正文区装不下：改成贴右墙画（可以越过分隔线盖住侧栏——和方案列表浮层一样），
     * 因为「看得见完整 6 位」比「侧栏此刻少两行菜单」重要得多。 */
    pw = host_cols - 1;
    if (pw > HEX_EDIT_POPUP_W) pw = HEX_EDIT_POPUP_W;
    if (pw < HEX_EDIT_POPUP_MIN) return 0;   /* 连 8 位值都放不下：退回表行内联 */
    return pw;
}

void render_hex_edit_popup(char *out, int bs, int *posp, int host_rows, int host_cols) {
    int pos = *posp;
    if (host_rows < 3 || hex_edit_popup_w(host_cols) < HEX_EDIT_POPUP_MIN) return;  /* 退回内联 */
    int top, left, pw, ph;
    hex_edit_popup_geom(host_rows, host_cols, &top, &left, &pw, &ph);
    /* 只补「框左边到页面正文起点」这一段，右侧什么都不写：
     *  · framediff 是按行 diff 的，右侧上一帧的残字会由页面自己那一段重画（提示行
     *    本来就裁到行尾），不会像只画框那样在右边界外冒出半个边框；
     *  · 左侧必须补，否则框左沿会把侧栏文字切掉一截。 */
    /* 只补「正文起点 → 框左沿」的空隙（正文比框宽时才有）；
     * 框已经贴到左侧（正文区太窄）时整行重写，此时侧栏让位给颜色编辑。 */
    int fill_from = (left <= settings_host_sidebar_w(host_cols) + 2)
                        ? 1 : settings_host_sidebar_w(host_cols) + 3;
    if (fill_from < 1) fill_from = 1;
    if (fill_from > left - 1) fill_from = left;
    #define HEX_POPUP_FILL(rr)                                                          \
        do {                                                                            \
            if (fill_from < left) {                                                     \
                pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[0m\x1b[048;2;022;027;034m", (rr), fill_from); \
                for (int k = fill_from; k < left && pos < bs - 8; k++) out[pos++] = ' '; \
            }                                                                           \
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH", (rr), left);            \
        } while (0)
    int r, g, b;
    char shown[16];
    snprintf(shown, sizeof(shown), "#%s", g_hex_edit_buf);
    if (g_hex_edit_role >= 0) theme_role_rgb(g_hex_edit_role, &r, &g, &b);
    else if (!theme_pane_rgb(-g_hex_edit_role - 1, &r, &g, &b))
        theme_pane_fallback_rgb(-g_hex_edit_role - 1, &r, &g, &b);
    const char *label = (g_hex_edit_role >= 0) ? theme_role_name(g_hex_edit_role)
                                               : theme_pane_slot_label(-g_hex_edit_role - 1);
    const char *BG = "\x1b[048;2;022;027;034m";
    const char *BD = "\x1b[048;2;033;038;045m";

    char hdr[128];
    int hcols;
    int n = snprintf(hdr, sizeof(hdr), "┌─ 颜色编辑: %s ", label);
    if (n < 0) n = 0;
    if (n > (int)sizeof(hdr) - 1) n = (int)sizeof(hdr) - 1;
    hcols = utf8_cols(hdr, n);
    HEX_POPUP_FILL(top);
    pos += snprintf(out + pos, bs - pos, "\x1b[038;2;255;255;255m\x1b[048;2;031;136;061;1m%s", hdr);
    while (hcols < pw - 1 && pos < bs - 8) { out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80'; hcols++; }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");

    /* 值段：│ + 两空格 + 色块 2 + 空格 + 8 位（'#' + 6 位，光标停在第 7/8 位） */
    int cols = 1;
    HEX_POPUP_FILL(top + 1);
    pos += snprintf(out + pos, bs - pos, "%s\x1b[0m%s  ", BD, BG);
    append_swatch(out, bs, &pos, r, g, b);
    cols += 1 + 2;
    pos += snprintf(out + pos, bs - pos,
                    "\x1b[0m \x1b[048;2;038;060;088m\x1b[038;2;255;255;255;1m%-8s\x1b[0m%s", shown, BG);
    cols += 1 + 8;
    {
        const char *hint = "输入 6 位 · Enter 确认 · Esc 取消";
        int hw = utf8_cols(hint, (int)strlen(hint));
        if (pw - cols - 2 >= hw) {          /* 放不下就整段不写（宁可空着也不切一半） */
            pos += snprintf(out + pos, bs - pos, "\x1b[038;2;139;148;158m   %s\x1b[0m%s", hint, BG);
            cols += 3 + hw;
        }
    }
    pad_to_right_border(out, bs, &pos, &cols, pw);

    cols = 0;                                  /* └/┘ 之外的一格都不属于这个计数 */
    HEX_POPUP_FILL(top + 2);
    pos += snprintf(out + pos, bs - pos, "%s\x1b[0m%s", BD, BG);
    /* 上边框写的是「┌ + (pw-1) 根横线」共 pw 格；底边不能像原先那样先按 cols=1 起算
     * （那样横线只有 pw-2 根，右下角 ┘ 比上边框的 ┐ 短 1 列，用户报「颜色编辑弹出的框
     * 右侧没有对齐」，60/44/100 列实测都短一格）。这里从 0 起算，横线 pw-1 根 + ┘，
     * 与上边框逐格同宽。 */
    while (cols < pw - 1 && pos < bs - 8) { out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80'; cols++; }
    pos += snprintf(out + pos, bs - pos, "┘\x1b[0m");
    *posp = pos;
    #undef HEX_POPUP_FILL
}

/* ---- v2.1.1：滚轮滚动设置页右侧内容 --------------------------------------
 * 用户反馈「终端过矮时，设置右边窗格无法滚轮滚动」：设置面板的鼠标处理函数开头
 * 只认「按下」事件，滚轮整个被丢掉；矮终端里超出可见带的行只能靠 ↑/↓ 挪选中项
 * 才看得到。这里按当前页把滚轮翻成「选中项移动 + 页面跟着走」（与调色板浮层里
 * 滚轮的既有手感一致）；滚动量用的就是渲染/命中那一套 *scroll，天然同源。 */
void settings_wheel_scroll(int delta) {
    int dir = (delta > 0) ? -1 : 1;     /* 向上滚 = 看更早的内容 */
    int step = 3;
    if (g_settings_show_pane_schemes) {
        int nn = theme_pane_scheme_count();
        if (nn > 0)
            g_settings_pane_scheme = (g_settings_pane_scheme + (dir > 0 ? 1 : nn - 1)) % nn;
        g_mux.needs_redraw = 1;
        return;
    }
    if (g_settings_show_presets) {
        if (g_preset_count > 0)
            g_preset_sel = (g_preset_sel + (dir > 0 ? 1 : g_preset_count - 1)) % g_preset_count;
        g_mux.needs_redraw = 1;
        return;
    }
    if (g_settings_nav == SETTINGS_NAV_APPEARANCE) {
        int total = theme_count() + TH_ROLE_COUNT;
        if (g_settings_theme_sel < 0) g_settings_theme_sel = 0;
        if (dir > 0) { if (g_settings_theme_sel < total - 1) g_settings_theme_sel++; }
        else if (g_settings_theme_sel > 0) g_settings_theme_sel--;
        int vis = g_mux.host_rows - SETTINGS_THEME_ROW0;
        if (vis < 1) vis = 1;
        g_settings_appear_scroll += (dir > 0 ? step : -step);
        if (g_settings_appear_scroll > total - vis) g_settings_appear_scroll = total - vis;
        if (g_settings_appear_scroll < 0) g_settings_appear_scroll = 0;
    } else if (g_settings_nav == SETTINGS_NAV_BEHAVIOR) {
        int total = SETTINGS_BEHAVIOR_TOGGLES;
        if (g_settings_behavior_sel < 0) g_settings_behavior_sel = 0;
        if (dir > 0) { if (g_settings_behavior_sel < total) g_settings_behavior_sel++; }
        else if (g_settings_behavior_sel > 0) g_settings_behavior_sel--;
        int vis = g_mux.host_rows - SETTINGS_BEHAVIOR_ROW0;
        if (vis < 1) vis = 1;
        g_settings_behavior_scroll += (dir > 0 ? step : -step);
        if (g_settings_behavior_scroll > total + 2 - vis) g_settings_behavior_scroll = total + 2 - vis;
        if (g_settings_behavior_scroll < 0) g_settings_behavior_scroll = 0;
    } else if (g_settings_nav == SETTINGS_NAV_KEYS) {
        int total = settings_keys_rows();
        if (g_settings_keys_sel < 0) g_settings_keys_sel = 0;
        if (dir > 0) { if (g_settings_keys_sel < total - 1) g_settings_keys_sel++; }
        else if (g_settings_keys_sel > 0) g_settings_keys_sel--;
        g_settings_keys_scroll += (dir > 0 ? step : -step);
        if (g_settings_keys_scroll > total - 1) g_settings_keys_scroll = total - 1;
        if (g_settings_keys_scroll < 0) g_settings_keys_scroll = 0;
    } else if (g_settings_nav == SETTINGS_NAV_PANE) {
        if (g_settings_pane_sel < -1) g_settings_pane_sel = -1;
        if (g_settings_pane_sel >= THEME_PANE_SLOTS) g_settings_pane_sel = THEME_PANE_SLOTS - 1;
        if (dir > 0) { if (g_settings_pane_sel < THEME_PANE_SLOTS - 1) g_settings_pane_sel++; }
        else if (g_settings_pane_sel > -1) g_settings_pane_sel--;
        int vis = settings_pane_visible_rows(g_mux.host_rows, g_mux.host_cols, 0);
        g_settings_pane_scroll += (dir > 0 ? step : -step);
        if (g_settings_pane_scroll > THEME_PANE_SLOTS - vis)
            g_settings_pane_scroll = THEME_PANE_SLOTS - vis;
        if (g_settings_pane_scroll < 0) g_settings_pane_scroll = 0;
    } else if (g_settings_nav == SETTINGS_NAV_ITEMS) {
        /* v2.1.4：条目管理页 —— 滚轮滚「聚焦行」，窗口跟着走（settings_manage_row_view
         * 里每帧夹），到两端才停；页面本身不再单独滚。 */
        int total = g_chooser_item_count;
        if (total > 0) {
            if (g_settings_table_sel < 0) g_settings_table_sel = 0;
            if (dir > 0) { if (g_settings_table_sel < total - 1) g_settings_table_sel++; }
            else if (g_settings_table_sel > 0) g_settings_table_sel--;
            settings_sidebar_clamp_sel(g_mux.host_rows, total);
        }
    } else if (g_settings_nav == 0) {
        /* v2.1.2：「启动」页的滚轮先滚侧栏那个菜单项窗口（用户报「导航选项要可以滚动」），
         * 到顶/到底之后才带动右侧表格的选中项。 */
        int total = g_chooser_item_count;
        int vis = settings_sidebar_geom_cap(g_mux.host_rows, total);
        int maxs = total - vis;
        if (maxs < 0) maxs = 0;
        int before = g_settings_sidebar_scroll;
        g_settings_sidebar_scroll += (dir > 0 ? step : -step);
        if (g_settings_sidebar_scroll > maxs) g_settings_sidebar_scroll = maxs;
        if (g_settings_sidebar_scroll < 0) g_settings_sidebar_scroll = 0;
        if (g_settings_sidebar_scroll == before) {     /* 窗口到头了，才动选中项 */
            if (g_settings_table_sel < 0) g_settings_table_sel = 0;
            if (dir > 0) { if (g_settings_table_sel < g_chooser_item_count - 1) g_settings_table_sel++; }
            else if (g_settings_table_sel > 0) g_settings_table_sel--;
        }
        g_settings_startup_scroll += (dir > 0 ? step : -step);
        if (g_settings_startup_scroll < 0) g_settings_startup_scroll = 0;
        int mx = settings_startup_last(g_mux.host_rows) - 3 - (g_mux.host_rows - 3);
        if (mx < 0) mx = 0;
        if (g_settings_startup_scroll > mx) g_settings_startup_scroll = mx;
    } else {
        int f = (g_settings_field < 0) ? 0 : g_settings_field;
        f = (f + (dir > 0 ? 1 : 3)) % 4;
        g_settings_field = f;
        g_settings_detail_scroll += (dir > 0 ? step : -step);
        if (g_settings_detail_scroll < 0) g_settings_detail_scroll = 0;
        int mx = (SETTINGS_DETAIL_LAST - SETTINGS_DETAIL_FIRST + 1)
                 - (g_mux.host_rows - SETTINGS_DETAIL_FIRST + 1);
        if (mx < 0) mx = 0;
        if (g_settings_detail_scroll > mx) g_settings_detail_scroll = mx;
    }
    g_mux.needs_redraw = 1;
}

static void render_settings_pane(char *out, int bs, int *posp, int host_rows, int host_cols, int main_left) {
    int pos = *posp;
    int avail = host_cols - main_left + 1;           /* 右侧可用列数 */
    if (avail < 1) avail = 1;
    settings_line_begin(out, bs, &pos, 3, main_left, host_cols, "\x1b[038;2;121;192;255;1m");
    settings_line_text(out, bs, &pos, "■ 窗格配色 (Pane Palette)");
    settings_line_end(out, bs, &pos);
    settings_line_begin(out, bs, &pos, 4, main_left, host_cols, "\x1b[038;2;139;148;158m");
    settings_line_text(out, bs, &pos, "cmd / shell 文字的默认背景、字色、滚动条与 16 个索引色（像 Windows Terminal 的配色方案）。");
    settings_line_end(out, bs, &pos);

    /* 方案行：[方案] ‹ 名字 ›  ←/→ 换  Enter 应用。当前 20 槽位恰好等于某方案时标 ● */
    {
        int sel_scheme = (g_settings_pane_sel == -1);
        int hovered = (g_mouse_y == SETTINGS_PANE_SCHEME_ROW - 1 && g_mouse_x >= main_left - 1 && g_mouse_x < main_left - 1 + avail);
        int matched = theme_pane_scheme_matches(g_settings_pane_scheme);
        char line[128];
        snprintf(line, sizeof(line), " 预设方案  ‹ %-14s ›  [选择…]  ←/→ 换  Enter 打开方案列表  %s",
                 theme_pane_scheme_name(g_settings_pane_scheme), matched ? "● 已应用" : " ");
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s", SETTINGS_PANE_SCHEME_ROW, main_left, settings_row_style(sel_scheme, hovered));
        settings_tip_arm(SETTINGS_PANE_SCHEME_ROW, main_left);
        append_clipped_utf8(out, bs, &pos, line, avail);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m");
    }

    settings_pane_clamp_scroll(host_rows, host_cols, main_left);
    int vis_rows = settings_pane_visible_rows(host_rows, host_cols, main_left);
    for (int p = 0; p < THEME_PANE_SLOTS; p++) {
        int slot = settings_pane_order_slot(p);
        int row = settings_pane_row(host_cols, main_left, slot);
        int col = settings_pane_col(host_cols, main_left, slot);
        if (row < 0 || row >= SETTINGS_PANE_ROW0 + vis_rows) continue;   /* 滚出可见区 */
        int item_avail = host_cols - col + 1;
        if (item_avail < 4) continue;
        int selected = (g_settings_pane_sel == slot);
        int hovered = (g_mouse_y == row - 1 && g_mouse_x >= col - 1 && g_mouse_x <= col + SETTINGS_PANE_ITEM_W - 2);
        int r, g, b;
        int set = theme_pane_rgb(slot, &r, &g, &b);
        if (!set) theme_pane_fallback_rgb(slot, &r, &g, &b);
        /* 标签按【显示宽度】补齐到 16 列（%-16s 按字节补，中文标签会错位——用户反馈「没对齐」） */
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s ", row, col, settings_row_style(selected, hovered));
        append_padded_utf8(out, bs, &pos, NULL, theme_pane_slot_label(slot), 16);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m ");
        if (item_avail < SETTINGS_PANE_VALUE_OFF) continue;   /* 极窄：只放得下标签 */
        append_swatch(out, bs, &pos, r, g, b);
        int val_avail = item_avail - SETTINGS_PANE_VALUE_OFF;
        settings_tip_arm(row, col + SETTINGS_PANE_VALUE_OFF - 1);
        if (g_hex_edit_active && g_hex_edit_role == -(slot + 1) && hex_edit_popup_shown(host_rows, host_cols)) {
            /* v2.1.1：编辑框在浮层里（窄终端上这一行的值段会被裁到屏幕外） */
        } else if (g_hex_edit_active && g_hex_edit_role == -(slot + 1)) {
            char shown[16];
            snprintf(shown, sizeof(shown), "#%s", g_hex_edit_buf);
            pos += snprintf(out + pos, bs - pos, " \x1b[048;2;038;060;088m\x1b[038;2;255;255;255;1m");
            append_padded_utf8(out, bs, &pos, NULL, shown, val_avail < 8 ? val_avail : 8);
            pos += snprintf(out + pos, bs - pos, "\x1b[0m");
        } else if (set) {
            char shown[16];
            snprintf(shown, sizeof(shown), "#%02x%02x%02x*", r, g, b);
            pos += snprintf(out + pos, bs - pos, " \x1b[038;2;139;148;158m");
            append_clipped_utf8(out, bs, &pos, shown, val_avail);
            pos += snprintf(out + pos, bs - pos, "\x1b[0m");
        } else {
            pos += snprintf(out + pos, bs - pos, " \x1b[038;2;110;118;129m");
            append_clipped_utf8(out, bs, &pos,
                (slot == THEME_PANE_SB_THUMB || slot == THEME_PANE_SB_TRACK) ? "(内置渐变)" : "(跟随终端)", val_avail);
            pos += snprintf(out + pos, bs - pos, "\x1b[0m");
        }
    }
    int hint_r = settings_pane_hint_row(host_rows, host_cols, main_left);
    if (hint_r > host_rows - 1) hint_r = host_rows - 1;
    if (hint_r < SETTINGS_PANE_ROW0) hint_r = SETTINGS_PANE_ROW0;
    if (g_settings_pane_scroll > 0 || vis_rows < settings_pane_rows_per_col(host_cols, main_left)) {
        /* 有滚动时在提示行右端标 (n/20) */
        char tag[24]; int p = settings_pane_order_pos(g_settings_pane_sel);
        snprintf(tag, sizeof(tag), "(%d/%d)", p < 0 ? 0 : p + 1, THEME_PANE_SLOTS);
        int tl = (int)strlen(tag);
        if (host_cols - tl >= main_left)
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;110;118;129m%s\x1b[0m", hint_r, host_cols - tl, tag);
    }
    if (g_hex_edit_active && g_hex_edit_role < 0) {
        char line[160];
        snprintf(line, sizeof(line), "正在编辑 %s：输入 6 位十六进制，Enter 确认，Esc 取消", theme_pane_slot_label(-g_hex_edit_role - 1));
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;210;153;034;1m", hint_r, main_left);
        settings_tip_arm(hint_r, main_left);
        append_clipped_utf8(out, bs, &pos, line, avail);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m");
    } else {
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;139;148;158m", hint_r, main_left);
        settings_tip_arm(hint_r, main_left);
        append_clipped_utf8(out, bs, &pos, "提示: ↑/↓ 选择, ←/→ 换列/换方案, Enter 编辑/应用方案, R 复位当前项, Ctrl+R 清除全部, Esc 返回", avail);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m");
        int ex_r = hint_r + 1;
        if (ex_r <= host_rows - 1) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;110;118;129m", ex_r, main_left);
            settings_tip_arm(ex_r, main_left);
            append_clipped_utf8(out, bs, &pos, "例：浅色窗格 → 选「GitHub Light」方案 Enter；或手动改默认背景 ffffff、默认前景 24292f。改完立即生效并写入 termux.ini。", avail);
            pos += snprintf(out + pos, bs - pos, "\x1b[0m");
        }
    }
    *posp = pos;
}

/* 列宽：标记 3、动作名 20、说明 36、键位 20，按钮 [前缀]@+0(6) [改]@+8(4) [复位]@+13(6)，
 * 右边界 = 前缀列 + 19。宽终端（可用 >= 98）就是原来的常量；窄时先砍说明列（36），
 * 再压动作名列（>= 8），键位列保底 12，让按钮区永远留在屏幕内。 */
static void keys_widths(int host_cols, int main_left, int *name_w, int *desc_w, int *combo_w, int *show_reset) {
    int avail = host_cols - main_left + 1;
    int n = 20, c = 20, d = 36, sr = 1;
    if (avail >= 98) {
        /* 宽终端：与 v2.0.8 之前完全一致 */
    } else if (avail >= 42) {
        /* 窄：砍掉「说明」列，键位列压到 12~20，动作名列必要时缩到 8 */
        d = 0;
        c = avail - 42;                 /* 42 = 3 标记 + 20 动作名 + 19 按钮区 */
        if (c > 20) c = 20;
        if (c < 12) { n = avail - 3 - 12 - 19; c = 12; }
        if (n < 8) n = 8;
    } else {
        /* 极窄（< 42）：再砍掉 [复位] 按钮（R 键即可复位），保住 [前缀] 与 [改] */
        d = 0; sr = 0;
        c = avail - 26;                 /* 26 = 3 标记 + 12 按钮区([前缀]+间隔+[改]+1 列余量) */
        if (c > 20) c = 20;
        if (c < 10) c = 10;
        n = avail - 3 - c - 12;
        if (n > 20) n = 20;
        if (n < 6) n = 6;
    }
    if (name_w) *name_w = n;
    if (desc_w) *desc_w = d;
    if (combo_w) *combo_w = c;
    if (show_reset) *show_reset = sr;
}
int settings_keys_show_reset(int host_cols, int main_left) { int n, d, c, s; keys_widths(host_cols, main_left, &n, &d, &c, &s); return s; }
int settings_keys_name_w(int host_cols, int main_left) { int n, d, c, s; keys_widths(host_cols, main_left, &n, &d, &c, &s); return n; }
int settings_keys_desc_w(int host_cols, int main_left) { int n, d, c, s; keys_widths(host_cols, main_left, &n, &d, &c, &s); return d; }
int settings_keys_combo_w(int host_cols, int main_left) { int n, d, c, s; keys_widths(host_cols, main_left, &n, &d, &c, &s); return c; }
int settings_keys_prefix_col(int host_cols, int main_left) {
    int n, d, c, s; keys_widths(host_cols, main_left, &n, &d, &c, &s); return 3 + n + d + c;
}
int settings_keys_edit_col(int host_cols, int main_left) { return settings_keys_prefix_col(host_cols, main_left) + 8; }
int settings_keys_reset_col(int host_cols, int main_left) { return settings_keys_edit_col(host_cols, main_left) + 5; }

/* 菜单行：2(▶ [1]) + name_w(显示名称) + 2 + cmd_w(启动命令行) + 2 + 按钮区(12/10/无)
 * v2.1.3：名称列与按钮去留也随宽度收缩。原先名称固定 12 列 ⇒ 前缀 2 + 6 + 2 + 12 + 2 = 24
 * 比可用宽度还宽时，光前缀就把整行顶过 host_cols，终端自动折行 —— 折出来的尾巴落在【下一行】
 * 开头，把下一行的行首（含侧栏那一段）整段盖掉（40 列实测：[改][删] 出现在 r12/r13/r14 行首）。
 * 这里把「总宽 ≤ avail」做成硬约束，放不下就依次丢 [↑][↓] → 缩名称 → 整块按钮（键盘仍可用）。 */
/* 菜单行几何（v2.1.3 重写）：整行必须塞进「右侧可用宽度」，装不下就按优先级丢件。
 * 原先名称列固定 12 列、按钮列写死在 2+12+2+cw+2 ⇒ 40 列时前缀就比整行宽，
 * 终端自动折行把下一行的行首（连侧栏那一段）整段盖掉。渲染与鼠标命中都走这一份。 */
/* 行首宽度（v2.1.3）：正常是 " ▶" + "[n] " = 6 列；右侧正文窄到连它都放不下时压成
 * "[n]"（3 列），再放不下就是 1 列 —— 渲染端的尾部空格必须跟着这里走，否则行会超宽，
 * 终端把尾巴折到下一行开头，连侧栏的行首一起盖掉。 */
static int menu_pre_w(int host_cols, int main_left) {
    int avail = host_cols - main_left - 1;
    if (avail >= 6) return 6;
    if (avail >= 3) return 3;
    return (avail >= 1) ? 1 : 0;
}
static void menu_geom(int host_cols, int main_left, int *name_w, int *cmd_w,
                      int *btn_col, int *show_ud, int *show_btn) {
    /* v2.1.3：右侧正文在窄终端下可能只有十几列。原先各列只夹「下界」、不查总宽，
     * 6(行首) + 12(名称) + 2 + 2 + 2 = 24 超过 avail ⇒ 终端把尾巴折到【下一行】开头，
     * 把下一行的行首连侧栏一起盖掉（40 列实测：r12 行首出现 [改][删]）。
     * 改成「从最-rich 的布局往下试，每步都用真正会写出的宽度校验总宽」：
     *   丢 [↑][↓] → 收窄名称列 → 丢 [改][删] → 丢命令行整列 → 名称列按剩余宽度裁。
     * v2.1.4：g_menu_narrow_av>0 = 【视口口径】的一轮（横向滚动开着）。这时按钮不跟画布走
     * （40 列时它会落在画布列 54，永远看不见），而是贴视口右端；能同时摆下行首 + 8 列
     * 名称 + 按钮才画名称列，放不下就整列不画（滚出来的是命令行列）。 */
    int avail = host_cols - main_left - 1;
    int pre = menu_pre_w(host_cols, main_left);
    const int pinned = (g_menu_narrow_av > 0);
    if (pinned) { avail = g_menu_narrow_av; if (g_menu_narrow_pre >= 0) pre = g_menu_narrow_pre; }
    if (pinned) {
        /* 横滚开着：按钮贴【视口】右端（画布那一档的按钮位置滚一辈子也滚不到），名称列 /
         * 命令行列的【列宽】沿用画布口径，行内容与表头因此总是对齐。一行的排布是
         *   行首(pre) + 名称(n) + 间隔(gap) + 命令行(c) + 2 + 按钮(10，含 [↑][↓] 再 +6)
         * 段与段之间不留缝，所以「按钮贴右端」和「行首 + 名称不留缝」两件事不能同时要：
         * 让位顺序 = 丢命令行 → 丢列间隔 → 收窄名称列 → 丢 [↑][↓] → 连 [改][删] 一起丢
         * （退成「纯键盘」，靠 M/E/数字键操作）。 */
        const int nws[4] = { 12, 8, 4, 0 };       /* 0 = 名称列整列不画 */
        int nw_canvas = *name_w, cw_canvas = *cmd_w;
        int bc_btn = 1, bc_ud = 1, bc_n = 0, bc_c = 0, bc_bc = 0, bc_score = -1;
        for (int k = 0; k < 4; k++) {
            int btn = (k < 2), ud = (k == 0);
            int bw = btn ? (10 + (ud ? 6 : 0)) : 0;
            int bmin = avail - bw;            /* 按钮整排的左端不能比这更靠左 */
            for (int gap = 2; gap >= 0; gap -= 2) {   /* 列之间能留 2 列就别留 0 */
                for (int ni = 0; ni < 4; ni++) {
                    int n = nws[ni];
                    if (n > nw_canvas) continue;        /* 不比画布口径宽 */
                    int name_end = pre + n + (n ? gap : 0);
                    int bc = bmin;
                    if (!btn) bc = avail;
                    int mid = bc - name_end - (btn ? 2 : 0);
                    if (mid < 0) mid = 0;
                    int c = (mid > cw_canvas) ? cw_canvas : mid;
                    if (c < 2) c = 0;                   /* 放不下命令行整列 ⇒ 丢掉 */
                    int total = name_end + (c ? 2 + c : 0) + (btn ? bw : 0);
                    if (total > avail) continue;        /* 会溢出 ⇒ 这一档不成 */
                    /* 先保「这一页的主角」：能画按钮就画按钮（[↑][↓][改][删] 就是这一页
                     * 存在的理由），其次名称列越宽越好，然后才是命令行列。 */
                    /* 「有名字可看」优先（没有名字的话，按钮点了也不知道改的是谁）；
                     * 在保证这点之后，能画按钮就画（[↑][↓][改][删] 是这一页存在的原因）。 */
                    int score = (n ? 10000 : 0) + n * 500 + (c ? 40 : 0) + c + gap * 10 + (ud ? 8 : 0);
                    if (btn) score += (n >= 8 ? 12000 : -2000);   /* 按钮要么配得上名字，要么别画 */
                    if (score > bc_score) {
                        bc_score = score; bc_btn = btn; bc_ud = ud;
                        bc_n = n; bc_c = c; bc_bc = btn ? (name_end + (c ? 2 + c : 0)) : avail;
                    }
                }
            }
        }
        if (bc_score < 0) {                              /* 极端窄：只放行首 + 名称 */
            bc_n = nw_canvas; if (bc_n > avail - pre) bc_n = (avail - pre > 0) ? (avail - pre) : 0;
            bc_c = 0; bc_btn = 0; bc_ud = 0; bc_bc = avail;
        }
        if (show_btn) *show_btn = bc_btn;
        if (show_ud) *show_ud = bc_ud;
        if (btn_col) *btn_col = bc_bc;
        if (name_w) *name_w = bc_n;
        if (cmd_w) *cmd_w = bc_c;
        return;
    }
    const int nws[4] = { 12, 8, 4, 0 };       /* 0 = 名称列整列不画 */
    for (int variant = 0; variant < 4; variant++) {
        /* variant: 0 = 箭头+按钮；1 = 只按钮；2 = 按钮无命令行；3 = 什么都不画（纯键盘操作） */
        int ud = (variant == 0);
        int btn = (variant <= 2);
        int want_cmd = (variant <= 1);
        int btn_w = btn ? (10 + (ud ? 6 : 0)) : 0;
        for (int ni = 0; ni < 4; ni++) {
            int nw = nws[ni];
            int gap = nw ? 2 : 0;                     /* 名称列后的列间隔（列没了就不留） */
            if (pre + nw + gap > avail) continue;     /* 前缀 + 名称就超宽 */
            int cgap = want_cmd ? 2 : 0;               /* 命令行前后各留 2 列 */
            int cw = want_cmd ? (avail - (pre + nw + gap) - cgap - btn_w) : 0;
            if (cw > 30) cw = 30;
            if (want_cmd && cw < 2) continue;          /* 命令行放不下 ⇒ 这一档不成 */
            int total = pre + nw + gap + cgap + cw + btn_w;
            if (total > avail) continue;
            if (name_w) *name_w = nw;
            if (cmd_w) *cmd_w = want_cmd ? cw : 0;     /* 0 = 命令行整列不画 */
            if (show_btn) *show_btn = btn;
            if (show_ud) *show_ud = ud;
            if (btn_col) *btn_col = pre + nw + gap + cgap + cw + 2;
            return;
        }
    }
    /* 极端窄：只画行首 + 能塞下的几个字，剩下的靠 Enter 进详情页看。
     * v2.1.4：横滚开着时先别认输 —— 画布放得下富布局，视口放不下四键 + 名称的话，
     * 按「剩余宽度」裁名称（宁可命令行滚出来看），也别整列消失。 */
    int nw = avail - pre;
    if (pinned) {
        if (name_w) *name_w = (nw > 0) ? nw : 0;
        if (cmd_w) *cmd_w = 0;
        if (show_ud) *show_ud = 0;
        if (show_btn) *show_btn = 0;
        if (btn_col) *btn_col = avail;
        return;
    }
    if (nw < 0) nw = 0;
    if (name_w) *name_w = nw;
    if (cmd_w) *cmd_w = 0;
    if (show_ud) *show_ud = 0;
    if (show_btn) *show_btn = 0;
    if (btn_col) *btn_col = pre + nw + 2;
}
static void menu_widths(int host_cols, int main_left, int *cmd_w, int *btn_col, int *show_ud) {
    menu_geom(host_cols, main_left, NULL, cmd_w, btn_col, show_ud, NULL);
}
int settings_menu_cmd_w(int host_cols, int main_left) { int a, b, c; menu_widths(host_cols, main_left, &a, &b, &c); return a; }
int settings_menu_btn_col(int host_cols, int main_left) { int a, b, c; menu_widths(host_cols, main_left, &a, &b, &c); return b; }
int settings_menu_show_ud(int host_cols, int main_left) { int a, b, c; menu_widths(host_cols, main_left, &a, &b, &c); return c; }
int settings_menu_name_w(int host_cols, int main_left) { int n; menu_geom(host_cols, main_left, &n, NULL, NULL, NULL, NULL); return n; }
int settings_menu_pre_w(int host_cols, int main_left) { return menu_pre_w(host_cols, main_left); }
int settings_menu_show_btn(int host_cols, int main_left) { int b; menu_geom(host_cols, main_left, NULL, NULL, NULL, NULL, &b); return b; }
/* 行首（" ▶"/"  " + "[n] "）+ 名称 + 间隔 + 命令行 + 间隔 的总宽：表头按它对齐 */
/* 表头那一行：按实际列宽拼，保证不超过 avail（超了会被终端折行盖掉下一行）。 */
int settings_menu_header(char *buf, int bs, int host_cols, int main_left) {
    int nw, cw, bc, ud, btn;
    menu_geom(host_cols, main_left, &nw, &cw, &bc, &ud, &btn);   /* 只在没开横滚时被调用 */
    int avail = host_cols - main_left - 1;
    int pre = menu_pre_w(host_cols, main_left);
    char head[192];
    /* 表头的列起点必须和行完全一致：pre(" ▶[n] ") + nw + 2 + cw + 2；被丢掉的列连标题一起丢掉。 */
    size_t hp = 0;
    for (int i = 0; i < pre - 3; i++) head[hp++] = ' ';
    hp += (size_t)snprintf(head + hp, sizeof(head) - hp, "序号");
    if (nw) hp += (size_t)snprintf(head + hp, sizeof(head) - hp, "  %-*s", nw, "显示名称");
    if (cw > 0) hp += (size_t)snprintf(head + hp, sizeof(head) - hp, "  %-*s", cw, "启动命令行");
    if (btn) hp += (size_t)snprintf(head + hp, sizeof(head) - hp, "  操作");
    (void)bc; (void)ud;
    int w = utf8_cols(head, (int)strlen(head)) + pre;
    if (w > avail) { head[0] = 0; if (buf && bs > 0) buf[0] = 0; return 0; }
    if (buf && bs > 0) snprintf(buf, bs, "%s", head);
    return w;
}


/* v2.1.4：启动项页与「条目管理」页共用的一张表（只有行号换算不同，其余逐像素一致）。 */
void render_menu_rows(MenuRowCtx *rc) {
    char *out = rc->out; int bs = rc->bs; int *posp = rc->pos;
    int host_rows = rc->host_rows, host_cols = rc->host_cols, main_left = rc->main_left;
    int h_on = rc->h_on;
    int mname = h_on ? rc->nw : settings_menu_name_w(host_cols, main_left);
    int mcw = h_on ? rc->cw2 : settings_menu_cmd_w(host_cols, main_left);
    int mbtn = h_on ? rc->bc : settings_menu_btn_col(host_cols, main_left);
    int mud = h_on ? rc->ud : settings_menu_show_ud(host_cols, main_left);
    int mbtn_on = (h_on ? rc->btn : settings_menu_show_btn(host_cols, main_left)) && rc->show_ops;
    if (!rc->show_ops) { mud = 0; }
    int mpre = settings_menu_pre_w(h_on ? rc->cw + main_left + 1 : host_cols, main_left);
    int mcmd_on = (mcw > 0);
    /* 横滚模式下按钮钉在视口右端（与 settings_menu_table_geom 同一式），
     * 非横滚时按钮列就是画布列，h_sc 恒为 0。 */
    /* 横滚时按钮由 settings_menu_table_geom 钉在视口右端；行首用 lpin 钉在左端。 */
    int ecol = mbtn + (mud ? 6 : 0);
    int vmbtn = mbtn, vecol = ecol;
    int pos = *posp;
    for (int i = 0; i < g_chooser_item_count; i++) {
        int r = rc->row_view(host_rows, 10 + i);
        if (r < 0) continue;
        int row_hover = (g_mouse_y == r - 1 && g_mouse_x >= main_left - 1 && g_mouse_x < host_cols);
        int row_focus = rc->show_ops ? (i == rc->sel) : 0;
        int h_up = (mud && row_hover && g_mouse_x >= main_left + vmbtn && g_mouse_x <= main_left + vmbtn + 2);
        int h_dn = (mud && row_hover && g_mouse_x >= main_left + vmbtn + 3 && g_mouse_x <= main_left + vmbtn + 5);
        int h_ed = (mbtn_on && row_hover && g_mouse_x >= main_left + vecol && g_mouse_x <= main_left + vecol + 3);
        int h_del = (mbtn_on && row_hover && g_mouse_x >= main_left + vecol + 4 && g_mouse_x <= main_left + vecol + 7);
        char dname[32] = {0}; format_name_display(dname, sizeof(dname), g_chooser_items[i].name);
        char dcmd[64] = {0}; format_cmd_display(dcmd, sizeof(dcmd), g_chooser_items[i].cmd);
        if (h_on) {   /* 横滚：段宽由视口几何给，名称/命令行要先按列裁（否则会把按钮顶出屏） */
            char tnm[32], tcm[64];
            fit_cols(tnm, sizeof(tnm), dname, mname);
            fit_cols(tcm, sizeof(tcm), dcmd, mcw ? mcw : 0);
            snprintf(dname, sizeof(dname), "%s", tnm);
            snprintf(dcmd, sizeof(dcmd), "%s", tcm);
        }
        const char *row_bg = (row_focus && !h_up && !h_dn && !h_ed && !h_del) ? "\x1b[048;2;038;050;068m" :
                             ((row_hover && !h_up && !h_dn && !h_ed && !h_del) ? "\x1b[048;2;027;033;044m" : "");
        char row_tag[16];
        snprintf(row_tag, sizeof(row_tag), "[%d]", i + 1);
        if (h_on) {
            char tbuf[48], cbuf[192];
            SettingsSeg sg[8]; int ns = 0;
            int cmd_start = mpre + (mname ? mname + 2 : 0);
            snprintf(tbuf, sizeof(tbuf), "%s%s", (row_focus ? "\xe2\x96\xb6" : " "), row_tag);
            snprintf(cbuf, sizeof(cbuf), "%s", dcmd);
            sg[ns].sgr = row_bg; sg[ns].text = tbuf; sg[ns].pad_to_col = mpre; ns++;
            sg[ns].sgr = "\x1b[038;2;230;237;243;1m"; sg[ns].text = dname;
            sg[ns].pad_to_col = mcmd_on ? cmd_start : (mbtn_on ? mbtn : 0); ns++;
            if (mcmd_on) { sg[ns].sgr = "\x1b[038;2;139;148;158m"; sg[ns].text = cbuf; sg[ns].pad_to_col = mbtn; ns++; }
            if (mbtn_on) {
                if (mud) {
                    sg[ns].sgr = "\x1b[0m"; sg[ns].text = ""; sg[ns].pad_to_col = mbtn; ns++;
                    sg[ns].sgr = h_up ? "\x1b[048;2;063;185;080m\x1b[038;2;013;017;023;1m" : TB_BG "\x1b[038;2;063;185;080m";
                    sg[ns].text = "[\xe2\x86\x91]"; sg[ns].pad_to_col = 0; ns++;
                    sg[ns].sgr = h_dn ? "\x1b[048;2;217;119;054m\x1b[038;2;013;017;023;1m" : TB_BG "\x1b[038;2;217;119;054m";
                    sg[ns].text = "[\xe2\x86\x93]"; sg[ns].pad_to_col = 0; ns++;
                    sg[ns].sgr = "\x1b[0m"; sg[ns].text = ""; sg[ns].pad_to_col = ecol; ns++;
                }
                sg[ns].sgr = h_ed ? "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m" : TB_BG "\x1b[038;2;121;192;255m";
                sg[ns].text = "[\xe6\x94\xb9]"; sg[ns].pad_to_col = 0; ns++;
                sg[ns].sgr = h_del ? "\x1b[048;2;248;081;073m\x1b[038;2;255;255;255;1m" : TB_BG "\x1b[038;2;248;081;073m";
                sg[ns].text = "[\xe5\x88\xa0]"; sg[ns].pad_to_col = 0; ns++;
            }
            pos = *posp;
            settings_hline2(out, bs, &pos, r, main_left, host_cols, rc->cw, sg, ns,
                            mpre + mname + (mcmd_on ? 2 + mcw : 0), mpre, 0);
            *posp = pos;
            continue;
        }
        int row_cols = 0;
        /* 行首：聚焦行 " ▶"（前导空格 + ▶），非聚焦行 "  "（两空格）。▶ 在
         * 本代码库按宽 1 渲染，所以两种行首都是 2 列——列起点逐行一致，[↑] 等
         * 按钮与硬编码热区不错位。（v1.8.39 曾误删聚焦行前导空格，使聚焦行整行
         * 左移 1 列，菜单项管理的当前项显得靠左。） */
        const char *tail = (mpre >= 6) ? "  " : ((mpre >= 3) ? "" : " ");
        if (mpre >= 6)
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s %s\x1b[038;2;210;153;034m%s\x1b[0m%s%s",
                            r, main_left, row_bg, (row_focus ? "\xe2\x96\xb6" : " "), row_tag, row_bg, tail);
        else
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s\x1b[038;2;210;153;034m%s\x1b[0m%s",
                            r, main_left, row_bg, row_tag, tail);
        row_cols = mpre;

        /* %-Ns pads bytes, not terminal columns.  Build both fixed
         * columns with the same UTF-8 display-width helper used by the
         * renderer's other tables so CJK names cannot move the buttons. */
        pos += snprintf(out + pos, bs - pos, "\x1b[038;2;230;237;243;1m");
        append_padded_utf8(out, bs, &pos, &row_cols, dname, mname);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m%s  ", row_bg);
        row_cols += 2;
        if (mcmd_on) {
            pos += snprintf(out + pos, bs - pos, "\x1b[038;2;139;148;158m");
            append_padded_utf8(out, bs, &pos, &row_cols, dcmd, mcw);
            pos += snprintf(out + pos, bs - pos, "\x1b[0m%s  ", row_bg);
            row_cols += 2;
        }

        /* 按钮非 hover 时也要给一块自己的面板底色（022;027;034），否则聚焦/悬停
         * 行的整行底色会透过按钮文字格显示出来（行底色盖到 [↑] 上）；hover 时用
         * 按钮各自的高亮底色，行底色与按钮底色不叠加（行 bg 已排除按钮列）。 */
        if (mbtn_on) {
        if (mud) {
            pos += snprintf(out + pos, bs - pos, "%s[\xe2\x86\x91]\x1b[0m", h_up ? "\x1b[048;2;063;185;080m\x1b[038;2;013;017;023;1m" : TB_BG "\x1b[038;2;063;185;080m");
            pos += snprintf(out + pos, bs - pos, "%s[\xe2\x86\x93]\x1b[0m", h_dn ? "\x1b[048;2;217;119;054m\x1b[038;2;013;017;023;1m" : TB_BG "\x1b[038;2;217;119;054m");
        }
        pos += snprintf(out + pos, bs - pos, "%s[\xe6\x94\xb9]\x1b[0m", h_ed ? "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m" : TB_BG "\x1b[038;2;121;192;255m");
        pos += snprintf(out + pos, bs - pos, "%s[\xe5\x88\xa0]\x1b[0m", h_del ? "\x1b[048;2;248;081;073m\x1b[038;2;255;255;255;1m" : TB_BG "\x1b[038;2;248;081;073m");
        }
    }
    *posp = pos;
}


static void render_settings_keys(char *out, int bs, int *posp, int host_rows, int host_cols, int main_left) {
    int pos = *posp;
    int name_w = settings_keys_name_w(host_cols, main_left);
    int desc_w = settings_keys_desc_w(host_cols, main_left);
    int combo_w = settings_keys_combo_w(host_cols, main_left);
    int prefix_col = settings_keys_prefix_col(host_cols, main_left);
    int edit_col = settings_keys_edit_col(host_cols, main_left);
    int reset_col = settings_keys_reset_col(host_cols, main_left);
    settings_line_begin(out, bs, &pos, 3, main_left, host_cols, "\x1b[038;2;121;192;255;1m");
        settings_line_text(out, bs, &pos, "■ 键位 (Key Bindings)");
        settings_line_sgr(out, bs, &pos, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "   ↑/↓ 选择, Enter 录制新键, R 复位, Ctrl+R 全部复位");
        settings_line_end(out, bs, &pos);
    settings_line_begin(out, bs, &pos, 4, main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "所有改动写入 termux.ini 的 [general] prefix 与 [keys] 段；帮助页会同步显示。");
        settings_line_end(out, bs, &pos);
    {
        /* 表头与数据列严格对齐：列起点（相对 main_left，0 基）：
         * 标记 0..2、动作名 3..22(宽20)、说明 23..58(宽36)、当前键位 59..78(宽20)、
         * [前缀]@79、[改]@87、[复位]@92（按钮用绝对列常量）。表头段宽度与数据列
         * 逐列相同（标记段 pad 到 3、动作名段 pad 到 20、说明段 pad 到 36、键位段
         * pad 到 20）。 */
        int hc = 0;
        pos += snprintf(out + pos, bs - pos, "\x1b[5;%dH\x1b[038;2;121;192;255;1m", main_left);
        append_padded_utf8(out, bs, &pos, &hc, "   动作名", 3 + name_w);
        if (desc_w) append_padded_utf8(out, bs, &pos, &hc, "说明", desc_w);
        append_padded_utf8(out, bs, &pos, &hc, "当前键位", combo_w);
        settings_line_end(out, bs, &pos);   /* 表头不裁剪：按钮标题按剩余宽度自然被 ?7l 截 */
        pos += snprintf(out + pos, bs - pos, "\x1b[5;%dH\x1b[038;2;121;192;255;1m%s\x1b[0m",
                        main_left + prefix_col,
                        settings_keys_show_reset(host_cols, main_left) ? "前缀   操作" : "前缀 操作");
    }

    settings_keys_clamp_scroll(host_rows);
    int total = settings_keys_rows();
    for (int entry = 0; entry < total; entry++) {
        int row = settings_keys_row_at(host_rows, entry);
        if (row < 0) continue;
        int selected = (g_settings_keys_sel == entry);
        /* 整行 hover 热区只覆盖文字区（到 [前缀] 按钮前一列），不含右侧按钮列
         * （[前缀]/[改]/[复位]）：按钮有自己的高亮底色，若整行 hover 也亮，鼠标
         * 停在按钮上时行底色和按钮底色会叠加，看上去「文字行的 hover 带到了按钮
         * 上」。鼠标在任一按钮列上时整行不亮底，只让被悬停的那个按钮亮。 */
        int keys_on_btn = (entry > 0 &&
                           g_mouse_x >= main_left + prefix_col - 1 &&
                           g_mouse_x < main_left + (settings_keys_show_reset(host_cols, main_left) ? reset_col + 5 : edit_col + 4));
        int hovered = (g_mouse_y == row - 1 && g_mouse_x >= main_left - 1 &&
                       g_mouse_x < main_left + prefix_col - 1 && !keys_on_btn);
        int capturing = (g_key_capture_active && selected);

        /* v1.8.44：label 缓冲必须放得下最长中文说明（UTF-8 多字节），旧的
         * label[24] 会被 snprintf 直接截断；label[80] 足够。 */
        char name[48], label[80], combo[64];
        int custom = 0;
        if (entry == 0) {
            snprintf(name, sizeof(name), "prefix");
            snprintf(label, sizeof(label), "前缀键");
            /* 界面显示 Ctrl+B，而不是 ini 里的 C-b 写法。 */
            keymap_prefix_describe(combo, sizeof(combo));
            custom = !keymap_prefix_is_default();
        } else {
            int action = keymap_action_at(entry - 1);
            snprintf(name, sizeof(name), "%s", keymap_action_name(action));
            snprintf(label, sizeof(label), "%s", keymap_action_label(action));
            keymap_describe(action, combo, sizeof(combo));
            if (!combo[0]) snprintf(combo, sizeof(combo), "(未绑定)");
            custom = keymap_action_is_overridden(action);
        }
        if (capturing) snprintf(combo, sizeof(combo), "按下新键…");

        int cols = 0;
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s %s ",
                        row, main_left, settings_row_style(selected, hovered), selected ? "▶" : " ");
        cols += 3;
        /* 列宽（v1.8.44，相对 main_left 的偏移）：
         *   标记 0..2、动作名 3..22(宽20)、说明 23..58(宽36)、键位 59..78(宽20)；
         *   右侧按钮 [前缀]@79 [改]@87 [复位]@92（绝对列常量）。
         * 说明列宽 36 容纳最长中文说明；动作名是英文标识（split-horizontal-pane 等，
         * 最长 19 列）给 20；键位组合（"Ctrl+B Shift+tab"=18 列、自定义更长）给 20。 */
        append_padded_utf8(out, bs, &pos, &cols, name, name_w);
        if (desc_w) append_padded_utf8(out, bs, &pos, &cols, label, desc_w);
        pos += snprintf(out + pos, bs - pos, "%s", capturing ? "\x1b[038;2;210;153;034;1m" : "");
        append_padded_utf8(out, bs, &pos, &cols, combo, combo_w);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m");

        /* 是否需要先按前缀键，可以按 P 或点这里切换。按钮高亮独立判断行/列，
         * 不依赖整行 hover（hover 在按钮列上被关掉，避免行底色与按钮底色叠加）。 */
        int row_under_mouse = (g_mouse_y == row - 1);
        if (entry > 0) {
            int action = keymap_action_at(entry - 1);
            int uses_prefix = keymap_action_uses_prefix(action);
            int h_prefix = (row_under_mouse && g_mouse_x >= main_left + prefix_col - 1 &&
                            g_mouse_x < main_left + prefix_col + 5);
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s%s\x1b[0m",
                            row, main_left + prefix_col,
                            h_prefix ? "\x1b[048;2;137;087;229m\x1b[038;2;255;255;255;1m"
                                     : (uses_prefix ? "\x1b[038;2;139;148;158m" : "\x1b[038;2;063;185;080;1m"),
                            uses_prefix ? "[前缀]" : "[直接]");
        }

        int show_reset = settings_keys_show_reset(host_cols, main_left);
        int h_edit = (row_under_mouse && g_mouse_x >= main_left + edit_col - 1 &&
                      g_mouse_x < main_left + (show_reset ? reset_col : edit_col + 4) - 1);
        int h_reset = (show_reset && row_under_mouse && g_mouse_x >= main_left + reset_col - 1 &&
                       g_mouse_x < main_left + reset_col + 5);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s[改]\x1b[0m",
                        row, main_left + edit_col,
                        h_edit ? "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m" : "\x1b[038;2;121;192;255m");
        if (show_reset)
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s[复位]\x1b[0m",
                            row, main_left + reset_col,
                            h_reset ? "\x1b[048;2;248;081;073m\x1b[038;2;255;255;255;1m"
                                    : (custom ? "\x1b[038;2;248;081;073m" : "\x1b[038;2;048;054;061m"));
    }

    int hint_r = host_rows;
    settings_line_begin(out, bs, &pos, hint_r, main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos,
        g_key_capture_active ? "请按下新的组合键（Esc 取消）；修饰键单独按无效。"
                             : "提示: Enter/[改] 录制, P/[前缀] 切换是否需要前缀, R/[复位] 默认, Esc 返回");
        settings_line_end(out, bs, &pos);
    *posp = pos;
}

static void render_settings_behavior(char *out, int bs, int *posp, int host_rows, int host_cols, int main_left) {
    int pos = *posp;
    (void)host_cols;
    settings_line_begin(out, bs, &pos, settings_behavior_row_view(host_rows, 3),
                        main_left, host_cols, "\x1b[038;2;121;192;255;1m");
        settings_line_text(out, bs, &pos, "■ 行为 (Behavior)");
        settings_line_end(out, bs, &pos);
    settings_line_begin(out, bs, &pos, settings_behavior_row_view(host_rows, 4),
                        main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "↑/↓ 选择，Space/Enter 切换开关，←/→ 调整数值（scrollback 步进 1000）：");
        settings_line_end(out, bs, &pos);

    struct { const char *key; const char *desc; int value; } toggles[SETTINGS_BEHAVIOR_TOGGLES] = {
        {"mouse",           "鼠标支持（标签点击 / 拖选 / 滚轮）", g_mouse_enabled},
        {"copy_move_deselect", "复制模式直接移动（无 Shift/Alt）丢弃高亮", g_copy_move_deselect},
        {"confirm_on_exit", "退出 termux 前二次确认",             g_confirm_on_exit},
        {"confirm_on_close", "关闭窗格 / 标签前二次确认",          g_confirm_on_close},
        {"search_case_sensitive", "搜索锁定大小写（区分大小写）",  g_search_case_sensitive},
    };
    for (int i = 0; i < SETTINGS_BEHAVIOR_TOGGLES; i++) {
        int row = settings_behavior_row_view(host_rows, SETTINGS_BEHAVIOR_ROW0 + i);
        if (row < 0) continue;   /* 滚出可见区 */
        int selected = (g_settings_behavior_sel == i);
        int hovered = (g_mouse_y == row - 1 && g_mouse_x >= main_left - 1 && g_mouse_x < main_left + 60);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s %s %-22s \x1b[0m%s%s\x1b[0m",
                        row, main_left, settings_row_style(selected, hovered),
                        toggles[i].value ? "[x]" : "[ ]", toggles[i].key,
                        settings_row_style(selected, hovered), toggles[i].desc);
    }

    int sb_row = settings_behavior_row_view(host_rows, SETTINGS_BEHAVIOR_ROW0 + SETTINGS_BEHAVIOR_TOGGLES);
    if (sb_row > 0) {
        int selected = (g_settings_behavior_sel == SETTINGS_BEHAVIOR_TOGGLES);
        /* scrollback 行右侧有 [-] / [+] 按钮，整行 hover 只覆盖文字区（到 [-]
         * 前一列）；按钮有自己的高亮，避免行底色与按钮底色叠加。 */
        int row_w0 = settings_right_row_limit(host_rows, host_cols, main_left, sb_row);
        if (row_w0 < 0) row_w0 = host_cols - main_left;
        int sb_minus = (row_w0 >= SETTINGS_SB_MINUS_COL + 3);
        int sb_plus = sb_minus && (row_w0 >= SETTINGS_SB_PLUS_COL + 3);
        int sb_on_btn = (g_mouse_x >= main_left + SETTINGS_SB_MINUS_COL - 1 &&
                         g_mouse_x < main_left + SETTINGS_SB_PLUS_COL + 3);
        int hovered = (g_mouse_y == sb_row - 1 && g_mouse_x >= main_left - 1 &&
                       g_mouse_x < main_left + SETTINGS_SB_MINUS_COL - 1 && !sb_on_btn);
        int sb_row_under_mouse = (g_mouse_y == sb_row - 1);
        int h_minus = (sb_row_under_mouse && sb_minus && g_mouse_x >= main_left + SETTINGS_SB_MINUS_COL - 1 &&
                       g_mouse_x < main_left + SETTINGS_SB_MINUS_COL + 2);
        int h_plus = (sb_row_under_mouse && sb_plus && g_mouse_x >= main_left + SETTINGS_SB_PLUS_COL - 1 &&
                      g_mouse_x < main_left + SETTINGS_SB_PLUS_COL + 2);
        /* v2.1.3：这一行原先一律按固定列位排版（[-] 在 +22、值在 +26、[+] 在 +33、
         * 「(对之后新建的 pane 生效)」在 +37）⇒ 40~50 列时行尾直接冲进左侧栏那一列，
         * 把分隔线 │ 连同侧栏 [B]/[W] 一起擦掉（实测 14x40 的 r12、24x30 的 r8）。
         * 现在整行受「行宽 row_w = host_cols - main_left」约束（和 settings_line_begin
         * 同一口径）：装不下就从右往左依次舍弃 括注 → [+] → [-]，label 的补位空格同步压缩。 */
        int row_w = row_w0;
        int sb_val = sb_minus && (SETTINGS_SB_MINUS_COL + 4 + 8 <= row_w);
        int sb_note = sb_plus && (SETTINGS_SB_PLUS_COL + 4 + 22 <= row_w);
        {
            char lab[32];
            snprintf(lab, sizeof(lab), "     %s", "scrollback");
            int hc = 0;
            settings_line_begin(out, bs, &pos, sb_row, main_left, host_cols + 1,
                                settings_row_style(selected, hovered));
            g_sl_left = row_w;                          /* 本行自己逐段记账 */
            append_padded_utf8(out, bs, &pos, &hc, lab, sb_minus ? SETTINGS_SB_MINUS_COL - 1 : row_w);
            pos += snprintf(out + pos, bs - pos, "\x1b[0m");
            g_sl_left -= hc;
        }
        if (sb_minus) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s[-]\x1b[0m", sb_row,
                            main_left + SETTINGS_SB_MINUS_COL,
                            h_minus ? "\x1b[048;2;217;119;054m\x1b[038;2;013;017;023;1m" : "\x1b[038;2;217;119;054m");
            g_sl_left -= 3;
        }
        if (sb_val) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;230;237;243;1m%6d\x1b[0m 行",
                            sb_row, main_left + SETTINGS_SB_MINUS_COL + 4, g_scrollback_lines);
            g_sl_left -= 8;
        }
        if (sb_plus) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s[+]\x1b[0m", sb_row,
                            main_left + SETTINGS_SB_PLUS_COL,
                            h_plus ? "\x1b[048;2;063;185;080m\x1b[038;2;013;017;023;1m" : "\x1b[038;2;063;185;080m");
            g_sl_left -= 3;
        }
        if (sb_note) {
            /* 括注是从 SETTINGS_SB_PLUS_COL + 4 起【绝对定位】另起一段的，预算必须按
             * 它自己的起点重算：沿用 label+按钮记账后剩下的 g_sl_left 会把这段裁到 9 列，
             * 80 列宽终端上「(对之后新建的 pane 生效)」就整段没了。 */
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH", sb_row, main_left + SETTINGS_SB_PLUS_COL + 4);
            g_sl_left = host_cols - main_left - (SETTINGS_SB_PLUS_COL + 4);
            settings_line_sgr(out, bs, &pos, "\x1b[038;2;139;148;158m");
            settings_line_text_e(out, bs, &pos, "(对之后新建的 pane 生效)", 0);
            pos += snprintf(out + pos, bs - pos, "\x1b[0m");
        }
        settings_line_end(out, bs, &pos);
    }

    int hint_r = settings_behavior_row_view(host_rows, SETTINGS_BEHAVIOR_ROW0 + SETTINGS_BEHAVIOR_TOGGLES + 3);
    /* v2.1.3：滚出去了就不画，别兜到末行去盖侧栏的 [Ctrl+S] 保存行。 */
    settings_line_begin(out, bs, &pos, hint_r, main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "提示: Space/Enter 切换, ←/→ 调整 scrollback, ↑/↓ 越界自动翻页, Ctrl+S 保存, Esc 返回");
        settings_line_end(out, bs, &pos);
        settings_page_mark(out, bs, &pos, hint_r, host_cols, host_rows,
                           3, SETTINGS_BEHAVIOR_LAST, &g_settings_behavior_scroll);
    *posp = pos;
}

void render_settings_panel(char *out, int bs, int *posp, int host_rows, int host_cols) {
    int pos = *posp;
    settings_tip_reset();          /* v2.1.0：截断行登记表每帧重建 */
    g_sl_host_rows = host_rows;    /* v2.1.3：给 settings_line_begin 的逐行限宽用 */
    SettingsSidebarGeom sbg;
    settings_sidebar_geom(host_rows, g_chooser_item_count, &sbg);
    int sb_w = settings_host_sidebar_w(host_cols);

    for (int y = 0; y < host_rows; y++) {
        int r = y + 2;
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H\x1b[0m\x1b[K", r);
    }

    pos += snprintf(out + pos, bs - pos, "\x1b[2;1H\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m  *  termux - 设置面板 (Settings Panel)");
    int hdr_used = utf8_cols("  *  termux - 设置面板 (Settings Panel)", (int)strlen("  *  termux - 设置面板 (Settings Panel)"));
    while (hdr_used < host_cols && pos < bs - 8) { out[pos++] = ' '; hdr_used++; }
    pos += snprintf(out + pos, bs - pos, "\x1b[0m");

    for (int y = 1; y < host_rows; y++) {
        int r = y + 2;
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;048;054;061m│\x1b[0m", r, sb_w);
    }

    if (sbg.nav_label) {          /* 矮终端：表头/分隔行先省掉，把行留给入口 */
        char navlab[64];
        int shown0 = sbg.items_scroll + 1;
        int shown1 = sbg.items_scroll + sbg.items_cap;
        if (sbg.items_cap < g_chooser_item_count)
            snprintf(navlab, sizeof(navlab), "  导航选项 (%d-%d/%d)", shown0, shown1, g_chooser_item_count);
        else
            snprintf(navlab, sizeof(navlab), "  导航选项");
        /* v2.1.3：侧栏每行都必须停在分隔线之前 —— 原先写死的整串（21~22 列）在 30 列
         * 终端（sb_w=15）直接溢出，折到下一行开头把侧栏自己的条目顶掉。 */
        char navfit[96];
        sidebar_clip(navfit, sizeof(navfit), navlab, sb_w - 1);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H\x1b[038;2;121;192;255;1m%s\x1b[0m", sbg.nav_label, navfit);
    }
    if (sbg.sep1 && sb_w > 1) {
        char sep[128];
        sidebar_fill(sep, sizeof(sep), "─", sb_w - 2);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H\x1b[038;2;048;054;061m%s\x1b[0m", sbg.sep1, sep);
    }

    int is_sel0 = (g_settings_nav == 0);
    int h_start = (g_mouse_y == sbg.start - 1 && g_mouse_x >= 0 && g_mouse_x < sb_w);
    const char *start_style = is_sel0 ? (h_start ? "\x1b[048;2;048;075;110m\x1b[038;2;255;255;255;1m" : "\x1b[048;2;038;060;088m\x1b[038;2;121;192;255;1m")
                                      : (h_start ? "\x1b[048;2;033;038;045m\x1b[038;2;255;255;255;1m" : "\x1b[038;2;230;237;243m");
    {   /* v2.1.3：这一行也裁到 sb_w - 1（原先写死 20 列，30 终端溢出折行） */
        char lab[64], fit[96];
        snprintf(lab, sizeof(lab), "  %s 启动 (Startup)  ", (is_sel0 ? "▶" : " "));
        sidebar_clip(fit, sizeof(fit), lab, sb_w - 1);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H%s%s\x1b[0m", sbg.start, start_style, fit);
    }

    for (int i = 0; i < sbg.items_cap; i++) {
        int it = sbg.items_scroll + i;            /* v2.1.2：窗口内第 i 行 = 第 it 个菜单项 */
        if (it < 0 || it >= g_chooser_item_count) continue;
        int r = sbg.items_row0 + i;
        if (r > host_rows - 3) break;
        int is_sel = (g_settings_nav == it + 1);      /* 选中项按「自然项」比，不是窗口内序号 */
        int h_item = (g_mouse_y == r - 1 && g_mouse_x >= 0 && g_mouse_x < sb_w);
        const char *item_style = is_sel ? (h_item ? "\x1b[048;2;048;075;110m\x1b[038;2;255;255;255;1m" : "\x1b[048;2;038;060;088m\x1b[038;2;121;192;255;1m")
                                        : (h_item ? "\x1b[048;2;033;038;045m\x1b[038;2;255;255;255;1m" : "\x1b[038;2;230;237;243m");
        /* v2.1.2：名字按侧栏宽度截断并补「...」，同时登记悬停气泡（原先固定 %-10s，
         * 10 列以后的名字直接消失，既没有省略号也查不到全文 —— 用户报「可以触发旁边的
         * 悬停提示」却只有右侧窗格登记了气泡）。
         * 这里【不能】借用 settings_line_*：它按 g_sl_left 记账，会吃掉下一次 begin
         * 之前的预算，把侧栏下面的 [+] / [A] 等行整段挤掉（实测过，别再来一遍）。 */
        char line[96];
        snprintf(line, sizeof(line), "  %s [%d] ", is_sel ? "▶" : " ", it + 1);
        int used = utf8_cols(line, (int)strlen(line));
        int room = sb_w - 2 - used;                 /* v2.1.3：留出右侧 │ 那一列，宽字符舍入也不能踩到 */
        if (room < 4) room = 4;
        const char *nm = g_chooser_items[it].name;
        int nw = utf8_cols(nm, (int)strlen(nm));
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H%s%s", r, item_style, line);
        if (nw <= room) {
            pos += snprintf(out + pos, bs - pos, "%s", nm);
        } else {
            char cut[64];
            int ci = 0, cc = 0;
            for (int s0 = 0; s0 < (int)sizeof(cut) - 4; s0 = ci) {
                int adv = 0;
                unsigned cp = utf8_decode_cp(nm + s0, (int)strlen(nm) - s0, &adv);
                if (adv <= 0) break;
                int cw = is_wide_cp(cp) ? 2 : 1;
                if (cc + cw > room - 3) break;
                for (int q = 0; q < adv && ci < (int)sizeof(cut) - 4; q++) cut[ci++] = nm[s0 + q];
                cc += cw;
            }
            cut[ci++] = '.'; cut[ci++] = '.'; cut[ci++] = '.'; cut[ci] = 0;
            pos += snprintf(out + pos, bs - pos, "%s", cut);
            if (g_tip_n < SETTINGS_TIP_MAX) {
                SettingsTip *t = &g_tips[g_tip_n++];
                t->row = r; t->col = used + 1; t->len = used + nw;
                /* 气泡正文的拷贝必须停在【字符】边界上：snprintf 按字节截断会把半个
                 * UTF-8 序列留在串尾，渲染时解码成 '?'，框宽也会跟着差一列。 */
                int nb = 0;
                int s0 = 0, nmlen = (int)strlen(nm);
                while (s0 < nmlen) {
                    int adv = 0;
                    (void)utf8_decode_cp(nm + s0, nmlen - s0, &adv);
                    if (adv <= 0 || s0 + adv > nmlen) break;
                    if (nb + adv >= (int)sizeof(t->full) - 4) break;   /* 留位给「...」 */
                    for (int q = 0; q < adv; q++) t->full[nb++] = nm[s0 + q];
                    s0 += adv;
                }
                if (s0 < nmlen) { t->full[nb++]='.'; t->full[nb++]='.'; t->full[nb++]='.'; }
                t->full[nb] = 0;
            }
        }
        pos += snprintf(out + pos, bs - pos, "\x1b[0m");
    }

    int items_r = sbg.items;
    if (items_r > 0 && items_r <= host_rows - 2) {
        /* v2.1.4：[+] 添加新条目 / [P] 快速预设库 → 一条「[M] 条目管理」入口。 */
        int is_sel = (g_settings_nav == SETTINGS_NAV_ITEMS);
        int h_it = (g_mouse_y == items_r - 1 && g_mouse_x >= 0 && g_mouse_x < sb_w);
        char fit[96];
        sidebar_clip(fit, sizeof(fit), "  [M] 条目管理      ", sb_w - 1);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H%s%s\x1b[0m", items_r,
                        is_sel ? "\x1b[048;2;031;111;235m\x1b[038;2;255;255;255;1m"
                               : (h_it ? "\x1b[048;2;063;185;080m\x1b[038;2;013;017;023;1m" : "\x1b[038;2;063;185;080;1m"), fit);
    }

    int app_r = sbg.app, keys_r = sbg.keys, beh_r = sbg.beh;
    const struct { int row; const char *label; int nav; } extra_nav[4] = {
        {app_r,  "  [A] 外观 / 主题   ", SETTINGS_NAV_APPEARANCE},
        {keys_r, "  [K] 键位设置      ", SETTINGS_NAV_KEYS},
        {beh_r,  "  [B] 行为开关      ", SETTINGS_NAV_BEHAVIOR},
        {sbg.pane,   "  [W] 窗格配色      ", SETTINGS_NAV_PANE},
    };
    for (int i = 0; i < 4; i++) {
        int row = extra_nav[i].row;
        if (row > host_rows - 1) break;
        int is_sel = (g_settings_nav == extra_nav[i].nav);
        int hovered = (g_mouse_y == row - 1 && g_mouse_x >= 0 && g_mouse_x < sb_w);
        /* v2.1.3：这四条原本写死 18~20 列的定宽串。侧栏被窄终端夹到 15~19 列时，
         * 行尾那几个补齐用的空格正好落在分隔线 │ 那一列上，把它擦成空格（40 列实测
         * [A] 那行的 │ 就是这么丢的）。改成按 sb_w-1 现算，线永远保住。 */
        char fit[64];
        sidebar_clip(fit, sizeof(fit), extra_nav[i].label, sb_w - 1);   /* v2.1.3 */
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H%s%s\x1b[0m",
                        row, settings_row_style(is_sel, hovered), fit);
    }

    int save_r = host_rows;
    int h_save_btn = (g_mouse_y == save_r - 1 && g_mouse_x >= 0 && g_mouse_x < sb_w);
    {
        char sv[64];
        sidebar_clip(sv, sizeof(sv), " [Ctrl+S] 保存配置  ", sb_w - 1);   /* v2.1.3 */
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H%s%s\x1b[0m", save_r, h_save_btn ? "\x1b[048;2;063;185;080m\x1b[038;2;013;017;023;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;063;185;080;1m", sv);
    }

    int main_left = sb_w + 3;
    int right_max_w = host_cols - main_left - 2;
    if (right_max_w < 10) right_max_w = 10;

    if (g_settings_nav == SETTINGS_NAV_APPEARANCE) {
        render_settings_appearance(out, bs, &pos, host_rows, host_cols, main_left);
    } else if (g_settings_nav == SETTINGS_NAV_KEYS) {
        render_settings_keys(out, bs, &pos, host_rows, host_cols, main_left);
    } else if (g_settings_nav == SETTINGS_NAV_BEHAVIOR) {
        render_settings_behavior(out, bs, &pos, host_rows, host_cols, main_left);
    } else if (g_settings_nav == SETTINGS_NAV_PANE) {
        render_settings_pane(out, bs, &pos, host_rows, host_cols, main_left);
    } else if (g_settings_nav == SETTINGS_NAV_ITEMS) {
        render_settings_items(out, bs, &pos, host_rows, host_cols, main_left);
    } else if (g_settings_nav == 0) {
        int s0 = settings_startup_row_view(host_rows, 3);
        int s4 = settings_startup_row_view(host_rows, 4);
        int s5 = settings_startup_row_view(host_rows, 5);
        settings_line_begin(out, bs, &pos, s0, main_left, host_cols, "\x1b[038;2;121;192;255;1m");
        settings_line_text(out, bs, &pos, "■ 默认启动项设置 (Default Startup Item)");
        settings_line_end(out, bs, &pos);
        settings_line_begin(out, bs, &pos, s4, main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "选择每次打开 termux 窗口时默认显示的界面 (按 ←/→/Space/T/H 切换)：");
        settings_line_end(out, bs, &pos);

        /* v2.1.3：两段单选的实际绘制宽度由 settings_startup_radio_spans() 给出，
         * 悬停高亮与 input 的命中判定用同一份，不再各自写死 25/50 列。 */
        int r0_on = 0, r0_w = 0, r1_on = 0, r1_w = 0;
        settings_startup_radio_spans(host_cols, main_left, &r0_on, &r0_w, &r1_on, &r1_w);
        int opt0_hover = (s5 > 0 && r0_on && g_mouse_y == s5 - 1 &&
                          g_mouse_x >= main_left - 1 && g_mouse_x < main_left - 1 + r0_w);
        int opt1_hover = (s5 > 0 && r1_on && g_mouse_y == s5 - 1 &&
                          g_mouse_x >= main_left - 1 + r0_w + 3 && g_mouse_x < main_left - 1 + r0_w + 3 + r1_w);

        const char *opt0_style = (g_default_startup == 0) ? (opt0_hover ? "\x1b[048;2;140;205;255m\x1b[038;2;013;017;023;1m" : "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m")
                                                          : (opt0_hover ? "\x1b[048;2;045;055;072m\x1b[038;2;255;255;255;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;230;237;243m");
        const char *opt1_style = (g_default_startup == 1) ? (opt1_hover ? "\x1b[048;2;140;205;255m\x1b[038;2;013;017;023;1m" : "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m")
                                                          : (opt1_hover ? "\x1b[048;2;045;055;072m\x1b[038;2;255;255;255;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;230;237;243m");

        {   /* v2.1.3：这一行原先是裸 snprintf（43 列），窄终端整行溢出 ⇒ 折到下一行、
             * 把侧栏的「启动 (Startup)」顶掉（30/40 列实测）。改走带预算的行渲染：
             * 第一段放不下就整段不画，不留半截。 */
            char lab[80], fit[96];
            settings_line_begin(out, bs, &pos, s5, main_left, host_cols, opt0_style);
            snprintf(lab, sizeof(lab), " [●] 默认终端 (Terminal) ");
            settings_line_text(out, bs, &pos, lab);
            settings_line_sgr(out, bs, &pos, "\x1b[0m   ");
            settings_line_sgr(out, bs, &pos, opt1_style);
            snprintf(lab, sizeof(lab), " [○] 内置帮助 (Help) ");
            settings_line_text_e(out, bs, &pos, lab, 0);
            settings_line_end(out, bs, &pos);
            (void)fit;
        }

        settings_line_begin(out, bs, &pos, settings_startup_row_view(host_rows, 7), main_left, host_cols, "\x1b[038;2;121;192;255;1m");
        settings_line_text(out, bs, &pos, "■ 菜单项一览 (Menu Items)");
        settings_line_end(out, bs, &pos);
        settings_line_begin(out, bs, &pos, settings_startup_row_view(host_rows, 8), main_left, host_cols, "\x1b[038;2;139;148;158m");
        settings_line_text(out, bs, &pos, "启动菜单项：点它 / ↑↓ 选中，←/→ 或 Space 设为启动默认；增删改在 [M] 条目管理");
        settings_line_end(out, bs, &pos);

        /* 表头：几何在下面的 settings_menu_table_geom() 里算，所以先算好再画这一行。 */
        int g_cw = 0, g_nw = 0, g_cw2 = 0, g_bc = 0, g_ud = 0, g_btn = 0;
        settings_menu_table_geom(host_rows, host_cols, main_left, &g_cw, &g_nw, &g_cw2,
                                 &g_bc, &g_ud, &g_btn);
        int h_on = (g_cw > host_cols - main_left - 1);
        int h_sc = h_on ? settings_hscroll() : 0;
        {
            int hr = settings_startup_row_view(host_rows, 9);
            if (hr > 0) {
                if (!h_on) {
                    settings_line_begin(out, bs, &pos, hr, main_left, host_cols, "\x1b[038;2;121;192;255;1m");
                    char head[192];
                    /* v2.1.3：表头按实际列宽拼（原先写死一串空格，窄终端下比整行还宽 ⇒ 折行盖掉下一行） */
                    if (settings_menu_header(head, sizeof(head), host_cols, main_left) > 0)
                        settings_line_text(out, bs, &pos, head);
                    settings_line_end(out, bs, &pos);
                } else {
                    int pre = settings_menu_pre_w(g_cw + main_left + 1, main_left);
                    SettingsSeg sg[5]; int ns = 0;
                    sg[ns].sgr = "\x1b[0m"; sg[ns].text = ""; sg[ns].pad_to_col = pre - 5; ns++;
                    sg[ns].sgr = "\x1b[038;2;121;192;255;1m"; sg[ns].text = "\xe5\xba\x8f\xe5\x8f\xb7"; sg[ns].pad_to_col = pre; ns++;
                    if (g_nw) { sg[ns].sgr = "\x1b[038;2;121;192;255;1m"; sg[ns].text = "\xe6\x98\xbe\xe7\xa4\xba\xe5\x90\x8d\xe7\xa7\xb0"; sg[ns].pad_to_col = pre + g_nw; ns++; }
                    if (g_btn) { sg[ns].sgr = "\x1b[0m\x1b[038;2;121;192;255;1m"; sg[ns].text = "\xe6\x93\x8d\xe4\xbd\x9c"; sg[ns].pad_to_col = 0; ns++; }
                    int mleft = settings_mark_left(host_rows, host_cols, hr, SETTINGS_HMARK_W - 1);
                    settings_hline2(out, bs, &pos, hr, main_left, mleft ? mleft - 1 : host_cols,
                                    g_cw, sg, ns, 40, 0, 0);   /* 同上：给 «n/m» 标记让位 */
                settings_hscroll_mark(out, bs, &pos, hr, host_cols, h_sc,
                                      host_cols - main_left - 1, g_cw, host_rows);
                }
            }
        }


        {   /* 表格行（与「条目管理」页共用同一支绘制函数） */
            MenuRowCtx rc; rc.out = out; rc.bs = bs; rc.pos = &pos;
            rc.host_rows = host_rows; rc.host_cols = host_cols; rc.main_left = main_left;
            rc.sel = g_settings_table_sel; rc.row_view = settings_startup_row_view;
            rc.h_on = h_on; rc.h_sc = h_sc; rc.cw = g_cw;
            rc.nw = g_nw; rc.cw2 = g_cw2; rc.bc = g_bc; rc.ud = g_ud; rc.btn = g_btn;
            rc.show_ops = 0;   /* 启动项页的表只读：编辑动作都在「条目管理」页 */
            render_menu_rows(&rc);
        }
        /* v2.1.4：这一页原先有「[+] 添加条目 / [P] 快速预设」两个按钮和每行的
         * [↑][↓][改][删]。现在整张表只读（只有「设为启动默认」的单选与 ←/→），
         * 增删改与排序统一到侧栏的「[M] 条目管理」页 —— 少一行按钮，多一行表。 */

        /* v2.1.3：滚出可见区就整行不画（原先 `if (hint_r < 0) hint_r = host_rows;`
         * 会把它硬塞到末行，正好盖住侧栏的保存行）。 */
        int hint_r = settings_startup_row_view(host_rows, 12 + g_chooser_item_count);
        settings_line_begin(out, bs, &pos, hint_r, main_left, host_cols, "\x1b[038;2;139;148;158m");
        {   /* 同「条目管理」页：窄终端下先给行窗口标记让位 */
            int room = settings_mark_room(host_rows, host_cols, main_left, hint_r, 3,
                                          settings_startup_last(host_rows));
            g_sl_left = (g_sl_left > room) ? (g_sl_left - room) : 0;
            settings_line_text(out, bs, &pos, "提示: ←/→/Space 设启动默认, T 终端, H 帮助, M 条目管理, Ctrl+S 保存, Esc 退出");
        }
        settings_line_end(out, bs, &pos);
        settings_page_mark(out, bs, &pos, hint_r, host_cols, host_rows, 3,
                           settings_startup_last(host_rows), &g_settings_startup_scroll);
    } else {
        int item_idx = g_settings_nav - 1;
        /* v2.1.0：菜单项详细配置页有 11 行（标题 / 4 组字段 / 按钮 / 提示），终端一矮
         * 就整片画到屏幕外（用户报「终端太矮时设置显示不全」）。改成整页上滚，行号
         * 一律经 settings_detail_row_view() 换算；滚出可见区的行不画，命中判定同函数反查。 */
        int d_title = settings_detail_row_view(host_rows, 3);
        int d_f0 = settings_detail_row_view(host_rows, 5), d_f0i = settings_detail_row_view(host_rows, 6);
        int d_f1 = settings_detail_row_view(host_rows, 8), d_f1i = settings_detail_row_view(host_rows, 9);
        int d_f2 = settings_detail_row_view(host_rows, 11), d_f2i = settings_detail_row_view(host_rows, 12);
        int d_f3 = settings_detail_row_view(host_rows, 14), d_f3i = settings_detail_row_view(host_rows, 15);
        int act_r = settings_detail_row_view(host_rows, 17);
        int d_hint = settings_detail_row_view(host_rows, 19);

        if (d_title > 0)
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;121;192;255;1m■ 菜单项详细配置: [%d] %s\x1b[0m",
                            d_title, main_left, item_idx + 1, g_chooser_items[item_idx].name);

        int input_w = right_max_w - 4;
        if (input_w > 50) input_w = 50;
        if (input_w < 20) input_w = 20;

        int f0_sel = (g_settings_field == 0);
        int f0_hover = (d_f0i > 0 && g_mouse_y + 1 == d_f0i && g_mouse_x >= main_left - 1 && g_mouse_x <= main_left + input_w + 2);
        const char *f0_bg = f0_sel ? "\x1b[048;2;038;060;088m" : (f0_hover ? "\x1b[048;2;033;038;045m" : "\x1b[048;2;022;027;034m");
        if (d_f0 > 0) {
            settings_line_begin(out, bs, &pos, d_f0, main_left, host_cols, "\x1b[038;2;230;237;243;1m");
            settings_line_text(out, bs, &pos, "1. 显示名称 (Display Name):");
            settings_line_end(out, bs, &pos);
        }
        if (d_f0i > 0) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s ", d_f0i, main_left, f0_bg);
            render_scrollable_input(out, bs, &pos, g_edit_name, g_edit_name_len, g_edit_name_pos, input_w, f0_bg, NULL);
            pos += snprintf(out + pos, bs - pos, "%s \x1b[0m\x1b[048;2;033;038;045m│\x1b[0m", f0_bg);
        }

        int f1_sel = (g_settings_field == 1);
        int f1_hover = (d_f1i > 0 && g_mouse_y + 1 == d_f1i && g_mouse_x >= main_left - 1 && g_mouse_x <= main_left + input_w + 2);
        const char *f1_bg = f1_sel ? "\x1b[048;2;038;060;088m" : (f1_hover ? "\x1b[048;2;033;038;045m" : "\x1b[048;2;022;027;034m");
        if (d_f1 > 0) {
            settings_line_begin(out, bs, &pos, d_f1, main_left, host_cols, "\x1b[038;2;230;237;243;1m");
            settings_line_text(out, bs, &pos, "2. 启动命令行 (Command Line):");
            settings_line_end(out, bs, &pos);
        }
        if (d_f1i > 0) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s ", d_f1i, main_left, f1_bg);
            render_scrollable_input(out, bs, &pos, g_edit_cmd, g_edit_cmd_len, g_edit_cmd_pos, input_w, f1_bg, NULL);
            pos += snprintf(out + pos, bs - pos, "%s \x1b[0m\x1b[048;2;033;038;045m│\x1b[0m", f1_bg);
        }

        int f2_sel = (g_settings_field == 2);
        int f2_hover = (d_f2i > 0 && g_mouse_y + 1 == d_f2i && g_mouse_x >= main_left - 1 && g_mouse_x <= main_left + input_w + 2);
        const char *f2_bg = f2_sel ? "\x1b[048;2;038;060;088m" : (f2_hover ? "\x1b[048;2;033;038;045m" : "\x1b[048;2;022;027;034m");
        if (d_f2 > 0) {
            /* v2.1.2：这两行原来是裸 snprintf，一窄就整段越过行尾把下一行/侧栏顶掉
             * （用户报「[用此项新建标签页时的颜色，默认=蓝]: 这个太窄时没有...提示」）。
             * 改走 settings_line_* 后：切到行尾补「...」+ 悬停气泡给全文。 */
            settings_line_begin(out, bs, &pos, d_f2, main_left, host_cols, "\x1b[038;2;230;237;243;1m");
            settings_line_text(out, bs, &pos, "3. 启动目录 (Working Directory) [留空为当前目录，支持 %USERPROFILE%]:");
            settings_line_end(out, bs, &pos);
        }
        if (d_f2i > 0) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s ", d_f2i, main_left, f2_bg);
            render_scrollable_input(out, bs, &pos, g_edit_dir, g_edit_dir_len, g_edit_dir_pos, input_w, f2_bg, NULL);
            pos += snprintf(out + pos, bs - pos, "%s \x1b[0m\x1b[048;2;033;038;045m│\x1b[0m", f2_bg);
        }

        /* v1.8.9: 第 4 个字段 —— 这个菜单项启动出来的标签页默认用什么颜色。 */
        int f3_sel = (g_settings_field == 3);
        if (d_f3 > 0) {
            settings_line_begin(out, bs, &pos, d_f3, main_left, host_cols, "\x1b[038;2;230;237;243;1m");
            settings_line_text(out, bs, &pos, "4. 启动默认颜色 (Tab Color) [用此项新建标签页时的颜色，默认=蓝]:");
            settings_line_end(out, bs, &pos);
        }
        if (d_f3i > 0) {
            g_item_color_max = settings_detail_color_w(host_cols, main_left);
            render_item_color_row(out, bs, &pos, d_f3i, main_left, g_edit_color, f3_sel);
        }

        int h_apply = (act_r > 0 && g_mouse_y == act_r - 1 && g_mouse_x >= main_left - 1 && g_mouse_x < main_left + 17);
        int h_imp = (act_r > 0 && g_mouse_y == act_r - 1 && g_mouse_x >= main_left + 19 && g_mouse_x < main_left + 35);
        int h_del = (act_r > 0 && g_mouse_y == act_r - 1 && g_mouse_x >= main_left + 37 && g_mouse_x < main_left + 49);

        if (act_r > 0) {
            /* 三个按钮：前两枚按剩余宽度整枚丢弃，最后一枚才带「...」——
             * 裸 snprintf 时它们会跨过行尾把下一行顶掉（v2.1.2 一并修掉）。 */
            settings_line_begin(out, bs, &pos, act_r, main_left, host_cols, "");
            settings_line_sgr(out, bs, &pos, h_apply ? "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;121;192;255;1m");
            settings_line_text_e(out, bs, &pos, " [保存并应用此项] ", 0);
            settings_line_end(out, bs, &pos);
            settings_line_begin(out, bs, &pos, act_r, main_left + 19, host_cols,
                                h_imp ? "\x1b[048;2;031;136;061m\x1b[038;2;255;255;255;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;031;136;061;1m");
            settings_line_text_e(out, bs, &pos, " [从预设库导入] ", 0);
            settings_line_end(out, bs, &pos);
            settings_line_begin(out, bs, &pos, act_r, main_left + 37, host_cols,
                                h_del ? "\x1b[048;2;248;081;073m\x1b[038;2;255;255;255;1m" : "\x1b[048;2;033;038;045m\x1b[038;2;248;081;073;1m");
            settings_line_text_e(out, bs, &pos, " [删除此项] ", 0);
            settings_line_end(out, bs, &pos);
        }

        if (d_hint > 0) {
            settings_line_begin(out, bs, &pos, d_hint, main_left, host_cols, "\x1b[038;2;139;148;158m");
            settings_line_text(out, bs, &pos, "提示: Tab 切换字段, ←/→ 选颜色, Enter 保存应用, Ctrl+P 导入预设, Ctrl+D 删除, Esc 返回");
            settings_line_end(out, bs, &pos);
            settings_page_mark(out, bs, &pos, d_hint, host_cols, host_rows,
                               SETTINGS_DETAIL_FIRST, SETTINGS_DETAIL_LAST, &g_settings_detail_scroll);
        }
    }

    if (g_settings_show_presets) {
        render_settings_presets(out, bs, &pos, host_rows, host_cols);
    }
    if (g_settings_show_pane_schemes) {
        render_pane_scheme_picker(out, bs, &pos, host_rows, host_cols);
    }
    /* v2.1.1：十六进制编辑浮层最后画（盖在页面上），保证编辑内容任何宽度都完整可见 */
    if (hex_edit_popup_shown(host_rows, host_cols))
        render_hex_edit_popup(out, bs, &pos, host_rows, host_cols);
    render_settings_tooltip(out, bs, &pos, host_rows, host_cols);

    *posp = pos;
}

/* ---------------------------------------------------------------------------
 * 顶栏右侧状态徽章
 *
 * 复制模式与搜索以前把整条操作提示常驻在屏幕上（复制模式占 60 列，搜索占满
 * 底行），信息量大但绝大多数时间是噪音。现在只保留一个短徽章贴在右上角，
 * 提示文字改成鼠标悬停时才向左展开；搜索的“上一个 / 下一个 / 关闭”做成可点
 * 的按钮，热区与绘制位置来自同一个 status_badge_layout()。
 * ------------------------------------------------------------------------- */
#define BADGE_ROW        2
#define BADGE_BTN_COLS   6   /* "[U 上]" / "[D 下]" */
#define BADGE_CLOSE_COLS 3   /* "[×]" */

static void badge_collapsed_text(char *out, int n) {
    if (g_copy_mode) {
        const char *state = g_copy_sel_active ? (g_copy_block ? "块选区" : "行选区")
                                              : (g_copy_quick ? "点选区" : "移动光标");
        snprintf(out, n, " [复制模式 %s] ", state);
        return;
    }
    char query[28] = {0};
    /* 长关键词截断，徽章宽度必须可预测。g_search_buf 是 64 字节，比 query 大，
     * 所以这里【故意】截断 —— 用 %.*s 把这件事写明，顺带消掉 -Wformat-truncation。
     * snprintf 本来就会在这个长度上截断并补 NUL，输出完全一致。 */
    snprintf(query, sizeof(query), "%.*s", (int)sizeof(query) - 1, g_search_buf);
    int qcols = utf8_cols(query, (int)strlen(query));
    if (qcols > 16) {
        int keep = 0, cols = 0;
        while (query[keep] && cols < 13) {
            int adv = 0;
            unsigned int cp = utf8_decode_cp(query + keep, (int)strlen(query + keep), &adv);
            if (adv <= 0) break;
            cols += is_wide_cp(cp) ? 2 : 1;
            keep += adv;
        }
        query[keep] = 0;
        snprintf(out, n, " [搜索 \"%s…\" %d/%d] ", query, g_search_match_cur + 1, g_search_match_count);
        return;
    }
    snprintf(out, n, " [搜索 \"%s\" %d/%d] ", query, g_search_match_cur + 1, g_search_match_count);
}

int status_badge_layout(int host_cols, StatusBadge *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    int kind = 0;
    if (g_copy_mode) kind = 1;
    else if (g_search_active && g_search_match_count > 0 && !g_search_mode) kind = 2;
    if (!kind) return 0;

    char text[96];
    badge_collapsed_text(text, sizeof(text));
    int badge_cols = utf8_cols(text, (int)strlen(text));
    int width = badge_cols;
    if (kind == 2) width += BADGE_BTN_COLS * 2 + BADGE_CLOSE_COLS;

    int start = host_cols - width + 1;
    if (start < 1) start = 1;
    out->kind = kind;
    out->row = BADGE_ROW;
    out->start = start;
    out->end = start + width;
    if (kind == 2) {
        out->prev_s = start + badge_cols;
        out->prev_e = out->prev_s + BADGE_BTN_COLS;
        out->next_s = out->prev_e;
        out->next_e = out->next_s + BADGE_BTN_COLS;
        out->close_s = out->next_e;
        out->close_e = out->close_s + BADGE_CLOSE_COLS;
    }
    return kind;
}

int status_badge_hovered(const StatusBadge *b) {
    if (!b || !b->kind) return 0;
    return (g_mouse_y + 1 == b->row && g_mouse_x + 1 >= b->start && g_mouse_x + 1 < b->end);
}

void render_status_badge(char *out, int bs, int *posp, int host_cols) {
    StatusBadge b;
    if (!status_badge_layout(host_cols, &b)) return;
    int pos = *posp;
    int hovered = status_badge_hovered(&b);
    const char *panel = "\x1b[048;2;033;038;045m";

    /* 搜索徽章（kind==2）本身就常驻 [U 上]/[D 下]/[×] 按钮，悬停不再展开
     * 重复的「U 上一个·D 下一个」长提示；只给复制模式（kind==1）保留悬停说明。 */
    if (hovered && b.kind == 1) {
        const char *hint = " Enter/Ctrl+C 复制 · Shift/Alt+方向改选区 · Esc 退出 ";
        int hint_cols = utf8_cols(hint, (int)strlen(hint));
        int hint_left = b.start - hint_cols;
        if (hint_left >= 1)
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s\x1b[038;2;230;237;243m%s\x1b[0m",
                            b.row, hint_left, panel, hint);
    }

    char text[96];
    badge_collapsed_text(text, sizeof(text));
    const char *badge_style = (b.kind == 1)
        ? "\x1b[048;2;210;153;034m\x1b[038;2;013;017;023;1m"
        : "\x1b[048;2;031;111;235m\x1b[038;2;255;255;255;1m";
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s%s\x1b[0m", b.row, b.start, badge_style, text);

    if (b.kind == 2) {
        int mrow = g_mouse_y + 1, mcol = g_mouse_x + 1;
        struct { int s, e; const char *label; } btns[3] = {
            { b.prev_s, b.prev_e, "[U 上]" },
            { b.next_s, b.next_e, "[D 下]" },
            { b.close_s, b.close_e, "[×]" },
        };
        for (int i = 0; i < 3; i++) {
            int hot = (mrow == b.row && mcol >= btns[i].s && mcol < btns[i].e);
            const char *style = hot
                ? (i == 2 ? "\x1b[048;2;248;081;073m\x1b[038;2;255;255;255;1m"
                          : "\x1b[048;2;121;192;255m\x1b[038;2;013;017;023;1m")
                : (i == 2 ? "\x1b[048;2;033;038;045m\x1b[038;2;248;081;073m"
                          : "\x1b[048;2;033;038;045m\x1b[038;2;121;192;255m");
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s%s\x1b[0m",
                            b.row, btns[i].s, style, btns[i].label);
        }
    }
    *posp = pos;
}

/* v1.8.10: 搜索输入框不再占用整条底行 —— 那等于凭空吃掉一行终端内容。
 * 现在它和搜索状态徽章一样，是贴右上角的一个紧凑小框，宽度固定、右对齐，
 * 渲染与光标定位共用 search_box_layout()。 */
#define SEARCH_BOX_PREFIX_COLS 6    /* " 搜索 " */
#define SEARCH_BOX_INPUT_COLS  24
#define SEARCH_BOX_SUFFIX_COLS 12   /* " Enter/Esc " + 右侧留白 */
#define SEARCH_BOX_COLS (SEARCH_BOX_PREFIX_COLS + SEARCH_BOX_INPUT_COLS + SEARCH_BOX_SUFFIX_COLS)

void search_box_layout(int host_cols, int *row, int *left, int *input_col, int *input_w) {
    int w = SEARCH_BOX_COLS;
    if (w > host_cols) w = host_cols;
    int l = host_cols - w + 1;
    if (l < 1) l = 1;
    int iw = SEARCH_BOX_INPUT_COLS;
    if (w < SEARCH_BOX_COLS) {
        iw = w - SEARCH_BOX_PREFIX_COLS - SEARCH_BOX_SUFFIX_COLS;
        if (iw < 1) iw = 1;
    }
    if (row) *row = BADGE_ROW;
    if (left) *left = l;
    if (input_col) *input_col = l + SEARCH_BOX_PREFIX_COLS;
    if (input_w) *input_w = iw;
}

/* 搜索框后缀里「Aa / aa」大小写标记的列区间（1 基，右排他）。后缀紧跟输入框：
 * 输入框占 input_w 列，后缀第 1-2 列即大小写标记，第 3 列起是空格 + Enter/Esc。
 * 渲染、hover、点击三处共用本几何，保证「高亮哪里就能点哪里」。 */
static void search_box_case_geom(int host_cols, int *case_col, int *case_w) {
    int row, left, input_col, box_w;
    search_box_layout(host_cols, &row, &left, &input_col, &box_w);
    (void)row;
    if (case_col) *case_col = input_col + box_w;   /* 1 基首列 */
    if (case_w)   *case_w = 2;                     /* "Aa" / "aa" 占 2 列 */
}

int search_box_case_hit(int host_cols, int r, int c) {
    int row, left, input_col, box_w;
    search_box_layout(host_cols, &row, &left, &input_col, &box_w);
    if (r != row) return 0;
    int cc, cw;
    search_box_case_geom(host_cols, &cc, &cw);
    return (c >= cc && c < cc + cw);
}

int search_box_case_hovered(int host_cols) {
    return search_box_case_hit(host_cols, g_mouse_y + 1, g_mouse_x + 1);
}

#define CONFIRM_W 34
#define CONFIRM_H 4
#define CONFIRM_YES_W 10   /* [ Y 确认 ] */
#define CONFIRM_NO_W  14   /* [ N/Esc 取消 ] */
#define CONFIRM_GAP    2

void confirm_exit_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h) {
    int width = CONFIRM_W, height = CONFIRM_H;
    if (width > host_cols) width = host_cols;
    int t = (host_rows - height) / 2 + 1;
    if (t < 2) t = 2;
    int l = (host_cols - width) / 2 + 1;
    if (l < 1) l = 1;
    if (top) *top = t;
    if (left) *left = l;
    if (w) *w = width;
    if (h) *h = height;
}

/* Both the renderer and the mouse handler derive the button boxes from this
 * one helper, so the highlighted area and the clickable area can never drift
 * apart.  Columns are 1-based ANSI columns; *_end is exclusive. */
void confirm_exit_button_geom(int host_rows, int host_cols, int *row,
                              int *yes_start, int *yes_end,
                              int *no_start, int *no_end) {
    int top, left, w, h;
    confirm_exit_geom(host_rows, host_cols, &top, &left, &w, &h);
    (void)h;
    int interior = w - 2;
    if (interior < 0) interior = 0;
    int used = CONFIRM_YES_W + CONFIRM_GAP + CONFIRM_NO_W;
    int pad = interior - used;
    if (pad < 0) pad = 0;
    int lead = pad / 2;
    int ys = left + 1 + lead;
    int ns = ys + CONFIRM_YES_W + CONFIRM_GAP;
    if (row) *row = top + 2;
    if (yes_start) *yes_start = ys;
    if (yes_end) *yes_end = ys + CONFIRM_YES_W;
    if (no_start) *no_start = ns;
    if (no_end) *no_end = ns + CONFIRM_NO_W;
}

static void confirm_pad(char *out, int bs, int *posp, int n) {
    int pos = *posp;
    while (n-- > 0 && pos < bs - 2) out[pos++] = ' ';
    *posp = pos;
}

void render_confirm_dialog(char *out, int bs, int *posp, int host_rows, int host_cols, int kind) {
    int pos = *posp, top, left, w, h;
    confirm_exit_geom(host_rows, host_cols, &top, &left, &w, &h);
    (void)h;
    int interior = w - 2;
    if (interior < 0) interior = 0;

    const char *panel = "\x1b[048;2;033;038;045m";
    const char *hdr = kind ? "┌─ 关闭窗格确认 " : "┌─ 退出确认 ";
    int cols = utf8_cols(hdr, (int)strlen(hdr));
    pos += snprintf(out + pos, bs - pos,
        "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;248;081;073m%s", top, left, hdr);
    while (cols < w - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80'; cols++;
    }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");

    const char *msg = kind ? " 确定要关闭当前窗格吗？" : " 确定要退出 termux 吗？";
    int msg_cols = utf8_cols(msg, (int)strlen(msg));
    pos += snprintf(out + pos, bs - pos,
        "\x1b[%d;%dH%s│%s\x1b[038;2;230;237;243m%s", top + 1, left, panel, panel, msg);
    confirm_pad(out, bs, &pos, interior - msg_cols);
    pos += snprintf(out + pos, bs - pos, "\x1b[0m%s│\x1b[0m", panel);

    int row, ys, ye, ns, ne;
    confirm_exit_button_geom(host_rows, host_cols, &row, &ys, &ye, &ns, &ne);
    int mrow = g_mouse_y + 1, mcol = g_mouse_x + 1;
    int yes_hover = (mrow == row && mcol >= ys && mcol < ye);
    int no_hover = (mrow == row && mcol >= ns && mcol < ne);

    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s│%s", row, left, panel, panel);
    confirm_pad(out, bs, &pos, ys - (left + 1));
    if (yes_hover)
        pos += snprintf(out + pos, bs - pos,
            "\x1b[048;2;248;081;073m\x1b[038;2;255;255;255;1m[ Y 确认 ]\x1b[0m%s", panel);
    else
        pos += snprintf(out + pos, bs - pos,
            "%s\x1b[038;2;248;081;073;1m[\x1b[22m\x1b[038;2;230;237;243m Y 确认 \x1b[038;2;248;081;073;1m]\x1b[22m", panel);
    confirm_pad(out, bs, &pos, CONFIRM_GAP);
    if (no_hover)
        pos += snprintf(out + pos, bs - pos,
            "\x1b[048;2;063;185;080m\x1b[038;2;013;017;023;1m[ N/Esc 取消 ]\x1b[0m%s", panel);
    else
        pos += snprintf(out + pos, bs - pos,
            "%s\x1b[038;2;063;185;080;1m[\x1b[22m\x1b[038;2;230;237;243m N/Esc 取消 \x1b[038;2;063;185;080;1m]\x1b[22m", panel);
    confirm_pad(out, bs, &pos, (left + w - 1) - ne);
    pos += snprintf(out + pos, bs - pos, "\x1b[0m%s│\x1b[0m", panel);

    palette_hline(out, bs, &pos, top + 3, left, w, "└", "┘");
    /* 光标显隐由帧尾光标段统一处理（render_screen 的 cursor_pos 之后），这里
     * 不再发 ?25l——它在 body 里会被脏区按行差分跳过（弹窗行内容不变时不发），
     * 反而盖不住帧尾终端 pane 的 ?25h，导致弹窗上出现游离光标。 */
    *posp = pos;
}

void render_confirm_exit(char *out, int bs, int *posp, int host_rows, int host_cols) {
    render_confirm_dialog(out, bs, posp, host_rows, host_cols, 0);
}

void render_search_box(char *out, int bs, int *posp, int host_rows, int host_cols) {
    int pos = *posp;
    int row, left, input_col, box_w;
    (void)host_rows;
    search_box_layout(host_cols, &row, &left, &input_col, &box_w);

    pos += snprintf(out + pos, bs - pos,
                    "\x1b[%d;%dH\x1b[048;2;033;038;045m\x1b[038;2;121;192;255;1m 搜索 \x1b[0m", row, left);
    pos += snprintf(out + pos, bs - pos, "\x1b[048;2;022;027;034m\x1b[038;2;255;255;255m");
    render_scrollable_input(out, bs, &pos, g_search_buf, g_search_len, g_search_pos, box_w,
                            "\x1b[048;2;022;027;034m", NULL);
    /* 后缀固定 12 列（SEARCH_BOX_SUFFIX_COLS）：前 3 列大小写模式标记
     * "Aa "（区分大小写=高亮）/ "aa "（忽略=灰），其余 9 列为 "Enter/Esc" 提示。 */
    /* 后缀固定 12 列：2 列大小写标记 Aa/aa + 1 空格 + "Enter/Esc" 9 列 = 12。
     * Aa/aa 可点击切换区分大小写：悬停时加深底色提示可点（search_box_case_hit
     * 与点击命中同一几何）。区分=蓝 121;192;255，忽略=灰 139;148;158。 */
    int case_hover = search_box_case_hovered(host_cols);
    pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m");
    /* hover 加深色复用设置页选中行蓝 048;075;110（theme.c 已登记，勿自造色）。 */
    pos += snprintf(out + pos, bs - pos, case_hover ? "\x1b[048;2;048;075;110m" : "");
    pos += snprintf(out + pos, bs - pos, g_search_case_sensitive
                        ? "\x1b[038;2;121;192;255;1m"
                        : "\x1b[038;2;139;148;158m");
    pos += snprintf(out + pos, bs - pos, g_search_case_sensitive ? "Aa" : "aa");
    pos += snprintf(out + pos, bs - pos, "\x1b[048;2;033;038;045m\x1b[038;2;139;148;158m Enter/Esc\x1b[0m");
    *posp = pos;
}

#define PALETTE_MAX_VISIBLE 9
#define PALETTE_W 72
#define PALETTE_EDITOR_W 78
#define PALETTE_EDITOR_H 12   /* 三个输入框 + 颜色行 + 分隔线 + 操作行 */

typedef struct {
    const char *id;
    const char *title;
    const char *desc;
    const char *shortcut;
    PaletteAction action;
    int value;
    int number;
    int color;
} PaletteStaticItem;

static const PaletteStaticItem g_palette_root_items[] = {
    { "operations", "操作命令面板", "新建、切换、搜索与终端操作", "Enter 进入", PALETTE_ACTION_OPEN_OPERATIONS, 0, 1, 4 },
    { "settings",   "设置命令面板", "启动项、INI 文件与菜单项设置", "Enter 进入", PALETTE_ACTION_OPEN_SETTINGS, 0, 2, 6 },
};

static const PaletteStaticItem g_palette_operation_items[] = {
    { "new-terminal",       "新建终端",           "选择已配置终端，支持搜索",       "Enter 进入", PALETTE_ACTION_OPEN_NEW_TERMINAL, 0, 1, 1 },
    { "custom-command",    "启动自定义命令行",   "输入并启动任意自定义命令行",       "",           PALETTE_ACTION_START_CUSTOM,       0, 2, 2 },
    { "rename",             "修改标题",           "修改当前标签页显示标题",           "",           PALETTE_ACTION_RENAME,             0, 3, 3 },
    { "color",              "修改颜色",           "修改当前标签页的主题颜色",         "",           PALETTE_ACTION_COLOR,              0, 4, 4 },
    { "search-history",     "搜索历史",           "搜索当前终端的滚动历史",           "",           PALETTE_ACTION_SEARCH,             0, 5, 5 },
    { "switch-panel",       "切换 panel",         "按编号或标题选择并切换 panel",     "Enter 进入", PALETTE_ACTION_SWITCH_PANEL,       0, 6, 6 },
    { "copy-mode",          "进入复制模式",       "移动光标、行选/块选终端文本并复制",     "",           PALETTE_ACTION_COPY_MODE,          0, 7, 7 },
    { "split-vertical",     "分屏：左右切分",     "在当前标签内右侧新建一个窗格（前缀 -）", "", PALETTE_ACTION_SPLIT_VERTICAL,     0, 14, 4 },
    { "split-horizontal",   "分屏：上下切分",     "在当前标签内下方新建一个窗格（前缀 _）", "", PALETTE_ACTION_SPLIT_HORIZONTAL,   0, 15, 4 },
    { "split-next",         "分屏：切换窗格",     "在当前标签的各窗格间循环切换焦点（前缀 Tab）", "", PALETTE_ACTION_SPLIT_NEXT,    0, 16, 6 },
    { "split-zoom",         "分屏：全屏缩放",     "把当前窗格临时铺满全屏 / 还原（前缀 z）", "", PALETTE_ACTION_SPLIT_ZOOM,        0, 17, 3 },
    { "split-close",        "分屏：关闭窗格",     "关闭当前窗格，仅一个窗格时关闭整个标签（前缀 q）", "", PALETTE_ACTION_SPLIT_CLOSE, 0, 18, 8 },
    { "reload",             "热重载",             "重新加载 termux.ini 配置文件",      "",           PALETTE_ACTION_RELOAD,             0, 8, 5 },
    { "open-settings-page", "打开设置页面",       "进入图形化设置页面",               "Enter 打开", PALETTE_ACTION_GRAPHICAL_SETTINGS, 0, 9, 4 },
    { "settings-command-panel", "打开设置命令面板", "切换到设置命令面板",           "Enter 进入", PALETTE_ACTION_OPEN_SETTINGS,     0, 10, 6 },
    { "about",              "关于",               "查看版本、作者与系统信息",         "Enter 打开", PALETTE_ACTION_OPEN_ABOUT,         0, 11, 7 },
    { "close-panel",        "关闭当前 panel",     "关闭当前活动 panel",                "",           PALETTE_ACTION_CLOSE_PANEL,       0, 12, 7 },
    { "quit",               "退出 termux",       "退出整个 termux 程序并关闭所有会话", "",           PALETTE_ACTION_QUIT,              0, 13, 8 },
};

static const PaletteStaticItem g_palette_setting_items[] = {
    { "operations-command-panel", "打开操作命令面板", "切换到操作命令面板",       "Enter 进入", PALETTE_ACTION_OPEN_OPERATIONS,   0, 1, 1 },
    { "default-startup",    "修改默认启动项", "选择启动时显示终端或帮助页面",     "Enter 进入", PALETTE_ACTION_DEFAULT_STARTUP,     0, 2, 6 },
    { "open-ini",           "打开设置文件 (.ini)", "使用系统默认编辑器打开 termux.ini", "",           PALETTE_ACTION_OPEN_INI,          0, 3, 6 },
    { "add-panel",          "添加 panel 条目", "选择预设或自定义并继续编辑",       "Enter 进入", PALETTE_ACTION_ADD_PANEL,         0, 4, 2 },
    { "menu-settings",      "菜单项设置",     "在子面板中选择并编辑 panel 条目",       "Enter 进入", PALETTE_ACTION_MENU_SETTINGS,     0, 5, 4 },
    { "next-theme",         "切换配色主题",   "在内置主题之间轮换并写入 termux.ini", "",           PALETTE_ACTION_NEXT_THEME,        0, 6, 8 },
    { "appearance",         "外观 / 主题",    "设置页：选择主题、编辑 16 个语义色",  "Enter 打开", PALETTE_ACTION_OPEN_APPEARANCE,   0, 7, 6 },
    { "key-bindings",       "键位设置",       "设置页：前缀键与全部动作键位录制",    "Enter 打开", PALETTE_ACTION_OPEN_KEYS,         0, 8, 1 },
    { "behavior",           "行为开关",       "设置页：鼠标、自动复制、退出确认、滚动行数", "Enter 打开", PALETTE_ACTION_OPEN_BEHAVIOR, 0, 9, 2 },
    { "pane-palette",       "窗格配色",       "设置页：cmd / shell 的默认背景、字色与 16 个索引色", "Enter 打开", PALETTE_ACTION_OPEN_PANE_PALETTE, 0, 10, 2 },
};

static const PaletteStaticItem g_palette_startup_items[] = {
    { "terminal", "默认终端", "启动时创建并显示终端 panel", "", PALETTE_ACTION_SELECT_DEFAULT, 0, 1, 1 },
    { "help",     "内置帮助", "启动时显示内置帮助页面",     "", PALETTE_ACTION_SELECT_DEFAULT, 1, 2, 6 },
};

static int palette_static_count(int page) {
    switch (page) {
        case PALETTE_PAGE_ROOT: return (int)(sizeof(g_palette_root_items) / sizeof(g_palette_root_items[0]));
        case PALETTE_PAGE_OPERATIONS: return (int)(sizeof(g_palette_operation_items) / sizeof(g_palette_operation_items[0]));
        case PALETTE_PAGE_SETTINGS: return (int)(sizeof(g_palette_setting_items) / sizeof(g_palette_setting_items[0]));
        case PALETTE_PAGE_DEFAULT_STARTUP: return (int)(sizeof(g_palette_startup_items) / sizeof(g_palette_startup_items[0]));
        default: return 0;
    }
}

static int palette_strcasestr(const char *haystack, const char *needle) {
    if (!needle || !*needle) return 1;
    if (!haystack || !*haystack) return 0;
    int nlen = (int)strlen(needle);
    int hlen = (int)strlen(haystack);
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int k = 0; k < nlen; k++) {
            char ch1 = haystack[i + k];
            char ch2 = needle[k];
            if (ch1 >= 'A' && ch1 <= 'Z') ch1 = (char)(ch1 + ('a' - 'A'));
            if (ch2 >= 'A' && ch2 <= 'Z') ch2 = (char)(ch2 + ('a' - 'A'));
            if (ch1 != ch2) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static int palette_strcase_prefix(const char *haystack, const char *needle) {
    if (!needle || !*needle) return 1;
    if (!haystack) return 0;
    int nlen = (int)strlen(needle);
    int hlen = (int)strlen(haystack);
    if (nlen > hlen) return 0;
    for (int i = 0; i < nlen; i++) {
        char ch1 = haystack[i];
        char ch2 = needle[i];
        if (ch1 >= 'A' && ch1 <= 'Z') ch1 = (char)(ch1 + ('a' - 'A'));
        if (ch2 >= 'A' && ch2 <= 'Z') ch2 = (char)(ch2 + ('a' - 'A'));
        if (ch1 != ch2) return 0;
    }
    return 1;
}

static int palette_strcase_equal(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    return (int)strlen(haystack) == (int)strlen(needle) &&
           palette_strcase_prefix(haystack, needle);
}

static int palette_match_score(const PaletteItemInfo *item, const char *query, int title_only) {
    if (!item || !query) return -1;
    if (!*query) return 0;

    const char *fields[4] = { item->title, item->desc, item->shortcut, item->id };
    int best = -1;
    int field_count = title_only ? 1 : 4;
    for (int field = 0; field < field_count; field++) {
        const char *text = fields[field];
        if (!text || !*text || !palette_strcasestr(text, query)) continue;
        int score = field * 30 + 20;
        if (palette_strcase_prefix(text, query)) score = field * 30 + 10;
        if (palette_strcase_equal(text, query)) score = field * 30;
        if (best < 0 || score < best) best = score;
    }
    return best;
}

/* 分屏项的快捷键：把静态占位「前缀 x」替换成真实组合（随用户配置的前缀键而变，
 * 例如默认 Ctrl+B 时显示 "Ctrl+B -"），从键位表动态生成，不写死。 */
static char g_split_shortcut_buf[64];   /* "前缀 键" 两段各最长 24 字节 */
static const char *palette_split_shortcut(PaletteAction a) {
    int act = ACT_NONE;
    switch (a) {
        case PALETTE_ACTION_SPLIT_VERTICAL:   act = ACT_SPLIT_VERTICAL;   break;
        case PALETTE_ACTION_SPLIT_HORIZONTAL: act = ACT_SPLIT_HORIZONTAL; break;
        case PALETTE_ACTION_SPLIT_NEXT:       act = ACT_SPLIT_NEXT;       break;
        case PALETTE_ACTION_SPLIT_CLOSE:      act = ACT_SPLIT_CLOSE;      break;
        case PALETTE_ACTION_SPLIT_ZOOM:       act = ACT_SPLIT_ZOOM;       break;
        default: return NULL;
    }
    keymap_describe(act, g_split_shortcut_buf, sizeof(g_split_shortcut_buf));
    return g_split_shortcut_buf[0] ? g_split_shortcut_buf : NULL;
}

static int palette_copy_static(const PaletteStaticItem *src, PaletteItemInfo *out) {
    if (!src || !out) return 0;
    out->id = src->id;
    out->title = src->title;
    out->desc = src->desc;
    const char *sc = palette_split_shortcut(src->action);
    out->shortcut = sc ? sc : src->shortcut;
    out->action = src->action;
    out->value = src->value;
    out->number = src->number;
    out->color = src->color;
    return 1;
}

int palette_item_count(int page) {
    switch (page) {
        case PALETTE_PAGE_ROOT:
        case PALETTE_PAGE_OPERATIONS:
        case PALETTE_PAGE_SETTINGS:
        case PALETTE_PAGE_DEFAULT_STARTUP:
            return palette_static_count(page);
        case PALETTE_PAGE_NEW_TERMINAL:
            return g_chooser_item_count;
        case PALETTE_PAGE_SWITCH_PANEL: {
            int count = 0;
            for (int i = 0; i < g_mux.pane_count; i++)
                if (g_mux.panes[i].active) count++;
            return count;
        }
        case PALETTE_PAGE_ADD_PANEL:
            return g_preset_count;
        case PALETTE_PAGE_MENU_SETTINGS:
            /* Menu item settings edits existing entries only.  Adding a new
             * entry remains available from the settings command panel, not
             * from this management subpanel. */
            return g_chooser_item_count;
        default:
            return 0;
    }
}

int palette_item_info(int page, int item_index, PaletteItemInfo *out) {
    if (!out || item_index < 0) return 0;
    memset(out, 0, sizeof(*out));

    if (page == PALETTE_PAGE_ROOT && item_index < palette_static_count(page))
        return palette_copy_static(&g_palette_root_items[item_index], out);
    if (page == PALETTE_PAGE_OPERATIONS && item_index < palette_static_count(page))
        return palette_copy_static(&g_palette_operation_items[item_index], out);
    if (page == PALETTE_PAGE_SETTINGS && item_index < palette_static_count(page))
        return palette_copy_static(&g_palette_setting_items[item_index], out);
    if (page == PALETTE_PAGE_DEFAULT_STARTUP && item_index < palette_static_count(page)) {
        if (!palette_copy_static(&g_palette_startup_items[item_index], out)) return 0;
        out->shortcut = (g_default_startup == item_index) ? "当前默认" : "";
        return 1;
    }

    if (page == PALETTE_PAGE_NEW_TERMINAL && item_index < g_chooser_item_count) {
        out->id = "configured-terminal";
        out->title = g_chooser_items[item_index].name;
        out->desc = g_chooser_items[item_index].cmd;
        out->shortcut = g_chooser_items[item_index].cmd;
        out->action = PALETTE_ACTION_SELECT_TERMINAL;
        out->value = item_index;
        out->number = item_index + 1;
        out->color = (item_index % 8) + 1;
        return 1;
    }

    if (page == PALETTE_PAGE_SWITCH_PANEL) {
        int visible_index = 0;
        for (int i = 0; i < g_mux.pane_count; i++) {
            if (!g_mux.panes[i].active) continue;
            if (visible_index++ != item_index) continue;
            out->id = "panel";
            out->title = g_mux.panes[i].title[0] ? g_mux.panes[i].title : "cmd";
            out->desc = g_mux.panes[i].full_title;
            out->shortcut = "";
            out->action = PALETTE_ACTION_SELECT_PANEL;
            out->value = i;
            out->number = item_index + 1;
            out->color = (g_mux.panes[i].color >= 0 && g_mux.panes[i].color <= 8) ?
                         g_mux.panes[i].color : 0;
            return 1;
        }
        return 0;
    }

    if (page == PALETTE_PAGE_ADD_PANEL && item_index < g_preset_count) {
        out->id = "panel-preset";
        out->title = g_presets[item_index].name;
        out->desc = g_presets[item_index].cmd;
        out->shortcut = g_presets[item_index].cmd;
        out->action = PALETTE_ACTION_SELECT_PANEL;
        out->value = item_index;
        out->number = item_index + 1;
        out->color = item_index == g_preset_count - 1 ? 2 : 5;
        return 1;
    }

    if (page == PALETTE_PAGE_MENU_SETTINGS) {
        if (item_index >= 0 && item_index < g_chooser_item_count) {
            out->id = "menu-item";
            out->title = g_chooser_items[item_index].name[0] ? g_chooser_items[item_index].name : "未命名 panel";
            out->desc = g_chooser_items[item_index].cmd;
            out->shortcut = "Enter 编辑";
            out->action = PALETTE_ACTION_EDIT_PANEL;
            out->value = item_index;
            out->number = item_index + 1;
            out->color = (item_index % 8) + 1;
            return 1;
        }
    }
    return 0;
}

int palette_filter_cmds(int page, int *out_indices, int max_out, const char *query) {
    if (!out_indices || max_out <= 0) return 0;
    if (max_out > 64) max_out = 64;
    int count = 0;
    int total = palette_item_count(page);
    int title_only = 0;

    /* A terminal chooser should search the configured display name first.
     * If a name matches, command-line substrings from other entries must not
     * drown it out (for example, 'w' should prefer WSL over powershell.exe). */
    if (page == PALETTE_PAGE_NEW_TERMINAL && query && *query) {
        for (int i = 0; i < total; i++) {
            PaletteItemInfo item;
            if (palette_item_info(page, i, &item) && palette_strcasestr(item.title, query)) {
                title_only = 1;
                break;
            }
        }
    }

    int scores[64];
    for (int i = 0; i < total; i++) {
        PaletteItemInfo item;
        if (!palette_item_info(page, i, &item)) continue;
        int score = palette_match_score(&item, query ? query : "", title_only);
        if (score < 0) continue;
        if (count >= max_out && score >= scores[count - 1]) continue;

        int new_count = count < max_out ? count + 1 : max_out;
        int at = new_count - 1;
        while (at > 0 && scores[at - 1] > score) {
            out_indices[at] = out_indices[at - 1];
            scores[at] = scores[at - 1];
            at--;
        }
        out_indices[at] = i;
        scores[at] = score;
        count = new_count;
    }
    return count;
}

int palette_visible_rows(int host_rows) {
    int visible = PALETTE_MAX_VISIBLE;
    int max_visible = host_rows - 5; /* top=2, footer+bottom end at top+visible+4 */
    if (max_visible < 1) max_visible = 1;
    if (visible > max_visible) visible = max_visible;
    return visible;
}

void palette_editor_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h, int *input_w) {
    int pw = PALETTE_EDITOR_W;
    if (pw > host_cols) pw = host_cols;
    if (pw < 1) pw = 1;
    int ph = PALETTE_EDITOR_H;
    /* The three label/input pairs, divider and combined action/help row fit
     * in ten rows.  Do not stretch the child to the parent's list height:
     * that old filler area was visible as needless blank space below the
     * editor.  render_palette_editor() clears the stale parent rows below the
     * compact child surface instead. */
    if (host_rows > 0 && ph > host_rows) ph = host_rows;
    if (ph < 1) ph = 1;
    if (top) *top = 2;
    if (left) *left = (host_cols - pw) / 2 + 1;
    if (w) *w = pw;
    if (h) *h = ph;
    if (input_w) {
        int iw = pw - 4;
        if (iw < 8) iw = 8;
        *input_w = iw;
    }
}

void palette_geom(int host_rows, int host_cols, int *top, int *left, int *w, int *h) {
    if (g_mux.palette_page == PALETTE_PAGE_PANEL_EDITOR) {
        palette_editor_geom(host_rows, host_cols, top, left, w, h, NULL);
        return;
    }
    int pw = PALETTE_W;
    if (pw > host_cols) pw = host_cols;
    if (pw < 1) pw = 1;
    int visible = palette_visible_rows(host_rows);
    if (top) *top = 2;
    if (left) *left = (host_cols - pw) / 2 + 1;
    if (w) *w = pw;
    if (h) *h = visible + 5;
}

static const char *palette_page_title(int page) {
    switch (page) {
        case PALETTE_PAGE_ROOT: return "命令面板";
        case PALETTE_PAGE_OPERATIONS: return "命令面板 / 操作命令面板";
        case PALETTE_PAGE_SETTINGS: return "命令面板 / 设置命令面板";
        case PALETTE_PAGE_NEW_TERMINAL: return "操作 / 新建终端";
        case PALETTE_PAGE_SWITCH_PANEL: return "操作 / 切换 panel";
        case PALETTE_PAGE_DEFAULT_STARTUP: return "设置 / 默认启动项";
        case PALETTE_PAGE_ADD_PANEL: return "设置 / 添加 panel 条目";
        case PALETTE_PAGE_MENU_SETTINGS: return "设置 / 菜单项设置";
        case PALETTE_PAGE_PANEL_EDITOR: return "设置 / 编辑 panel 条目";
        default: return "命令面板";
    }
}

static void palette_hline(char *out, int bs, int *posp, int row, int left, int width, const char *prefix, const char *suffix) {
    int pos = *posp;
    int cols = 0;
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m%s", row, left, prefix);
    cols += utf8_cols(prefix, (int)strlen(prefix));
    while (cols < width - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80';
        cols++;
    }
    pos += snprintf(out + pos, bs - pos, "%s\x1b[0m", suffix);
    *posp = pos;
}

static void render_palette_item_row(char *out, int bs, int *posp, int row, int left, int width,
                                    int page, int item_index, int display_number, int selected, int hovered,
                                    const PaletteItemInfo *item) {
    int pos = *posp;
    const char *bg = (selected || hovered) ? "\x1b[048;2;038;060;088m" : "\x1b[048;2;022;027;034m";
    const char *fg = selected ? "\x1b[038;2;255;255;255;1m" : "\x1b[038;2;230;237;243m";
    char tag[16];
    int tagw;
    if (display_number > 0) {
        snprintf(tag, sizeof(tag), "[%d]", display_number);
        tagw = utf8_cols(tag, (int)strlen(tag));
    } else {
        snprintf(tag, sizeof(tag), "   ");
        tagw = 3;
    }

    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s", row, left, bg);
    int cols = 1; /* left border */
    pos += snprintf(out + pos, bs - pos, "%s ", selected ? "▶" : " ");
    cols += 2;

    /* v1.8.8: 序号一律用普通文字色，不再按标签颜色上色块、也不再用琥珀色，
     * 避免一列 [1][2][3] 花花绿绿抢视线。 */
    pos += snprintf(out + pos, bs - pos, "%s%s%s", bg,
                    selected ? "\x1b[038;2;230;237;243m" : "\x1b[038;2;139;148;158m", tag);
    cols += tagw;
    out[pos++] = ' '; cols++;

    int shortcut_w = item && item->shortcut ? utf8_cols(item->shortcut, (int)strlen(item->shortcut)) : 0;
    if (shortcut_w > 18) shortcut_w = 18;
    /* Reserve exactly the separator plus the shortcut; the final padding
     * loop supplies any remaining space before the right border. */
    int title_w = width - 1 - cols - (shortcut_w > 0 ? shortcut_w + 1 : 1);
    if (title_w < 4) title_w = 4;
    pos += snprintf(out + pos, bs - pos, "%s", fg);
    if (item) append_padded_utf8(out, bs, &pos, &cols, item->title ? item->title : "", title_w);
    else append_padded_utf8(out, bs, &pos, &cols, "", title_w);
    pos += snprintf(out + pos, bs - pos, "%s", bg);

    if (shortcut_w > 0 && item && item->shortcut) {
        out[pos++] = ' '; cols++;
        pos += snprintf(out + pos, bs - pos, "\x1b[038;2;139;148;158m");
        append_padded_utf8(out, bs, &pos, &cols, item->shortcut, shortcut_w);
        pos += snprintf(out + pos, bs - pos, "%s", bg);
    } else {
        out[pos++] = ' '; cols++;
    }
    while (cols < width - 1 && pos < bs - 8) { out[pos++] = ' '; cols++; }
    pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");
    (void)page;
    (void)item_index;
    *posp = pos;
}

static void render_palette_editor(char *out, int bs, int *posp, int host_rows, int host_cols) {
    int top, left, pw, ph, input_w;
    palette_editor_geom(host_rows, host_cols, &top, &left, &pw, &ph, &input_w);
    int pos = *posp;
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "┌─ %s ", palette_page_title(PALETTE_PAGE_PANEL_EDITOR));
    int hcols = utf8_cols(hdr, (int)strlen(hdr));
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;137;087;229;1m%s", top, left, hdr);
    while (hcols < pw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80'; hcols++;
    }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");

    const char *labels[3] = {
        "显示名称 (Display Name)",
        "启动命令行 (Command Line)",
        "启动目录 (Working Directory)"
    };
    char *bufs[3] = { g_edit_name, g_edit_cmd, g_edit_dir };
    int lens[3] = { g_edit_name_len, g_edit_cmd_len, g_edit_dir_len };
    int poss[3] = { g_edit_name_pos, g_edit_cmd_pos, g_edit_dir_pos };
    for (int i = 0; i < 3; i++) {
        int label_row = top + 1 + i * 2;
        int input_row = label_row + 1;
        int active = (g_mux.palette_field == i);
        const char *label_style = active ? "\x1b[038;2;230;237;243;1m" : "\x1b[038;2;230;237;243m";
        int label_cols = 1;
        pos += snprintf(out + pos, bs - pos,
                        "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[048;2;033;038;045m ",
                        label_row, left);
        pos += snprintf(out + pos, bs - pos, "%s", label_style);
        label_cols++;
        int label_w = pw - 1 - label_cols;
        if (label_w < 1) label_w = 1;
        append_padded_utf8(out, bs, &pos, &label_cols, labels[i], label_w);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");
        const char *field_bg = active ? "\x1b[048;2;038;060;088m" : "\x1b[048;2;022;027;034m";
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s ", input_row, left, field_bg);
        render_scrollable_input(out, bs, &pos, bufs[i], lens[i], poss[i], input_w, field_bg, NULL);
        pos += snprintf(out + pos, bs - pos, "%s \x1b[0m\x1b[048;2;033;038;045m│\x1b[0m", field_bg);
    }

    /* v1.8.9: 第 4 个字段 —— 启动默认颜色。 */
    {
        int label_row = top + 7;
        int active = (g_mux.palette_field == 3);
        const char *label = "启动默认颜色 (Tab Color)";
        int label_cols = 1;
        pos += snprintf(out + pos, bs - pos,
                        "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[048;2;033;038;045m ",
                        label_row, left);
        pos += snprintf(out + pos, bs - pos, "%s",
                        active ? "\x1b[038;2;230;237;243;1m" : "\x1b[038;2;230;237;243m");
        label_cols++;
        int label_w = pw - 1 - label_cols;
        if (label_w < 1) label_w = 1;
        append_padded_utf8(out, bs, &pos, &label_cols, label, label_w);
        pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");

        int color_row = top + 8;
        pos += snprintf(out + pos, bs - pos,
                        "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[048;2;022;027;034m",
                        color_row, left);
        int fill = pw - 2;
        for (int k = 0; k < fill && pos < bs - 8; k++) out[pos++] = ' ';
        pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");
        /* 浮层里也按宽度限格（v2.1.1）：极窄时别把色块条画到框外 */
        {
            int pc = (pw - 2 - ITEM_COLOR_DEFAULT_W) / ITEM_COLOR_SWATCH_W;
            if (pc > 8) pc = 8;
            if (pc < 1) pc = 1;
            g_item_color_max = pc;
        }
        render_item_color_row(out, bs, &pos, color_row, left + 1, g_edit_color, g_mux.palette_field == 3);
    }

    palette_hline(out, bs, &pos, top + 9, left, pw, "├", "┤");
    const char *save_hint = " [Enter] 保存并返回上一级";
    const char *editor_hint = "  Tab 切换字段 · ←/→ 选颜色 · Esc 返回 · Ctrl+S 保存 ";
    int action_row = top + 10;
    pos += snprintf(out + pos, bs - pos,
                    "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[038;2;121;192;255;1m%s"
                    "\x1b[038;2;139;148;158m%s",
                    action_row, left, save_hint, editor_hint);
    int used = 1 + utf8_cols(save_hint, (int)strlen(save_hint)) +
               utf8_cols(editor_hint, (int)strlen(editor_hint));
    while (used < pw - 1 && pos < bs - 8) { out[pos++] = ' '; used++; }
    pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");

    /* A page switch does not redraw the old parent list first.  Erase only
     * the rows that the old, taller parent box occupied below this compact
     * editor; do not use EL because the terminal pane continues to the right. */
    int parent_pw = PALETTE_W;
    if (parent_pw > host_cols) parent_pw = host_cols;
    if (parent_pw < 1) parent_pw = 1;
    int parent_left = (host_cols - parent_pw) / 2 + 1;
    int parent_h = palette_visible_rows(host_rows) + 5;
    for (int r = top + ph; r < top + parent_h; r++) {
        pos += snprintf(out + pos, bs - pos,
                        "\x1b[%d;%dH\x1b[048;2;022;027;034m",
                        r, parent_left);
        int clear_cols = 0;
        while (clear_cols < parent_pw && pos < bs - 8) {
            out[pos++] = ' ';
            clear_cols++;
        }
        pos += snprintf(out + pos, bs - pos, "\x1b[0m");
    }

    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m└", top + ph - 1, left);
    int cols = 1;
    while (cols < pw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80'; cols++;
    }
    pos += snprintf(out + pos, bs - pos, "┘\x1b[0m");
    *posp = pos;
}

void render_command_palette(char *out, int bs, int *posp, int host_rows, int host_cols) {
    if (g_mux.palette_page == PALETTE_PAGE_PANEL_EDITOR) {
        render_palette_editor(out, bs, posp, host_rows, host_cols);
        return;
    }

    int top, left, pw, ph;
    palette_geom(host_rows, host_cols, &top, &left, &pw, &ph);
    int pos = *posp;
    int filtered[64];
    int filtered_count = palette_filter_cmds(g_mux.palette_page, filtered, 64, g_mux.palette_query);
    int visible = palette_visible_rows(host_rows);
    if (g_mux.palette_sel >= filtered_count) g_mux.palette_sel = filtered_count > 0 ? filtered_count - 1 : 0;
    if (g_mux.palette_sel < 0) g_mux.palette_sel = 0;
    if (g_mux.palette_sel < g_mux.palette_scroll) g_mux.palette_scroll = g_mux.palette_sel;
    if (g_mux.palette_sel >= g_mux.palette_scroll + visible)
        g_mux.palette_scroll = g_mux.palette_sel - visible + 1;
    int max_scroll = filtered_count > visible ? filtered_count - visible : 0;
    if (g_mux.palette_scroll > max_scroll) g_mux.palette_scroll = max_scroll;
    if (g_mux.palette_scroll < 0) g_mux.palette_scroll = 0;

    char hdr[160];
    snprintf(hdr, sizeof(hdr), "┌─ %s ", palette_page_title(g_mux.palette_page));
    int cols = utf8_cols(hdr, (int)strlen(hdr));
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[038;2;255;255;255m\x1b[048;2;137;087;229;1m%s", top, left, hdr);
    while (cols < pw - 1 && pos < bs - 8) {
        out[pos++] = '\xe2'; out[pos++] = '\x94'; out[pos++] = '\x80'; cols++;
    }
    pos += snprintf(out + pos, bs - pos, "┐\x1b[0m");

    int input_w = pw - 6;
    if (input_w < 8) input_w = 8;
    const char *palette_input_bg = g_mux.palette_focus == PALETTE_FOCUS_INPUT
        ? "\x1b[048;2;038;060;088m" : "\x1b[048;2;022;027;034m";
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m%s > ", top + 1, left, palette_input_bg);
    render_scrollable_input(out, bs, &pos, g_mux.palette_query, g_mux.palette_query_len, g_mux.palette_query_pos, input_w, palette_input_bg, NULL);
    pos += snprintf(out + pos, bs - pos, "%s \x1b[0m\x1b[048;2;033;038;045m│\x1b[0m", palette_input_bg);

    palette_hline(out, bs, &pos, top + 2, left, pw, "├", "┤");

    for (int vi = 0; vi < visible; vi++) {
        int row = top + 3 + vi;
        int fi = g_mux.palette_scroll + vi;
        if (fi < filtered_count) {
            PaletteItemInfo item;
            palette_item_info(g_mux.palette_page, filtered[fi], &item);
            int mouse_row = g_mouse_y + 1;
            int mouse_col = g_mouse_x + 1;
            int hovered = (mouse_row == row && mouse_col >= left && mouse_col < left + pw);
            render_palette_item_row(out, bs, &pos, row, left, pw, g_mux.palette_page,
                                    filtered[fi], vi + 1, fi == g_mux.palette_sel, hovered, &item);
        } else {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[048;2;022;027;034m", row, left);
            cols = 1;
            if (filtered_count == 0 && vi == 0) {
                const char *none = "  无匹配项目";
                pos += snprintf(out + pos, bs - pos, "\x1b[038;2;139;148;158m%s", none);
                cols += utf8_cols(none, (int)strlen(none));
            }
            while (cols < pw - 1 && pos < bs - 8) { out[pos++] = ' '; cols++; }
            pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");
        }
    }

    int footer = top + 3 + visible;
    const char *focus_label = g_mux.palette_focus == PALETTE_FOCUS_LIST ? "选择" : "输入";
    /* An untouched query field hands arrows/digits straight to the list, so
     * advertise the digits as a quick pick there too. */
    const char *digit_label = (g_mux.palette_focus == PALETTE_FOCUS_LIST ||
                               g_mux.palette_query_len == 0) ? "选当前" : "搜索";
    char footer_hint[192];
    if (g_mux.palette_page == PALETTE_PAGE_MENU_SETTINGS) {
        snprintf(footer_hint, sizeof(footer_hint),
                 g_mux.palette_query_len
                     ? " [焦点:%s] Tab切换 · 数字:%s · Enter编辑 · Ctrl+X删 · 搜索禁调序"
                     : " [焦点:%s] Tab切换 · 数字:%s · Enter编辑 · Ctrl+↑/↓调序 · X删",
                 focus_label, digit_label);
    } else {
        snprintf(footer_hint, sizeof(footer_hint),
                 " [焦点:%s] Tab切换 · 数字:%s · Enter执行/进入 · Esc返回",
                 focus_label, digit_label);
    }
    pos += snprintf(out + pos, bs - pos,
                    "\x1b[%d;%dH\x1b[048;2;033;038;045m│\x1b[0m\x1b[048;2;033;038;045m\x1b[038;2;139;148;158m",
                    footer, left);
    int footer_cols = 0;
    int footer_inner = pw > 2 ? pw - 2 : 0;
    if (footer_inner > 0)
        append_padded_utf8(out, bs, &pos, &footer_cols, footer_hint, footer_inner);
    pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[048;2;033;038;045m│\x1b[0m");
    palette_hline(out, bs, &pos, top + ph - 1, left, pw, "└", "┘");
    *posp = pos;
}

static const char *const g_help_head[] = {
    "\x1b[038;2;255;255;255m\x1b[048;2;031;111;235m termux - 帮助",
    "\x1b[038;2;139;148;158m  版本 v" TERMUX_VERSION " | " TERMUX_HELP_PLATFORM_U8 "\x1b[0m",
    "",
    "\x1b[038;2;121;192;255;1m  键盘快捷键\x1b[0m",
};
static const int g_help_head_count = (int)(sizeof(g_help_head) / sizeof(g_help_head[0]));

/* 快捷键区：键位从 keymap 实时取，改了 prefix / [keys] 后帮助页同步变化 */
typedef struct {
    int action;
    const char *extra;   /* 追加说明，可为 NULL */
    const char *desc;
} HelpShortcut;

static const HelpShortcut g_help_shortcuts[] = {
    {ACT_COMMAND_PALETTE, NULL,         "命令面板（操作 / 设置两类）"},
    {ACT_NEW_PANE,        NULL,         "新建默认 pane"},
    {ACT_NEW_PANE_MENU,   NULL,         "新建 pane 菜单 (选择/自定义命令行)"},
    {ACT_COPY_MODE,       NULL,         "进入复制模式 (Shift/Alt+方向选择, Enter/Ctrl+C 复制)"},
    {ACT_SPLIT_HORIZONTAL,NULL,         "分屏：上下切分（同标签新建窗格）"},
    {ACT_SPLIT_VERTICAL,  NULL,         "分屏：左右切分（同标签新建窗格）"},
    {ACT_SPLIT_NEXT,      "Shift 反向", "分屏：循环切换窗格"},
    {ACT_SPLIT_ZOOM,      NULL,         "分屏：当前窗格全屏缩放 / 还原"},
    {ACT_SPLIT_CLOSE,     NULL,         "分屏：关闭当前窗格（单窗格则关闭标签）"},
    {ACT_SEARCH,          NULL,         "搜索滚动历史 (n/N 跳转匹配, Esc 退出)"},
    {ACT_NEXT_PANE,       NULL,         "下一个 pane"},
    {ACT_PREV_PANE,       NULL,         "上一个 pane"},
    {ACT_CLOSE_PANE,      NULL,         "关闭当前 panel"},
    {ACT_SETTINGS,        NULL,         "打开图形化设置 (termux.ini)"},
    {ACT_RELOAD_CONFIG,   NULL,         "热重载配置文件 (termux.ini)"},
    {ACT_NEXT_THEME,      NULL,         "切换配色主题"},
    {ACT_HELP,            NULL,         "打开 / 关闭本帮助"},
    {ACT_QUIT,            NULL,         "退出 termux"},
    {ACT_TAB_COLOR_NEXT,  "Shift 反向", "轮换标签颜色"},
};
static const int g_help_shortcut_count = (int)(sizeof(g_help_shortcuts) / sizeof(g_help_shortcuts[0]));

static const char *const g_help_tail[] = {
    "",
    "\x1b[038;2;121;192;255;1m  鼠标操作\x1b[0m",
    "  \x1b[038;2;230;237;243m点击 tab\x1b[0m           切换 pane",
    "  \x1b[038;2;230;237;243m点击 [x]\x1b[0m           关闭该 pane",
    "  \x1b[038;2;230;237;243m右键 tab\x1b[0m           改颜色 / 改标题",
    "  \x1b[038;2;230;237;243m点击 [+]\x1b[0m           新建 pane (支持选择/自定义命令行)",
    "  \x1b[038;2;230;237;243m点击 [*]\x1b[0m           打开图形化设置页面",
    "  \x1b[038;2;230;237;243m点击 termux\x1b[0m        打开 / 关闭本帮助",
    "  \x1b[038;2;230;237;243m鼠标左键拖选\x1b[0m       框选终端文字，松开自动复制到剪贴板",
    "",
    "\x1b[038;2;121;192;255;1m  提示与警告\x1b[0m",
    "  - \x1b[038;2;248;081;073m警告: 终端必须使用等宽字体，否则会渲染故障\x1b[0m",
    "  - 每个 tab 带 \x1b[038;2;248;081;073m红 x\x1b[0m 关闭按钮（悬停红底）",
    "  - 编辑器 (nano/vim) 用 alt screen，退出后历史完整保留",
    "  - PgUp / PgDn / 滚轮可滚动本帮助与终端历史",
    "  - 按任意其它键返回",
};
static const int g_help_tail_count = (int)(sizeof(g_help_tail) / sizeof(g_help_tail[0]));

/* 把 "Ctrl+B c" 拆成前缀段与按键段分别着色 */
static const char *help_shortcut_line(int idx, char *buf, int buf_size) {
    const HelpShortcut *hs = &g_help_shortcuts[idx];
    char combo[64] = {0};
    keymap_describe(hs->action, combo, sizeof(combo));
    if (!combo[0]) {
        char pfx[32];
        keymap_prefix_describe(pfx, sizeof(pfx));
        snprintf(combo, sizeof(combo), "%s -", pfx);
    }

    /* 与 combo 同容量：直接键路径把整个 combo 拷进 key，不再让编译器算「单段
     * 最长 24 所以装得下」这种跨函数推理（-O1 门禁会报 format-truncation）。 */
    char prefix[64] = {0}, key[64] = {0};
    const char *sp = strrchr(combo, ' ');
    /* v1.8.7: 被设为「直接键」的动作 combo 里没有前缀段，左列改标注「直接」。 */
    if (!keymap_action_uses_prefix(hs->action)) {
        snprintf(prefix, sizeof(prefix), "%s", "直接");
        snprintf(key, sizeof(key), "%s", combo);
    } else if (sp) {
        int plen = (int)(sp - combo);
        if (plen > (int)sizeof(prefix) - 1) plen = (int)sizeof(prefix) - 1;
        memcpy(prefix, combo, plen);
        snprintf(key, sizeof(key), "%s", sp + 1);
    } else {
        snprintf(prefix, sizeof(prefix), "%s", combo);
    }

    int kw = (int)strlen(key);
    int pad = kw < 9 ? 9 - kw : 1;
    char keycol[80];                        /* key[64] + 最多 9 列补白 */
    snprintf(keycol, sizeof(keycol), "%s%*s", key, pad, "");

    if (hs->extra) {
        snprintf(buf, buf_size,
                 "  \x1b[038;2;210;153;034m%s\x1b[0m + \x1b[038;2;230;237;243m%s\x1b[0m%s (%s)",
                 prefix, keycol, hs->desc, hs->extra);
    } else {
        snprintf(buf, buf_size,
                 "  \x1b[038;2;210;153;034m%s\x1b[0m + \x1b[038;2;230;237;243m%s\x1b[0m%s",
                 prefix, keycol, hs->desc);
    }
    return buf;
}

static int help_total_lines(void) {
    return g_help_head_count + g_help_shortcut_count + g_help_tail_count;
}

static const char *help_line_at(int idx, char *buf, int buf_size) {
    if (idx < 0) return "";
    if (idx < g_help_head_count) return g_help_head[idx];
    idx -= g_help_head_count;
    if (idx < g_help_shortcut_count) return help_shortcut_line(idx, buf, buf_size);
    idx -= g_help_shortcut_count;
    if (idx < g_help_tail_count) return g_help_tail[idx];
    return "";
}

void render_help_content(char *out, int bs, int *posp, int host_rows, int host_cols) {
    (void)host_cols;
    int pos = *posp;
    int vis = host_rows;
    int line_count = help_total_lines();
    int max_sc = line_count - vis;
    if (max_sc < 0) max_sc = 0;
    if (g_mux.help_scroll > max_sc) g_mux.help_scroll = max_sc;
    if (g_mux.help_scroll < 0) g_mux.help_scroll = 0;
    for (int r = 0; r < vis; r++) {
        int li = g_mux.help_scroll + r;
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H\x1b[K", r + 2);
        if (li < line_count) {
            char linebuf[256];
            pos += snprintf(out + pos, bs - pos, "%s", help_line_at(li, linebuf, sizeof(linebuf)));
        }
    }
    *posp = pos;
}

static char *g_render_buf = NULL;
static int g_render_buf_cap = 0;
static int g_settings_nowrap_on = 0;   /* 本帧 body 里发过 ?7l，帧尾要补 ?7h */

/* v1.8.12 脏区渲染：上一帧影子。整帧仍照常生成（绝对光标定位），
 * 输出前按行与影子比对，没变的行不发，省掉绝大部分字节。 */
static FrameDiff g_frame_diff;
static char *g_diff_buf = NULL;
static size_t g_diff_buf_cap = 0;

static char *render_buffer_acquire(int needed) {
    if (needed <= 0) return NULL;
    if (g_render_buf_cap >= needed) return g_render_buf;
    int cap = g_render_buf_cap > 0 ? g_render_buf_cap : 16384;
    while (cap < needed) {
        if (cap > 0x3FFFFFFF) { cap = needed; break; }
        cap *= 2;
    }
    char *next = (char *)realloc(g_render_buf, (size_t)cap);
    if (!next) return NULL;
    g_render_buf = next;
    g_render_buf_cap = cap;
    return g_render_buf;
}

static const struct {
    unsigned char thumb_r, thumb_g, thumb_b;
    unsigned char track_bg_r, track_bg_g, track_bg_b;
    unsigned char track_fg_r, track_fg_g, track_fg_b;
} g_sb_grad[11] = {
    { 220, 230, 245,  33, 38, 45,  139, 148, 158 },
    { 190, 205, 225,  31, 36, 43,  125, 134, 144 },
    { 160, 178, 200,  28, 33, 40,  110, 118, 128 },
    { 130, 150, 175,  26, 30, 36,   95, 102, 112 },
    { 105, 125, 150,  23, 27, 33,   80,  88,  96 },
    {  85, 102, 125,  20, 24, 29,   66,  72,  80 },
    {  68,  82, 102,  17, 21, 26,   52,  58,  65 },
    {  55,  66,  82,  15, 18, 22,   40,  45,  52 },
    {  45,  54,  66,  13, 16, 19,   30,  35,  42 },
    {  38,  44,  52,  11, 14, 17,   26,  30,  38 },
    {  30,  35,  42,  10, 13, 16,   20,  24,  30 }
};

/* v2.0.8：[theme] pane_scrollbar / pane_scrollbar_track 设了就覆盖内置渐变
 * （渐变按鼠标距离变深，只为「不悬停时淡出」；用户自定义时保持恒定色）。
 * 轨道的「│」字色取轨道底色与滑块色的中间值，保证在任何底色上都看得见。 */
static void sb_custom_thumb(int *r, int *g, int *b) {
    int cr, cg, cb;
    if (theme_pane_rgb(THEME_PANE_SB_THUMB, &cr, &cg, &cb)) { *r = cr; *g = cg; *b = cb; }
}
static void sb_custom_track(int *br, int *bg, int *bb, int *fr, int *fg, int *fb) {
    int cr, cg, cb;
    if (!theme_pane_rgb(THEME_PANE_SB_TRACK, &cr, &cg, &cb)) return;
    *br = cr; *bg = cg; *bb = cb;
    int tr = 105, tg = 125, tb = 150;
    sb_custom_thumb(&tr, &tg, &tb);
    *fr = (cr + tr) / 2; *fg = (cg + tg) / 2; *fb = (cb + tb) / 2;
}

static int terminal_cursor_position(const ScreenBuffer *s, int scroll_offset,
                                     int host_rows, int host_cols, int *out_row, int *out_col) {
    if (!s || scroll_offset != 0 || !s->cursor_visible || host_rows < 1 || host_cols < 1)
        return 0;

    int rr = s->rows < host_rows ? s->rows : host_rows;
    int rc = s->cols < host_cols ? s->cols : host_cols;
    if (rr <= 0 || rc <= 0) return 0;

    int cx = s->cursor_x;
    int cy = s->cursor_y;
    /* VT auto-wrap is delayed until the next character.  During that
     * pending state the cursor still belongs to the last cell that was
     * written; moving it to the next row made the terminal output cursor
     * disappear from the rightmost cell. */
    if (cx >= rc) cx = rc - 1;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cy >= rr) cy = rr - 1;

    if (out_row) *out_row = cy + 2;
    if (out_col) *out_col = cx + 1;
    return 1;
}

/* 渲染时取屏幕第 row 行的整行 WCHAR（供选区端点整字吸附）。活屏与回滚历史
 * 都要覆盖：原来只在 vo>0 时才有 phys 行，活屏 ar=-1 会漏吸附，导致在当前
 * 屏幕上鼠标选中汉字时高亮把宽字符切成半个（看似光标停在字中间）。 */
/* 取屏幕第 row 行的单元格缓冲（供选区端点整字吸附）。返回 CHAR_INFO*（真实
 * 步长）；旧实现返回 &cells[0].Char.UnicodeChar（WCHAR*），按 2 字节步长索引
 * 会读到相邻单元格的 Attributes、列号错位。活屏 / 回滚历史 / alt 屏都覆盖。 */
static const CHAR_INFO *render_sel_line(ScreenBuffer *s, int row, int vo) {
    if (s->in_alt_screen && s->alt_buffer && row >= 0 && row < s->rows)
        return &s->alt_buffer[(size_t)row * s->cols];
    int abs_y = screen_to_abs_row(s, row, vo);
    int pr = (abs_y >= 0 && abs_y < s->total_lines)
             ? (s->scroll_top - s->hist_lines + abs_y + s->total_lines * 2) % s->total_lines : -1;
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) return NULL;
    return s->lines[pr].cells;
}

/* 选区在某一行上的端点吸附到完整字符：块选每行两端都吸；流式选区只在
 * 首行吸左、末行吸右，中间整行不碰。宽字符（中文/全角/emoji）由此不会被
 * 切成半个高亮/半个复制。sel_active 为 0 时直接返回。 */
static void snap_sel_row(ScreenBuffer *s, int row, int vo, int sel_active, int block,
                               int cur_abs_y, int min_abs_y, int max_abs_y,
                               int *sel_min_x, int *sel_max_x) {
    if (!sel_active) return;
    const CHAR_INFO *sl = render_sel_line(s, row, vo);
    if (!sl) return;
    int at_first = (cur_abs_y == min_abs_y), at_last = (cur_abs_y == max_abs_y);
    if (block) {
        *sel_min_x = snap_left_to_char(sl, s->cols, *sel_min_x);
        *sel_max_x = snap_right_to_char(sl, s->cols, *sel_max_x);
    } else {
        if (at_first) *sel_min_x = snap_left_to_char(sl, s->cols, *sel_min_x);
        if (at_last)  *sel_max_x = snap_right_to_char(sl, s->cols, *sel_max_x);
    }
}

/* =========================================================================
 * 分屏渲染：把当前标签页的多窗格按矩形裁剪后画到内容区。
 * 单窗格（整屏）路径完全不走这里——分屏状态下才调用。每个窗格的 screen 缓冲
 * 尺寸由 pane_resize_to() 与它的矩形保持一致，因此这里逐格读出、按 (rc+1,cc+1)
 * 绝对定位写出；窗格间 1 行/列画分隔边框，活动窗格边框高亮。
 * ========================================================================= */

/* 缩放（zoom）状态：非 0 时只把活动窗格铺满内容区，其余隐藏。全局由
 * input.c 定义（ACT_SPLIT_ZOOM 切换）。 */
extern int g_split_zoom;

/* 计算内容区里每个可见 pane 的矩形，并同步 screen/ConPTY 尺寸。返回可见叶子数。
 * zoom 时只让活动 pane 有效并铺满；其余 pane 标记为不可见（不 resize，保持原尺寸）。 */
static int split_compute_rects(PaneRect *rects) {
    for (int i = 0; i < MAX_PANES; i++) memset(&rects[i], 0, sizeof(rects[i]));
    int root = split_active_root();
    if (root < 0) return 0;
    /* 内容区：第 0 行标签栏已在上面画过；内容从第 1 行（终端行号 2）起。 */
    split_layout(root, 0, 0, g_mux.host_cols, g_mux.host_rows, split_nodes(), rects);
    int n = 0;
    int active = g_mux.active_pane;
    for (int i = 0; i < MAX_PANES; i++) {
        if (!rects[i].valid) continue;
        if (i >= g_mux.pane_count || !g_mux.panes[i].active) { rects[i].valid = 0; continue; }
        if (g_split_zoom && i != active) { rects[i].valid = 0; continue; }
        n++;
        /* zoom：活动窗格铺满整个内容区（无内缩、无边框）。 */
        if (g_split_zoom && i == active) {
            rects[i].oc0 = rects[i].c0 = 0;
            rects[i].or0 = rects[i].r0 = 0;
            rects[i].ocols = rects[i].cols = g_mux.host_cols;
            rects[i].orows = rects[i].rows = g_mux.host_rows;
        }
        if (rects[i].cols >= 1 && rects[i].rows >= 1)
            pane_resize_to(i, rects[i].cols, rects[i].rows);
    }
    return n;
}

static void render_split_borders(char *out, int bs, int *posp, PaneRect *rects) {
    int pos = *posp;
    /* 收集所有行/列的分隔线：任意两 pane 之间留白的整列/整行画边框。 */
    /* 列分隔：某一列 x 满足「左边有 pane 到 x-1、右边有 pane 从 x+1 开始」即画竖线。 */
    const char *v_on  = "\x1b[048;2;033;038;045m\x1b[038;2;121;192;255;1m│\x1b[0m";
    const char *v_off = "\x1b[048;2;022;027;034m\x1b[038;2;110;118;129m│\x1b[0m";
    const char *h_on  = "\x1b[048;2;033;038;045m\x1b[038;2;121;192;255;1m─\x1b[0m";
    const char *h_off = "\x1b[048;2;022;027;034m\x1b[038;2;110;118;129m─\x1b[0m";
    /* 竖线列：pane 外接分配区域右沿 x=oc0+ocols（其右邻 pane 从 x+1 起）。
     * 边框线跨度也按外接高度（整段分隔线连续）；窗格内容已内缩，线两侧各有空格。 */
    for (int i = 0; i < MAX_PANES; i++) {
        if (!rects[i].valid) continue;
        int bx = rects[i].oc0 + rects[i].ocols;   /* 0 基边框列 */
        if (bx >= g_mux.host_cols - 1) continue;
        int has_neighbor = 0;
        for (int j = 0; j < MAX_PANES; j++)
            if (rects[j].valid && j != i && rects[j].oc0 == bx + 1) has_neighbor = 1;
        if (!has_neighbor) continue;
        for (int r = rects[i].or0; r < rects[i].or0 + rects[i].orows; r++) {
            int active_border = (i == g_mux.active_pane);
            const char *seg = active_border ? v_on : v_off;
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s", r + 2, bx + 1, seg);
        }
    }
    /* 横线行：pane 外接分配区域下沿 y=or0+orows。 */
    for (int i = 0; i < MAX_PANES; i++) {
        if (!rects[i].valid) continue;
        int by = rects[i].or0 + rects[i].orows;   /* 0 基边框行 */
        if (by >= g_mux.host_rows - 1) continue;
        int has_neighbor = 0;
        for (int j = 0; j < MAX_PANES; j++)
            if (rects[j].valid && j != i && rects[j].or0 == by + 1) has_neighbor = 1;
        if (!has_neighbor) continue;
        int active_border = (i == g_mux.active_pane);
        for (int c = rects[i].oc0; c < rects[i].oc0 + rects[i].ocols; c++) {
            const char *seg = active_border ? h_on : h_off;
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH%s", by + 2, c + 1, seg);
        }
    }
    *posp = pos;
}

/* 画单个 pane 的一个单元格（带真彩/16 色与宽字符处理），定位到内容区绝对坐标。 */
/* 诊断（TERMUX_CELLDIAG）：把分屏逐格渲染【实际读到】的模型内容写进 cell_diag.log。
 * 排查「中文里凭空多出空格」——需要知道宽字符次格在渲染那一刻到底是 0（正确，会被
 * 跳过）还是空格（会被当成内容发出去）。默认关闭，getenv 门控，无环境变量时零开销。
 *
 * 帧号对齐（2026-09-13 修正）：早先这里声称「cell_diag_frame 与 render_dump.log 的
 * 帧序一致（都在 render_screen 每帧自增一次）」——【这句是错的】，真机日志证明两份
 * 日志对不上：cell_diag 记录 pane 86+33，而 render_dump 全文 307 帧的 model 宽度
 * 只有 {120,60,59,20,49,71,48}，【从未出现过 86 或 33】。
 *
 * 真实原因（不是帧号跳号——cell_diag 的 F71–F140 是连续的）：两个计数器不同源。
 * cell_diag_frame 是 render_screen 里自增的本地计数器，从 1 开始；而 render_dump
 * 那份 307 帧里包含大量 cell_diag 根本没有落盘的帧（cell_diag 只在「该行含宽字符」
 * 时才输出），于是同一个 F 号在两份日志里指向完全不同的时刻。日志里 70 帧 vs 307 帧
 * 的差距就是证据。
 *
 * 现在改成：每帧都写一行 FRAME 帧头，位置就在 dump_render_output 的紧前面，且受
 * 【完全相同】的三道门约束（TERMUX_DUMP、len>0、active pane 有效）；帧头里带上
 * render_dump 同一帧会打的 model 尺寸，以及每个 pane 的 rc->cols / s->cols /
 * min(两者)。这样两份日志可以逐帧核对，不用再靠猜。 */
static int cell_diag_frame = 0;

/* 帧头素材：render_split_pane 逐 pane 填写，render_screen 末尾（与
 * dump_render_output 同一位置）一次性写出。这样 cell_diag.log 与 render_dump.log
 * 的帧号严格同源，可以逐帧对照。 */
typedef struct {
    int valid;
    int oc0, ocols;        /* 布局：外接起点列 / 外接宽度 */
    int scols, srows;      /* 模型：s->cols / s->rows */
    int cols, rows;        /* 实际逐格渲染的 min(rc,s) */
    int vo, hist;          /* scroll_offset / hist_lines */
    int conpty_cols;       /* 上次下发给 ConPTY 的宽度 */
    int narrow;            /* 1 = 渲染宽度窄于模型宽度（拖动中），本帧走 reflow 网格 */
} CellDiagPane;
static CellDiagPane g_cd_pane[MAX_PANES];

/* 帧内缓冲：行级诊断先攒在这里，帧末与帧头一起落盘，日志里每帧就是一个完整块
 * （FRAME 行 + 该帧的行级明细），不会出现「明细在上、帧头在下」的割裂。
 * 容量给 1 MB：单帧最多 MAX_PANES*rows 行，每行约 300 字节，足够。 */
#define CELL_DIAG_BUF (1024 * 1024)
static char g_cd_buf[CELL_DIAG_BUF];
static int  g_cd_len = 0;

static void cell_diag_frame_buf(const char *fmt, ...) {
    if (g_cd_len >= CELL_DIAG_BUF - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_cd_buf + g_cd_len, (size_t)(CELL_DIAG_BUF - g_cd_len), fmt, ap);
    va_end(ap);
    if (n > 0) g_cd_len += (n < CELL_DIAG_BUF - g_cd_len) ? n : (CELL_DIAG_BUF - 1 - g_cd_len);
}

static void cell_diag(const char *fmt, ...) {
    static int diag_on = -1;
    if (diag_on < 0) diag_on = getenv("TERMUX_CELLDIAG") ? 1 : 0;
    if (!diag_on) return;
    FILE *f = fopen("cell_diag.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}


/* v2.0.6：16 色索引属性 → SGR。设了窗格 palette（[theme] pane_*）时改发真彩色，
 * 像 Windows Terminal 的 color scheme 那样让 cmd 的默认前后景 / 16 色跟主题走；
 * 一项都没设时和以前一字节不差（\x1b[0;37;40m 这种）。
 * 0x07 的 fg=7/bg=0 是 cmd 的「默认属性」（SGR 39/49 也归到这里，见 vt.c:102），
 * 有 pane_foreground / pane_background 时优先用它们，没有再落到索引 7 / 0。 */
static int emit_attr16(char *out, int bs, WORD attr, const char *ul) {
    static const int m8[8] = {0,4,2,6,1,5,3,7};
    int fg = attr & 0x0F, bg = (attr >> 4) & 0x0F;
    if (theme_pane_any()) {
        /* attr 里存的是 Win32 位标志（RED=4/GREEN=2/BLUE=1，见 screen.c build_attr
         * 的 ctab），pane_* 槽位按 ANSI 索引编号（red=1/green=2/blue=4）—— m8 就是
         * 这张位标志→ANSI 的换算表，查槽位前必须先换算（第一版直接拿 fg 当索引，
         * pane_red 落到了 pane_blue，verify_pane_palette.py C 项验红抓到）。 */
        int fr, fgc, fb, br, bgc, bb, have_f = 0, have_b = 0;
        int fg_ansi = m8[fg & 7] | (fg & 8), bg_ansi = m8[bg & 7] | (bg & 8);
        if (fg == 7 && theme_pane_rgb(THEME_PANE_FG, &fr, &fgc, &fb)) have_f = 1;
        else have_f = theme_pane_rgb(fg_ansi, &fr, &fgc, &fb);
        if (bg == 0 && theme_pane_rgb(THEME_PANE_BG, &br, &bgc, &bb)) have_b = 1;
        else have_b = theme_pane_rgb(bg_ansi, &br, &bgc, &bb);
        if (have_f || have_b) {
            int pos = snprintf(out, bs, "\x1b[0%s", ul);
            if (have_f) pos += snprintf(out + pos, bs - pos, ";38;2;%d;%d;%d", fr, fgc, fb);
            else        pos += snprintf(out + pos, bs - pos, ";%d", (fg & 8) ? 90 + m8[fg & 7] : 30 + m8[fg & 7]);
            if (have_b) pos += snprintf(out + pos, bs - pos, ";48;2;%d;%d;%d", br, bgc, bb);
            else        pos += snprintf(out + pos, bs - pos, ";%d", (bg & 8) ? 100 + m8[bg & 7] : 40 + m8[bg & 7]);
            pos += snprintf(out + pos, bs - pos, "m");
            return pos;
        }
    }
    if (fg & 8) return snprintf(out, bs, "\x1b[0%s;1;%d;%dm", ul, 90 + m8[fg & 7], (bg & 8) ? 100 + m8[bg & 7] : 40 + m8[bg & 7]);
    return snprintf(out, bs, "\x1b[0%s;%d;%dm", ul, 30 + m8[fg & 7], 40 + m8[bg & 7]);
}

static void render_split_cell(char *out, int bs, int *posp, ScreenBuffer *s,
                              Pane *pane, int leaf, int px, int py, int rr, int cc,
                              int use_rf) {
    int pos = *posp;
    int x = px, y = py;
    int vo = pane->scroll_offset;
    WCHAR wc = L' '; WORD attr = 0x07;
    WORD frgb = RGB565_WHITE, brgb = RGB565_BLACK; int fgv = 0, bgv = 0;
    if (use_rf) {
        /* 历史 reflow 视图：从窗格缓存网格取（逻辑行按当前宽重排后）。 */
        RGlyph *grid = (RGlyph *)pane->rf_grid;
        int cols = pane->rf_cols;
        if (grid && cols > 0) {
            RGlyph *g = &grid[y * cols + x];
            wc = g->ci.Char.UnicodeChar; attr = g->ci.Attributes;
            frgb = g->fg; brgb = g->bg; fgv = g->v & 1; bgv = (g->v >> 1) & 1;
        }
        if (wc == 0) { *posp = pos; return; }   /* 宽字符次格不写 */
    } else {
        int ar = (vo > 0 && !s->in_alt_screen) ? screen_phys_row(s, y - vo) : -1;
        CHAR_INFO *cell = (ar >= 0) ? ((s->lines && s->lines[ar].cells) ? &s->lines[ar].cells[x] : NULL)
                                    : screen_cell(s, y, x);
        if (cell) { wc = cell->Char.UnicodeChar; attr = cell->Attributes; }
        /* 空出的次格（宽字符占位）不写。 */
        if (wc == 0) { *posp = pos; return; }
        cell_truecolor(s, y, x, ar, &frgb, &brgb, &fgv, &bgv);
    }
    int active = (leaf == g_mux.active_pane);
    (void)active;

    /* 注意：不要把「黑底空白格」强行改成面板底色——cmd/程序正常的黑底背景里，
     * 空行、缩进、清屏区都是「空格 + 16 色黑底」，刷成深灰面板色会在黑色终端里
     * 冒出奇怪的深灰块。resize 新扩出的未填充区域由 render_split 的整行面板底色
     * 铺底覆盖（窗格矩形外）+ shell 随后重绘（窗格矩形内），不在逐格渲染时改色。 */
    /* 必须【先】用 CUP 定位到本格，【再】发 SGR + 字符。若先设颜色再定位，当本格
     * 背景与上一格不同（典型：窗格左缘第一格、或空格/黑底格），这段 SGR 的背景色
     * 会把「上一格末列 → 本格 CUP 位置」之间的间隙格染成当前背景——分屏下窗格左缘
     * （c0>0）这道间隙正好是窗格最左一列，表现为「移动窗格后每个窗格最左侧有一条
     * 浅背景」。先定位后设色，SGR 只作用于定位点之后的本格，不回染左侧间隙。 */
    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH", rr + 2, cc + 1);
    const char *ul = (attr & COMMON_LVB_UNDERSCORE) ? ";4" : "";
    if (fgv || bgv) {
        int fr, fg2, fb; rgb565_split(frgb, &fr, &fg2, &fb);
        int br2, bg2, bb; rgb565_split(brgb, &br2, &bg2, &bb);
        if (fgv && bgv)
            pos += snprintf(out + pos, bs - pos, "\x1b[0%s;38;2;%d;%d;%d;48;2;%d;%d;%dm", ul, fr, fg2, fb, br2, bg2, bb);
        else if (fgv)
            pos += snprintf(out + pos, bs - pos, "\x1b[0%s;38;2;%d;%d;%dm", ul, fr, fg2, fb);
        else
            pos += snprintf(out + pos, bs - pos, "\x1b[0%s;48;2;%d;%d;%dm", ul, br2, bg2, bb);
    } else {
        pos += emit_attr16(out + pos, bs - pos, attr, ul);
    }
    (void)active;

    /* 宽字符 emoji 代理对合成（与整屏路径一致）。 */
    int cols_max = use_rf ? pane->rf_cols : s->cols;
    if (wc >= 0xD800 && wc <= 0xDBFF && x + 1 < cols_max) {
        CHAR_INFO next_ci;
        const CHAR_INFO *next_cell = NULL;
        if (use_rf) {
            RGlyph *grid = (RGlyph *)pane->rf_grid;
            if (grid) { next_ci = grid[y * pane->rf_cols + x + 1].ci; next_cell = &next_ci; }
        } else {
            int ar2 = (vo > 0 && !s->in_alt_screen) ? screen_phys_row(s, y - vo) : -1;
            next_cell = (ar2 >= 0) ? ((s->lines && s->lines[ar2].cells) ? &s->lines[ar2].cells[x + 1] : NULL)
                                   : screen_cell(s, y, x + 1);
        }
        if (next_cell && next_cell->Char.UnicodeChar >= 0xDC00 && next_cell->Char.UnicodeChar <= 0xDFFF) {
            WCHAR low = next_cell->Char.UnicodeChar;
            unsigned int cp = 0x10000 + (((unsigned int)(wc & 0x3FF)) << 10) + (low & 0x3FF);
            out[pos++] = (char)(0xF0 | (cp >> 18));
            out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
            *posp = pos;
            return;
        }
    }
    if (wc >= 0xD800 && wc <= 0xDFFF) { out[pos++] = ' '; *posp = pos; return; }
    if (wc < 0x80) out[pos++] = (char)wc;
    else if (wc < 0x800) { out[pos++] = 0xC0 | (wc >> 6); out[pos++] = 0x80 | (wc & 0x3F); }
    else { out[pos++] = 0xE0 | (wc >> 12); out[pos++] = 0x80 | ((wc >> 6) & 0x3F); out[pos++] = 0x80 | (wc & 0x3F); }
    *posp = pos;
}

/* 滚动条可见性门槛（bug #22，2026-09-20）。
 *
 * 原来渲染侧写死 cols >= 10：窗格窄于 10 列时整条滚动条不画。而 src/input.c 的
 * 命中测试【没有】同样的门槛 —— 于是窄窗格里能拖一条看不见的滚动条，用户看到的
 * 就是「窗格 <=6 列时滚动条没了」。
 *
 * 滚动条只在鼠标悬停到窗格右缘时才出现，而且是【覆盖 pane 右缘内列】、不另占宽度，
 * 所以窄窗格照样画没有布局代价。只保留 >=2 的下限：1 列的窗格整个就是滚动条，
 * 画了只会把唯一一列内容全盖掉。
 *
 * alt 屏（vim 之类）没有滚动缓冲，一律不画。 */
#define SB_MIN_COLS 2

int render_sb_cols_ok(int cols, int in_alt_screen) {
    if (in_alt_screen) return 0;
    return cols >= SB_MIN_COLS;
}

/* 见 include/render.h 的说明。cursor_x 可以等于 cols（延迟换行的挂起态），
 * 所以判据是 >= cols-1 而不是 == cols-1。 */
int render_sb_spare_row(int cursor_visible, int cursor_x, int cursor_y, int cols) {
    if (!cursor_visible || cols < 1) return -1;
    if (cursor_x < cols - 1) return -1;
    if (cursor_y < 0) return -1;
    return cursor_y;
}

static void render_split_pane(char *out, int bs, int *posp, int leaf, PaneRect *rc) {
    Pane *pane = &g_mux.panes[leaf];
    ScreenBuffer *s = &pane->screen;
    int pos = *posp;
    if (pane->scroll_offset < 0) pane->scroll_offset = 0;
    if (pane->scroll_offset > 0) {
        /* 夹取必须按【实际渲染宽度】算，不能按 s->cols。拖动分屏边框时渲染宽度是
         * min(rc->cols, s->cols)，而 s->cols 已被冻结在拖动前的值（见
         * pane_resize_to），两者差得很多：真机实测同一份内容在 92 列下
         * scroll_limit=1367、在 4 列下=31409。按 s->cols 夹会把 scroll_offset
         * 写成一个和当前显示宽度无关的值。
         * 更关键的是这个夹取会【写回】pane->scroll_offset —— 用户正往上翻着
         * （比如 vo=5000），拖宽后 limit 骤降到 1367，vo 被永久夹到 1367，
         * 视图直接跳到底部，松手后 limit 恢复了但 vo 已经丢了、翻不回去。
         * 症状就是用户报的「左右拖动分屏导致历史直接消失」。
         * 拖动期间干脆不夹不写回：视图位置保持稳定，松手后再按新宽度夹一次。 */
        int lim_sc;
        if (split_drag_active()) {
            lim_sc = pane->scroll_offset;   /* 拖动中：不夹 */
        } else {
            int rw = rc->cols < s->cols ? rc->cols : s->cols;
            int h = screen_reflow_height(s, rw);
            lim_sc = h - s->rows;
            if (lim_sc < 0) lim_sc = 0;
        }
        if (pane->scroll_offset > lim_sc) pane->scroll_offset = lim_sc;
    }
    if (getenv("TERMUX_DUMP")) {
        FILE *sf = fopen("scroll_trace.log", "a");
        if (sf) { fprintf(sf, "[render_split] vo=%d hist=%d limit=%d rows=%d\n", pane->scroll_offset, s->hist_lines, screen_scroll_limit(s), s->rows); fclose(sf); }
    }
    int rows = rc->rows < s->rows ? rc->rows : s->rows;
    int cols = rc->cols < s->cols ? rc->cols : s->cols;
    /* 渲染宽度窄于模型宽度：拖动分屏边框期间模型被冻结（见 pane_resize_to）。
     * 2026-09-15 起这个标志【只用于诊断】，不再触发 reflow —— 详见下面 use_rf
     * 处的说明。曾经认为「不走 reflow 就会右侧内容消失」，实测确认那是真的，但
     * 相比之下 reflow 把硬换行折断造成的行序错乱更糟，两害相权选择裁剪。 */
    int narrow_view = (cols < s->cols);
    /* 诊断帧头素材：记下本 pane 这一帧的四个宽度。rc->cols 是布局给的外接宽度，
     * s->cols 是模型宽度，cols=min(两者) 才是真正逐格渲染的宽度。上次排查「渲染错位」
     * 时缺的正是 rc->cols 与 s->cols 的逐帧对照——只有末态的 render_dump 里那个
     * active pane 的 s->cols，无法知道另一个 pane 当时的值。 */
    if (leaf >= 0 && leaf < MAX_PANES) {
        g_cd_pane[leaf].valid = 1;
        g_cd_pane[leaf].oc0 = rc->c0;  g_cd_pane[leaf].ocols = rc->cols;
        g_cd_pane[leaf].scols = s->cols; g_cd_pane[leaf].srows = s->rows;
        g_cd_pane[leaf].cols = cols;  g_cd_pane[leaf].rows = rows;
        g_cd_pane[leaf].vo = pane->scroll_offset;
        g_cd_pane[leaf].hist = s->hist_lines;
        g_cd_pane[leaf].conpty_cols = pane->conpty_cols;
        g_cd_pane[leaf].narrow = narrow_view;
    }
    /* 窗格先铺底色：整矩形清成终端底色（TH_BG0，与 ConPTY 程序默认黑底一致），
     * 避免残留；未填充格与铺底无色差，分屏窗格之间不会冒出奇怪色块。 */
    for (int py = 0; py < rows; py++) {
        /* 不要写成 "\x1b[0" TERM_BG：TERM_BG 自身已是完整的 "\x1b[048;2;...m"
         * （首参数 0 就是 SGR reset）。多出来的 "\x1b[0" 是一条没有终结字节的残缺
         * CSI，而紧跟其后的 TERM_BG 里那个 '['（0x5b）正落在 VT 终结字节区间
         * 0x40-0x7e 内 —— 终端会把 "ESC[0 ESC[" 当成一条以 '[' 收尾的完整 CSI，
         * 然后把剩下的 "048;2;013;017;023m" 当【正文】写进窗格：每行凭空多 17 个
         * 字符、光标右移 17 列，铺底的空格从第 18 列起写满 120 列并折行溢到下一行。
         * 分屏铺底逐行如此，整屏内容被冲成一片重复/错位的碎片（左右分屏拖动时的
         * 「渲染出现严重故障」，render_dump.log 第 39 帧起可见）。 */
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH" TERM_BG,
                        rc->r0 + py + 2, rc->c0 + 1);
        for (int px = 0; px < cols; px++) pos += snprintf(out + pos, bs - pos, " ");
    }
    *posp = pos;
    /* v1.8.47/50：向上回看（非 alt 屏、scroll_offset>0）时，【整窗】统一用逻辑行
     * reflow 网格——网格含「历史 + 当前可见」按视口宽重排、底部锚定，vo 跳过最新
     * vo 个显示行，历史与实时内容在同一坐标系、边界连续不重复/不错位。vo==0（看
     * 实时屏）仍直取 ConPTY 缓冲。
     *
     * 追加（2026-09-14）：渲染宽度窄于模型宽度时【也必须】走网格。原因是
     * pane_resize_to 在拖动分屏边框期间冻结了模型（bug #12 的 freeze_model），
     * 于是 s->cols 停在拖动前的值、而 rc->cols 跟着鼠标变小，cols=min(两者) 只取
     * 到每行前 cols 列，【右侧内容直接消失，不折行】。真机日志实测（帧 53→60，
     * 模型冻结 59 列、窗格拖到 33 列）：
     *   帧53 |2026-04-19  14:38        20,549,329 EXPR.exe|
     *   帧60 |2026-04-19  14:38        20,549,3|     ← "29 EXPR.exe" 没了
     * 走网格则由 screen_reflow_view 按 cols 宽重新折行，内容完整。Linux 侧对同一份
     * 内容验证过：逐格取 33 列丢掉 "29 EXPR.exe"/"71 liquidbounce b100.jar"/
     * "10 P6008_6.in"，而 screen_reflow_view(width=33) 全部保留并正常折行。
     *
     * 与 bug #12 的区别：#12 的错位来自【模型】每帧 reflow（本地与 ConPTY 不同步）；
     * 这里只重排显示网格 rf_grid，s 与 ConPTY 都不动，所以不会让 #12 复发。
     * 代价：光标仍按模型坐标定位（见 render_screen 的分屏光标段），在拖动中可能与
     * 重排后的文本差几列；相比整行内容消失，这个代价可以接受。 */
    /* 2026-09-15 决定（方案 B）：拖动变窄时【不再】走 reflow，只有用户显式往上
     * 翻历史（scroll_offset>0）才重排。
     *
     * v5/v7/v8 三次失败的共同原因是：它们都保留「narrow_view 就 reflow」，只在
     * reflow 之后调 vo（看哪一段）。而 screen_reflow_view 会把【硬换行】的行也按
     * 新宽度折断 —— src/screen.c 的 reflow_append_rows 无条件按宽度折行，不看
     * line_wrap。于是一条逻辑行变成两条显示行：
     *   - 视觉上像「内容重复 / 行序混乱」（用户报的原话）；
     *   - 行数翻倍把老内容挤出视口，看起来像「光标乱跳」。
     * 本地 harness 实测（宿主 120→50，模型冻结 59 列，10 行 CUP 硬换行内容，
     * line_wrap 全 0，screen_reflow_height 却从 10 变 20）：
     *   reflow 版：|R05 ......|  |......ZZ|  |R06 ......|  视口从 R00 跳到 R05
     *   裁剪版：  |R00 ......|  |R01 ......|  |R02 ......|  行序与视口都不动
     * 见 analysis/拖动错乱-根因与三种可选行为.md 与
     * analysis/fixtures/harness_narrow_{reflow,noreflow}.log。
     *
     * 代价：拖动期间每行超出窗格宽度的右侧部分看不见，松手后模型同步即恢复。
     * 换来的是行序与视口在拖动全程完全稳定 —— 这也让「提示符正好在最下面一行」
     * 天然成立，因为行数根本不变。
     *
     * 回看路径不受影响：scroll_offset>0 时两版行为逐帧一致（已用 hist=500/2000
     * 验证），因为那条路径本来就该按当前宽度重排历史。 */
    int use_rf = (pane->scroll_offset > 0 &&
                  !s->in_alt_screen && s->line_wrap != NULL);
    if (use_rf) {
        RGlyph *grid = (RGlyph *)pane->rf_grid;
        /* 网格尺寸恒等于 rows×cols（读写同一步长），任何尺寸变化都重分配。
         *
         * 两条失败路径都必须退回逐格，不能用半成品网格（2026-09-14）：
         * 拖动分屏边框时 cols 每帧都变，于是每帧都要 realloc，函数内部还每帧
         * malloc —— 原本罕见的失败路径变成了每帧都在赌。一旦失败还继续用网格，
         * 表现就是用户报的「内容重复 / 行序错乱」：
         *   a) realloc 失败：grid 还是旧指针、旧步长（比如上一帧的 rows*59），
         *      但 rf_cols 会被写成新的 cols，逐格按 grid[py*cols+px] 读一份按
         *      旧步长摆放的数据 —— 行与行互相串位。
         *   b) screen_reflow_view 返回 0（内部 malloc 失败）：grid 只填了一部分
         *      甚至完全没填，rf_valid 却照样置 1，渲染读到上一帧残留。
         * 退回逐格的代价只是「窄窗下右侧内容不折行」，不会错乱，可以接受。 */
        int rf_ready = 1;
        if (pane->rf_rows != rows || pane->rf_cols != cols || !grid) {
            RGlyph *ng = (RGlyph *)realloc(grid, (size_t)rows * cols * sizeof(RGlyph));
            if (ng) {
                grid = ng;
            } else {
                /* realloc 失败不会释放原块，这里自己释放，避免泄漏。
                 * 同时必须把 pane->rf_grid 置空：它此前指向的正是这块已释放的
                 * 内存，留着就是悬垂指针，下一帧会用到已释放的堆块。 */
                free(grid);
                grid = NULL;
                pane->rf_grid = NULL;
                rf_ready = 0;
            }
        }
        /* 锚定方向：底部锚定（vo = scroll_offset）。
         *
         * 2026-09-15 回退记录：v7 曾在 narrow_view 时改用顶部锚定
         * （vo = screen_reflow_height(s, cols) - rows），理由是「拖动中内容顶端
         * 不动」。真机日志证明这是错的 —— render_dump.log 帧 40 vs 帧 60：
         *   帧40（拖动前，窗格 59 列）r2 = |2026-04-19  14:38  20,549,329 EXPR.exe|
         *   帧60（拖动中，窗格 33 列）r2 = |Microsoft Windows [版本 10.0.2620|
         * 顶部锚定把视口钉在【全部历史的第一行】，于是画面跳回 cmd 的 banner，
         * 用户正在看的内容和光标区整个被推出视口。这比 v5 的症状更糟。
         *
         * 底部锚定（vo=0）才是拖动中想要的：视口恒显示最新 rows 个显示行，
         * 光标所在区域始终可见；cols 每帧变化时只有上方内容被挤掉，下方稳定。
         * 用户往上翻历史时 vo = scroll_offset 本身就是他的显式意图，同样适用。 */
        if (rf_ready && grid &&
            screen_reflow_view(s, pane->scroll_offset, rows, cols, grid)) {
            /* 整窗 reflow 成功（返回 1 表示已填满整个视口）才发布这块网格。 */
            pane->rf_grid = grid;
            pane->rf_rows = rows;
            pane->rf_cols = cols;
            pane->rf_valid = 1;
            pane->rf_n = rows;
        } else {
            /* 失败：退回逐格。grid 此刻要么是有效但未填充的块（保留给下一帧复用），
             * 要么已经是 NULL。两种情况都不能标成有效网格。 */
            use_rf = 0;
            pane->rf_grid = grid;
            pane->rf_valid = 0;
            pane->rf_rows = pane->rf_cols = 0;
            pane->rf_n = 0;
        }
    } else {
        pane->rf_valid = 0;
        pane->rf_n = 0;
    }

    for (int py = 0; py < rows; py++) {
        /* vo>0 整窗走 reflow 网格；vo==0 走实时 ConPTY 缓冲。 */
        int use_rf_row = use_rf;
        /* 诊断（TERMUX_CELLDIAG）：只在该行含宽字符或次格疑似异常时落盘，避免日志暴涨。
         * 记录渲染那一刻【实际读到】的 wc 与次格，用于区分「次格=0（会被跳过，正确）」
         * 与「次格=空格（会被当内容发出，就是中文里的多余空格）」。 */
        if (getenv("TERMUX_CELLDIAG")) {
            int hit = 0;
            for (int px = 0; px < cols && !hit; px++) {
                WCHAR w = 0; int ud = -1;
                if (use_rf_row) {
                    RGlyph *grid = (RGlyph *)pane->rf_grid;
                    if (grid && pane->rf_cols > 0) w = grid[py * pane->rf_cols + px].ci.Char.UnicodeChar;
                } else {
                    CHAR_INFO *c = screen_cell(s, py, px);
                    if (c) w = c->Char.UnicodeChar;
                }
                if (w != 0 && w != L' ' && is_wide_cp((unsigned int)w)) hit = 1;
                (void)ud;
            }
            if (hit) {
                int pr_used = -1;
                if (!use_rf_row && s->lines) {
                    int apr = screen_phys_row(s, py);
                    if (apr >= 0 && apr < s->total_lines) pr_used = s->lines[apr].used;
                }
                cell_diag_frame_buf("F%d leaf%d pane=%dx%d s=%dx%d rf=%d vo=%d py=%d rr=%d used=%d |",
                          cell_diag_frame, leaf, rc->cols, rc->rows, s->cols, s->rows,
                          use_rf_row, pane->scroll_offset, py, rc->r0 + py, pr_used);
                for (int px = 0; px < cols; px++) {
                    WCHAR w = L' ', nx = L'?';
                    if (use_rf_row) {
                        RGlyph *grid = (RGlyph *)pane->rf_grid;
                        if (grid && pane->rf_cols > 0) {
                            w = grid[py * pane->rf_cols + px].ci.Char.UnicodeChar;
                            if (px + 1 < pane->rf_cols) nx = grid[py * pane->rf_cols + px + 1].ci.Char.UnicodeChar;
                        }
                    } else {
                        CHAR_INFO *c = screen_cell(s, py, px);
                        CHAR_INFO *c2 = (px + 1 < s->cols) ? screen_cell(s, py, px + 1) : NULL;
                        if (c) w = c->Char.UnicodeChar;
                        if (c2) nx = c2->Char.UnicodeChar;
                    }
                    cell_diag_frame_buf(" [%d]%s/%s", px,
                              w == 0 ? "0" : (w == L' ' ? "sp" : (w < 128 ? "a" : "W")),
                              nx == 0 ? "0" : (nx == L' ' ? "sp" : (nx < 128 ? "a" : "W")));
                }
                cell_diag_frame_buf("\n");
                /* 附带把该行的实际字符码点也打出来，便于和 render_dump.log 对齐。 */
                cell_diag_frame_buf("   cps:");
                for (int px = 0; px < cols; px++) {
                    WCHAR w = L' ';
                    if (use_rf_row) {
                        RGlyph *grid = (RGlyph *)pane->rf_grid;
                        if (grid && pane->rf_cols > 0) w = grid[py * pane->rf_cols + px].ci.Char.UnicodeChar;
                    } else {
                        CHAR_INFO *c = screen_cell(s, py, px);
                        if (c) w = c->Char.UnicodeChar;
                    }
                    if (w == 0) cell_diag_frame_buf(" {0}");
                    else if (w == L' ') cell_diag_frame_buf(" _");
                    else if (w < 128) cell_diag_frame_buf(" %c", (char)w);
                    else cell_diag_frame_buf(" <%04X>", w);
                }
                cell_diag_frame_buf("\n");
            }
        }
        for (int px = 0; px < cols; px++) {
            int rr = rc->r0 + py, cc = rc->c0 + px;
            int pos_before = *posp;
            render_split_cell(out, bs, posp, s, pane, leaf, px, py, rr, cc, use_rf_row);
            pos = *posp;
            /* 诊断：本格是否真的发了字节。次格（wc==0）不发字节 → 铺底那格的空格残留。
             * 这条是「中文里多空格」的另一条可能路径，必须能和模型侧区分开。 */
            if (getenv("TERMUX_CELLDIAG") && pos == pos_before)
                cell_diag_frame_buf("   NOBYTE px=%d cc=%d（次格未写，铺底空格残留）\n", px, cc);
        }
    }

    /* 滚动条：与整屏路径同款，画在 pane 右缘内列（覆盖该列，不另占宽度）。
     * 非 alt 屏、pane 足够宽且有历史时显示；活动 pane 才响应 hover/拖动。 */
    if (render_sb_cols_ok(cols, s->in_alt_screen) && leaf == g_mux.active_pane) {
        int rr = rows;
        int sb_top = 0, sb_bot = rr;
        int hist = screen_scroll_limit(s);   /* 滚动条跨度 = 可回看的显示行数 */
        if (hist > 0) {
            int total = hist + rr;
            int th = (rr * rr) / total;
            if (th < 1) th = 1;
            if (th >= rr) th = rr - 1;
            int vo = pane->scroll_offset;
            int vtop = hist - vo;
            int max_tpos = rr - th;
            if (max_tpos <= 0) max_tpos = 1;
            int tpos = (vtop * max_tpos + hist / 2) / hist;
            if (tpos < 0) tpos = 0;
            if (tpos + th > rr) tpos = rr - th;
            sb_top = tpos;
            sb_bot = tpos + th;
        }
        int popup_open = (g_mux.chooser_mode || g_mux.ctx_mode || g_mux.rename_mode ||
                          g_mux.custom_cmd_mode || g_search_mode || g_mux.palette_mode);
        /* 鼠标到该 pane 右缘列的水平距离（0 = 正在滚动条列上）。 */
        int sb_x = rc->c0 + cols - 1;
        int dist = (!popup_open && g_mouse_y >= 1 && g_mouse_x >= rc->c0)
                   ? (sb_x - g_mouse_x) : 99;
        if (dist < 0) dist = 99;
        /* 只在鼠标纵向落在本 pane 行范围内才算悬停它的滚动条。 */
        if (g_mouse_y - 1 < rc->r0 || g_mouse_y - 1 >= rc->r0 + rows) dist = 99;
        int is_hover = (!popup_open && dist <= 10);
        int gi = dist < 0 ? 10 : (dist > 10 ? 10 : dist);
        int mouse_on_thumb = 0;
        if (is_hover) {
            int my_row = g_mouse_y - 1 - rc->r0;
            if (my_row >= sb_top && my_row < sb_bot) mouse_on_thumb = 1;
            if (g_sb_dragging) mouse_on_thumb = 1;
        }
        /* 与整屏路径一致：只有鼠标悬停到该窗格右缘（或正在拖滚动条）才绘制滚动条；
         * 否则整列什么都不画，滚动条「消失」（不常驻）。无历史（hist<=0）时 track
         * 与 thumb 都没有意义，同样不画。 */
        if (is_hover && s->hist_lines > 0) {
            /* 光标正落在右缘列时让开那一行，否则刚敲的字会被滚动条盖掉。
             * 回看历史时终端光标本来就不显示（帧尾光标段有 scroll_offset==0 的
             * 条件），没有要让的东西。 */
            int spare = (pane->scroll_offset == 0)
                      ? render_sb_spare_row(s->cursor_visible, s->cursor_x, s->cursor_y, cols)
                      : -1;
            for (int py = 0; py < rows && pos < bs - 64; py++) {
                if (py == spare) continue;
                int term_col = rc->c0 + cols;           /* 右缘列，1 基 */
                int term_row = rc->r0 + py + 2;
                int in_thumb = (py >= sb_top && py < sb_bot);
                if (in_thumb) {
                    if (mouse_on_thumb)
                        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[0;048;2;225;235;250m \x1b[0m", term_row, term_col);
                    else {
                        int tr = g_sb_grad[gi].thumb_r, tg = g_sb_grad[gi].thumb_g, tb = g_sb_grad[gi].thumb_b;
                        sb_custom_thumb(&tr, &tg, &tb);
                        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[0;48;2;%d;%d;%dm \x1b[0m", term_row, term_col, tr, tg, tb);
                    }
                } else {
                    int br = g_sb_grad[gi].track_bg_r, bg = g_sb_grad[gi].track_bg_g, bb = g_sb_grad[gi].track_bg_b;
                    int fr = g_sb_grad[gi].track_fg_r, fg = g_sb_grad[gi].track_fg_g, fb = g_sb_grad[gi].track_fg_b;
                    sb_custom_track(&br, &bg, &bb, &fr, &fg, &fb);
                    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[0;48;2;%d;%d;%dm\x1b[38;2;%d;%d;%dm│\x1b[0m", term_row, term_col,
                                    br, bg, bb, fr, fg, fb);
                }
            }
        }
    }

    /* 活动窗格的终端光标由帧尾光标段统一定位/显隐（见 render_screen 尾部的
     * split_mode 分支），这里不再单独发 ?25h，避免重复定位。 */
    (void)rows; (void)cols;
    *posp = pos;
}

static void render_split(char *out, int bs, int *posp) {
    static PaneRect rects[MAX_PANES];
    int n = split_compute_rects(rects);
    /* n==0：没分屏（交回整屏路径）。n==1 也可能发生——zoom（全屏缩放）时只有
     * 活动窗格可见且铺满，此时仍要把它画出来（否则内容区空白，表现为「缩放后
     * 屏幕空了」）；n>=2 为正常多窗格。 */
    if (n < 1) return;
    int pos = *posp;
    /* 先把整个内容区整行显式铺成面板底色（每一列都写显式空格，不只用 \x1b[K）。
     * 布局变化——拖分隔线 / 关闭窗格——时，旧边框或旧窗格内容所在的列在本帧可能
     * 没有任何新字符；framediff 按「整行字节块」差分，\x1b[K 清行尾在增量帧里不
     * 能保证把这些列刷掉，结果旧背景色残留成「拖条/关窗后多出来的一块背景」。
     * 显式铺满整行后，行块一定包含每一列的字节，布局一变整行必判脏、整行重建，
     * 随后窗格铺底/单元格/边框再各自覆盖。 */
    for (int r = 0; r < g_mux.host_rows; r++) {
        /* 同上：TERM_BG 已是完整 CSI，前面不能再挂一条残缺的 "\x1b[0"。 */
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H" TERM_BG, r + 2);
        for (int c = 0; c < g_mux.host_cols; c++)
            pos += snprintf(out + pos, bs - pos, " ");
        pos += snprintf(out + pos, bs - pos, "\x1b[0m");
    }
    *posp = pos;
    for (int i = 0; i < MAX_PANES; i++)
        if (rects[i].valid) render_split_pane(out, bs, posp, i, &rects[i]);
    render_split_borders(out, bs, posp, rects);
}

void render_screen(void) {
    EnterCriticalSection(&g_mux.cs);
    /* 诊断帧号在这里自增，且【无条件】——本函数下面有多条提前 return 的路径
     * （host 尺寸非法、help_mode 等），若在别处自增会让帧号跳号，与
     * render_dump.log 的帧序错开。帧缓冲同样在帧首清空，避免提前 return 时
     * 把上一帧的残留明细带到下一帧。 */
    cell_diag_frame++;
    g_cd_len = 0;
    for (int i = 0; i < MAX_PANES; i++) g_cd_pane[i].valid = 0;
    if (g_mux.host_cols < 1 || g_mux.host_rows < 1 || g_mux.total_host_rows < 1) { LeaveCriticalSection(&g_mux.cs); return; }
    update_host_title();

    int bs = (g_mux.host_rows + 4) * (g_mux.host_cols * 48 + 1024) + 16384;
    char *out = render_buffer_acquire(bs);
    if (!out) { LeaveCriticalSection(&g_mux.cs); return; }
    int pos = 0;

    if (g_mux.help_mode) {
        render_help_content(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (!g_mux.settings_mode &&
               split_is_split() &&
               g_mux.active_pane >= 0 && !g_mux.panes[g_mux.active_pane].is_settings &&
               !g_mux.panes[g_mux.active_pane].is_about) {
        /* 分屏：当前标签页有 >=2 个窗格，走多窗格矩形渲染（整屏路径只画一个）。
         * 模态弹层（命令面板/chooser/ctx/rename/search/confirm）打开时也继续画
         * 分屏底图（窗格+分隔边框），弹层在 body 之后叠画；否则开命令面板会退回
         * 整屏路径，把分屏边框抹掉（BUG：面板一开中间分屏条就消失）。 */
        render_split(out, bs, &pos);
    } else if (g_mux.active_pane >= 0 && g_mux.active_pane < g_mux.pane_count && g_mux.panes[g_mux.active_pane].active) {
        Pane *pane = &g_mux.panes[g_mux.active_pane];
        /* 分屏收缩回单窗格（关闭窗格 / 取消 zoom 后树只剩 1 叶子）时，残留的
         * pane 屏幕还是分屏时的小尺寸；这里补回整屏尺寸，否则整屏路径只在左上
         * 小区域渲染、右边/下边大片残留（BUG：关闭到只剩一个窗格后显示不全）。 */
        if (!pane->is_settings && !pane->is_about &&
            (pane->screen.cols != g_mux.host_cols || pane->screen.rows != g_mux.host_rows)) {
            int sroot = split_root_for_tab(split_tab_of_pane(g_mux.active_pane));
            if (sroot >= 0 && split_count_leaves(sroot) == 1 && !g_split_zoom)
                pane_resize_to(g_mux.active_pane, g_mux.host_cols, g_mux.host_rows);
        }
        if (pane->is_settings) {
            /* v2.0.9：设置页所有行都用绝对 CUP 起笔、文字可能比右侧区域宽（窄终端）。
             * 关掉自动折行（DECAWM ?7l）让超出右边界的部分被终端就地丢弃，而不是
             * 折到下一行盖住侧栏/底栏（用户反馈「太窄会显示不了」）。?7h 在帧尾光标段
             * 无条件恢复（见 cursor_pos 之后），framediff 不会把它吞掉。
             * 这条 ?7l 没有 CUP 前缀，落在 framediff 的 always 段/前一行块里：设置页
             * 时下面紧跟的第一条 CUP 是标签栏之后的第 2 行，always 段每帧必发。 */
            pos += snprintf(out + pos, bs - pos, "\x1b[?7l");
            g_settings_nowrap_on = 1;
            render_settings_panel(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
        } else {
            ScreenBuffer *s = &pane->screen;
            WORD la_attr = 0xFFFF, la_fr = 0, la_br = 0; int la_fv = -1, la_bv = -1;
            if (pane->scroll_offset < 0) pane->scroll_offset = 0;
            if (pane->scroll_offset > 0) {
                /* 同 render_split_pane：拖动分屏边框期间不夹取、不写回，否则
                 * scroll_offset 会被一个与当前显示宽度无关的 limit 永久夹小，
                 * 表现为「拖动分屏后历史翻不上去」。 */
                int lim_sc = pane->scroll_offset;
                if (!split_drag_active()) {
                    int rw = g_mux.host_cols < s->cols ? g_mux.host_cols : s->cols;
                    int h = screen_reflow_height(s, rw);
                    lim_sc = h - s->rows;
                    if (lim_sc < 0) lim_sc = 0;
                }
                if (pane->scroll_offset > lim_sc) pane->scroll_offset = lim_sc;
            }
            int vo = pane->scroll_offset, rr = s->rows < g_mux.host_rows ? s->rows : g_mux.host_rows, rc = s->cols < g_mux.host_cols ? s->cols : g_mux.host_cols;
            int show_sb = render_sb_cols_ok(g_mux.host_cols, s->in_alt_screen);
            /* 与分屏路径同理：光标压在右缘列时，那一行不画滚动条轨道，
             * 否则刚敲的字被盖掉、看着像光标卡住。 */
            int sb_spare = (vo == 0)
                         ? render_sb_spare_row(s->cursor_visible, s->cursor_x, s->cursor_y, rc)
                         : -1;
            int sb_top = 0, sb_bot = 0;
            if (show_sb) {
                int hist = screen_scroll_limit(s);   /* 滚动条跨度 = 可回看的显示行数 */
                if (hist <= 0) {
                    sb_top = 0;
                    sb_bot = rr;
                } else {
                    int total = hist + rr;
                    int th = (rr * rr) / total;
                    if (th < 1) th = 1;
                    if (th >= rr) th = rr - 1;
                    int vtop = hist - vo;
                    int max_tpos = rr - th;
                    if (max_tpos <= 0) max_tpos = 1;
                    int tpos = (vtop * max_tpos + hist / 2) / hist;
                    if (tpos < 0) tpos = 0;
                    if (tpos + th > rr) tpos = rr - th;
                    sb_top = tpos;
                    sb_bot = tpos + th;
                }
            }
            int text_rc = rc;
            int use_rf_ws = 0;
            /* v1.8.52+：整屏（单窗格非分屏）回看历史原来直接按物理行取（y-vo），
             * 而 vo / 滚动上限 / 滚动条都按 reflow「非空显示行」计（screen_reflow_height
             * 空逻辑行不占位）。历史里夹着 CRLF 纯空白行时，两种口径不一致：物理口径
             * 滚到 vo=limit 仍够不到最老内容（顶部停在某个 echo 输出，更老的 for 行 /
             * banner 永远上不来），画面钉在中途——分屏回看却没这个问题。这里与分屏
             * 一致：vo>0 回看整窗走 reflow 网格（同一坐标系、内容完整到底）。选区 /
             * 搜索高亮目前仍按物理坐标算，这些模态激活时退回物理渲染（与现状一致）。 */
            if (vo > 0 && !s->in_alt_screen && s->line_wrap != NULL &&
                !g_copy_mode && !g_mouse_selecting && !g_search_active) {
                RGlyph *grid = (RGlyph *)pane->rf_grid;
                if (pane->rf_rows != rr || pane->rf_cols != text_rc || !grid) {
                    RGlyph *ng = (RGlyph *)realloc(grid, (size_t)rr * (size_t)text_rc * sizeof(RGlyph));
                    if (ng) grid = ng;
                }
                if (grid) {
                    pane->rf_grid = grid;
                    pane->rf_rows = rr;
                    pane->rf_cols = text_rc;
                    screen_reflow_view(s, vo, rr, text_rc, grid);
                    use_rf_ws = 1;
                }
            }

            int popup_open = (g_mux.chooser_mode || g_mux.ctx_mode || g_mux.rename_mode ||
                              g_mux.custom_cmd_mode || g_search_mode || g_mux.palette_mode);
            int dist = (popup_open) ? 99 : ((g_mouse_y >= 1 && g_mouse_x >= 0) ? ((g_mux.host_cols - 1) - g_mouse_x) : 99);
            if (dist < 0) dist = 0;
            int is_hover = (!popup_open && dist == 0 && (g_mouse_y >= 1 || g_sb_dragging));
            int mouse_on_thumb = 0;
            if (is_hover) {
                int my_row = g_mouse_y - 1;
                if (my_row >= sb_top && my_row < sb_bot) {
                    mouse_on_thumb = 1;
                }
                if (g_sb_dragging) {
                    mouse_on_thumb = 1;
                }
            }

            int sel_active = 0, sel_block = 0, sel_min_abs_y = 0, sel_max_abs_y = 0, sel_min_x = 0, sel_max_x = 0;
            if (g_copy_mode && g_copy_sel_active) {
                int cur_abs_y = screen_to_abs_row(s, g_copy_cy, vo);
                sel_min_abs_y = g_copy_anchor_abs_y < cur_abs_y ? g_copy_anchor_abs_y : cur_abs_y;
                sel_max_abs_y = g_copy_anchor_abs_y > cur_abs_y ? g_copy_anchor_abs_y : cur_abs_y;
                /* 键盘选区（!quick）= 半开区间 [锚点 caret, 端点 caret)：端点 caret
                 * 处的格子不包含，所以默认锚点==端点时没有任何格子高亮，→ 一次跨过
                 * 一个字符后恰好选中跨过的那一个（宽字符选中两列）。鼠标 Shift/Alt
                 * 两角（quick）= 闭合区间，端点格子包含。snap_sel_row 再整字吸附。 */
                int half = !g_copy_quick;
                if (g_copy_block) {
                    /* Rectangular selection: the same column range on every row.
                     * 半开区间 [锚点 caret, 端点 caret)：→ 按一次只前进一格，
                     * 高亮也只跟着多一格（宽字符一次跨两列由 copy_step_char 保证，
                     * 不再强制补偶数宽）。snap_sel_row 再把框边整字吸附。 */
                    sel_block = 1;
                    int lo = g_copy_anchor_x < g_copy_end_x ? g_copy_anchor_x : g_copy_end_x;
                    int hi = g_copy_anchor_x > g_copy_end_x ? g_copy_anchor_x : g_copy_end_x;
                    sel_min_x = lo;
                    sel_max_x = half ? hi - 1 : hi;   /* 半开：右 caret 排他 */
                    if (sel_max_x < sel_min_x) sel_max_x = sel_min_x;
                    if (sel_max_x >= s->cols) sel_max_x = s->cols - 1;
                    if (!half || g_copy_anchor_x != g_copy_end_x) sel_active = 1;
                } else if (g_copy_anchor_abs_y == cur_abs_y) {
                    sel_min_x = g_copy_anchor_x < g_copy_end_x ? g_copy_anchor_x : g_copy_end_x;
                    sel_max_x = g_copy_anchor_x > g_copy_end_x ? g_copy_anchor_x : g_copy_end_x;
                    if (half) sel_max_x -= 1;            /* 右端 caret 排他 */
                    /* 锚点==端点（同一 caret）时一个格子都不选；半开下 max-1<min 即空。 */
                    if (!half || g_copy_anchor_x != g_copy_end_x) sel_active = 1;
                } else if (g_copy_anchor_abs_y < cur_abs_y) {
                    /* 锚点在上：顶行从锚点 caret 起，底行到端点 caret 前（半开减 1）。 */
                    sel_min_x = g_copy_anchor_x;
                    sel_max_x = half ? g_copy_end_x - 1 : g_copy_end_x;
                    sel_active = 1;
                } else {
                    /* 锚点在下：顶行从端点 caret 起，底行到锚点 caret 前（半开减 1）。 */
                    sel_min_x = g_copy_end_x;
                    sel_max_x = half ? g_copy_anchor_x - 1 : g_copy_anchor_x;
                    sel_active = 1;
                }
            } else if (g_mouse_selecting) {
                sel_min_abs_y = g_mouse_sel_s_abs_y < g_mouse_sel_e_abs_y ? g_mouse_sel_s_abs_y : g_mouse_sel_e_abs_y;
                sel_max_abs_y = g_mouse_sel_s_abs_y > g_mouse_sel_e_abs_y ? g_mouse_sel_s_abs_y : g_mouse_sel_e_abs_y;
                if (g_mouse_sel_s_abs_y == g_mouse_sel_e_abs_y) {
                    sel_min_x = g_mouse_sel_sx < g_mouse_sel_ex ? g_mouse_sel_sx : g_mouse_sel_ex;
                    sel_max_x = g_mouse_sel_sx > g_mouse_sel_ex ? g_mouse_sel_sx : g_mouse_sel_ex;
                } else if (g_mouse_sel_s_abs_y < g_mouse_sel_e_abs_y) {
                    sel_min_x = g_mouse_sel_sx; sel_max_x = g_mouse_sel_ex;
                } else {
                    sel_min_x = g_mouse_sel_ex; sel_max_x = g_mouse_sel_sx;
                }
                sel_active = 1;
            }

            for (int y = 0; y < rr; y++) {
                pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H", y + 2);
                /* v1.8.13 脏区修复：每一行都必须自成一个 SGR 作用域。增量帧只会
                 * 发出与上一帧不同的行，此时终端里实际的 SGR 状态是「上一帧最后
                 * 发出的那一行」留下的，而不是整帧顺序里本行上一行的状态。本行
                 * 第一个 cell 以前靠「颜色和上一行末尾相同就不发 SGR」的跨行携带，
                 * 单独重发时背景/前景就会丢（colortool 色块行之后，后续变化行
                 * 不重发背景 SGR，整块背景就没了）。行首先复位，并强制本行第一个
                 * 非空 cell 重发完整颜色，保证任意一行单独发出都和整帧一致。 */
                pos += snprintf(out + pos, bs - pos, "\x1b[0m");
                la_attr = 0xFFFF; la_fr = 0; la_br = 0; la_fv = -1; la_bv = -1;
                int ar = (vo > 0 && !s->in_alt_screen) ? screen_phys_row(s, y - vo) : -1;
                int cur_cell_abs_y = screen_to_abs_row(s, y, vo);
                RGlyph *rf_row = use_rf_ws ? &((RGlyph *)pane->rf_grid)[(size_t)y * text_rc] : NULL;

                snap_sel_row(s, y, vo, sel_active, sel_block, cur_cell_abs_y, sel_min_abs_y, sel_max_abs_y, &sel_min_x, &sel_max_x);
                int match_lo = 0, match_hi = 0;
                if (g_search_active && g_search_match_count > 0) {
                    int lo = 0, hi = g_search_match_count;
                    while (lo < hi) {
                        int mid = (lo + hi) / 2;
                        if (g_search_matches[mid].abs_y < cur_cell_abs_y) lo = mid + 1;
                        else hi = mid;
                    }
                    match_lo = lo;
                    hi = g_search_match_count;
                    while (lo < hi) {
                        int mid = (lo + hi) / 2;
                        if (g_search_matches[mid].abs_y <= cur_cell_abs_y) lo = mid + 1;
                        else hi = mid;
                    }
                    match_hi = lo;
                }
                for (int x = 0; x < text_rc; x++) {
                    WCHAR wc = L' '; WORD attr = 0x07;
                    WORD frgb = RGB565_WHITE, brgb = RGB565_BLACK; int fgv = 0, bgv = 0;
                    if (rf_row) {
                        /* reflow 网格（vo>0 回看：逻辑行按当前宽重排、空逻辑行不占位，
                         * 底部锚定——与分屏历史同一坐标系，滚到 limit 即内容最顶）。 */
                        RGlyph *g = &rf_row[x];
                        wc = g->ci.Char.UnicodeChar; attr = g->ci.Attributes;
                        frgb = g->fg; brgb = g->bg; fgv = g->v & 1; bgv = (g->v >> 1) & 1;
                        if (wc == 0) continue;   /* 宽字符次格不写 */
                    } else {
                        CHAR_INFO *cell = (ar >= 0) ? ((s->lines && s->lines[ar].cells) ? &s->lines[ar].cells[x] : NULL) : screen_cell(s, y, x);
                        if (cell) { wc = cell->Char.UnicodeChar; attr = cell->Attributes; }
                        if (wc == 0) continue;
                        cell_truecolor(s, y, x, ar, &frgb, &brgb, &fgv, &bgv);
                    }

                    if (sel_active) {
                        int in_sel = 0;
                        if (sel_block) {
                            in_sel = (cur_cell_abs_y >= sel_min_abs_y && cur_cell_abs_y <= sel_max_abs_y &&
                                      x >= sel_min_x && x <= sel_max_x);
                        }
                        else if (cur_cell_abs_y > sel_min_abs_y && cur_cell_abs_y < sel_max_abs_y) in_sel = 1;
                        else if (sel_min_abs_y == sel_max_abs_y && cur_cell_abs_y == sel_min_abs_y) in_sel = (x >= sel_min_x && x <= sel_max_x);
                        else if (cur_cell_abs_y == sel_min_abs_y) in_sel = (x >= sel_min_x);
                        else if (cur_cell_abs_y == sel_max_abs_y) in_sel = (x <= sel_max_x);
                        if (in_sel) {
                            brgb = theme_role_rgb565(TH_SELECTION); bgv = 1;
                            frgb = theme_role_rgb565(TH_WHITE); fgv = 1;
                        }
                    }

                    if (match_lo < match_hi && (!sel_active || !(brgb == theme_role_rgb565(TH_SELECTION)))) {
                        for (int m = match_lo; m < match_hi; m++) {
                            if (x >= g_search_matches[m].start_x && x <= g_search_matches[m].end_x) {
                                if (m == g_search_match_cur) {
                                    brgb = theme_role_rgb565(TH_ORANGE); bgv = 1;
                                    frgb = theme_role_rgb565(TH_WHITE); fgv = 1;
                                } else {
                                    brgb = theme_role_rgb565(TH_YELLOW); bgv = 1;
                                    frgb = theme_role_rgb565(TH_BG0); fgv = 1;
                                }
                                break;
                            }
                        }
                    }

                    if (attr != la_attr || frgb != la_fr || brgb != la_br || fgv != la_fv || bgv != la_bv) {
                        const char *ul = (attr & COMMON_LVB_UNDERSCORE) ? ";4" : "";
                        if (fgv || bgv) {
                            int fr, fg2, fb; rgb565_split(frgb, &fr, &fg2, &fb);
                            int br2, bg2, bb; rgb565_split(brgb, &br2, &bg2, &bb);
                            if (fgv && bgv)
                                pos += snprintf(out + pos, bs - pos, "\x1b[0%s;38;2;%d;%d;%d;48;2;%d;%d;%dm", ul, fr, fg2, fb, br2, bg2, bb);
                            else if (fgv)
                                pos += snprintf(out + pos, bs - pos, "\x1b[0%s;38;2;%d;%d;%dm", ul, fr, fg2, fb);
                            else
                                pos += snprintf(out + pos, bs - pos, "\x1b[0%s;48;2;%d;%d;%dm", ul, br2, bg2, bb);
                        } else {
                            pos += emit_attr16(out + pos, bs - pos, attr, ul);
                        }
                        la_attr = attr; la_fr = frgb; la_br = brgb; la_fv = fgv; la_bv = bgv;
                    }
                    if (wc >= 0xD800 && wc <= 0xDBFF && x + 1 < text_rc) {
                        CHAR_INFO *next_cell = NULL;
                        CHAR_INFO next_ci;
                        if (rf_row) { next_ci = rf_row[x + 1].ci; next_cell = &next_ci; }
                        else next_cell = (ar >= 0) ? ((s->lines && s->lines[ar].cells) ? &s->lines[ar].cells[x + 1] : NULL) : screen_cell(s, y, x + 1);
                        if (next_cell && next_cell->Char.UnicodeChar >= 0xDC00 && next_cell->Char.UnicodeChar <= 0xDFFF) {
                            WCHAR low = next_cell->Char.UnicodeChar;
                            unsigned int cp = 0x10000 + (((unsigned int)(wc & 0x3FF)) << 10) + (low & 0x3FF);
                            out[pos++] = (char)(0xF0 | (cp >> 18));
                            out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[pos++] = (char)(0x80 | (cp & 0x3F));
                            x++;
                            if (pos > bs - 256) break;
                            continue;
                        }
                    }
                    if (wc >= 0xD800 && wc <= 0xDFFF) {
                        out[pos++] = ' ';
                        if (pos > bs - 256) break;
                        continue;
                    }
                    if (wc < 0x80) out[pos++] = (char)wc;
                    else if (wc < 0x800) { out[pos++] = 0xC0 | (wc >> 6); out[pos++] = 0x80 | (wc & 0x3F); }
                    else { out[pos++] = 0xE0 | (wc >> 12); out[pos++] = 0x80 | ((wc >> 6) & 0x3F); out[pos++] = 0x80 | (wc & 0x3F); }
                    if (pos > bs - 256) break;
                }
                /* v1.8.37：正文末尾(text_rc)之后到屏幕右缘这一段本帧不被任何显式
                 * 字符覆盖，必须用 \x1b[K 清行尾，否则布局收缩（关闭分屏窗格后
                 * 活动窗格扩宽）时会残留上一帧的内容（右侧屏幕不重置）。
                 * v2.0.5：但正文已经铺满宿主整宽（alt 屏 nano/vim 每行写到第 N 列）
                 * 时【不能】再发 \x1b[K —— 光标此刻处于「延迟折行挂起」态，逻辑上
                 * 仍在末列，libvterm 系终端会从末列开始清、把刚写的最后一列擦成空白
                 * （tmux/pyte 保留，libvterm 清掉，各家不一致，不能赌）。本帧右缘无
                 * 残留可清，直接跳过。真机 nano 打开 80 列宽文件末列 'E' 消失即此。 */
                if (text_rc < g_mux.host_cols)
                    pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[K");
                else
                    pos += snprintf(out + pos, bs - pos, "\x1b[0m");
                if (show_sb && dist <= 10 && y != sb_spare) {
                    /* 滚动条轨道画在最右列：先清行尾（上面已发 \x1b[K），再回到
                     * 最右列画 thumb / track。 */
                    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH", y + 2, g_mux.host_cols);
                    int in_thumb = (y >= sb_top && y < sb_bot);
                    if (in_thumb) {
                        if (mouse_on_thumb) {
                            pos += snprintf(out + pos, bs - pos, "\x1b[0;048;2;225;235;250m \x1b[0m");
                        } else {
                            int tr = g_sb_grad[dist].thumb_r, tg = g_sb_grad[dist].thumb_g, tb = g_sb_grad[dist].thumb_b;
                            sb_custom_thumb(&tr, &tg, &tb);
                            pos += snprintf(out + pos, bs - pos, "\x1b[0;48;2;%d;%d;%dm \x1b[0m", tr, tg, tb);
                        }
                    } else {
                        int br = g_sb_grad[dist].track_bg_r, bg = g_sb_grad[dist].track_bg_g, bb = g_sb_grad[dist].track_bg_b;
                        int fr = g_sb_grad[dist].track_fg_r, fg = g_sb_grad[dist].track_fg_g, fb = g_sb_grad[dist].track_fg_b;
                        sb_custom_track(&br, &bg, &bb, &fr, &fg, &fb);
                        pos += snprintf(out + pos, bs - pos, "\x1b[0;48;2;%d;%d;%dm\x1b[38;2;%d;%d;%dm│\x1b[0m",
                                        br, bg, bb, fr, fg, fb);
                    }
                    la_attr = 0xFFFF;
                }
                if (pos > bs - 256) break;
            }

            render_status_badge(out, bs, &pos, g_mux.host_cols);

            for (int y = rr; y < g_mux.host_rows && pos < bs - 64; y++)
                pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H\x1b[0m\x1b[K", y + 2);
        }
    } else {
        for (int y = 0; y < g_mux.host_rows && pos < bs - 64; y++)
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;1H\x1b[0m\x1b[K", y + 2);
    }

    if (g_mux.confirm_exit_mode) {
        render_confirm_exit(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (g_mux.confirm_close_mode) {
        render_confirm_dialog(out, bs, &pos, g_mux.host_rows, g_mux.host_cols, 1);
    } else if (g_search_mode) {
        render_search_box(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (g_mux.palette_mode) {
        render_command_palette(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (g_mux.chooser_mode) {
        render_chooser(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (g_mux.ctx_mode == 1) {
        render_ctx_menu(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (g_mux.ctx_mode == 2) {
        render_color_picker(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (g_mux.rename_mode) {
        render_rename_box(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    } else if (g_mux.custom_cmd_mode) {
        render_custom_cmd_box(out, bs, &pos, g_mux.host_rows, g_mux.host_cols);
    }

    /* 瞬时警告 toast：底部中央黄字深底，过期（或消息为空）则不画。 */
    /* GetTickCount64() 单调递增，until 为绝对过期时刻；未过期则 now < until。 */
    DWORD64 g_now_tick = GetTickCount64();
    if (g_toast_msg[0] && g_toast_until && g_now_tick < g_toast_until) {
        int tw = utf8_cols(g_toast_msg, (int)strlen(g_toast_msg));
        int box_w = tw + 4;
        int trow = g_mux.host_rows + 1;           /* 最后一行（终端 1 基） */
        int tcol = (g_mux.host_cols - box_w) / 2 + 1;
        if (tcol < 1) tcol = 1;
        pos += snprintf(out + pos, bs - pos,
                        "\x1b[%d;%dH\x1b[048;2;033;038;045m\x1b[038;2;210;153;034;1m  %s  \x1b[0m",
                        trow, tcol, g_toast_msg);
    } else if (g_toast_until) {
        g_toast_until = 0;
        g_toast_msg[0] = 0;
        g_mux.needs_redraw = 1;
    }

    pos += snprintf(out + pos, bs - pos, "\x1b[0m\x1b[1;1H");
    draw_tab_bar(out, bs, &pos);

    if (g_hover_preview_pane >= 0 && g_hover_preview_active &&
        !g_mux.chooser_mode && !g_mux.ctx_mode && !g_mux.rename_mode &&
        !g_mux.custom_cmd_mode && !g_mux.palette_mode && !g_mux.help_mode) {
        if (g_hover_preview_pane >= 0 && g_hover_preview_pane < g_mux.pane_count &&
            g_mux.panes[g_hover_preview_pane].active) {
            Pane *hp = &g_mux.panes[g_hover_preview_pane];
            const char *full_title = hp->full_title[0] ? hp->full_title :
                                     (hp->title[0] ? hp->title : "cmd");
            int tcols = utf8_cols(full_title, (int)strlen(full_title));
            if (tcols > 15) {
                int tab_col = 0;
                for (int i = 0; i < g_mux.tab_count; i++) {
                    if (g_mux.tab_info[i].pane_idx == g_hover_preview_pane) {
                        tab_col = g_mux.tab_info[i].start_col;
                        break;
                    }
                }
                int tw = tcols + 14;
                if (tw > g_mux.host_cols) tw = g_mux.host_cols;
                int left = tab_col;
                if (left + tw > g_mux.host_cols) left = g_mux.host_cols - tw;
                if (left < 0) left = 0;
                pos += snprintf(out + pos, bs - pos, "\x1b[2;%dH\x1b[048;2;033;038;045m\x1b[038;2;121;192;255;1m [完整标题] \x1b[038;2;255;255;255;1m%s \x1b[0m", left + 1, full_title);
            }
        }
    }

    /* v1.8.14 脏区修复：以下全是「光标」相关序列（定位 CUP + 显隐 ?25h/?25l），
     * 它们是整帧的全局尾部，不属于任何一行的内容。若让 framediff 按行切片，
     * 帧尾这个 CUP 会把光标序列折进「光标所在行」的 chunk——当那一行内容没变
     * （别处正在刷输出、或滚动隐藏/回底重现）时整段不发，光标定位被吞掉，终端
     * 光标就停在上一个发出的 CUP（常落在滚动条列），表现为「光标位置不对」。
     * 光标序列必须逐帧无条件发出，故记录起点，让 framediff 只对前面的 body
     * （标签栏 + 各内容行）做差分，光标段永远原样追加在增量帧末尾。 */
    int cursor_pos = pos;
    if (g_settings_nowrap_on) {
        /* 恢复自动折行 —— 放在光标段里，逐帧无条件发出 */
        pos += snprintf(out + pos, bs - pos, "\x1b[?7h");
        g_settings_nowrap_on = 0;
    }

    /* 帧尾光标段的分支必须与上面 body 弹层分支一一对应：body 里每个全屏/模态
     * UI 都在这里决定光标显隐。否则缺失的模式会掉进下面的 active_pane 终端
     * 分支而发出 ?25h，把弹窗本应隐藏的光标重新点亮（BUG-10：退出确认弹窗上
     * 出现游离的终端光标）。分支顺序与 body 的 if/else-if 链保持一致。 */
    /* 分屏模式（无模态弹层）：活动窗格的终端光标已在 render_split_pane 内定位
     * 并显隐；这里统一先把光标定位到活动窗格的终端光标处、置可见。有模态弹层
     * （搜索/命令面板/改名等）时落到下面对应分支由弹层管光标。 */
    int split_mode = (!g_mux.confirm_exit_mode && !g_mux.confirm_close_mode &&
                      !g_search_mode && !g_mux.palette_mode &&
                      !g_mux.chooser_mode && !g_mux.ctx_mode && !g_mux.rename_mode &&
                      !g_mux.custom_cmd_mode && split_is_split());
    if (g_mux.confirm_exit_mode || g_mux.confirm_close_mode) {
        /* 确认弹窗（退出 / 关闭窗格）是模态弹窗，没有输入光标，必须隐藏终端光标。 */
        pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
    } else if (split_mode) {
        Pane *ap = &g_mux.panes[g_mux.active_pane];
        ScreenBuffer *s = &ap->screen;
        PaneRect rects[MAX_PANES];
        for (int i = 0; i < MAX_PANES; i++) rects[i].valid = 0;
        int root = split_active_root();
        if (root >= 0) split_layout(root, 0, 0, g_mux.host_cols, g_mux.host_rows, split_nodes(), rects);
        /* zoom（全屏缩放）时活动窗格铺满内容区，光标矩形也要用全尺寸，否则
         * 光标仍停在缩放前的小窗格矩形里。 */
        PaneRect zoom_rc;
        PaneRect *rc = &rects[g_mux.active_pane];
        if (g_split_zoom) {
            zoom_rc.c0 = 0; zoom_rc.r0 = 0;
            zoom_rc.cols = g_mux.host_cols; zoom_rc.rows = g_mux.host_rows; zoom_rc.valid = 1;
            rc = &zoom_rc;
        }
        if (rc->valid && s->cursor_visible && ap->scroll_offset == 0 && !g_copy_mode) {
            int cx = s->cursor_x, cy = s->cursor_y;
            if (cx >= rc->cols) cx = rc->cols - 1;
            if (cy >= rc->rows) cy = rc->rows - 1;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h",
                            rc->r0 + cy + 2, rc->c0 + cx + 1);
        } else {
            pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
        }
    } else if (g_search_mode) {
        int row, left, input_col, box_w;
        search_box_layout(g_mux.host_cols, &row, &left, &input_col, &box_w);
        int scr_off = get_input_screen_offset(g_search_buf, g_search_len, g_search_pos, box_w);
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", row, input_col + scr_off);
    } else if (g_mux.palette_mode) {
        if (g_mux.palette_page == PALETTE_PAGE_PANEL_EDITOR) {
            int top, left, input_w;
            palette_editor_geom(g_mux.host_rows, g_mux.host_cols, &top, &left, NULL, NULL, &input_w);
            char *buf = NULL;
            int len = 0, text_pos = 0;
            if (g_mux.palette_field == 0) {
                buf = g_edit_name; len = g_edit_name_len; text_pos = g_edit_name_pos;
            } else if (g_mux.palette_field == 1) {
                buf = g_edit_cmd; len = g_edit_cmd_len; text_pos = g_edit_cmd_pos;
            } else if (g_mux.palette_field == 2) {
                buf = g_edit_dir; len = g_edit_dir_len; text_pos = g_edit_dir_pos;
            }
            if (!buf) {
                /* v1.8.9: 颜色选择行没有输入框，藏光标。 */
                pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
            } else {
                int scr_off = get_input_screen_offset(buf, len, text_pos, input_w);
                pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h",
                                top + 2 + g_mux.palette_field * 2, left + 2 + scr_off);
            }
        } else if (g_mux.palette_focus == PALETTE_FOCUS_INPUT) {
            int top, left, pw, ph;
            palette_geom(g_mux.host_rows, g_mux.host_cols, &top, &left, &pw, &ph);
            int input_w = pw - 6;
            if (input_w < 8) input_w = 8;
            int scr_off = get_input_screen_offset(g_mux.palette_query, g_mux.palette_query_len, g_mux.palette_query_pos, input_w);
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", top + 1, left + 4 + scr_off);
        } else {
            pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
        }
    } else if (g_copy_mode) {
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", g_copy_cy + 2, g_copy_cx + 1);
    } else if (g_mux.rename_mode) {
        int r_top = 2;
        int r_anchor0 = (g_pop_anchor_x >= 0) ? g_pop_anchor_x : g_mouse_x;
        int r_left = popup_left_1based(r_anchor0, RENAME_W, g_mux.host_cols);
        int scr_off = get_input_screen_offset(g_mux.rename_buf, g_mux.rename_len, g_mux.rename_pos, RENAME_W - 3);
        int cx = r_left + 2 + scr_off;
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", r_top + 1, cx);
    } else if (g_mux.custom_cmd_mode) {
        int c_top = 2;
        int c_anchor0 = (g_pop_anchor_x >= 0) ? g_pop_anchor_x : g_mouse_x;
        int c_left = popup_left_1based(c_anchor0, CMD_BOX_W, g_mux.host_cols);
        int scr_off = get_input_screen_offset(g_mux.custom_cmd_buf, g_mux.custom_cmd_len, g_mux.custom_cmd_pos, CMD_BOX_W - 3);
        int cx = c_left + 2 + scr_off;
        pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", c_top + 1, cx);
    } else if (g_mux.active_pane >= 0 && g_mux.active_pane < g_mux.pane_count && g_mux.panes[g_mux.active_pane].active && g_mux.panes[g_mux.active_pane].is_settings) {
        /* 只有菜单项详情页（文本输入）与颜色十六进制编辑才显示光标；
         * 外观 / 键位 / 行为页没有输入框，必须把光标藏起来，
         * 否则会留下一个位置错乱的闪烁光标。 */
        if (g_hex_edit_active && !g_settings_show_presets) {
            if (hex_edit_popup_shown(g_mux.host_rows, g_mux.host_cols)) {
                /* v2.1.1：编辑框在浮层里 ⇒ 光标也落在浮层的值段上（窄终端上表行里的
                 * 值段本来就被裁出屏幕，光标跟过去等于看不见）。 */
                int ptop, pleft, pw, ph;
                hex_edit_popup_geom(g_mux.host_rows, g_mux.host_cols, &ptop, &pleft, &pw, &ph);
                int ccol = pleft + HEX_EDIT_POPUP_HEX_OFF + g_hex_edit_len;
                if (ccol > g_mux.host_cols) ccol = g_mux.host_cols;
                if (ccol < 1) ccol = 1;
                pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", ptop + 1, ccol);
                goto cursor_done;
            }
            int sb_w = settings_host_sidebar_w(g_mux.host_cols);   /* v2.1.2：与画侧栏同一式 */
            int main_left = sb_w + 3;
            if (g_hex_edit_role < 0) {
                /* v2.0.9：窗格配色页的 hex 框（role 负数编码槽位）。以前这里拿负数去查外观页
                 * 的角色行列 —— 光标飞到别处（用户反馈「编辑时光标不对」）。 */
                int slot = -g_hex_edit_role - 1;
                int col = settings_pane_col(g_mux.host_cols, main_left, slot);
                int row = settings_pane_row(g_mux.host_cols, main_left, slot);
                if (row > 0)
                    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", row,
                                    col + SETTINGS_PANE_VALUE_OFF + g_hex_edit_len);
                else
                    pos += snprintf(out + pos, bs - pos, "\x1b[?25l");   /* 1 基：'#' 在 col+VALUE_OFF-1，光标停在已输入字符之后 */
            } else {
                /* 外观页也能纵向滚动（v2.1.0），光标必须跟着同一套行号换算 */
                int role_col = settings_role_col(main_left, g_hex_edit_role);
                int crow = settings_appearance_row_view(g_mux.host_rows, settings_role_row(g_hex_edit_role));
                if (crow > 0)
                    pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h",
                                    crow, role_col + 21 + g_hex_edit_len);
                else
                    pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
            }
        } else if (g_settings_nav >= 1 && g_settings_nav <= g_chooser_item_count && !g_settings_show_presets) {
            int sb_w = settings_host_sidebar_w(g_mux.host_cols);   /* v2.1.2：与画侧栏同一式 */
            int main_left = sb_w + 3;
            int right_max_w = g_mux.host_cols - main_left - 2;
            if (right_max_w < 10) right_max_w = 10;
            int input_w = right_max_w - 4;
            if (input_w > 50) input_w = 50;
            if (input_w < 20) input_w = 20;

            /* v2.1.0：详情页整页可滚，光标行号同样经 settings_detail_row_view() 换算 */
            static const int d_field_nat[3] = { 6, 9, 12 };
            int d_field = g_settings_field;
            int d_row = (d_field >= 0 && d_field <= 2)
                            ? settings_detail_row_view(g_mux.host_rows, d_field_nat[d_field]) : 0;
            if (d_field >= 0 && d_field <= 2 && d_row > 0) {
                const char *dbuf = (d_field == 0) ? g_edit_name : (d_field == 1) ? g_edit_cmd : g_edit_dir;
                int dlen = (d_field == 0) ? g_edit_name_len : (d_field == 1) ? g_edit_cmd_len : g_edit_dir_len;
                int dpos = (d_field == 0) ? g_edit_name_pos : (d_field == 1) ? g_edit_cmd_pos : g_edit_dir_pos;
                int scr_off = get_input_screen_offset(dbuf, dlen, dpos, input_w);
                pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", d_row, main_left + 2 + scr_off);
            } else if (d_field >= 0 && d_field <= 2) {
                pos += snprintf(out + pos, bs - pos, "\x1b[?25l");   /* 输入框滚出可见区 */
            } else {
                /* v1.8.9: 颜色选择行不是输入框，别留下闪烁光标。 */
                pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
            }
        } else {
            pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
        }
    cursor_done: ;   /* v2.1.1：clang 不许「标签是复合语句末项」（C23 扩展），补一条空语句 */
    } else if (g_mux.chooser_mode || g_mux.ctx_mode || g_mux.help_mode) {
        pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
    } else if (g_mux.active_pane >= 0 && g_mux.active_pane < g_mux.pane_count && g_mux.panes[g_mux.active_pane].active) {
        Pane *pane = &g_mux.panes[g_mux.active_pane];
        ScreenBuffer *s = &pane->screen;
        int vo = pane->scroll_offset;
        int cursor_row = 0, cursor_col = 0;
        if (terminal_cursor_position(s, vo, g_mux.host_rows, g_mux.host_cols,
                                     &cursor_row, &cursor_col)) {
            pos += snprintf(out + pos, bs - pos, "\x1b[%d;%dH\x1b[?25h", cursor_row, cursor_col);
        } else {
            pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
        }
    } else {
        pos += snprintf(out + pos, bs - pos, "\x1b[?25l");
    }

    /* 诊断帧头：必须与 dump_render_output 在同一位置打印，两份日志的帧号才同源。
     * 每帧一行，即使这一帧没有任何宽字符也照打——上次两份日志对不上，就是因为
     * cell_diag 只在含宽字符的行才输出，帧号跳过了大量帧。
     *
     * 条件必须与 dump_render_output 的调用条件【逐条相同】，否则一边写了另一边没写，
     * 帧号又会错开。dump_render_output 有三道门：外层要求 active_pane 有效且 .active，
     * 函数内部还要求 g_dump_enabled（TERMUX_DUMP）与 len>0。这里全部照抄。
     * 所以抓这份日志时 TERMUX_DUMP 和 TERMUX_CELLDIAG 必须【同时】设置。 */
    int cd_dump_will_write =
        (getenv("TERMUX_DUMP") != NULL) && pos > 0 &&
        (g_mux.active_pane >= 0 && g_mux.active_pane < g_mux.pane_count &&
         g_mux.panes[g_mux.active_pane].active);
    if (getenv("TERMUX_CELLDIAG") && cd_dump_will_write) {
        /* pb 必须初始化：某个 pane 都没有的帧（例如分屏关闭到只剩一个窗格、
         * 走整屏路径）pn 会是 0，此时 snprintf 不写入 pb，后面 "%s" 读到的就是
         * 栈上上一次调用残留的字节 —— 会把上一帧的 pane 列表拼到本帧帧头后面。 */
        char pb[512]; pb[0] = '\0'; int pn = 0;
        for (int i = 0; i < MAX_PANES; i++) {
            if (!g_cd_pane[i].valid) continue;
            pn += snprintf(pb + pn, sizeof pb - pn,
                           " pane%d[c0=%d ocols=%d scols=%d srows=%d cols=%d rows=%d vo=%d hist=%d conpty_cols=%d narrow=%d]",
                           i, g_cd_pane[i].oc0, g_cd_pane[i].ocols, g_cd_pane[i].scols,
                           g_cd_pane[i].srows, g_cd_pane[i].cols, g_cd_pane[i].rows,
                           g_cd_pane[i].vo, g_cd_pane[i].hist, g_cd_pane[i].conpty_cols,
                           g_cd_pane[i].narrow);
        }
        char hdr[640];
        int hn = snprintf(hdr, sizeof hdr,
                          "FRAME %d host=%dx%d model=%dx%d panes=%d active=%d drag=%d len=%d%s\n",
                          cell_diag_frame, g_mux.host_cols, g_mux.host_rows,
                          g_mux.panes[g_mux.active_pane].screen.cols,
                          g_mux.panes[g_mux.active_pane].screen.rows,
                          g_mux.pane_count, g_mux.active_pane,
                          split_drag_active(), pos, pb);
        if (hn < 0) hn = 0;
        if (hn > (int)sizeof hdr - 1) hn = (int)sizeof hdr - 1;
        /* 帧头在前、明细在后，一次写盘：日志里每帧是一个完整块。 */
        cell_diag("%.*s%.*s", hn, hdr, g_cd_len, g_cd_buf);
        g_cd_len = 0;
        for (int i = 0; i < MAX_PANES; i++) g_cd_pane[i].valid = 0;
    }
    if (g_mux.active_pane >= 0 && g_mux.active_pane < g_mux.pane_count && g_mux.panes[g_mux.active_pane].active)
        dump_render_output(out, pos, g_mux.panes[g_mux.active_pane].screen.cols, g_mux.panes[g_mux.active_pane].screen.rows, g_mux.host_cols, g_mux.host_rows);
    g_mux.needs_redraw = 0;
    LeaveCriticalSection(&g_mux.cs);

    theme_remap(out, pos);

    /* 脏区输出：整帧按 CUP 切成逐行字节，只发与上一帧不同的行。
     * host 尺寸变化会让 begin_frame 检测到行数不一致并强制整帧重发。
     * 注意 framediff 状态只在本线程（渲染循环）访问，放在锁外即可。
     * v1.8.14：只对 body（cursor_pos 之前：标签栏 + 各内容行 + 弹层）做行差分；
     * cursor_pos 之后是光标段（定位 + 显隐），逐帧无条件追加到增量末尾——否则它会
     * 被末尾 CUP 折进光标所在行的 chunk，那一行内容没变时整段被跳过，光标丢失。 */
    size_t cursor_len = (size_t)(pos - cursor_pos);
    framediff_begin_frame(&g_frame_diff, g_mux.total_host_rows);
    framediff_scan(&g_frame_diff, out, (size_t)cursor_pos);
    size_t dlen = framediff_emit(&g_frame_diff, NULL, 0) + cursor_len;
    if (dlen < (size_t)pos) {
        /* 增量路径：复用常驻缓冲，先写差分 body，再原样追加光标段。 */
        if (dlen + 1 > g_diff_buf_cap) {
            size_t cap = g_diff_buf_cap > 0 ? g_diff_buf_cap : 8192;
            while (cap < dlen + 1) cap *= 2;
            char *nb = (char *)realloc(g_diff_buf, cap);
            if (nb) { g_diff_buf = nb; g_diff_buf_cap = cap; }
        }
        if (g_diff_buf && dlen + 1 <= g_diff_buf_cap) {
            size_t blen = framediff_emit(&g_frame_diff, g_diff_buf, g_diff_buf_cap);
            if (blen + cursor_len <= g_diff_buf_cap)
                memcpy(g_diff_buf + blen, out + cursor_pos, cursor_len);
            dump_delta_output(g_diff_buf, (int)dlen, (int)pos);
            host_write(g_diff_buf, (int)dlen);
        } else {
            dump_delta_output(out, pos, (int)pos);
            host_write(out, pos);
        }
    } else {
        /* 增量没省到（或分配失败）：发整帧。 */
        dump_delta_output(out, pos, (int)pos);
        host_write(out, pos);
    }
}

void render_cleanup(void) {
    if (g_render_buf) {
        free(g_render_buf);
        g_render_buf = NULL;
        g_render_buf_cap = 0;
    }
    framediff_free(&g_frame_diff);
}
