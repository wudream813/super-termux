/* 级联拼接探测器（bug #16：拖动后整段历史并成一条逻辑行、按窗格宽重折）。
 *
 * 与 replay_pane_stream.c 的区别：那个只跑固定宽度链（59->19->70）并用【写死的
 * 日期表】判拼接；这个接受任意宽度序列（env SEQ），并用【通用判据】——一条显示行
 * 里出现 >=2 个 YYYY/MM/DD 或 YYYY-MM-DD 日期前缀就算跨记录拼接。
 *
 * 用户 2026-09-17 报的形态：内容顺序完好、词被从中间切开（Music -> Mu|sic、
 * Saved Games20|26-07-25），说明【行内容是对的、行边界没了】= 一整段连续行的
 * line_wrap 都是 1，reflow 把它们并成一条逻辑行后按新宽度重折。
 *
 * 用法:  SEQ=97,19,97 ./cascade_probe <stream.bin> [start_cols] [rows]
 *        SEQ 每项可写 "82" 或 "82x21"（后者同时改高度，走 repaint_reanchor 路径）
 *        MIDR=21  给 SPLIT/MID 那一步指定高度
 *        STEPS=212:59,1153:19,7589:80  多步交错回放（优先于 SPLIT/MID）
 *        VERBOSE=1 打印每次 resize 后最长的拼接行
 */
#include "common.h"
#include "types.h"
#include "screen.h"
#include "vt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

int g_scrollback_lines = 1000;
MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0, g_search_match_cur = -1, g_search_active = 0;

static int row_text(ScreenBuffer *s, int rel, char *out, int cap) {
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || pr >= s->total_lines) return 0;
    ScreenLine *ln = &s->lines[pr];
    if (!ln->cells) return 0;
    int w = ln->used < ln->len ? ln->used : ln->len;
    if (w > s->cols) w = s->cols;
    int n = 0;
    for (int x = 0; x < w && n < cap - 5; x++) {
        WCHAR ch = ln->cells[x].Char.UnicodeChar;
        if (ch == 0) continue;
        if (ch < 0x80) out[n++] = (char)ch;
        else if (ch < 0x800) { out[n++] = 0xC0 | (ch >> 6); out[n++] = 0x80 | (ch & 0x3F); }
        else { out[n++] = 0xE0 | (ch >> 12); out[n++] = 0x80 | ((ch >> 6) & 0x3F);
               out[n++] = 0x80 | (ch & 0x3F); }
    }
    out[n] = 0;
    return n;
}

/* 一行里出现几个 "YYYY/MM/DD" 或 "YYYY-MM-DD" 前缀（通用判据，不依赖具体日期表）。 */
static int count_dates(const char *t) {
    int hits = 0;
    for (const char *p = t; *p; p++) {
        if (!isdigit((unsigned char)p[0])) continue;
        if (!(isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) &&
              isdigit((unsigned char)p[3]))) continue;
        char sep = p[4];
        if (sep != '/' && sep != '-') continue;
        if (!(isdigit((unsigned char)p[5]) && isdigit((unsigned char)p[6]) &&
              p[7] == sep &&
              isdigit((unsigned char)p[8]) && isdigit((unsigned char)p[9]))) continue;
        hits++;
        p += 9;
    }
    return hits;
}

/* 最长的连续 line_wrap=1 段长度（级联的直接指标）。 */
static int max_wrap_run(ScreenBuffer *s) {
    int best = 0, run = 0;
    for (int rel = -s->hist_lines; rel < s->rows; rel++) {
        int pr = screen_phys_row(s, rel);
        if (pr < 0 || pr >= s->total_lines || !s->line_wrap) { run = 0; continue; }
        if (s->line_wrap[pr]) { run++; if (run > best) best = run; }
        else run = 0;
    }
    return best;
}

/* 扫描一次，返回拼接行数；worst 里放最严重那行。 */
static int scan(ScreenBuffer *s, const char *tag, char *worst, int wcap, int *worst_hits) {
    int bad = 0; char line[1024];
    *worst_hits = 0; worst[0] = 0;
    for (int rel = -s->hist_lines; rel < s->rows; rel++) {
        if (!row_text(s, rel, line, sizeof line)) continue;
        int h = count_dates(line);
        if (h >= 2) {
            bad++;
            if (h > *worst_hits) {
                *worst_hits = h;
                snprintf(worst, wcap, "rel=%+d hits=%d | %s", rel, h, line);
            }
        }
    }
    if (getenv("VERBOSE") && bad)
        printf("    [%s] 拼接行=%d 最长wrap段=%d\n      %s\n", tag, bad, max_wrap_run(s), worst);
    return bad;
}

/* ALIGN=1 时按真机 pane.c 的方式喂：在每个 ESC[H（整屏重绘起点）前断块，
 * 每块先 screen_repaint_align 再 screen_process_output。probe 以前根本不调 align，
 * 所以「短重绘 -> align return 2 -> 交给 reanchor」这条真实路径一直没被覆盖。 */
static int g_align = 0;
static void feed(ScreenBuffer *s, const char *p, int len) {
    int i = 0;
    if (len <= 0) return;
    if (!g_align) { screen_process_output(s, p, len); return; }
    while (i < len) {
        int j = i + 1;
        while (j + 2 < len) {
            if ((unsigned char)p[j] == 0x1b && p[j + 1] == '[' && p[j + 2] == 'H') break;
            j++;
        }
        if (j + 2 >= len) j = len;
        {
            int rv = screen_repaint_align(s, p + i, j - i);
            if (getenv("ALIGNDBG")) {
                int rr = 0, nn = 0;
                /* 数一下这块里有几个换行，好知道 conhost 这次画了几行 */
                for (int q = i; q < j; q++) if (p[q] == 0x0a) nn++;
                printf("[align] 块长=%4d 行数=%2d 返回=%d cursor_y=%d rows=%d hist=%d\n",
                       j - i, nn, rv, s->cursor_y, s->rows, s->hist_lines);
                (void)rr;
            }
        }
        screen_process_output(s, p + i, j - i);
        i = j;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("用法: %s <字节流> [start_cols] [rows]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("打不开 %s\n", argv[1]); return 2; }
    static char buf[1 << 17];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) { printf("空文件\n"); return 2; }

    int start_cols = (argc > 2) ? atoi(argv[2]) : 97;
    int rows = (argc > 3) ? atoi(argv[3]) : 32;
    ScreenBuffer s;
    g_align = getenv("ALIGN") ? atoi(getenv("ALIGN")) : 0;
    if (!screen_init(&s, start_cols, rows)) { printf("screen_init 失败\n"); return 1; }

    /* SPLIT/MID：前 SPLIT 字节在 start_cols 下喂 -> resize 到 MID -> 喂剩余部分。
     * 真机历史就是这种混合宽度状态（replay_pane_stream.c 里复现拼接的必要前提）。 */
    /* STEPS="off:cols,off:cols,..."：多步交错回放。真机一次会话里 resize 和输出是
     * 交错的（screen_resize_trace.log 有 16 帧），SPLIT/MID 只能表达一步，喂错宽度
     * 就等于在测切点。给了 STEPS 就优先用它。 */
    {
        const char *st = getenv("STEPS");
        char stbuf[512];
        if (st && *st) {
            snprintf(stbuf, sizeof stbuf, "%s", st);
            int prev = 0, nstep = 0;
            for (char *tk = strtok(stbuf, ","); tk; tk = strtok(NULL, ",")) {
                int off = atoi(tk);
                char *cp = strchr(tk, ':');
                int w = cp ? atoi(cp + 1) : 0;
                if (w < 2) continue;
                if (off < prev) off = prev;
                if (off > (int)n) off = (int)n;
                if (off > prev) feed(&s, buf + prev, off - prev);
                screen_resize(&s, w, rows);
                prev = off; nstep++;
            }
            if (prev < (int)n) feed(&s, buf + prev, (int)n - prev);
            printf("[STEPS] %d 步交错回放完成（%s）\n", nstep, st);
            goto fed;
        }
    }
    int split = getenv("SPLIT") ? atoi(getenv("SPLIT")) : 0;
    int mid = getenv("MID") ? atoi(getenv("MID")) : 0;
    /* MIDR：中间那步的高度（默认沿用命令行给的 rows）。上下分屏拖条改的是高度，
     * 之前没法回放，所以那条路径一直没被覆盖。 */
    int midr = getenv("MIDR") ? atoi(getenv("MIDR")) : rows;
    if (midr < 2) midr = rows;
    if (split > 0 && split < (int)n && mid >= 2) {
        feed(&s, buf, split);
        screen_resize(&s, mid, midr);
        feed(&s, buf + split, (int)n - split);
    } else {
        feed(&s, buf, (int)n);
    }

fed:
    ;
    const char *seq = getenv("SEQ");
    char seqbuf[256];
    if (!seq || !*seq) seq = "";
    snprintf(seqbuf, sizeof seqbuf, "%s", seq);

    int total_bad = 0;
    char worst[1024]; int wh = 0;
    total_bad += scan(&s, "feed", worst, sizeof worst, &wh);
    if (total_bad) printf("  喂完（%d 列）就已经拼接：%s\n", start_cols, worst);

    /* DUMP_ROWS=1：在做 SEQ 里的 resize 之前，把每一行的 wrap/used/内容打出来。
     * 用来定位「哪一条记录边界被错标成续行」——真机日志里没有 line_wrap。 */
    if (getenv("DUMP_ROWS")) {
        printf("\n--- resize 前逐行（rel: w used | 内容）---\n");
        for (int rel = -s.hist_lines; rel < s.rows; rel++) {
            int pr = screen_phys_row(&s, rel);
            if (pr < 0 || pr >= s.total_lines) continue;
            char t[512]; row_text(&s, rel, t, sizeof t);
            printf("  %+4d: w=%d used=%2d |%s|\n", rel,
                   s.line_wrap ? s.line_wrap[pr] : -1, s.lines[pr].used, t);
        }
    }

    /* SEQ 的每一项可以是 "82"（只改宽，高沿用）或 "82x21"（宽高一起改）。
     * 高度那一路会走 screen_repaint_reanchor（conhost 增高时只发 viewport 那几行），
     * 是「多余空格/重复行」的另一条嫌疑路径，之前 SEQ 只传宽度，覆盖不到。 */
    int widths[64], heights[64], nw = 0;
    for (char *tk = strtok(seqbuf, ","); tk && nw < 64; tk = strtok(NULL, ",")) {
        widths[nw] = atoi(tk);
        heights[nw] = 0;
        char *xp = strchr(tk, 'x');
        if (!xp) xp = strchr(tk, 'X');
        if (xp) heights[nw] = atoi(xp + 1);
        nw++;
    }
    for (int i = 0; i < nw; i++) {
        if (widths[i] < 2) continue;
        int rr = (heights[i] >= 2) ? heights[i] : rows;
        screen_resize(&s, widths[i], rr);
        char tag[64]; snprintf(tag, sizeof tag, "->%dx%d", widths[i], rr);
        char w2[1024]; int h2 = 0;
        int b = scan(&s, tag, w2, sizeof w2, &h2);
        if (b && !total_bad) { printf("  第一次出现在 resize %s：%s\n", tag, w2); }
        total_bad += b;
    }

    if (getenv("DUMP_FINAL")) {
        printf("\n--- 末态逐行（rel: w used | 内容）---\n");
        for (int rel = -s.hist_lines; rel < s.rows; rel++) {
            int pr = screen_phys_row(&s, rel);
            if (pr < 0 || pr >= s.total_lines) continue;
            char t[512]; row_text(&s, rel, t, sizeof t);
            printf("  %+4d: w=%d used=%2d |%s|\n", rel,
                   s.line_wrap ? s.line_wrap[pr] : -1, s.lines[pr].used, t);
        }
    }

    /* NEEDLES=逗号分隔列表：每个串必须在【同一行】里完整出现。
     * 用来抓「记录被拆开」——cascade 判据（一行里 >=2 个日期）只能抓合并，抓不到拆分：
     * 拆开后每行最多一个日期，看起来和正常折行没区别。2026-09-17 用户报的
     * 「有些地方多出了空格」就是这种：`<DIR>` + 一行纯空格 + `   arena`。 */
    int needle_bad = 0;
    {
        const char *nd = getenv("NEEDLES");
        char ndbuf[1024];
        if (nd && *nd) {
            snprintf(ndbuf, sizeof ndbuf, "%s", nd);
            for (char *tk = strtok(ndbuf, ","); tk; tk = strtok(NULL, ",")) {
                int found = 0;
                char line[1024];
                for (int rel = -s.hist_lines; rel < s.rows && !found; rel++) {
                    if (!row_text(&s, rel, line, sizeof line)) continue;
                    if (strstr(line, tk)) found = 1;
                }
                if (!found) { printf("  [拆分] 找不到完整记录: %s\n", tk); needle_bad++; }
            }
        }
    }
    total_bad += needle_bad;

    screen_free(&s);
    if (total_bad) { printf("[FAIL] SEQ=%s 累计拼接 %d 行次\n", seq, total_bad); return 1; }
    printf("[PASS] SEQ=%s\n", seq);
    return 0;
}
