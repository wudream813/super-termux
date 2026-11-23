#include "vt.h"
#include "config.h"        /* v2.3.6：graphics / graphics_max */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ================= TERMUX_VTTRACE：行边界诊断（仅在环境变量存在时写文件） =========
 * 每遇到一次「自动折行 / 显式换行 / 右缘附近的 EL·ECH / 带 pending 的 CUP」就记一行。
 * 行边界记账出错是「内容整体错位一两行」这类故障的根源，而帧日志只能看到结果、
 * 看不到模型在哪一列决定换行。默认关闭，正式使用零开销。 */
static FILE *g_vt_trace_fp = NULL;
static int g_vt_trace_state = 0;      /* 0 未探测 / 1 开 / -1 关 */
static long long g_vt_off = 0;        /* 本进程已处理的窗格字节数 */

static void vt_trace_open(void) {
    if (g_vt_trace_state) return;
    g_vt_trace_state = getenv("TERMUX_VTTRACE") ? 1 : -1;
    if (g_vt_trace_state == 1) g_vt_trace_fp = fopen("vttrace.log", "wb");
}

static void vt_trace(const char *fmt, ...) {
    if (!g_vt_trace_fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_vt_trace_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_vt_trace_fp);
    /* 诊断进程常被 TerminateProcess 杀掉：不 flush 就一个字都留不下。 */
    fflush(g_vt_trace_fp);
}

/* 一次同步更新（?2026h..?2026l）结束时，把模型里每一行的「最后非空格列」记下来。
 * 与 conhost 屏幕对照时，这是最省事的行边界指纹：行数错位/折行提前都会露出来。 */
static void vt_trace_rows(ScreenBuffer *s, const char *tag) {
    if (!g_vt_trace_fp) return;
    char buf[2048];
    int p = 0;
    for (int y = 0; y < s->rows && p < (int)sizeof buf - 16; y++) {
        int w = 0;
        for (int x = 0; x < s->cols; x++) {
            WCHAR ch = 0;
            if (s->in_alt_screen && s->alt_buffer) {
                ch = s->alt_buffer[(size_t)y * s->cols + x].Char.UnicodeChar;
            } else {
                int pr = screen_phys_row(s, y);
                if (pr >= 0 && pr < s->total_lines && s->lines && s->lines[pr].cells)
                    ch = s->lines[pr].cells[x].Char.UnicodeChar;
            }
            if (ch && ch != L' ') w = x + 1;
        }
        p += snprintf(buf + p, sizeof buf - (size_t)p, "%d,", w);
    }
    vt_trace("%s off=%lld cursor=(%d,%d) widths=%s", tag, g_vt_off,
             s->cursor_x, s->cursor_y, buf);
}

/* ===== 右缘「疑似填充空格」的延迟判定 =====
 * ConPTY 在宽字符正好占满右缘时会补一个纯排版空格（实测窗格 40 列、「驱」占
 * 36-37 列、空格落 38 列），写进模型会把 used 撑大，reflow 之后变成「驱 动」
 * 这种凭空多出的空格。老做法是在 screen_put_cp 里直接丢弃，但同样的形状也会
 * 出现在真实文本里：mdt 的界面就把「，」摆在 116-117 列、后面紧跟一个真空格
 * 在 118 列，再往后还有光标移动与边框字符。直接丢弃会让整行少一格，右缘边框
 * 落到 118 列、下一行开头被挤到上一行末尾，逐行累积成「右侧滚动条/状态行错位」
 * （2026-11-22 的 vttrace 实测 22 处）。
 * 所以改成两步：先按普通空格写入，等下一个事件再定性质——
 *   紧跟可打印字符（中间可以夹 SGR/模式序列）= 内容里的真实空格，保留；
 *   紧跟换行、光标移动、擦除 = 排版填充，撤回（等价于从未写入）。 */
static void vt_pad_space_mark(ScreenBuffer *s, int x, int y) {
    s->pad_pend = 1;
    s->pad_x = x;
    s->pad_y = y;
}

static void vt_pad_space_undo(ScreenBuffer *s) {
    if (!s->pad_pend) return;
    s->pad_pend = 0;
    int x = s->pad_x, y = s->pad_y;
    if (y < 0 || y >= s->rows || x < 0 || x >= s->cols) return;
    WORD attr = build_attr(s);
    /* screen_erase_range 擦到行尾时会把 used 收缩到起始列，正好等于「没写过」。 */
    screen_erase_range(s, y, x, x, attr);
    if (s->cursor_y == y && (s->cursor_x == x || s->cursor_x == x + 1)) {
        s->cursor_x = x;
        s->wraparound_pending = 0;
    }
    vt_trace("PADUNDO off=%lld x=%d y=%d", g_vt_off, x, y);
}

static void screen_put_cp(ScreenBuffer *s, unsigned int cp) {
    vt_trace_open();
    /* 来了一个真正的可打印字符：上一格那个可疑空格就是内容，保留。 */
    s->pad_pend = 0;
    s->cup_eol_row = -1;   /* 写了内容，「刚 CUP 到底行末列」的判定窗口即作废 */
    if (s->wraparound_pending) {
        vt_trace("WRAP off=%lld x=%d y=%d cp=U+%04X wide=%d cols=%d rows=%d",
                 g_vt_off, s->cursor_x, s->cursor_y, cp, is_wide_cp(cp), s->cols, s->rows);
        s->cursor_x = 0;
        screen_newline(s);
        screen_mark_softwrap(s);   /* 自动折行：新物理行是上一行的软换行续行 */
        s->wraparound_pending = 0;
    }
    /* ConPTY 右缘填充空格：宽字符正好写完一行的最后两列时，conhost 会在折行前
     * 补一个空格把右缘填满（实测字节流 uploads/termux_dump.log 偏移 6145，
     * [pane 0 len 115]：「…Windows驱 \n」，窗格宽 40，「驱」占 36-37 列，那个空格
     * 落在 38 列）。这个空格是纯排版填充、不是内容，但一旦写进模型就会把 used 撑大，
     * 之后窗格变宽 reflow 把折行拼回去，它就永久夹在宽字符和下一个字之间——表现为
     * 「驱 动」「洛 谷」这类中文里凭空多出空格（cell_diag.log F104-F131 实测 26 条）。
     * 判据：本格左邻是宽字符次格（ch==0）、且光标已贴近右缘。阈值取 cols-2 而非
     * cols-1：conhost 自己算的右缘与本模型可能差一列（实测「驱」占 36-37、
     * cursor_x=38、cols=40，填充空格仍会发来），只卡 cols-1 会漏。
     *
     * 但这个形状在真实内容里也会出现（mdt 就在 118 列放过真空格，后面紧跟光标
     * 移动与边框字符），当场丢弃会把整行挤短一格，累积成右侧边框错位。所以这里
     * 只做「标记」，由下一个事件决定是保留还是撤回（见 vt_pad_space_undo）。 */
    int maybe_pad_space = (cp == 0x20 && s->cursor_x > 0 && s->cursor_x >= s->cols - 2);
    if (maybe_pad_space) {
        CHAR_INFO *prev = screen_cell(s, s->cursor_y, s->cursor_x - 1);
        maybe_pad_space = (prev && prev->Char.UnicodeChar == 0);
    }
    int wide = is_wide_cp(cp);
    if (wide && s->cursor_x >= s->cols - 1) {
        /* 宽字符在只剩一列（cursor_x == cols-1）时放不下，要整体移到下一行
         * 行首。此时旧行最后一列（cursor_x == cols-1）放不下宽字的两格，
         * 必须显式清成空格：否则它会保留上一帧的脏内容（可能是空格、旧字符，
         * 甚至是 ConPTY 重绘时残留的宽字次格 0 / 主格）。吸附函数
         * snap_left_to_char 只看「本格 ch==0 且左邻是宽字主格」就把该格认成
         * 宽字次格而左退一列——一旦这条「因汉字换行」的脏行出现在块选里，
         * 选区左沿在那一行被错误吸附；更常见的是脏末格被渲染/复制路径按宽字
         * 处理，造成经过该行之后所有行的高亮整体错位一列。清成空格后它就是
         * 普通空白，吸附与渲染都不会再误判。 */
        if (s->cursor_x == s->cols - 1) {
            WORD attr = build_attr(s);
            screen_erase_range(s, s->cursor_y, s->cursor_x, s->cursor_x, attr);
        }
        s->cursor_x = 0;
        screen_newline(s);
        screen_mark_softwrap(s);   /* 宽字符放不下整字换行：同样是软换行续行 */
        s->wraparound_pending = 0;
    }
    int wx = s->cursor_x, wy = s->cursor_y;
    WORD attr = build_attr(s);
    if (cp >= 0x10000) {
        WCHAR high = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10));
        WCHAR low = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        screen_write_cell(s, s->cursor_y, s->cursor_x, high, attr);
        if (s->cursor_x + 1 < s->cols) {
            screen_write_cell(s, s->cursor_y, s->cursor_x + 1, low, attr);
        }
    } else {
        screen_write_cell(s, s->cursor_y, s->cursor_x, (WCHAR)cp, attr);
        if (wide) {
            screen_write_cell(s, s->cursor_y, s->cursor_x + 1, 0, attr);
        }
    }
    if (s->cursor_x + (wide ? 2 : 1) < s->cols) {
        s->cursor_x += (wide ? 2 : 1);
    } else if (s->auto_wrap) {
        s->wraparound_pending = 1;
        if (s->detect_count <= 100) detect_conpty_width(s, 0);
    }
    if (maybe_pad_space) vt_pad_space_mark(s, wx, wy);
}

static void screen_send_response(ScreenBuffer *s, const char *resp) {
    int len = (int)strlen(resp);
    if (len < (int)sizeof(s->response_buf)) {
        memcpy(s->response_buf, resp, len);
        s->response_len = len;
    }
}

static int parse_params(const char *buf, int len, int *params, int max) {
    int count = 0, val = 0, has = 0;
    for (int i = 0; i < len && count < max; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (val > 9999999) val = 9999999;
            has = 1;
        }
        else if (c == ';' || c == ':') { params[count++] = has ? val : 0; val = 0; has = 0; }
    }
    if ((has || count > 0) && count < max) params[count++] = has ? val : 0;
    return count;
}

void process_sgr(ScreenBuffer *s, const int *p, int n) {
    if (n == 0) { s->fg_color = 7; s->bg_color = 0; s->bold = s->underline = s->reverse_video = 0; s->fg_rgb_on = s->bg_rgb_on = 0; s->current_attr = build_attr(s); return; }
    for (int i = 0; i < n; i++) {
        int v = p[i];
        switch (v) {
            case 0: s->fg_color = 7; s->bg_color = 0; s->bold = s->underline = s->reverse_video = 0; s->fg_rgb_on = s->bg_rgb_on = 0; break;
            case 1: s->bold = 1; break;
            case 4: s->underline = 1; break;
            case 7: s->reverse_video = 1; break;
            case 22: s->bold = 0; break;
            case 24: s->underline = 0; break;
            case 27: s->reverse_video = 0; break;
            case 39: s->fg_color = 7; s->fg_rgb_on = 0; break;
            case 49: s->bg_color = 0; s->bg_rgb_on = 0; break;
            default:
                if (v >= 30 && v <= 37) { s->fg_color = v - 30; s->fg_rgb_on = 0; }
                else if (v >= 40 && v <= 47) { s->bg_color = v - 40; s->bg_rgb_on = 0; }
                else if (v >= 90 && v <= 97) { s->fg_color = v - 90 + 8; s->fg_rgb_on = 0; }
                else if (v >= 100 && v <= 107) { s->bg_color = v - 100 + 8; s->bg_rgb_on = 0; }
                else if (v == 38 && i + 2 < n && p[i+1] == 5) {
                    int c = p[i+2];
                    if (c < 16) s->fg_color = c;
                    else if (c < 232) { c -= 16; s->fg_color = ((c/36)>2?1:0)|((c/6%6)>2?2:0)|((c%6)>2?4:0); if((c/36)>3||(c/6%6)>3||(c%6)>3) s->fg_color|=8; }
                    else s->fg_color = (c-232)>12?15:7;
                    s->fg_rgb_on = 0;
                    i += 2;
                } else if (v == 48 && i + 2 < n && p[i+1] == 5) {
                    int c = p[i+2];
                    if (c < 16) s->bg_color = c;
                    else if (c < 232) { c -= 16; s->bg_color = ((c/36)>2?1:0)|((c/6%6)>2?2:0)|((c%6)>2?4:0); if((c/36)>3||(c/6%6)>3||(c%6)>3) s->bg_color|=8; }
                    else s->bg_color = (c-232)>12?15:0;
                    s->bg_rgb_on = 0;
                    i += 2;
                } else if (v == 38 && i + 4 < n && p[i+1] == 2) {
                    int r = p[i+2], g = p[i+3], b = p[i+4];
                    s->fg_color = (r>127?4:0)|(g>127?2:0)|(b>127?1:0); if(r>191||g>191||b>191) s->fg_color|=8;
                    s->fg_r = r; s->fg_g = g; s->fg_b = b; s->fg_rgb_on = 1;
                    i += 4;
                } else if (v == 48 && i + 4 < n && p[i+1] == 2) {
                    int r = p[i+2], g = p[i+3], b = p[i+4];
                    s->bg_color = (r>127?4:0)|(g>127?2:0)|(b>127?1:0);
                    s->bg_r = r; s->bg_g = g; s->bg_b = b; s->bg_rgb_on = 1;
                    i += 4;
                }
                break;
        }
    }
    s->current_attr = build_attr(s);
}

static inline int ci_str_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + ('a' - 'A')) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + ('a' - 'A')) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

static inline int ci_str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        char ca = (*str >= 'A' && *str <= 'Z') ? (char)(*str + ('a' - 'A')) : *str;
        char cb = (*prefix >= 'A' && *prefix <= 'Z') ? (char)(*prefix + ('a' - 'A')) : *prefix;
        if (ca != cb) return 0;
        str++; prefix++;
    }
    return 1;
}

void sanitize_title(const char *raw, int raw_len, char *out, int out_size) {
    if (!raw || raw_len <= 0 || out_size <= 0) {
        if (out && out_size > 0) out[0] = 0;
        return;
    }
    char buf[512];
    int len = raw_len < 511 ? raw_len : 511;
    memcpy(buf, raw, len);
    buf[len] = 0;

    while (len > 0 && ((unsigned char)buf[len - 1] <= ' ' || buf[len - 1] == 0x07)) {
        buf[--len] = 0;
    }

    const char *p = buf;

    if (strncmp(p, "\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98", 9) == 0) {
        p += 9;
        while (*p == ':' || *p == ' ') p++;
    } else if (ci_str_starts_with(p, "Administrator")) {
        p += 13;
        while (*p == ':' || *p == ' ') p++;
    }

    const char *colon = strstr(p, ":   ");
    if (!colon) colon = strstr(p, ":  ");
    if (!colon) colon = strstr(p, ": ");
    if (colon) {
        p = colon + 1;
        while (*p == ' ' || *p == ':') p++;
    }

    const char *dash = strstr(p, " - ");
    if (dash && (strstr(buf, ".exe") || strstr(buf, "\\") || strstr(buf, "/"))) {
        p = dash + 3;
        while (*p == ' ') p++;
    }

    while (*p == ':' || *p == '-' || *p == ' ') p++;

    if (strstr(p, "\\") || strstr(p, "/")) {
        const char *last_slash = p;
        for (const char *sp = p; *sp; sp++) {
            if (*sp == '\\' || *sp == '/') last_slash = sp + 1;
        }
        p = last_slash;
    }

    if (ci_str_eq(p, "cmd.exe") || ci_str_eq(p, "cmd")) {
        p = "cmd";
    } else if (ci_str_eq(p, "powershell.exe") || ci_str_eq(p, "powershell")) {
        p = "PowerShell";
    }

    if (!*p) p = "cmd";

    snprintf(out, out_size, "%s", p);
}

void execute_osc(ScreenBuffer *s) {
    if ((s->osc_num == 0 || s->osc_num == 1 || s->osc_num == 2) && s->osc_len > 0) {
        int idx = s->pane_index;
        if (idx >= 0 && idx < g_mux.pane_count && g_mux.panes[idx].active) {
            if (!g_mux.panes[idx].is_about && !g_mux.panes[idx].is_settings) {
                char raw[256];
                int rlen = s->osc_len < 255 ? s->osc_len : 255;
                memcpy(raw, s->osc_buf, rlen);
                raw[rlen] = 0;
                while (rlen > 0 && ((unsigned char)raw[rlen - 1] <= ' ' || raw[rlen - 1] == 0x07)) raw[--rlen] = 0;
                snprintf(g_mux.panes[idx].full_title, sizeof(g_mux.panes[idx].full_title), "%s", raw);

                sanitize_title(s->osc_buf, s->osc_len, g_mux.panes[idx].title, sizeof(g_mux.panes[idx].title));
            }
        }
    }
    s->osc_num = -1; s->osc_len = 0; s->osc_sep = 0;
}

void execute_esc(ScreenBuffer *s, char final, const char *inter, int inter_len) {
    (void)inter;
    if (inter_len > 0) return;
    s->cup_eol_row = -1;

    switch (final) {
        case 'D': screen_newline(s); break;
        case 'E': s->cursor_x = 0; screen_newline(s); break;
        case 'M': if (s->cursor_y <= s->scroll_region_top) screen_scroll_down(s, s->scroll_region_top, s->scroll_region_bottom, 1); else s->cursor_y--; break;
        case '7': s->saved_cx = s->cursor_x; s->saved_cy = s->cursor_y; break;
        case '8': s->cursor_x = s->saved_cx; s->cursor_y = s->saved_cy; s->wraparound_pending = 0; break;
        case '=': s->app_keypad = 1; break;
        case '>': s->app_keypad = 0; break;
        case 'c': s->fg_color = 7; s->bg_color = 0; s->bold = s->underline = s->reverse_video = 0; s->fg_rgb_on = s->bg_rgb_on = 0; s->current_attr = build_attr(s); break;
        case 'H': if (s->cursor_x < 512) s->tab_stops[s->cursor_x] = 1; break;
    }
}

static void execute_csi_internal(ScreenBuffer *s, char final, char prefix, const char *params_str, int params_len, const char *inter, int inter_len) {
    (void)inter;
    int params[32] = {0};
    int pc = parse_params(params_str, params_len, params, 32);
    int p1 = pc > 0 ? params[0] : 0;
    int p2 = pc > 1 ? params[1] : 0;

    if (inter_len > 0) return;
    /* 右缘可疑填充空格：只要来的是移动光标/擦除类 CSI，就确认它是排版填充并撤回。
     * SGR（m）与私有模式开关（?h/?l）既不移光标也不动内容，不作结论。 */
    switch (final) {
        case 'H': case 'f': case 'A': case 'B': case 'C': case 'D': case 'E':
        case 'F': case 'G': case 'd': case 'J': case 'K': case 'X': case 'L':
        case 'M': case 'P': case 'S': case 'T': case '@':
            vt_pad_space_undo(s);
            break;
        default:
            break;
    }
    /* CSI ? Ps h/l（DECTCEM 光标显隐、鼠标模式等私有开关）既不移光标也不动内容，
     * 不能作废「底行末列 CUP」的判定窗口 —— 真机字节里它就夹在那个 CUP 和随后的
     * CRLF 中间（uploads/termux_dump.log 2026-09-17 14:10 那批，pane0 偏移 3878）：
     *     ESC[29;39H ESC[?25l CR LF "            Users  "
     * 一并作废就会让这条记录被拆成「<DIR> 那半 + 名字那半」两行（本地回放实测）。
     * 除此之外任何 CSI 都作废该窗口。 */
    if (!(prefix == '?' && (final == 'h' || final == 'l'))) s->cup_eol_row = -1;

    if (prefix == '?') {
        if (final == 'h') {
            for (int i = 0; i < pc; i++) {
                switch (params[i]) {
                    case 1: s->app_cursor_keys = 1; break;
                    case 7: s->auto_wrap = 1; break;
                    case 25:
                        s->cursor_visible = 1;
                        s->repaint_candidate = 0;
                        /* 整屏重绘到此结束：若提示符下方被 conhost 补了一串空行，
                         * 从 scrollback 拉等量历史把它顶回最后一行（见函数注释）。
                         * 用户正在回看历史时不动，免得视图跳。 */
                        if (s->repaint_active) {
                            int pi = s->pane_index;
                            int at_bottom = !(pi >= 0 && pi < MAX_PANES &&
                                              g_mux.panes[pi].scroll_offset != 0);
                            int pending = s->resize_repaint_pending;
                            s->repaint_active = 0;
                            /* 用户正在回看历史时不动，免得视图跳。 */
                            if (at_bottom && pending) {
                                /* 返回 1 = 这趟重绘写到了最后一行、没东西可锚定，
                                 * 是 conhost 两趟重绘的第一趟：pending 和快照都留给
                                 * 下一趟（真机 2026-09-20：第一趟 ESC[29;1H ESC[?25h
                                 * 收尾，第二趟才是 ESC[5;26H + 24 行 ESC[K）。
                                 * 让过一趟就封顶，免得 pending 长期挂着。 */
                                if (screen_repaint_reanchor(s) == 1 &&
                                    ++s->resize_repaint_pass < 2) {
                                    /* 保留 resize_repaint_pending 与 repaint_snap */
                                } else {
                                    s->resize_repaint_pending = 0;
                                    screen_repaint_snapshot_free(s);
                                }
                            } else {
                                s->resize_repaint_pending = 0;
                                screen_repaint_snapshot_free(s);
                            }
                        }
                        break;
                    case 47: case 1047:
                        if (!s->in_alt_screen) {
                            s->in_alt_screen = 1; s->alt_scroll_top = s->scroll_top;
                            s->alt_hist_lines = s->hist_lines;
                        }
                        for (int j = 0; j < s->rows * s->cols; j++) {
                            s->alt_buffer[j].Char.UnicodeChar = L' '; s->alt_buffer[j].Attributes = s->current_attr;
                            if (s->alt_fg_rgb) { s->alt_fg_rgb[j] = RGB565_WHITE; s->alt_bg_rgb[j] = RGB565_BLACK; s->alt_rgb_valid[j] = 0; }
                        }
                        { int pi = s->pane_index; if (pi >= 0 && pi < MAX_PANES) g_mux.panes[pi].scroll_offset = 0; }
                        break;
                    case 1049:
                        s->saved_cx = s->cursor_x; s->saved_cy = s->cursor_y;
                        vt_trace("ALT_ON off=%lld", g_vt_off);
                        if (!s->in_alt_screen) {
                            s->in_alt_screen = 1; s->alt_scroll_top = s->scroll_top;
                            s->alt_hist_lines = s->hist_lines;
                        }
                        for (int j = 0; j < s->rows * s->cols; j++) {
                            s->alt_buffer[j].Char.UnicodeChar = L' '; s->alt_buffer[j].Attributes = s->current_attr;
                            if (s->alt_fg_rgb) { s->alt_fg_rgb[j] = RGB565_WHITE; s->alt_bg_rgb[j] = RGB565_BLACK; s->alt_rgb_valid[j] = 0; }
                        }
                        s->cursor_x = s->cursor_y = 0;
                        { int pi = s->pane_index; if (pi >= 0 && pi < MAX_PANES) g_mux.panes[pi].scroll_offset = 0; }
                        break;
                    case 1048: s->saved_cx = s->cursor_x; s->saved_cy = s->cursor_y; break;
                    case 1000: case 1002: case 1003: s->mouse_tracking = params[i]; break;
                    case 1006: s->mouse_sgr = 1; break;
                    case 2004: s->bracketed_paste = 1; break;
                    case 9001: s->win32_input_mode = 1; break;
                    case 6: s->origin_mode = 1; break;
                    case 2026:
                        vt_trace("SYNC_ON off=%lld x=%d y=%d", g_vt_off, s->cursor_x, s->cursor_y);
                        break;
                }
            }
        } else if (final == 'l') {
            for (int i = 0; i < pc; i++) {
                switch (params[i]) {
                    case 1: s->app_cursor_keys = 0; break;
                    case 7: s->auto_wrap = 0; break;
                    case 25:
                        s->cursor_visible = 0;
                        s->repaint_candidate = 1;
                        s->repaint_active = 0;
                        break;
                    case 47: case 1047:
                        if (s->in_alt_screen) {
                            s->in_alt_screen = 0; s->scroll_top = s->alt_scroll_top;
                            s->hist_lines = s->alt_hist_lines;
                            int pi2 = s->pane_index;
                            if (pi2 >= 0 && pi2 < MAX_PANES && g_mux.panes[pi2].active) {
                                if (g_mux.panes[pi2].scroll_offset > s->hist_lines) g_mux.panes[pi2].scroll_offset = s->hist_lines;
                            }
                        }
                        break;
                    case 1049:
                        if (s->in_alt_screen) {
                            vt_trace("ALT_OFF off=%lld", g_vt_off);
                            s->in_alt_screen = 0; s->scroll_top = s->alt_scroll_top;
                            s->hist_lines = s->alt_hist_lines;
                            s->cursor_x = s->saved_cx; s->cursor_y = s->saved_cy;
                            int pi2 = s->pane_index;
                            if (pi2 >= 0 && pi2 < MAX_PANES && g_mux.panes[pi2].active) {
                                if (g_mux.panes[pi2].scroll_offset > s->hist_lines) g_mux.panes[pi2].scroll_offset = s->hist_lines;
                            }
                        }
                        break;
                    case 1048: s->cursor_x = s->saved_cx; s->cursor_y = s->saved_cy; s->wraparound_pending = 0; break;
                    case 1000: case 1002: case 1003: s->mouse_tracking = 0; break;
                    case 1006: s->mouse_sgr = 0; break;
                    case 2004: s->bracketed_paste = 0; break;
                    case 9001: s->win32_input_mode = 0; break;
                    case 6: s->origin_mode = 0; break;
                    case 2026:
                        vt_trace_rows(s, "SYNC_OFF");
                        break;
                }
            }
        }
        return;
    }

    if (prefix == '>' || prefix == '=' || prefix == '<' || prefix == '!') return;

    switch (final) {
        case 'A': { int n = p1 ? p1 : 1; s->cursor_y -= n; if (s->cursor_y < 0) s->cursor_y = 0; s->wraparound_pending = 0; break; }
        case 'B': case 'e': { int n = p1 ? p1 : 1; s->cursor_y += n; if (s->cursor_y >= s->rows) s->cursor_y = s->rows - 1; s->wraparound_pending = 0; break; }
        case 'C': case 'a': { int n = p1 ? p1 : 1; s->cursor_x += n; if (s->cursor_x >= s->cols) s->cursor_x = s->cols - 1; s->wraparound_pending = 0; break; }
        case 'D': { int n = p1 ? p1 : 1; s->cursor_x -= n; if (s->cursor_x < 0) s->cursor_x = 0; s->wraparound_pending = 0; break; }
        case 'E': { int n = p1 ? p1 : 1; s->cursor_x = 0; s->cursor_y += n; if (s->cursor_y >= s->rows) s->cursor_y = s->rows - 1; s->wraparound_pending = 0; break; }
        case 'F': { int n = p1 ? p1 : 1; s->cursor_x = 0; s->cursor_y -= n; if (s->cursor_y < 0) s->cursor_y = 0; s->wraparound_pending = 0; break; }
        case 'G': case '`': { s->cursor_x = (p1 ? p1 : 1) - 1; if (s->cursor_x >= s->cols) s->cursor_x = s->cols - 1; if (s->cursor_x < 0) s->cursor_x = 0; s->wraparound_pending = 0; break; }
        case 'H': case 'f': {
            /* ConPTY resize repaint 固定以隐藏光标后 HOME 开始。仅用控制状态识别，
             * 不检查提示符文本；这样启动 banner 的合法空行也不会被误删。 */
            if (s->repaint_candidate && (p1 == 0 || p1 == 1) &&
                (p2 == 0 || p2 == 1) && !s->in_alt_screen) {
                s->repaint_active = 1;
                /* 存下重绘前的可见行：conhost 增高后只发它 viewport 里那几行，
                 * 少发的那几行要靠这份快照在重绘结束时补回顶部。
                 * 同一次 resize 只取【第一趟】重绘前的快照：第二趟之前屏幕已经被
                 * 第一趟改过了，那时再取就存到一份被污染的快照。 */
                if (s->resize_repaint_pending && !s->repaint_snap) screen_repaint_snapshot(s);
            }
            s->cursor_y = (p1 ? p1 : 1) - 1; s->cursor_x = (p2 ? p2 : 1) - 1;
            if (s->origin_mode) s->cursor_y += s->scroll_region_top;
            if (s->cursor_y >= s->rows) s->cursor_y = s->rows - 1;
            if (s->cursor_y < 0) s->cursor_y = 0;
            if (s->cursor_x >= s->cols) s->cursor_x = s->cols - 1;
            if (s->cursor_x < 0) s->cursor_x = 0;
            if (s->wraparound_pending)
                vt_trace("CUP  off=%lld req=%d,%d -> x=%d y=%d (pending 被清)",
                         g_vt_off, p1, p2, s->cursor_x, s->cursor_y);
            s->wraparound_pending = 0;
            /* 这里曾经有一整套「ConPTY 窄屏行内续写」的 line_wrap 特殊处理
             * （bug #11 加的置 1 + bug #15 第一层加的 CUP 换行清 0）。
             * 2026-09-17 用真机字节流证明【两个分支都是错的】，已全部删除。
             *
             * conhost 折行续写的真实字节形态（uploads/termux_dump.log 2026-09-17
             * 那批，pane0 偏移 1288，窗格 24 列，共 55 处）：
             *     …are ESC]0;…cmd.exe - dir BEL ESC[?25h CR LF ESC[28;24H ena CR LF …
             * ESC[28;24H = 0 基 (27,23) = 【刚写满那一行的最后一列】。写第一个字符
             * 就触发自动折行，所以真正的续行是【下面那一行】—— 而 screen_put_cp
             * 的自动折行路径（screen_newline + screen_mark_softwrap）本来就会正确
             * 地把它标成 line_wrap=1。不需要任何额外规则。
             *
             * 旧代码却标 line_wrap[cursor_y]（= 被续写的那一行），语义正好反了：
             * line_wrap[r]=1 的含义是「r 是 r-1 的续行」。每误标一次就多并一条记录
             * 边界 —— 那次真机会话里触发 47 次，于是 24 列下整份 dir 列表被并成
             * 一条 1484 字符的逻辑行，拖宽到 82 后按 82 重折，就是用户看到的
             * 「内容顺序完好、词从中间切开」的级联错位。
             *
             * CUP 换行清 0 同样错：ESC[28;24H 的落点行本来就在一条逻辑行【中间】，
             * 清掉它的标记会把记录拆成两行（本地实测 A 变体：
             * 「2026-09-17  13:52」与「319,844 cell_diag.log」被拆成上下两行）。
             *
             * 保留的只有 screen_lf() 里那条 LF 清 0（硬换行落点行不可能是续行），
             * A/B 证明它是必需的：去掉它，2026-09-15 那批字节流在 split=4200..4800
             * 复现跨记录拼接。 */
            /* 形态 (2) 的识别：CUP 落在屏幕【最后一行的末列】。conhost 用它把
             * 「已自动折到下一行」的内部光标收回末列，紧跟的 CR LF 才会在底行触发
             * 整屏滚动，续写文本落在滚出来的新底行上（判定见 screen_lf 滚动分支）。
             * 真机字节：ESC[29;47H —— 窗格 24x29，列 47 越界被钳到末列 23。 */
            s->cup_eol_row = (s->cursor_y == s->rows - 1 && s->cursor_x == s->cols - 1)
                             ? s->cursor_y : -1;
            if (p1 <= 1 && p2 <= 1) s->detect_count = 0;
            break;
        }
        case 'J': {
            WORD attr = build_attr(s);
            if (p1 == 0 || p1 == 2) {
                int sy = (p1 == 0) ? s->cursor_y : 0, sx = (p1 == 0) ? s->cursor_x : 0;
                for (int y = sy; y < s->rows; y++)
                    screen_erase_range(s, y, (y == sy ? sx : 0), s->cols - 1, attr);
            }
            if (p1 == 1) {
                for (int y = 0; y <= s->cursor_y; y++) {
                    int ex = (y == s->cursor_y) ? s->cursor_x : s->cols - 1;
                    screen_erase_range(s, y, 0, ex, attr);
                }
            }
            /* 只有 ED(3)（清滚动缓冲）才清历史；ED(2)（清显示）只清可见区、保留
             * 滚动历史（xterm 语义）。ConPTY 在窗格新建/重绘时会发 ED(2)，若把它
             * 也当清历史，分屏等场景本地 scrollback 会被整段冲掉（v1.8.52）。 */
            if (p1 == 3 && !s->in_alt_screen) {
                s->hist_lines = 0;
                int pi = s->pane_index;
                if (pi >= 0 && pi < MAX_PANES) g_mux.panes[pi].scroll_offset = 0;
            }
            /* v2.4.2：整屏清（ED 2/3）之后，恢复历史【不再参与实时视图】。
             * 之前 session_view 一旦置上就永不撤销，`cls` 只清掉了 ConPTY 的可见区，
             * 而实时视图是「历史 + 可见区」的重排网格 ⇒ 旧内容照旧铺满屏幕，用户
             * 看到的就是「cls 后光标回位了、内容没清」。清屏是明确的「从现在重新
             * 开始看」的意图：视图退回普通实时屏（空屏 + 光标），历史仍在回滚缓冲里，
             * 往上滚就能翻到 —— 与普通终端一致。ED(2) 不动 hist_lines，所以历史没丢。 */
            if ((p1 == 2 || p1 == 3) && !s->in_alt_screen) {
                s->session_view = 0;
                int pi2 = s->pane_index;
                if (pi2 >= 0 && pi2 < MAX_PANES) {
                    g_mux.panes[pi2].session_view = 0;
                    g_mux.panes[pi2].session_cursor_row = -1;
                }
            }
            /* conhost 实测：ED 与 EL/ECH 一样会清掉「延迟换行」标志（120 列写满后
             * 发 ESC[J 再写字符，字符落在末列而不是折到下一行）。 */
            s->wraparound_pending = 0;
            break;
        }
        case 'K': {
            WORD attr = build_attr(s);
            int sx = (p1 == 1 || p1 == 2) ? 0 : s->cursor_x;
            int ex = (p1 == 0 || p1 == 2) ? s->cols - 1 : s->cursor_x;
            if (s->cursor_x >= s->cols - 3 || s->wraparound_pending)
                vt_trace("EL   off=%lld x=%d y=%d pending=%d p1=%d", g_vt_off,
                         s->cursor_x, s->cursor_y, s->wraparound_pending, p1);
            screen_erase_range(s, s->cursor_y, sx, ex, attr);
            s->wraparound_pending = 0;   /* conhost 实测：EL 会清掉延迟换行 */
            break;
        }
        case 'L': screen_scroll_down(s, s->cursor_y, s->scroll_region_bottom, p1 ? p1 : 1); break;
        case 'M': screen_scroll_up(s, s->cursor_y, s->scroll_region_bottom, p1 ? p1 : 1); break;
        case 'P': {
            int n = p1 ? p1 : 1;
            for (int x = s->cursor_x; x < s->cols; x++) {
                CHAR_INFO *d = screen_cell(s, s->cursor_y, x), *sr = screen_cell(s, s->cursor_y, x + n);
                if (d) { if (sr) *d = *sr; else screen_write_cell(s, s->cursor_y, x, L' ', build_attr(s)); }
            }
            break;
        }
        case '@': {
            int n = p1 ? p1 : 1; WORD attr = build_attr(s);
            for (int x = s->cols - 1; x >= s->cursor_x + n; x--) { CHAR_INFO *d = screen_cell(s, s->cursor_y, x), *sr = screen_cell(s, s->cursor_y, x - n); if (d && sr) *d = *sr; }
            for (int x = s->cursor_x; x < s->cursor_x + n && x < s->cols; x++) screen_write_cell(s, s->cursor_y, x, L' ', attr);
            break;
        }
        case 'X': {
            int n = p1 ? p1 : 1; WORD attr = build_attr(s);
            if (s->cursor_x >= s->cols - 3 || s->wraparound_pending)
                vt_trace("ECH  off=%lld x=%d y=%d pending=%d n=%d", g_vt_off,
                         s->cursor_x, s->cursor_y, s->wraparound_pending, n);
            screen_erase_range(s, s->cursor_y, s->cursor_x, s->cursor_x + n - 1, attr);
            s->wraparound_pending = 0;   /* conhost 实测：ECH 也会清 */
            break;
        }
        case 'S': screen_scroll_up(s, s->scroll_region_top, s->scroll_region_bottom, p1 ? p1 : 1); break;
        case 'T': screen_scroll_down(s, s->scroll_region_top, s->scroll_region_bottom, p1 ? p1 : 1); break;
        case 'd': { s->cursor_y = (p1 ? p1 : 1) - 1; if (s->cursor_y >= s->rows) s->cursor_y = s->rows - 1; if (s->cursor_y < 0) s->cursor_y = 0; s->wraparound_pending = 0; break; }
        case 'm': process_sgr(s, params, pc); break;
        case 'r': {
            int top = p1 ? p1 : 1, bot = p2 ? p2 : s->rows;
            s->scroll_region_top = top - 1; s->scroll_region_bottom = bot - 1;
            if (s->scroll_region_top < 0) s->scroll_region_top = 0;
            if (s->scroll_region_bottom >= s->rows) s->scroll_region_bottom = s->rows - 1;
            if (s->scroll_region_top >= s->scroll_region_bottom) { s->scroll_region_top = 0; s->scroll_region_bottom = s->rows - 1; }
            s->cursor_x = 0; s->cursor_y = s->origin_mode ? s->scroll_region_top : 0;
            s->wraparound_pending = 0; break;
        }
        case 's': s->saved_cx = s->cursor_x; s->saved_cy = s->cursor_y; break;
        case 'u': s->cursor_x = s->saved_cx; s->cursor_y = s->saved_cy; s->wraparound_pending = 0; break;
        case 'n': if (p1 == 5) screen_send_response(s, "\x1b[0n"); else if (p1 == 6) { char r[32]; snprintf(r, sizeof(r), "\x1b[%d;%dR", s->cursor_y + 1, s->cursor_x + 1); screen_send_response(s, r); } break;
        case 'c': screen_send_response(s, "\x1b[?62;c"); break;
        case 't':
            if (p1 == 18) { char r[32]; snprintf(r, sizeof(r), "\x1b[8;%d;%dt", s->rows, s->cols); screen_send_response(s, r); }
            else if (p1 == 8 && pc >= 3 && p2 > 0 && params[2] > 0) {
                int nr = p2, nc = params[2];
                if (nr >= 2 && nr <= 500 && nc >= 2 && nc <= 1000)
                    screen_resize(s, nc, nr);
            }
            break;
        case 'g': if (p1 == 0 && s->cursor_x < 512) s->tab_stops[s->cursor_x] = 0; else if (p1 == 3) memset(s->tab_stops, 0, sizeof(s->tab_stops)); break;
        case 'h':
            for (int i = 0; i < pc; i++) {
                if (params[i] == 47 || params[i] == 1047 || params[i] == 1049) {
                    if (!s->in_alt_screen) {
                        s->saved_cx = s->cursor_x; s->saved_cy = s->cursor_y;
                        s->in_alt_screen = 1; s->alt_scroll_top = s->scroll_top;
                        s->alt_hist_lines = s->hist_lines;
                    }
                    for (int j = 0; j < s->rows * s->cols; j++) {
                        s->alt_buffer[j].Char.UnicodeChar = L' '; s->alt_buffer[j].Attributes = s->current_attr;
                        if (s->alt_fg_rgb) { s->alt_fg_rgb[j] = RGB565_WHITE; s->alt_bg_rgb[j] = RGB565_BLACK; s->alt_rgb_valid[j] = 0; }
                    }
                    if (params[i] == 1049) s->cursor_x = s->cursor_y = 0;
                    int pi = s->pane_index;
                    if (pi >= 0 && pi < MAX_PANES && g_mux.panes[pi].active) {
                        g_mux.panes[pi].scroll_offset = 0;
                    }
                }
            }
            break;
        case 'l':
            for (int i = 0; i < pc; i++) {
                if (params[i] == 47 || params[i] == 1047 || params[i] == 1049) {
                    if (s->in_alt_screen) {
                        s->in_alt_screen = 0; s->scroll_top = s->alt_scroll_top;
                        s->hist_lines = s->alt_hist_lines;
                        if (params[i] == 1049) { s->cursor_x = s->saved_cx; s->cursor_y = s->saved_cy; }
                        int pi2 = s->pane_index;
                        if (pi2 >= 0 && pi2 < MAX_PANES && g_mux.panes[pi2].active) {
                            if (g_mux.panes[pi2].scroll_offset > s->hist_lines) g_mux.panes[pi2].scroll_offset = s->hist_lines;
                        }
                    }
                }
            }
            break;
    }
}

/* LF 必须始终按终端字节流行进。CR 与 LF 之间允许夹 OSC 标题，因此
 * `CR OSC LF` 仍是真实换行；连续的另一个 `OSC LF` 则明确产生空行。
 * 不能因为光标已在底部空行就吸收裸 LF，否则长命令输出结束时
 * `LINE-80, blank, prompt` 会错误地压成 `LINE-80, prompt`。
 *
 * ⚠️ 别再把「裸 LF 吸收」加回来。cmd 命令结束后那个空行有 4 种真实字节形态
 * （verify_wide_wrap.py 已逐条固化）：
 *   A  ESC]0;标题 BEL + LF      ← 无 CR
 *   B  LF                       ← 无 CR
 *   C  CR LF CR LF
 *   D  CR + ESC]0;标题 BEL + LF
 * A / B 是【裸 LF】，任何「裸 LF 且底行空白就吸收」的规则都会把它们吃掉，真机
 * 症状就是「命令结束后少了一个空行」。cr_pending 那条规则（ESC 不打断 CR/LF
 * 配对）只救得了 D，救不了 A / B。幻影空行的正主是 ConPTY 的整屏重绘，已由
 * repaint_active + screen_scroll_viewport_up 处理，不需要靠猜裸 LF。 */
static void screen_lf(ScreenBuffer *s, int real_newline) {
    (void)real_newline;
    vt_trace("LF   off=%lld x=%d y=%d pending=%d cols=%d rows=%d",
             g_vt_off, s->cursor_x, s->cursor_y, s->wraparound_pending, s->cols, s->rows);
    /* 形态 (2)：CUP 刚把光标收到【本行末列且本行是屏幕底行】，那么这个 LF 不是
     * 逻辑行结束，而是 conhost 为了继续写这条逻辑行而强制的一次滚动。 */
    int eol_cont = (s->cup_eol_row >= 0 && s->cup_eol_row == s->cursor_y);
    s->cup_eol_row = -1;
    if (s->cursor_y >= s->scroll_region_bottom) {
        /* 整屏重绘的 CRLF 只是按行遍历 viewport。若在底边产生普通滚动，
         * 每次拖动分隔线都会把重绘尾部空行塞入历史，并最终挤掉下方 pane 的
         * LINE-1..80。真实程序输出不处于 repaint_active，仍照常滚动。 */
        if (s->repaint_active) {
            /* 重绘仍须像真实 viewport 一样上移，否则超出当前高度的序列会全部
             * 覆盖最后一行；区别仅在于不得把被移出的行追加到 scrollback。 */
            screen_scroll_viewport_up(s, 1);
            return;
        }
        screen_scroll_up(s, s->scroll_region_top, s->scroll_region_bottom, 1);
        /* 滚出来的新底行是上一行的【软换行续行】。前提：上一行确实被写满 —— 写满
         * 才可能是折行；没写满就说明那个 CUP 只是碰巧停在右下角，LF 是真硬换行。
         * 只在整屏滚动分支成立：部分滚动不旋转环形缓冲，行号语义不同。
         * 不加这条会怎样（本地实测，tests/fixtures_pane0_stream_v2.bin，24 列）：
         * bin / Ext2Fsd / Vape 三条记录的 <DIR> 半截与名字半截分家，拖宽后名字
         * 独占一行、前面挂着一串空格 —— 用户报的「多余空格」。 */
        if (eol_cont && !s->in_alt_screen && s->line_wrap && s->cursor_y > 0 &&
            s->scroll_region_top == 0 && s->scroll_region_bottom == s->rows - 1) {
            int pr = screen_phys_row(s, s->cursor_y - 1);
            if (pr >= 0 && pr < s->total_lines && s->lines && s->lines[pr].cells &&
                s->lines[pr].used >= s->cols)
                screen_mark_softwrap(s);
        }
    } else if (s->cursor_y < s->rows - 1) {
        s->cursor_y++;
        /* 硬换行落到一个【已有旧内容】的行上时，必须清掉它残留的软换行标记。
         *
         * 这一行可能上一轮输出里是自动折行的续行（line_wrap=1）。LF 是硬换行，
         * 落到这里的内容是一条新的逻辑行，绝不是上一行的续行；标记不清就会让
         * 之后的 reflow 把两行并成一行。
         *
         * 真机触发路径（2026-09-15 那批日志）：拖动分屏 -> ConPTY 用 ESC[H 整屏
         * 重绘 -> 重绘的 CRLF 落在上一轮残留的续行上 -> line_wrap 仍是 1 ->
         * 松手 reflow 把「1,593 termux_dump.log」和下一条「2025/12/23 … test.bat」
         * 并成一行（cell_diag F110..F220 第一行恒为「…docx.txt2026/0…」）。
         * 滚动分支不需要处理：screen_scroll_up 会把新底行填空并清标记。
         * 自动折行也不受影响：那条路径走 screen_newline + screen_mark_softwrap，
         * 不经过本函数，且在本函数之后才置 1。 */
        if (s->line_wrap && !s->in_alt_screen) {
            int pr = screen_phys_row(s, s->cursor_y);
            if (pr >= 0 && pr < s->total_lines) s->line_wrap[pr] = 0;
        }
    }
}

static inline int is_param_byte(unsigned char c) { return c >= 0x30 && c <= 0x3F; }
static inline int is_inter_byte(unsigned char c) { return c >= 0x20 && c <= 0x2F; }
static inline int is_final_byte(unsigned char c) { return c >= 0x40 && c <= 0x7E; }
static inline int is_c0(unsigned char c) { return c < 0x20 || c == 0x7F; }

static void screen_process_byte(ScreenBuffer *s, unsigned char c) {
    if (c == 0x18 || c == 0x1A) { s->state = ST_NORMAL; s->cr_pending = 0; return; }
    if (c == 0x1B) {
        s->state = ST_ESC;
        s->param_len = 0;
        s->inter_len = 0;
        /* 不清 cr_pending：ConPTY/cmd 经常输出 CR + OSC(窗口标题) + LF。
         * OSC 是带外元数据，不应打断逻辑 CRLF；否则这个 LF 会被当成裸 LF，
         * 光标位于底部空行时被 screen_lf() 吸收，最后一个真实空行消失，直到
         * resize 的整屏重绘才重新出现。 */
        return;
    }

    switch (s->state) {
        case ST_NORMAL:
            if (c < 0x20) {
                /* CR 不打断（它与随后的 LF 是一对）；LF/VT/FF 由 screen_lf 消费；
                 * 其余 C0 一律作废「底行末列 CUP」的判定窗口。 */
                if (c != 0x0A && c != 0x0B && c != 0x0C && c != 0x0D) s->cup_eol_row = -1;
                /* 光标移动/换行类 C0：右缘那个可疑空格到此确认为排版填充。BEL 不动光标，
                 * 不作结论（真实内容里空格后面跟一声提示音是可能的）。 */
                if (c != 0x07 && c != 0x0E && c != 0x0F) vt_pad_space_undo(s);
                switch (c) {
                    case 0x07: break;
                    case 0x08: if (s->cursor_x > 0) s->cursor_x--; s->wraparound_pending = 0; break;
                    case 0x09: { int x = s->cursor_x + 1; while (x < s->cols && x < 512 && !s->tab_stops[x]) x++; s->cursor_x = (x < s->cols) ? x : s->cols - 1; s->wraparound_pending = 0; } break;
                    case 0x0A: s->wraparound_pending = 0; screen_lf(s, s->cr_pending); s->cr_pending = 0; break;
                    case 0x0B: case 0x0C: s->wraparound_pending = 0; screen_lf(s, 0); break;
                    case 0x0D: s->cursor_x = 0; s->wraparound_pending = 0; s->cr_pending = 1; break;
                    case 0x0E: break;
                    case 0x0F: break;
                }
            } else if (c == 0x7F) {
                // DEL - ignore
            } else {
                screen_put_cp(s, (unsigned char)c);
            }
            break;

        case ST_ESC:
            if (is_inter_byte(c)) {
                if (s->inter_len < 15) s->inter_buf[s->inter_len++] = c;
                s->state = ST_ESC_INTER;
            } else if (c >= 0x30 && c <= 0x7E) {
                if (c == '[') { s->state = ST_CSI_ENTRY; s->param_len = 0; s->inter_len = 0; }
                else if (c == ']') { s->state = ST_OSC_STRING; s->osc_num = -1; s->osc_len = 0; s->osc_sep = 0; }
                else if (c == 'P') { s->state = ST_DCS_ENTRY; s->param_len = 0; s->inter_len = 0; }
                else if (c == 'X' || c == '^' || c == '_') { s->state = ST_SOS_STRING; }
                else { execute_esc(s, c, s->inter_buf, s->inter_len); s->state = ST_NORMAL; }
            } else if (is_c0(c)) {
                s->state = ST_NORMAL;
                screen_process_byte(s, c);
            } else {
                s->state = ST_NORMAL;
            }
            break;

        case ST_ESC_INTER:
            if (is_inter_byte(c)) {
                if (s->inter_len < 15) s->inter_buf[s->inter_len++] = c;
            } else if (c >= 0x30 && c <= 0x7E) {
                execute_esc(s, c, s->inter_buf, s->inter_len);
                s->state = ST_NORMAL;
            } else {
                s->state = ST_NORMAL;
            }
            break;

        case ST_CSI_ENTRY:
            if (is_param_byte(c)) {
                if (s->param_len < 255) s->param_buf[s->param_len++] = c;
                s->state = ST_CSI_PARAM;
            } else if (is_inter_byte(c)) {
                if (s->inter_len < 15) s->inter_buf[s->inter_len++] = c;
                s->state = ST_CSI_INTER;
            } else if (is_final_byte(c)) {
                execute_csi_internal(s, c, 0, s->param_buf, s->param_len, s->inter_buf, s->inter_len);
                s->state = ST_NORMAL;
            } else if (is_c0(c)) {
            } else {
                s->state = ST_CSI_IGNORE;
            }
            break;

        case ST_CSI_PARAM:
            if (is_param_byte(c)) {
                if (s->param_len < 255) s->param_buf[s->param_len++] = c;
            } else if (is_inter_byte(c)) {
                if (s->inter_len < 15) s->inter_buf[s->inter_len++] = c;
                s->state = ST_CSI_INTER;
            } else if (is_final_byte(c)) {
                char prefix = 0;
                if (s->param_len > 0 && (s->param_buf[0] == '?' || s->param_buf[0] == '>' || s->param_buf[0] == '=' || s->param_buf[0] == '<' || s->param_buf[0] == '!')) {
                    prefix = s->param_buf[0];
                }
                execute_csi_internal(s, c, prefix, s->param_buf, s->param_len, s->inter_buf, s->inter_len);
                s->state = ST_NORMAL;
            } else if (is_c0(c)) {
            } else {
                s->state = ST_CSI_IGNORE;
            }
            break;

        case ST_CSI_INTER:
            if (is_inter_byte(c)) {
                if (s->inter_len < 15) s->inter_buf[s->inter_len++] = c;
            } else if (is_final_byte(c)) {
                char prefix = 0;
                if (s->param_len > 0 && (s->param_buf[0] == '?' || s->param_buf[0] == '>' || s->param_buf[0] == '=')) prefix = s->param_buf[0];
                execute_csi_internal(s, c, prefix, s->param_buf, s->param_len, s->inter_buf, s->inter_len);
                s->state = ST_NORMAL;
            } else {
                s->state = ST_CSI_IGNORE;
            }
            break;

        case ST_CSI_IGNORE:
            if (is_final_byte(c)) s->state = ST_NORMAL;
            break;

        case ST_OSC_STRING:
            if (c == 0x07) {
                execute_osc(s);
                s->state = ST_NORMAL;
            } else if (c == 0x1B) {
                execute_osc(s);
                s->state = ST_ESC;
                s->param_len = 0;
                s->inter_len = 0;
            } else if (c >= 0x20 && c != 0x7F) {
                if (!s->osc_sep) {
                    if (c >= '0' && c <= '9') {
                        s->osc_num = (s->osc_num < 0 ? 0 : s->osc_num) * 10 + (c - '0');
                    } else if (c == ';') {
                        s->osc_sep = 1;
                    } else {
                        s->osc_sep = 1;
                    }
                } else {
                    if (s->osc_len < 511) s->osc_buf[s->osc_len++] = (char)c;
                }
            }
            break;

        case ST_DCS_ENTRY:
        case ST_DCS_PARAM:
        case ST_DCS_INTER:
        case ST_DCS_PASSTHROUGH:
        case ST_DCS_IGNORE:
        case ST_SOS_STRING:
            if (c == 0x07) s->state = ST_NORMAL;
            else if (c == 0x1B) { s->state = ST_ESC; s->param_len = 0; s->inter_len = 0; }
            break;
    }
}

/* ================= v2.3.6：图形协议直通（sixel / kitty / iTerm2）=================
 * 开关与上限的定义放在本文件（config.h 里 extern、ini 解析在 config.c）：状态机在这里，
 * 而 tests/cascade_probe.c 那批 harness 只链 vt.c、不链 config.c —— 定义放 config.c
 * 就得每个 harness 自己补一份，早晚漏一个（漏了就是链接错，CI 才发现）。
 * 默认值是「生效」：与 session 同一口径，要静默得自己在 ini 里写 off。 */
int g_graphics_relay = 1;
int g_graphics_max_kb = 32768;   /* 32 MB：够 imgcat 塞进一张大 PNG，也挡住失控的 cat */

/* 逐字节挂在正常状态机【之前】跑一个小判定：认出这三种序列就整段原样收下，交给 render 在
 * 本帧末尾按宿主坐标转发。为什么要「整段或什么都没有」：半截 DCS-SetGraphicsAttributes 会
 * 把宿主的 sixel 解析器停在半张图上，之后每一行文字都被它当图像数据吃掉 —— 那是比「看不到
 * 图片」糟得多的故障，所以中途出错、超上限、结尾不是 ST/BEL 的，一律丢掉不发。
 * 状态号（每种都必须在 switch 里有分支 —— 跳进一个不存在的 case 会被 default 清态，
 * 表现是「这条协议整个静默消失」，本项目第一版就这么栽过一次）：
 *   0 空闲 / 1 刚见 ESC / 2 DCS 参数段（还没到 'q'）/ 3 载荷采集中（sixel 与 kitty 共用，
 *   因为两者都以 ST/BEL 收尾）/ 4 APC 前缀（等 'G'）/ 6 OSC 编号 / 7 OSC 载荷
 *   （只有 1337 且载荷里出现 File= 才走到 7 并保留）。*/
static int gfx_put(ScreenBuffer *s, unsigned char c) {
    int need = s->gfx_cur_len + 1;
    if (need > s->gfx_cap * 1024) { s->gfx_oversize = 1; return 0; }
    if (need > s->gfx_cur_cap) {
        int cap = s->gfx_cur_cap > 0 ? s->gfx_cur_cap : 4096;
        while (cap < need) cap *= 2;
        char *nb = (char *)realloc(s->gfx_cur, (size_t)cap);
        if (!nb) { s->gfx_oversize = 1; return 0; }
        s->gfx_cur = nb; s->gfx_cur_cap = cap;
    }
    s->gfx_cur[s->gfx_cur_len++] = (char)c;
    return 1;
}

static void gfx_qpush(ScreenBuffer *s) {
    if (s->gfx_cur_len <= 0 || s->gfx_oversize) return;
    if (s->gfx_qn >= GFX_RELAY_QUEUE) return;      /* 队列满：丢弃最新的（不阻塞、不崩） */
    struct TermuxGfxItem *it = &s->gfx_q[s->gfx_qn];
    it->p = (char *)malloc((size_t)s->gfx_cur_len);
    if (!it->p) return;
    memcpy(it->p, s->gfx_cur, (size_t)s->gfx_cur_len);
    it->len = s->gfx_cur_len;
    it->hist = s->hist_lines;
    it->y = s->cursor_y; it->x = s->cursor_x;
    s->gfx_qn++;
}

static void gfx_done(ScreenBuffer *s) {         /* 收尾：入队 + 复位 */
    gfx_qpush(s);
    s->gfx_cur_len = 0;
    s->gfx_state = 0; s->gfx_escpend = 0; s->gfx_sawfile = 0; s->gfx_oversize = 0;
}

static void gfx_abort(ScreenBuffer *s) {        /* 不完整/认不出：整段丢掉 */
    s->gfx_cur_len = 0;
    s->gfx_state = 0; s->gfx_escpend = 0; s->gfx_sawfile = 0; s->gfx_oversize = 0;
}

/* iTerm2 的载荷以 base64 为主，`File=` 出现在参数段里；只要认到它就整段收。 */
/* 锚定匹配：iTerm2 的图片只有「载荷以 File= 开头」这一种写法。原来允许在整段
 * base64 里任意位置找 "file="，于是 `1337;NotFile=1` 之类也会被当成图片转发；
 * 一旦某个字符不匹配就把计数打成负数（粘住），收尾时按 <5 丢掉。 */
static void gfx_scan_osc1337(ScreenBuffer *s, unsigned char c) {
    /* 模式必须写成小写：下面把输入字符统一 tolower 再比（大小写不敏感）。
     * 原来这里写的是 "File="，与小写化的 lc 永远配不上 —— 靠「4KB 内没认到才丢」
     * 兜底才没出事；改成锚定判定后这条必须真配对，否则 BEL 收尾的图整条被判不是图片。 */
    static const char pat[] = "file=";      /* 认到 5 就够，别把下标写出界 */
    if (s->gfx_sawfile >= 5 || s->gfx_sawfile < 0) return;
    char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    if (lc == pat[s->gfx_sawfile]) s->gfx_sawfile++;
    else s->gfx_sawfile = -1;               /* 第 5 个字符之后不再看 */
}

static void gfx_tap(ScreenBuffer *s, unsigned char c) {
    if (!g_graphics_relay) { if (s->gfx_state) gfx_abort(s); return; }
    switch (s->gfx_state) {
        case 0:
            if (c == 0x1B) {
                /* 每次起头都重新读一遍上限：设置页里改 graphics_max（或另一路 ini）
                 * 下一张图就生效，不必重启。gfx_cap 单位是 KB，与 ini 同口径。 */
                s->gfx_cap = g_graphics_max_kb > 0 ? g_graphics_max_kb : 32768;
                s->gfx_state = 1; s->gfx_cur_len = 0; s->gfx_oversize = 0; s->gfx_sawfile = 0;
            }
            break;
        case 1:                                  /* ESC 之后看首字节分三种 */
            if (c == 'P') { gfx_put(s, 0x1B); gfx_put(s, c); s->gfx_state = 2; }
            else if (c == '_') { s->gfx_state = 4; s->gfx_escpend = 0; gfx_put(s, 0x1B); gfx_put(s, c); }
            else if (c == ']') { s->gfx_state = 6; s->gfx_sawfile = 0; s->gfx_oscnum = 0; gfx_put(s, 0x1B); gfx_put(s, c); }
            else s->gfx_state = 0;
            break;
        case 6:                                  /* OSC 的编号（十进制，到 ';' 为止） */
            if (c >= '0' && c <= '9') {
                if (s->gfx_oscnum <= 100000) s->gfx_oscnum = s->gfx_oscnum * 10 + (c - '0');
                gfx_put(s, c);
            } else if (c == ';' && s->gfx_oscnum == 1337) {
                gfx_put(s, c); s->gfx_state = 7; s->gfx_sawfile = 0;
            } else { gfx_abort(s); s->gfx_state = 0; }
            break;
        case 7:                                  /* iTerm2 载荷：认 File=，认结尾 */
            if (s->gfx_escpend) {
                s->gfx_escpend = 0;
                /* 收尾这两个字节必须一起进缓冲：漏掉 '\\' 就等于把一条没有 ST 的 OSC
                 * 发给宿主 —— 宿主的解析器会停在半截 OSC 上，之后整屏文字都被它当
                 * 载荷吃掉。单测里「iTerm2 ST 收尾原样入队」红过一次就是这么来的。 */
                if (c == '\\') { gfx_put(s, c); gfx_done(s); }
                else gfx_abort(s);
                break;
            }
            if (c == 0x07) {
                if (s->gfx_sawfile >= 5) { gfx_put(s, c); gfx_done(s); }
                else gfx_abort(s);               /* 载荷开头不是 File= ⇒ 这不是图片，整条丢掉 */
                break;
            }
            if (c == 0x1B) { s->gfx_escpend = 1; gfx_put(s, c); break; }
            gfx_scan_osc1337(s, c);
            gfx_put(s, c);
            break;
        case 4:                                  /* APC：kitty 是 ESC _ G ...，其它 APC 不理 */
            /* 状态号在 switch 里必须有对应分支：早先这里跳到 8，而下面根本没有
             * case 8 —— 于是走 default、状态清零，kitty 整条静默消失（探针里
             * sixel 与 iTerm2 都通、只有 kitty 不通，就是这个原因）。*/
            if (c == 'G') { gfx_put(s, c); s->gfx_state = 3; s->gfx_sawfile = 0; }
            else gfx_abort(s);
            break;
        case 2:                                  /* DCS 参数段：只有以 'q' 收尾的才是 sixel */
            gfx_put(s, c);
            if (c == 'q') s->gfx_state = 3;
            else if (c >= 0x40 && c <= 0x7E) gfx_abort(s);   /* 别的 DCS（DECRQSS 之类）*/
            break;
        case 3:                                  /* 载荷段：sixel(DCS …q) 与 kitty(APC …G) 共用 */
            if (s->gfx_escpend) {
                s->gfx_escpend = 0;
                if (c == '\\') { gfx_put(s, c); gfx_done(s); }
                else gfx_abort(s);               /* 采集途中出现别的 ESC ⇒ 认不准，整段丢 */
                break;
            }
            if (c == 0x1B) { s->gfx_escpend = 1; gfx_put(s, c); break; }
            /* sixel 与 kitty 都允许 BEL 收尾（ST 的两种写法，规范上等价）*/
            if (c == 0x07) { gfx_put(s, c); gfx_done(s); break; }
            gfx_put(s, c);
            break;
        default: s->gfx_state = 0; break;
    }
}

void screen_process_output(ScreenBuffer *s, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        g_vt_off++;

        gfx_tap(s, c);        /* v2.3.6：图形协议直通的小判定（与状态机并行，互不影响） */

        /* CRLF 逻辑配对：普通文本会打断 CR；ESC 序列不打断，因为 ConPTY/cmd
         * 会把窗口标题 OSC 插在 CR 与 LF 之间（CR OSC LF 仍是一条真实换行）。
         * 若把 ESC 当普通字节清掉 cr_pending，底部的 LF 会被误判为留位裸 LF，
         * 真实空行在实时缓冲中消失，resize 重绘后又出现。 */
        if (s->state == ST_NORMAL) {
            if (c == '\r') s->cr_pending = 1;
            else if (c != '\n' && c != 0x1B) s->cr_pending = 0;
        }

        if (s->state == ST_NORMAL) {
            if (c >= 0xC0 && c < 0xFE) {
                if ((c & 0xE0) == 0xC0) {
                    if (c < 0xC2) continue;
                    s->utf8_cp = c & 0x1F; s->utf8_state = 1; continue;
                }
                else if ((c & 0xF0) == 0xE0) { s->utf8_cp = c & 0x0F; s->utf8_state = 2; continue; }
                else if ((c & 0xF8) == 0xF0) { s->utf8_cp = c & 0x07; s->utf8_state = 3; continue; }
                else continue;
            } else if (s->utf8_state == 0 && c >= 0xA0 && c < 0xC0) {
                continue;
            }
        }
        if (s->utf8_state > 0) {
            if ((c & 0xC0) == 0x80) {
                s->utf8_cp = (s->utf8_cp << 6) | (c & 0x3F);
                if (--s->utf8_state == 0) {
                    if (s->state == ST_NORMAL)
                        screen_put_cp(s, s->utf8_cp);
                }
                continue;
            } else {
                s->utf8_state = 0;
            }
        }

        screen_process_byte(s, c);
    }
}
