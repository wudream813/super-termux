/*
 * tests/replay_conpty_resize.c  (Linux, gcc -Itests/stub)
 *
 * resize / ConPTY 整屏重绘回归（v1.8.52 resize-repaint 修复）。
 *
 * fixture: tests/fixtures/termux_dump.log —— 真实双 pane 拖动分隔条抓包
 *   （pane0 = `for /l %i in (1,1,30) do @echo %i`，宽度 120→59 的 ConPTY 重绘）。
 *
 * 重放规则与 src/pane.c 的 pane_read_thread 一致：每块先 screen_repaint_align
 * 再喂 screen_process_output；
 * 块头若带 ESC[8;rows;colst 先做本地 reflow。
 *
 * 验收（本轮 bug 的直接断言）：
 *   1) 数字 1..30 一旦出现就永不消失（历史上 chunk14/chunk87 两次整屏重绘各吞
 *      一行：3、4 消失——“空格没了”的真相）。故每个数字行出现集合只增不减，
 *      终态恰为 {1..30}。
 *   2) 任何时刻同一数字不得重复出现（行重复 = 幽灵行/跳行）。
 *   3) 1..30 全部就位时它们必须占据连续 rel 槽位（槽数==30），中间夹空行或
 *      非数字内容 = repaint 在中途插入了幻影空行。
 */
#include "screen.h"
#include "vt.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_scrollback_lines = 10000;
MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0, g_search_match_cur = -1, g_search_active = 0;

typedef struct { int pane, len, off; } R;

static int is_pure_digits(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

static void rowtext_ring(ScreenBuffer *s, int rel, char *out) {
    int pr = screen_phys_row(s, rel);
    int hp = 0; out[0] = 0;
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) return;
    ScreenLine *ln = &s->lines[pr];
    for (int x = 0; x < ln->len && hp < 90; x++) {
        unsigned c = (unsigned)ln->cells[x].Char.UnicodeChar;
        if (c == 0 || c == L' ') { if (hp && out[hp - 1] != ' ') out[hp++] = ' '; }
        else if (c < 128) out[hp++] = (char)c; else out[hp++] = '?';
    }
    while (hp && out[hp - 1] == ' ') hp--;
    out[hp] = 0;
}

static void rowtext_view(ScreenBuffer *s, int vo, int row, char *out) {
    int hp = 0; out[0] = 0;
    RGlyph *view = malloc((size_t)s->rows * s->cols * sizeof(RGlyph));
    if (!view) return;
    if (!screen_reflow_view(s, vo, s->rows, s->cols, view)) { free(view); return; }
    for (int x = 0; x < s->cols && hp < 90; x++) {
        unsigned c = (unsigned)view[row * s->cols + x].ci.Char.UnicodeChar;
        if (c == 0 || c == L' ') { if (hp && out[hp - 1] != ' ') out[hp++] = ' '; }
        else if (c < 128) out[hp++] = (char)c; else out[hp++] = '?';
    }
    while (hp && out[hp - 1] == ' ') hp--;
    out[hp] = 0;
    free(view);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/fixtures/conpty_resize_dump.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)sz); fread(data, 1, (size_t)sz, f); fclose(f);

    char *p = data, *end = data + sz;
    R *rs = NULL; int nr = 0, cap = 0;
    while (p < end) {
        int pane = -1, len = -1;
        if (sscanf(p, "[pane %d len %d]", &pane, &len) != 2) { p++; continue; }
        char *nl = strchr(p, '\n'); if (!nl) break;
        char *body = nl + 1; if (body + len > end) break;
        if (nr == cap) { cap = cap ? cap * 2 : 256; rs = realloc(rs, (size_t)cap * sizeof(R)); }
        rs[nr].pane = pane; rs[nr].len = len; rs[nr].off = (int)(body - data); nr++;
        p = body + len;
    }

    ScreenBuffer s; memset(&s, 0, sizeof s);
    screen_init(&s, 120, 29);
    memset(&g_mux, 0, sizeof g_mux);

    static int seen[31];        /* 每个数字 d 当前是否出现在 hist+visible */
    int ever_lost = 0, dup_seen = 0, gap_seen = 0, fail_chunk = 0;

    for (int i = 0; i < nr; i++) {
        if (rs[i].pane != 0) continue;
        const char *h = data + rs[i].off; int hl = rs[i].len;
        /* pane_read_thread 序：块头 8;t → 本地 reflow（置 nop）；先 align 再喂 */
        int k = 0;
        if (hl >= 6 && h[0] == 0x1b && h[1] == '[' && h[2] == '?' && h[3] == '2' && h[4] == '5' && h[5] == 'l') k = 6;
        if (hl - k >= 8 && h[k] == 0x1b && h[k + 1] == '[' && h[k + 2] == '8' && h[k + 3] == ';') {
            int row1 = 0, col1 = 0, cur = 0, pos = k + 4; char num[16]; int np = 0;
            while (pos < hl && h[pos] != 't') {
                char cc = h[pos];
                if (cc == ';') { num[np] = 0; if (cur == 0) row1 = atoi(num); else col1 = atoi(num); np = 0; cur++; }
                else if (np < 15) num[np++] = cc;
                pos++;
            }
            num[np] = 0; if (cur == 0) row1 = atoi(num); else if (cur == 1) col1 = atoi(num);
            if (row1 > 0 && col1 > 0) screen_resize(&s, col1, row1);
        }
        screen_repaint_align(&s, data + rs[i].off, rs[i].len);
        screen_process_output(&s, data + rs[i].off, rs[i].len);

        /* 扫描当前 hist+visible，逐行记录数字行 */
        int now[31]; for (int d = 0; d <= 30; d++) now[d] = 0;
        int first_rel = 0, last_rel = 0, have_numeric = 0;
        char rel_txt[64][8]; int rel_n = 0;
        for (int rel = -(s.hist_lines); rel < s.rows; rel++) {
            int pr = screen_phys_row(&s, rel);
            char txt[96];
            if (pr < 0 || pr >= s.total_lines || !s.lines || !s.lines[pr].cells) { strcpy(txt, ""); }
            else {
                ScreenLine *ln = &s.lines[pr]; int hp = 0;
                for (int x = 0; x < ln->len && hp < 90; x++) {
                    unsigned c = (unsigned)ln->cells[x].Char.UnicodeChar;
                    if (c == 0 || c == L' ') { if (hp && txt[hp - 1] != ' ') txt[hp++] = ' '; }
                    else if (c < 128) txt[hp++] = (char)c; else { txt[hp++] = '?'; }
                }
                while (hp && txt[hp - 1] == ' ') hp--;
                txt[hp] = 0;
            }
            if (is_pure_digits(txt)) {
                int v = atoi(txt);
                if (v >= 1 && v <= 30) {
                    now[v] = 1;
                    if (!have_numeric) { first_rel = rel; have_numeric = 1; }
                    last_rel = rel;
                    if (rel_n < 60) strcpy(rel_txt[rel_n++], txt);
                }
            }
        }
        /* 断言 1: 集合只增不减 */
        for (int d = 1; d <= 30; d++) {
            if (seen[d] && !now[d]) { ever_lost = 1; if (!fail_chunk) fail_chunk = i + 1; }
            if (now[d]) seen[d] = 1;
        }
        /* 断言 2: 无重复 */
        for (int a = 0; a < rel_n; a++)
            for (int b = a + 1; b < rel_n; b++)
                if (!strcmp(rel_txt[a], rel_txt[b])) { dup_seen = 1; if (!fail_chunk) fail_chunk = i + 1; }
        /* 断言 3: 全 1..30 就位时 rel 槽连续且跨度为 30 */
        int full = 1; for (int d = 1; d <= 30; d++) if (!now[d]) { full = 0; break; }
        if (full && have_numeric && (last_rel - first_rel + 1) != 30) {
            gap_seen = 1; if (!fail_chunk) fail_chunk = i + 1;
        }
    }

    int full_now = 1;
    for (int d = 1; d <= 30; d++) if (!seen[d]) full_now = 0;

    printf("RESULT: chunks=%d lost=%s dup=%s gap=%s cols=%d rows=%d hist=%d limit=%d total=%d\n",
           nr, ever_lost ? "YES" : "no", dup_seen ? "YES" : "no", gap_seen ? "YES" : "no",
           s.cols, s.rows, s.hist_lines, screen_scroll_limit(&s), s.total_lines);

    /* ---- 附加验收：resize 后“向上滚可达到顶部锚定” ----
     * 滚到 vo=limit（最上）时视口顶行 == 环最老行（banner），证明 resize + 反复
     * ConPTY 重绘之后滚动回看仍然可达内容最顶（前轮验收的“滚到 banner”不被本轮
     * 修复破坏）。1..30 唯一/连续已由上文断言。 ---- */
    int reach_ok = 1;
    {
        int limit = screen_scroll_limit(&s);
        if (limit <= 0) { reach_ok = 0; }
        else {
            char a[96], b[96];
            rowtext_ring(&s, -s.hist_lines, a);        /* 环最老行 */
            rowtext_view(&s, limit, 0, b);             /* 滚到顶时视口顶行 */
            if (strcmp(a, b)) reach_ok = 0;
        }
    }
    if (ever_lost || dup_seen || gap_seen || !full_now || !reach_ok) {
        printf("[FAIL] resize-repaint 回归：%s%s%s%s%s (first bad chunk ~%d)\n",
               ever_lost ? " 数字行消失" : "", dup_seen ? " 数字行重复" : "",
               gap_seen ? " 内容中缝出现空行/断层" : "", full_now ? "" : " 终态缺数字",
               reach_ok ? "" : " 滚动回看栈不完整", fail_chunk);
        return 1;
    }
    printf("[OK] 全部 1..30 全程唯一、不丢、连续；滚动回看栈完整可达 banner；resize-repaint 回归通过。\n");
    return 0;
}
