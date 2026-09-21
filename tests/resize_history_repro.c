/*
 * tests/resize_history_repro.c  (Linux, gcc -Itests/stub -Iinclude)
 *
 * 「子终端(分屏窗格) resize 时记录历史记录」回归。
 *
 * 背景：src/screen.c 的 screen_resize_reflow() 会把「历史 + 可见」合并成逻辑行、
 * 按新宽度重折，再【按绝对布局】重建物理环：可见区 = 物理槽 0..nr-1，历史 =
 * 物理槽 nt-hist..nt-1。这个布局要求环头 scroll_top == 0（screen_phys_row()
 * 用 (scroll_top+rel)%total_lines 定位，渲染/复制/搜索/reflow 全部走它）。
 *
 * 下面 6 组断言都是「按应用真实读法」检查（screen_phys_row / screen_cell）：
 *
 *   1) resize 后 scroll_top 必须为 0，否则历史与可见屏整体错位 scroll_top 行
 *      —— 最上面若干行从历史里消失、底部多出幻影空行、后续输出写进错误的物理槽。
 *   2) 内容不足一屏时必须从顶部排（base=0）；底部锚定会让内容整体下坠、上方垫空行。
 *   3) 纯增高（nc==cols && nr>rows）不得往时间流【中间】插空行，否则每拖高 k 行
 *      就往滚动历史里永久塞 k 个空行，并抬高 scroll_limit。
 *   4) reflow 之后 cursor_y 必须跟着内容走；否则 resize 后第一行新输出会覆盖
 *      已有内容（实测把 LINE-08 覆盖成 LINE-13）。
 *
 * 编译运行：
 *   gcc -O1 -g -Itests/stub -Iinclude src/screen.c src/vt.c src/utf8.c \
 *       tests/resize_history_repro.c -o /tmp/resize_repro && /tmp/resize_repro
 */
#include "screen.h"
#include "vt.h"
#include "types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int g_scrollback_lines = 100;
MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0, g_search_match_cur = -1, g_search_active = 0;

static int g_fail = 0;
static void ck(const char *name, int ok, const char *detail) {
    if (!ok) g_fail = 1;
    printf("[%s] %s%s%s\n", ok ? "ok" : "FAIL", name,
           detail && detail[0] ? "  -> " : "", detail ? detail : "");
}

static void feed(ScreenBuffer *s, const char *t) {
    screen_process_output(s, t, (int)strlen(t));
}

/* 真机管线（src/pane.c:96,104）：整屏重绘块先 screen_repaint_align 再喂解析器。 */
static void feed_repaint(ScreenBuffer *s, const char *t, int n) {
    screen_repaint_align(s, t, n);
    screen_process_output(s, t, n);
}

/* 应用真实读法：历史 = screen_phys_row(rel<0)，可见 = screen_phys_row(0..rows-1)。 */
static void rowtext(ScreenBuffer *s, int rel, char *out) {
    int pr = screen_phys_row(s, rel);
    int hp = 0;
    out[0] = 0;
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) {
        strcpy(out, "<noline>");
        return;
    }
    ScreenLine *ln = &s->lines[pr];
    for (int x = 0; x < ln->len && hp < 80; x++) {
        WCHAR c = ln->cells[x].Char.UnicodeChar;
        if (c == 0) continue;                     /* 宽字符次格占位 */
        else if (c == L' ') out[hp++] = ' ';
        else if (c < 128) out[hp++] = (char)c;
        else out[hp++] = '?';
    }
    while (hp && out[hp - 1] == ' ') hp--;
    out[hp] = 0;
}

/* 把 hist+visible 拼成一行摘要：内容行的文本序列 + 【内容之间】的空行数
 * （首条内容之上 / 末条内容之下的空白不算——屏幕比内容高时那是正常留白）。 */
static void summarize(ScreenBuffer *s, char *out, int cap, int *blanks) {
    int pos = 0;
    int seen_content = 0, pend = 0;
    *blanks = 0;
    out[0] = 0;
    for (int rel = -s->hist_lines; rel < s->rows; rel++) {
        char t[96];
        rowtext(s, rel, t);
        if (!t[0] || !strcmp(t, "<noline>")) { if (seen_content) pend++; continue; }
        if (seen_content) { *blanks += pend; }
        pend = 0;
        seen_content = 1;
        int n = (int)strlen(t);
        if (pos + n + 2 >= cap) break;
        if (pos) out[pos++] = ',';
        memcpy(out + pos, t, (size_t)n);
        pos += n;
        out[pos] = 0;
    }
}

static void fill_lines(ScreenBuffer *s, int from, int to) {
    char b[64];
    for (int i = from; i <= to; i++) {
        snprintf(b, sizeof b, "LINE-%02d\r\n", i);
        feed(s, b);
    }
}

int main(void) {
    memset(&g_mux, 0, sizeof g_mux);

    /* ---- 1) resize 后环头必须归零（否则历史/可见整体错位） ---- */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 20, 10);
        fill_lines(&s, 1, 12);
        char before[2048];
        int bb = 0;
        summarize(&s, before, sizeof before, &bb);
        screen_resize(&s, 20, 8);                 /* 拖分屏横条：10 行 -> 8 行 */
        char after[2048];
        int ab = 0;
        summarize(&s, after, sizeof after, &ab);
        char det[4200];
        snprintf(det, sizeof det, "scroll_top=%d hist=%d；resize 前[%s] resize 后[%s]",
                 s.scroll_top, s.hist_lines, before, after);
        ck("resize(20x10->20x8) 后 scroll_top==0", s.scroll_top == 0, det);
        ck("resize 后历史+可见内容逐字不变(不丢 LINE-01)",
           strcmp(before, after) == 0, det);
        ck("resize 后无幻影空行(内容之间不夹空白)", ab == 0, det);
        (void)bb;
        screen_free(&s);
    }

    /* ---- 2) 收窄后内容不丢、且按「有历史 -> 底部锚定」落位 ----
     * 80 列满行滚进历史后收窄到 20：这条逻辑行按新宽折成 4 行，内容逐字不变；
     * 因为有历史，整块内容底部锚定（光标落在最后一行），提示符下方不留空行。 */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 80, 10);
        for (int x = 0; x < 80; x++)
            screen_write_cell(&s, 0, x, (WCHAR)('A' + (x % 26)), 0x07);
        screen_scroll_up(&s, 0, s.rows - 1, 1);   /* 满行进历史 */
        screen_resize(&s, 20, 10);                /* 左右分屏拖窄：80 -> 20 */
        /* 内容逐字保留：把可见区所有非空格拼起来应等于 A..Z 循环 80 字符 */
        char joined[256];
        int jp = 0;
        joined[0] = 0;
        for (int y = 0; y < s.rows && jp < 200; y++) {
            char t[96];
            rowtext(&s, y, t);
            for (int k = 0; t[k] && jp < 200; k++) if (t[k] != ' ') joined[jp++] = t[k];
        }
        joined[jp] = 0;
        char want[96];
        for (int x = 0; x < 80; x++) want[x] = (char)('A' + (x % 26));
        want[80] = 0;
        char det[512];
        snprintf(det, sizeof det, "cursor_y=%d/%d 内容长度=%d(应80)", s.cursor_y, s.rows - 1, jp);
        ck("收窄 80->20 后 80 字符逐字保留", strcmp(joined, want) == 0, det);
        /* 重折后 80 字符占 4 个显示行、不足一屏 ⇒ hist 归 0 ⇒ 顶对齐（和 ConPTY
         * 的整屏重绘一致）；提示符贴底那条不变量只在内容超过一屏时成立。 */
        {
            char t0[96];
            rowtext(&s, 0, t0);
            ck("收窄 80->20 后内容从 rel0 起（不足一屏 -> 顶对齐）",
               s.hist_lines == 0 && t0[0] != 0 && strcmp(t0, "<noline>") != 0, det);
        }
        screen_free(&s);
    }

    /* ---- 3) 纯增高不得往时间流中间插空行 ---- */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 20, 10);
        fill_lines(&s, 1, 12);
        screen_resize(&s, 20, 14);                /* 上下分屏拖高：10 -> 14 */
        char seq[2048];
        int blanks = 0;
        summarize(&s, seq, sizeof seq, &blanks);
        int ok = 1;
        for (int i = 1; i <= 12; i++) {           /* 内容必须连续、无空行插入 */
            char want[16];
            snprintf(want, sizeof want, "LINE-%02d", i);
            if (!strstr(seq, want)) { ok = 0; break; }
        }
        char det[2200];
        snprintf(det, sizeof det, "内容间空行=%d；序列[%s]", blanks, seq);
        ck("纯增高 10->14 不在内容中间插入空行", ok && blanks == 0, det);
        screen_free(&s);
    }

    /* ---- 4) resize 后光标必须跟着内容走，新输出不得覆盖已有行 ---- */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 20, 10);
        fill_lines(&s, 1, 12);
        screen_resize(&s, 20, 14);                /* 放大 */
        feed(&s, "LINE-13\r\n");
        char seq[2048];
        int blanks = 0;
        summarize(&s, seq, sizeof seq, &blanks);
        char det[2200];
        snprintf(det, sizeof det, "cursor_y=%d；序列[%s]", s.cursor_y, seq);
        ck("放大后新输出 LINE-13 不覆盖 LINE-08", strstr(seq, "LINE-08") != NULL, det);
        screen_free(&s);
    }

    /* ---- 5) 上下分屏改高度后 reflow 的落位 ----
     * 真机症状：拖上下分屏条后 `C:\\Users\\...>` 下方凭空多出很多空行。
     * 内容超过一屏（resize 后 hist>0）时提示符必须正好落在最后一行、下方没有任何
     * 东西；内容整屏放得下（hist==0）时顶对齐，和 ConPTY 重绘保持一致。 */
    {
        struct { const char *tag; int rows0, nlines, nr; } cs[] = {
            { "多历史 10行/25行输出 -> 14行", 10, 25, 14 },
            { "多历史 10行/25行输出 -> 6行",  10, 25, 6  },
            { "少历史 10行/12行输出 -> 14行", 10, 12, 14 },
            { "少历史 10行/12行输出 -> 20行", 10, 12, 20 },
            { "无历史 10行/ 3行输出 -> 14行", 10,  3, 14 },
            { "无历史 10行/ 3行输出 -> 6行",  10,  3, 6  },
        };
        for (unsigned ci = 0; ci < sizeof cs / sizeof cs[0]; ci++) {
            ScreenBuffer s;
            memset(&s, 0, sizeof s);
            screen_init(&s, 40, cs[ci].rows0);
            fill_lines(&s, 1, cs[ci].nlines);
            feed(&s, "C:\\Users\\wu_dr\\Downloads>");
            int hist_before = s.hist_lines;
            screen_resize(&s, 40, cs[ci].nr);
            int blanks_below = 0, below_content = 0, blanks_above = 0, seen = 0;
            for (int y = 0; y < s.rows; y++) {
                char t[96];
                rowtext(&s, y, t);
                int bl = (!t[0] || !strcmp(t, "<noline>"));
                if (bl && !seen) blanks_above++;
                if (!bl) seen = 1;
                if (y > s.cursor_y) { if (bl) blanks_below++; else below_content++; }
            }
            /* 落位规则（与 ConPTY 整屏重绘保持一致，见 screen_resize_reflow 的 base 注释）：
             *   resize 后 hist > 0（内容超过一屏）→ 可见区 = 最新 nr 行，提示符正好在
             *     最后一行、下方既无空行也无内容；
             *   resize 后 hist == 0（内容整屏放得下）→ 顶对齐，空白留在下方 —— conhost
             *     自己就是这样（增高时内容留在原处、只在下方补空行），两边必须一致，
             *     否则每次拖动都会看到内容在「底对齐的一帧」和「顶对齐的重绘」之间跳。 */
            int ok = (s.hist_lines > 0)
                   ? (s.cursor_y == s.rows - 1 && blanks_below == 0 && below_content == 0)
                   : (blanks_above == 0 && below_content == 0);
            (void)hist_before;
            char nm[128], det[192];
            snprintf(nm, sizeof nm, "改高度后提示符锚定: %s", cs[ci].tag);
            snprintf(det, sizeof det, "历史=%d cursor_y=%d/%d 上方空白=%d 下方空白=%d 下方有内容=%d",
                     hist_before, s.cursor_y, s.rows - 1, blanks_above, blanks_below, below_content);
            ck(nm, ok, det);
            screen_free(&s);
        }
    }

    /* ---- 6) resize 后 ConPTY 的整屏重绘：提示符必须重新贴底、历史不得被吃掉 ----
     * 真机 uploads/termux_dump.log 实测（用户拖上下分屏条时抓的 ConPTY 字节流）：
     * conhost 在窗口【增高】时不把滚动历史拉回来、只在下方补空行，所以它回给我们的
     * 整屏重绘只有「h 行内容 + (rows-h) 行空白」，末尾还把光标绝对定位到提示符那一
     * 行（真机是 ESC[11;26H，而窗格高 21 行）。照实画 ⇒ 提示符停在第 h 行、下面一片
     * 空行；更糟的是下一次 reflow 只扫到光标行为止（scan_end），那片空白不算内容，
     * 于是 hist = tcount - nr 每拖一次就少 (rows-h) 行 —— 真机日志里 hist 26 → 0。
     * 修法：重绘结束后把内容整体下移、让提示符落回最后一行，上面空出的行用重绘前的
     * 可见行补回（screen_repaint_snapshot / screen_repaint_reanchor）。 */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 120, 12);
        fill_lines(&s, 1, 30);                       /* 30 行输出，早已滚出历史 */
        feed(&s, "C:\\Users\\wu_dr\\Downloads>");
        int total0 = s.hist_lines + s.rows;          /* 内容总行数，全程不得变化 */
        int anchored = 1, conserved = 1, top_ok = 1;
        char det[256];
        det[0] = 0;
        for (int step = 0; step < 6; step++) {
            int nr = 14 + step * 2;                  /* 逐步拖高：14,16,18,20,22,24 */
            int h = 12;                              /* conhost 只发 12 行内容 */
            char b[4096];
            int n = 0;
            screen_resize(&s, 120, nr);
            /* 逐字节照真机结构：每行 "文本 ESC[K CRLF"，最后一行只有 ESC[K，
             * 然后 ESC[<提示符行>;26H 把光标绝对定位回去、ESC[?25h 结束重绘。 */
            n += snprintf(b + n, sizeof b - (size_t)n, "\x1b[?25l\x1b[H");
            for (int i = 0; i < h - 1; i++)
                n += snprintf(b + n, sizeof b - (size_t)n, "ROW-%02d\x1b[K\r\n", i);
            n += snprintf(b + n, sizeof b - (size_t)n,
                          "C:\\Users\\wu_dr\\Downloads>\x1b[K\r\n");
            for (int j = h; j < nr - 1; j++)         /* (nr-h) 行空白尾巴 */
                n += snprintf(b + n, sizeof b - (size_t)n, "\x1b[K\r\n");
            n += snprintf(b + n, sizeof b - (size_t)n, "\x1b[K\x1b[%d;26H\x1b[?25h", h);
            feed_repaint(&s, b, n);
            if (s.cursor_y != s.rows - 1) anchored = 0;
            if (s.hist_lines + s.rows != total0) conserved = 0;
            {   /* 顶部补回的必须是真的历史行，不能是空白 */
                char t[96];
                rowtext(&s, 0, t);
                if (!t[0] || !strcmp(t, "<noline>")) top_ok = 0;
            }
            snprintf(det, sizeof det, "末次: %dx%d hist=%d 内容总行=%d(应%d) cursor_y=%d/%d",
                     s.cols, s.rows, s.hist_lines, s.hist_lines + s.rows, total0,
                     s.cursor_y, s.rows - 1);
        }
        {   /* 提示符必须就在最后一行上 */
            char t[96];
            rowtext(&s, s.rows - 1, t);
            ck("重绘后提示符重新贴底（连续拖高 6 次）", anchored, det);
            ck("重绘后最后一行就是提示符", strstr(t, "Downloads>") != NULL, t);
            ck("重绘不吃历史（内容总行数恒定）", conserved, det);
            ck("顶部补回的是历史行而非空白", top_ok, det);
        }
        screen_free(&s);
    }

    /* ---- 7) 重绘不得转动环形缓冲：内容里有重复行时（真机「历史重复」bug） ----
     * 真机 uploads/termux_dump.log 实测（用户报「历史重复」）：dir 式输出里同名行
     * 反复出现（termux_dump.log 在可见区出现多次）。旧代码在喂解析器前先跑
     * screen_repaint_align()：拿重绘的第一条非空行去整个「历史+可见」里找同文本的行、
     * 跳过 rel==0 取 |rel| 最小者，然后 hist_lines += best_rel、scroll_top 前滚。
     * 有重复行时它必然认错位置 ⇒ 重绘落到错误偏移、写出新的重复行，下一次 align 又去
     * 匹配这条新重复行 —— 自我放大。真机重放：内容流 40 行变 56 行，termux_dump.log
     * 出现 13 次、test.bat 4 次，hist 被削短 7 行。 */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 120, 20);
        const char *names[] = { "termux_dump.log", "test.bat", "termux_dump.log",
                                "screen_resize_trace.log", "termux_dump.log", "test.bat" };
        for (int rep = 0; rep < 5; rep++)
            for (int i = 0; i < 6; i++) {
                char b[128];
                snprintf(b, sizeof b, "%s\r\n", names[i]);
                feed(&s, b);
            }
        feed(&s, "C:\\Users\\wu_dr\\Downloads>");
        int total0 = s.hist_lines + s.rows;
        char before[4096];
        int bb = 0;
        summarize(&s, before, sizeof before, &bb);
        screen_resize(&s, 120, 22);                  /* 拖高一次 */
        /* conhost 的重绘就是「把当前可见区照抄一遍」：直接用缓冲区里的可见行文本构造，
         * 只发 19 行内容（真机 pane0 首次重绘就是 19 行）+ 空白尾巴 + 绝对定位光标。 */
        {
            int h = 19;
            char rp[8192];
            int n = 0;
            n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[?25l\x1b[H");
            /* 前 h 行 = 可见区 rel (rows-h)..(rows-1) 的原文，最后一行就是提示符；
             * conhost 不会把滚出屏外的历史拉回来，所以它的顶行对应本地 rel+3。 */
            for (int i = 0; i < h; i++) {
                char t[96];
                rowtext(&s, s.rows - h + i, t);
                n += snprintf(rp + n, sizeof rp - (size_t)n, "%s\x1b[K\r\n", t);
            }
            for (int j = h; j < s.rows - 1; j++)     /* 其余行 conhost 补空白 */
                n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[K\r\n");
            n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[K\x1b[%d;26H\x1b[?25h", h);
            feed_repaint(&s, rp, n);
        }
        {
            char after[4096];
            int ab = 0, content_below = 0;
            summarize(&s, after, sizeof after, &ab);
            for (int rel = s.cursor_y + 1; rel < s.rows; rel++) {
                char t[96];
                rowtext(&s, rel, t);
                if (t[0] && strcmp(t, "<noline>")) content_below++;
            }
            char det[512];
            snprintf(det, sizeof det, "hist=%d 内容总行=%d(应%d) cursor_y=%d/%d；重绘前%d条内容行、重绘后%d条",
                     s.hist_lines, s.hist_lines + s.rows, total0, s.cursor_y, s.rows - 1, bb, ab);
            ck("重复行内容 + 重绘：内容流逐字不变（无重复行被写出）",
               strcmp(before, after) == 0, det);
            ck("重复行内容 + 重绘：内容总行数恒定", s.hist_lines + s.rows == total0, det);
            ck("重复行内容 + 重绘：提示符仍在最后一行",
               s.cursor_y == s.rows - 1, det);
            ck("重复行内容 + 重绘：提示符下方没有任何内容行",
               content_below == 0, det);
            {
                char t[96];
                rowtext(&s, s.rows - 1, t);
                ck("重复行内容 + 重绘：最后一行就是提示符", strstr(t, "Downloads>") != NULL, t);
            }
        }
        screen_free(&s);
    }

    /* ---- 8) 重绘比窗格【高】时不得丢行（ConPTY 视口比本地环高） ----
     * 这是 screen_repaint_align() 注释里说它要解决的场景：加宽/缩小后 conhost 的重绘
     * 行数超过本地窗格高度，顶行落在本地历史里（rel<0）。正确行为由 screen_process_output
     * 内部的重绘视口上滚处理：可见区最终 = 重绘的最后 rows 行，历史一行不动。
     * 旧 align 允许负向偏移，在这里把 hist_lines 从 21 削到 14、LINE-15..LINE-21 直接
     * 消失；新实现只接受正向偏移，此块必须完全不动环。 */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 120, 10);
        fill_lines(&s, 1, 30);
        feed(&s, "C:\\Users\\wu_dr\\Downloads>");
        int total0 = s.hist_lines + s.rows;
        char before[4096];
        int bb = 0;
        summarize(&s, before, sizeof before, &bb);
        {
            char rp[8192];
            int n = 0;
            n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[?25l\x1b[H");
            for (int i = 15; i <= 30; i++)           /* 16 行内容，比 10 行窗格高 */
                n += snprintf(rp + n, sizeof rp - (size_t)n, "LINE-%02d\x1b[K\r\n", i);
            n += snprintf(rp + n, sizeof rp - (size_t)n,
                          "C:\\Users\\wu_dr\\Downloads>\x1b[K\x1b[17;26H\x1b[?25h");
            feed_repaint(&s, rp, n);
        }
        {
            char after[4096];
            int ab = 0;
            summarize(&s, after, sizeof after, &ab);
            char det[512];
            snprintf(det, sizeof det, "hist=%d 内容总行=%d(应%d) cursor_y=%d/%d",
                     s.hist_lines, s.hist_lines + s.rows, total0, s.cursor_y, s.rows - 1);
            ck("重绘比窗格高：内容逐字不变（不丢 LINE-15..21）",
               strcmp(before, after) == 0, det);
            ck("重绘比窗格高：hist_lines 没被削短（不接受负向对齐）",
               s.hist_lines == 21, det);
            ck("重绘比窗格高：内容总行数恒定", s.hist_lines + s.rows == total0, det);
            ck("重绘比窗格高：提示符仍在最后一行", s.cursor_y == s.rows - 1, det);
            {
                char t[96];
                rowtext(&s, s.rows - 1, t);
                ck("重绘比窗格高：最后一行就是提示符", strstr(t, "Downloads>") != NULL, t);
            }
        }
        screen_free(&s);
    }


    /* ---- 9) 内容正好铺满窗格（hist==0）时，重绘不得吃掉顶部内容 ----
     * 真机 screen_resize_trace.log 实测：窗格被一路缩到 120x1（hist 0->1->2->3，
     * height 恒为 4），再拖回来；120x4 时 height=4，【紧接着一次重绘后 height=1】——
     * banner 两行直接消失，用户报的「历史被直接吃了」。
     * 那几次重绘的字节（termux_dump.log 原文）只有提示符 + 空白：conhost 在窗格只有
     * 1 行高时把 banner 滚进了【它自己的】滚动缓冲，长回来时不回收、只回画「提示符在
     * 顶、下面全空」。照实画就会把本地还可见的顶部内容覆盖掉。
     * 旧 reanchor 用 hist_lines>0 当守卫，这里 hist==0 于是直接放弃；判据换成
     * 「conhost 画的内容行数 < 本地内容区行数」，下移量 = min(窗格高, 内容行数) - 重绘行数。 */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 120, 4);
        feed(&s, "Microsoft Windows [10.0.26200.9168]\r\n");
        feed(&s, "(c) Microsoft Corporation\r\n");
        feed(&s, "\r\n");
        feed(&s, "C:\\Users\\wu_dr\\Downloads>");
        char want[1024];
        int wb = 0;
        summarize(&s, want, sizeof want, &wb);
        int total0 = s.hist_lines + s.rows;
        const char *P = "C:\\Users\\wu_dr\\Downloads>";
        int ok_rows = 1, conserved = 1;
        char det[2304];
        det[0] = 0;
        /* 真机时序：缩到 1 行、回到 2 行、回到 4 行，每次跟一条 conhost 的整屏重绘 */
        for (int step = 0; step < 3; step++) {
            int nr = (step == 0) ? 1 : (step == 1) ? 2 : 4;
            char rp[512];
            int n = 0;
            screen_resize(&s, 120, nr);
            n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[?25l\x1b[H%s\x1b[K", P);
            for (int j = 1; j < nr; j++)
                n += snprintf(rp + n, sizeof rp - (size_t)n, "\r\n\x1b[K");
            /* 光标绝对定位回第 1 行（真机是 ESC[1;26H）——提示符在顶、下面全空 */
            n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[1;26H\x1b[?25h");
            feed_repaint(&s, rp, n);
            snprintf(det, sizeof det, "末次: 120x%d hist=%d 内容总行=%d(应%d)",
                     s.rows, s.hist_lines, s.hist_lines + s.rows, total0);
            if (s.hist_lines + s.rows != total0) conserved = 0;
        }
        {
            char got[1024];
            int gb = 0;
            summarize(&s, got, sizeof got, &gb);
            ok_rows = (strcmp(got, want) == 0);
            snprintf(det, sizeof det, "内容流[%s] 应为[%s]", got, want);
        }
        ck("内容铺满窗格(hist=0) + 重绘：banner 等内容一行不少", ok_rows, det);
        ck("内容铺满窗格(hist=0) + 重绘：内容总行数恒定", conserved, det);
        {
            char t[96];
            rowtext(&s, 3, t);
            ck("内容铺满窗格(hist=0) + 重绘：提示符回到内容末行(第4行)",
               strstr(t, "Downloads>") != NULL, t);
            rowtext(&s, 0, t);
            ck("内容铺满窗格(hist=0) + 重绘：banner 仍在第一行",
               strstr(t, "Microsoft Windows") != NULL, t);
        }
        screen_free(&s);
    }

    /* ---- 10) 内容【不足一屏】时，短重绘一律不得重新锚定 ----
     * 真机左右分屏（host 120x29，左窗格被拖到 21..60 列）实测：拖动中左窗格的 banner
     * 在第 2、3、9、14、18、21、23 行重复出现 7 次，(c) 行与提示符被挤到最底下，整屏
     * 渲染成碎片（render_dump.log 第 39 帧起；用严格 VT 状态机重放该帧可逐格复现）。
     *
     * 机制：reanchor 曾在内容不足一屏时也按 area = min(rows, content) 下移。
     *   rep(重绘画了几行) < area  =>  tail = area - rep > 0
     * 重绘内容整体下移 tail 行，顶部 tail 行再用【重绘前的快照】补回 —— 补回的正是
     * 重绘内容开头那几行，于是同几行出现两遍、末尾几行被挤出。左右分屏拖动时每帧都在
     * resize + 重绘，逐帧累积（真机重放：宽 48 时 2 份 -> 宽 45 时 4 份 -> 更多）。
     *
     * 判据必须是「重绘前内容一直铺满到最后一行」(repaint_snap_content >= rows)：只有
     * 那种情形下 conhost 少发的行才一定是更老的内容（组 9）；不足一屏时内容是顶对齐
     * 的，conhost 少画几行照实覆盖即可。 */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 60, 29);
        feed(&s, "Microsoft Windows [10.0.26200.9168]\r\n");
        feed(&s, "(c) Microsoft Corporation\r\n");
        feed(&s, "\r\n");
        feed(&s, "C:\\Users\\wu_dr\\Downloads>");
        /* 真机：拖窄一格（60 -> 48），置 resize_repaint_pending */
        screen_resize(&s, 48, 29);
        /* conhost 的整屏重绘只画了 3 行内容（banner / (c) / 空行），光标停在第 3 行。
         * rep = 3 < 内容行数 4 —— 这正是旧公式算出 tail=1 的地方。 */
        char rp[2048];
        int n = 0;
        n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[?25l\x1b[H");
        n += snprintf(rp + n, sizeof rp - (size_t)n, "Microsoft Windows [10.0.26200.9168]\x1b[K\r\n");
        n += snprintf(rp + n, sizeof rp - (size_t)n, "(c) Microsoft Corporation\x1b[K\r\n");
        n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[K\r\n");
        for (int j = 3; j < 28; j++)
            n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[K\r\n");
        n += snprintf(rp + n, sizeof rp - (size_t)n, "\x1b[K\x1b[3;26H\x1b[?25h");
        feed_repaint(&s, rp, n);
        int cnt = 0;
        char det[2304];
        det[0] = 0;
        for (int rel = 0; rel < s.rows; rel++) {
            char t[256];
            rowtext(&s, rel, t);
            if (strstr(t, "Microsoft Windows")) cnt++;
        }
        snprintf(det, sizeof det, "banner 在可见区出现 %d 次(应 1)；48x29 hist=%d cursor_y=%d",
                 cnt, s.hist_lines, s.cursor_y);
        ck("内容不足一屏 + 短重绘：banner 不被复制(左右分屏拖动首帧)", cnt == 1, det);
        {
            /* 这条合成重绘自己用 ESC[K 擦掉了提示符行，所以只断言「内容顶对齐、
             * 没被整体下坠」：第 1 行必须是 banner，重绘没画的第 4 行起必须全空。 */
            char t[256], t0[256];
            int blanks = 1;
            rowtext(&s, 0, t0);
            int top_ok = (strstr(t0, "Microsoft Windows") != NULL);
            for (int rel = 3; rel < s.rows; rel++) {
                rowtext(&s, rel, t);
                if (t[0]) { blanks = 0; break; }
            }
            snprintf(det, sizeof det, "第1行[%s] 第4行起全空=%d", t0, blanks);
            ck("内容不足一屏 + 短重绘：内容顶对齐、未整体下坠", top_ok && blanks, det);
        }
        /* 真机是逐帧累积：继续拖动 + 重绘，份数不能越滚越多 */
        int worst = cnt;
        for (int step = 0; step < 40; step++) {
            int nc = 41 + (step % 20);
            screen_resize(&s, nc, 29);
            int m = 0;
            m += snprintf(rp + m, sizeof rp - (size_t)m, "\x1b[?25l\x1b[H");
            m += snprintf(rp + m, sizeof rp - (size_t)m, "Microsoft Windows [10.0.26200.9168]\x1b[K\r\n");
            m += snprintf(rp + m, sizeof rp - (size_t)m, "(c) Microsoft Corporation\x1b[K\r\n");
            m += snprintf(rp + m, sizeof rp - (size_t)m, "\x1b[K\r\n");
            for (int j = 3; j < 28; j++)
                m += snprintf(rp + m, sizeof rp - (size_t)m, "\x1b[K\r\n");
            m += snprintf(rp + m, sizeof rp - (size_t)m, "\x1b[K\x1b[3;26H\x1b[?25h");
            feed_repaint(&s, rp, m);
            int c2 = 0;
            for (int rel = 0; rel < s.rows; rel++) {
                char t[256];
                rowtext(&s, rel, t);
                if (strstr(t, "Microsoft Windows")) c2++;
            }
            if (c2 > worst) worst = c2;
        }
        snprintf(det, sizeof det, "40 轮拖动+重绘后 banner 最多 %d 份(应 1)", worst);
        ck("内容不足一屏 + 反复拖动重绘：banner 份数不累积", worst == 1, det);
        screen_free(&s);
    }

    /* ---- 组 11：ConPTY 右缘填充空格不得进入模型（中文里凭空多出空格） ----
     * 真机字节流（uploads/termux_dump.log 偏移 6145，[pane 0 len 115]）：
     *   ESC[28;40H "2Fsd v0.68 （Ext2,3,4文件系统Windows驱 \n"
     * 窗格宽 40 时「驱」占 37-38 列，conhost 在折行前补一个空格填满右缘（39 列）。
     * 这个空格是排版填充、不是内容。若被吸收进模型，used 撑到 cols，之后窗格变宽
     * reflow 把折行拼回去，它就永久夹在宽字符与下一个字之间 —— 表现为「驱 动」
     * 「洛 谷」这类中文里凭空多出空格（cell_diag.log F104-F131 实测 26 条）。
     * 修复：src/vt.c screen_put_cp 丢弃「左邻是宽字符次格且光标在右缘末列」的空格。 */
    {
        ScreenBuffer s;
        char det[512];
        char t0[96];
        memset(&s, 0, sizeof s);
        screen_init(&s, 40, 29);
        /* conhost 那条重绘：宽字符贴右缘 + 填充空格 + LF */
        /* 直接用 UTF-8 字面量，避免 \xNN 在 -Wextra 下报 hex escape out of range。 */
        feed(&s, "\x1b[1;1H2Fsd v0.68 （Ext2,3,4文件系统Windows驱"
                 " \n");
        /* 把整条逻辑行流拼起来（历史 + 可见，跳过次格），模拟 reflow 拼接后的视图 */
        WCHAR stream[4096];
        int sn = 0;
        for (int rel = -s.hist_lines; rel < s.rows && sn < 4000; rel++) {
            int pr = screen_phys_row(&s, rel);
            if (pr < 0 || pr >= s.total_lines || !s.lines || !s.lines[pr].cells) continue;
            ScreenLine *ln = &s.lines[pr];
            for (int x = 0; x < ln->used && sn < 4000; x++)
                stream[sn++] = ln->cells[x].Char.UnicodeChar;
        }
        int at = -1;
        for (int i = 0; i < sn; i++) if (stream[i] == 0x9A71) { at = i; break; }
        snprintf(det, sizeof det, "驱@%d 后一格=%s", at,
                 (at >= 0 && at + 1 < sn) ? (stream[at + 1] == 0 ? "{0}次格"
                  : (stream[at + 1] == L' ' ? "空格(填充未被吸收)" : "其他")) : "<eol>");
        ck("ConPTY 右缘填充空格不进入模型（宽字符贴右缘）",
           at >= 0 && at + 1 < sn && stream[at + 1] == 0, det);
        int no_pad = 1;
        if (at >= 0 && at + 2 < sn && stream[at + 2] == L' ') no_pad = 0;
        ck("宽字符后没有残留填充空格", no_pad,
           no_pad ? "无残留" : "驱 的次格后仍是空格");
        /* 窗格变宽 + conhost 重绘补全后半：「驱动」必须紧邻 */
        screen_resize(&s, 86, 29);
        feed(&s, "\x1b[1;1H2Fsd v0.68 （Ext2,3,4文件系统Windows驱动程序）");
        sn = 0;
        for (int rel = -s.hist_lines; rel < s.rows && sn < 4000; rel++) {
            int pr = screen_phys_row(&s, rel);
            if (pr < 0 || pr >= s.total_lines || !s.lines || !s.lines[pr].cells) continue;
            ScreenLine *ln = &s.lines[pr];
            for (int x = 0; x < ln->used && sn < 4000; x++)
                stream[sn++] = ln->cells[x].Char.UnicodeChar;
        }
        at = -1;
        for (int i = 0; i < sn; i++) if (stream[i] == 0x9A71) { at = i; break; }
        int adj = (at >= 0 && at + 2 < sn && stream[at + 1] == 0 && stream[at + 2] == 0x52A8);
        snprintf(det, sizeof det, "驱@%d 后=[%s %s]", at,
                 (at >= 0 && at + 1 < sn) ? (stream[at + 1] == 0 ? "{0}" : "?") : "-",
                 (at >= 0 && at + 2 < sn) ? (stream[at + 2] == 0x52A8 ? "动" : "?") : "-");
        ck("变宽重绘后「驱动」紧邻（中间无空格）", adj, det);
        /* 反向保护：ASCII 填满右缘后的真实空格不能被误吞 */
        ScreenBuffer t;
        memset(&t, 0, sizeof t);
        screen_init(&t, 40, 29);
        feed(&t, "\x1b[1;1H1234567890123456789012345678901234567890   x");
        rowtext(&t, 0, t0);
        /* 40 列填满后折行，续行应有 3 个空格再 x */
        char t1[256];
        rowtext(&t, 1, t1);
        int kept = (strstr(t1, "   x") != NULL);
        snprintf(det, sizeof det, "续行[%s]", t1);
        ck("ASCII 填满右缘后的真实空格不被误吞", kept, det);
        screen_free(&s);
        screen_free(&t);
    }

    /* ---- 组 12：ConPTY 窄屏行内续写（CUP 回跳）—— 续行是【下面那一行】 ----
     * conhost 折行续写的真实字节形态（2026-09-17 真机 termux_dump.log，pane0
     * 偏移 1288，窗格 24 列，共 55 处）：
     *     …are ESC]0;…cmd.exe - dir BEL ESC[?25h CR LF ESC[28;24H ena CR LF …
     * ESC[28;24H = 0 基 (27,23) = 【刚写满那一行的最后一列】。在那一列写第一个字符
     * 就触发自动折行，所以真正的续行是【下面那一行】，而 screen_put_cp 的自动折行
     * 路径（screen_newline + screen_mark_softwrap）本来就会正确标它。
     *
     * ⚠️ 这里曾经断言「CUP 落点行自己被标成续行」，那是 bug #11 的错误假设。
     * line_wrap[r]=1 的含义是「r 是 r-1 的续行」；标 CUP 落点行等于把【被续写的
     * 那一行】说成它上一行的续行，语义正好反了。真机那次会话里这条规则误命中
     * 47 次，24 列下整份 dir 列表被并成一条 1484 字符的逻辑行，拖宽到 82 后就是
     * 用户报的「内容顺序完好、词从中间切开」的级联错位。详见
     * analysis/拖动错乱-根因与三种可选行为.md §11。 */
    {
        ScreenBuffer s;
        memset(&s, 0, sizeof s);
        screen_init(&s, 16, 29);
        /* 真机形态：写满 16 列 -> CRLF -> CUP 回本行末列 -> 续写剩余字符 */
        feed(&s, "2026-08-18  15:0");
        feed(&s, "\r\n");
        feed(&s, "\x1b[2;16H");
        feed(&s, "06");
        /* CUP 落点行（rel=1）是被续写的那一行，不是续行 -> 必须 0 */
        int pr = screen_phys_row(&s, 1);
        int wrap = (pr >= 0 && pr < s.total_lines && s.line_wrap) ? s.line_wrap[pr] : -1;
        char det[256];
        snprintf(det, sizeof det, "line_wrap[rel=1]=%d（应 0）", wrap);
        ck("CUP 落点行不被误标为续行（它是被续写的那行）", wrap == 0, det);
        /* 在末列写字符触发自动折行，真正的续行是 rel=2，由 auto-wrap 标记 */
        int pr_n = screen_phys_row(&s, 2);
        int wrap_n = (pr_n >= 0 && pr_n < s.total_lines && s.line_wrap) ? s.line_wrap[pr_n] : -1;
        snprintf(det, sizeof det, "line_wrap[rel=2]=%d（应 1）", wrap_n);
        ck("末列续写触发的自动折行把【下一行】标为续行", wrap_n == 1, det);

        /* 滚动场景：光标在底行时 CR LF 走 screen_scroll_up，内容上移一行而
         * cursor_y 不变，conhost 续写的 CUP 落在 lf_row-1。真机实测 9514 次里
         * 9417 次是这个形态（dy=-1、cx=末列），早先只判 == lf_row 时命中 1 次。 */
        {
            ScreenBuffer u;
            memset(&u, 0, sizeof u);
            screen_init(&u, 16, 6);
            /* 填满并滚过底行，让光标停在最后一行 */
            for (int i = 0; i < 12; i++) feed(&u, "0123456789abcdef\r\n");
            int y_before = u.cursor_y;
            feed(&u, "ABCDEFGHIJKLMNOP");   /* 写满底行 16 列 */
            feed(&u, "\r\n");               /* 底行 CRLF -> 滚动 */
            int lf_row = u.cursor_y;
            feed(&u, "\x1b[5;16H");         /* 1 基行5 = 0 基 4 = lf_row-1 */
            /* 写两个字符：第一个落在末列（只挂起折行），第二个才真正触发
             * screen_newline + screen_mark_softwrap，把【下一行】标成续行。 */
            feed(&u, "ZQ");
            int pr3 = screen_phys_row(&u, 4);
            int wrap3 = (pr3 >= 0 && pr3 < u.total_lines && u.line_wrap) ? u.line_wrap[pr3] : -1;
            char d3[256];
            snprintf(d3, sizeof d3, "y_before=%d lf_row=%d line_wrap[rel=4]=%d（应 0）",
                     y_before, lf_row, wrap3);
            ck("滚动后 CUP 落在 lf_row-1：落点行不被误标为续行", wrap3 == 0, d3);
            int pr4 = screen_phys_row(&u, 5);
            int wrap4 = (pr4 >= 0 && pr4 < u.total_lines && u.line_wrap) ? u.line_wrap[pr4] : -1;
            snprintf(d3, sizeof d3, "line_wrap[rel=5]=%d（应 1）", wrap4);
            ck("滚动后末列续写：【下一行】被标为续行", wrap4 == 1, d3);
            screen_free(&u);
        }

        /* 反向保护：普通 CUP（跳到别的行/回到行首）不得误标 */
        ScreenBuffer t;
        memset(&t, 0, sizeof t);
        screen_init(&t, 16, 29);
        feed(&t, "aaaaaaaaaaaaaaaa");   /* 写满 16 列 */
        feed(&t, "\r\n");
        feed(&t, "\x1b[5;1H");            /* 跳到第 5 行行首：不是续写 */
        int pr2 = screen_phys_row(&t, 4);
        int wrap2 = (pr2 >= 0 && pr2 < t.total_lines && t.line_wrap) ? t.line_wrap[pr2] : -1;
        snprintf(det, sizeof det, "line_wrap[rel=4]=%d（应 0）", wrap2);
        ck("跳到别处的 CUP 不误标为续行", wrap2 == 0, det);
        screen_free(&s);
        screen_free(&t);
    }

    printf("\n%s\n", g_fail ? "RESIZE-HISTORY REPRO: FAILURE(S)" : "RESIZE-HISTORY REPRO: ALL PASS");
    return g_fail ? 1 : 0;
}
