/* 用真机 ConPTY 字节流回放，复现「拖宽 + 松手后历史区跨记录拼接」。
 *
 * 数据来自用户 2026-09-15 05:55 那批 termux_dump.log 里 pane 0 的全部输出
 * （33 块、5557 字节），由 tools 侧抽成裸字节流后用 argv[1] 传入。
 *
 * 合成数据（自己编几条 dir 记录）复现不出来 —— 试过 1 次 reflow、2 次 reflow、
 * 带/不带历史，全部通过。所以这里必须用真机字节流。
 *
 * 复刻的宽度序列取自 cell_diag.log 的 FRAME 行：
 *   F1  model=120   （启动，整屏）
 *   F4  model=59    （第一次分屏后 pane0）
 *   F64 model=19    （第一次拖窄松手）
 *   F110 model=70   （第二次拖宽松手）  <- 错位就在这一帧之后出现
 */
#include "common.h"
#include "types.h"
#include "screen.h"
#include "vt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int g_scrollback_lines = 1000;
MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0, g_search_match_cur = -1, g_search_active = 0;

/* 把 rel 行取成 UTF-8 文本（宽字符原样输出，便于肉眼核对）。 */
static int row_text(ScreenBuffer *s, int rel, char *out, int cap) {
    ScreenLine *ln = &s->lines[screen_phys_row(s, rel)];
    if (!ln->cells) return 0;
    int w = ln->used < ln->len ? ln->used : ln->len;
    int n = 0;
    for (int x = 0; x < w && n < cap - 5; x++) {
        WCHAR ch = ln->cells[x].Char.UnicodeChar;
        if (ch == 0) continue;                       /* 宽字符次格 */
        if (ch < 0x80) out[n++] = (char)ch;
        else if (ch < 0x800) { out[n++] = 0xC0 | (ch >> 6); out[n++] = 0x80 | (ch & 0x3F); }
        else { out[n++] = 0xE0 | (ch >> 12); out[n++] = 0x80 | ((ch >> 6) & 0x3F);
               out[n++] = 0x80 | (ch & 0x3F); }
    }
    out[n] = 0;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("用法: %s <pane0 字节流>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("打不开 %s\n", argv[1]); return 2; }
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    printf("回放 %zu 字节\n", n);

    /* 起始宽度可用 argv[2] 覆盖。真机链条是 120 -> 59（分屏）-> 19（第一次拖窄
     * 松手）-> 70（第二次拖宽松手），而且从 F64 起 conpty_cols=19，dir 的碎片是
     * 【在 19 列下产生】的。第一版在 59 列下喂完整条流，碎片形态与真机不同，
     * 没能复现；所以这里支持直接在 19 列下喂。 */
    int start_cols = (argc > 2) ? atoi(argv[2]) : 59;
    ScreenBuffer s;
    if (!screen_init(&s, start_cols, 32)) { printf("screen_init 失败\n"); return 1; }

    /* split_at > 0：前 split_at 字节在 start_cols 列下喂（模拟第一次 dir，之后
     * 经历 59->19 的 reflow），剩余部分在 19 列下喂（模拟 F64 之后 conpty_cols=19
     * 时产生的新输出，只经历 19->70 一次 reflow）。真机的历史正是这种混合宽度
     * 状态，而让全部内容都走两次 reflow 复现不出拼接。 */
    int split_at = (argc > 3) ? atoi(argv[3]) : 0;
    if (split_at > 0 && split_at < (int)n) {
        screen_process_output(&s, buf, split_at);
        printf("  第一段 %d 字节 @%d 列：hist=%d\n", split_at, start_cols, s.hist_lines);
        if (getenv("DUMP_STAGE1")) {
            printf("  --- 第一段之后（59 列）逐行 ---\n");
            for (int rel = -s.hist_lines; rel < s.rows; rel++) {
                char t[256];
                if (!row_text(&s, rel, t, sizeof t)) continue;
                if (!t[0]) continue;
                int pr = screen_phys_row(&s, rel);
                printf("    %+4d w=%d used=%2d |%s|\n", rel,
                       s.line_wrap ? s.line_wrap[pr] : -1, s.lines[pr].used, t);
            }
        }
        screen_resize(&s, 19, 32);
        printf("  resize->19：hist=%d\n", s.hist_lines);
        screen_process_output(&s, buf + split_at, (int)n - split_at);
        printf("  第二段 %d 字节 @19 列：hist=%d\n", (int)n - split_at, s.hist_lines);
    } else {
        screen_process_output(&s, buf, (int)n);
        printf("%d 列喂完：hist=%d\n", start_cols, s.hist_lines);
        if (start_cols != 19) {
            screen_resize(&s, 19, 32);
            printf("resize->19：hist=%d\n", s.hist_lines);
        }
    }
    /* resize 前把每行的 used / line_wrap / 内容打出来，定位哪条记录边界被错标成
     * 续行（line_wrap=1）。真机日志里没有 line_wrap，只能在本地看。 */
    if (getenv("DUMP_ROWS")) {
        printf("\n--- resize 前逐行（rel: wrap used | 内容）---\n");
        for (int rel = -s.hist_lines; rel < s.rows; rel++) {
            int pr = screen_phys_row(&s, rel);
            ScreenLine *ln = &s.lines[pr];
            char t[256]; row_text(&s, rel, t, sizeof t);
            int wr = s.line_wrap ? s.line_wrap[pr] : -1;
            printf("  %+4d: w=%d used=%2d |%s|\n", rel, wr, ln->used, t);
        }
    }

    screen_resize(&s, 70, 32);
    printf("resize->70：hist=%d\n", s.hist_lines);

    /* 检查：任何一行里同时出现两条不同记录的日期前缀 = 跨记录拼接。 */
    static const char *dates[] = {
        "2026/08/23", "2026/09/07", "2026/09/05", "2025/12/23",
        "2026/09/13", "2026/09/15", "2026/07/24",
    };
    int nd = (int)(sizeof dates / sizeof dates[0]);
    int bad = 0;
    char line[512];
    for (int rel = -s.hist_lines; rel < s.rows; rel++) {
        if (!row_text(&s, rel, line, sizeof line)) continue;
        int hits = 0;
        for (int k = 0; k < nd; k++) if (strstr(line, dates[k])) hits++;
        if (hits >= 2) {
            printf("  [拼接] rel=%+d hits=%d | %s\n", rel, hits, line);
            bad++;
            if (bad > 8) break;
        }
    }

    screen_free(&s);
    if (bad) { printf("\n[FAIL] %d 行含跨记录拼接（真机症状复现）\n", bad); return 1; }
    printf("\n[PASS] 没有跨记录拼接\n");
    return 0;
}
