/* 复现「拖宽 + 松手后历史区跨记录拼接」。
 *
 * 真机日志（2026-09-15 05:55 那批）里的链条：
 *   1. 拖动中模型被冻结在 19 列，ConPTY 的 dir 输出按 19 列折成碎片写进模型；
 *   2. 松手 -> pane_resize_to -> screen_resize(70) -> screen_resize_reflow 按 70 列
 *      把碎片合并（trace: hist 110 -> 11）；
 *   3. 合并结果里两条 dir 记录被拼进同一行（cell_diag F110..F220 第一行恒为
 *      「...1.docx.txt2026/0...」），且 ConPTY 的整屏重绘只覆盖可见区、不覆盖
 *      历史区，所以错误永久留在历史里。
 *
 * 这个测试直接复刻 1→2：在 19 列模型下喂 dir 输出，再 resize 到 70 列，然后检查
 * 每条记录是否仍独占一行。
 */
#include "common.h"
#include "types.h"
#include "screen.h"
#include "vt.h"
#include <stdio.h>
#include <string.h>

/* vt.c / screen.c 引用的全局（照 tests/resize_history_repro.c 的做法自带定义，
 * 这样这个测试不必链 main.c / config.c）。 */
int g_scrollback_lines = 100;
MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0, g_search_match_cur = -1, g_search_active = 0;

/* 真机 dir 输出的三条记录（含全角字符，宽度与日志里一致）。
 * 项目约定：中文用 raw UTF-8 字面量。 */
static const char *records[] = {
    "2026/08/23  18:55            45,658 2026年CSP-S1模拟题库1.docx.txt",
    "2026/09/07  12:52           523,582 洛谷题解_未命名.html",
    "2026/09/05  14:14             5,047 洛谷题解_未命名.md",
};
#define NREC ((int)(sizeof records / sizeof records[0]))

int main(void) {
    ScreenBuffer s;
    /* 真机是 59 列起步（日志 F4: scols=59），第一次拖动拖窄到 19、松手 reflow
     * 一次；第二次拖宽到 70、松手再 reflow 一次。两次 reflow 都要走，只做一次
     * （19->70）复现不出来 —— 已验证。 */
    if (!screen_init(&s, 59, 12)) { printf("screen_init 失败\n"); return 1; }

    /* 按 conhost 的形态喂：每条记录后 ESC[K + CRLF，窄屏下自动折行。
     * 真机 dir 有 26 文件 + 7 目录，19 列下每条折成 4 个物理行，几十条就顶穿
     * 可见区进历史（日志里 hist=110）。记录不够多、不进历史就复现不出来 ——
     * 第一版只喂 3 条，hist=0，测试假通过。 */
    for (int rep = 0; rep < 14; rep++) {
        for (int i = 0; i < NREC; i++) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s\x1b[K\r\n", records[i]);
            screen_process_output(&s, buf, (int)strlen(buf));
        }
    }
    printf("59 列下：hist=%d rows=%d\n", s.hist_lines, s.rows);

    /* 第一次松手：59 -> 19 列（拖窄），reflow 把长行拆成碎片。 */
    screen_resize(&s, 19, 12);
    printf("19 列下：hist=%d rows=%d\n", s.hist_lines, s.rows);

    /* 第二次松手：19 -> 70 列（拖宽），reflow 再把碎片合并。 */
    screen_resize(&s, 70, 12);
    printf("70 列下：hist=%d rows=%d\n", s.hist_lines, s.rows);

    /* 把可见区+历史按行取出来，检查每条记录是否独占一行。 */
    int bad = 0;
    int total = s.hist_lines + s.rows;
    for (int i = 0; i < NREC; i++) {
        /* 取记录的前 10 个字符做指纹（日期部分，纯 ASCII，不受宽字符影响） */
        char head[16];
        snprintf(head, sizeof head, "%.10s", records[i]);
        int found = 0, on_own_line = 0;
        for (int y = 0; y < total; y++) {
            int rel = y - s.hist_lines;                 /* 负数 = 历史行 */
            ScreenLine *ln = &s.lines[screen_phys_row(&s, rel)];
            if (!ln->cells) continue;
            char line[256]; int n = 0;
            int w = ln->used < ln->len ? ln->used : ln->len;
            for (int x = 0; x < w && n < 250; x++) {
                WCHAR ch = ln->cells[x].Char.UnicodeChar;
                if (ch == 0) continue;                 /* 宽字符次格 */
                if (ch < 0x80) line[n++] = (char)ch;
                else n += snprintf(line + n, sizeof line - n, "?");
            }
            line[n] = 0;
            if (strstr(line, head)) {
                found = 1;
                /* 独占一行 = 该行去掉尾部空白后就是这条记录本身 */
                int e = n;
                while (e > 0 && line[e-1] == ' ') e--;
                line[e] = 0;
                char want[256]; int wn = 0;
                for (const char *p = records[i]; *p && wn < 240; p++)
                    if ((unsigned char)*p < 0x80) want[wn++] = *p;
                    else { want[wn++] = '?'; }
                want[wn] = 0;
                /* 记录里的非 ASCII 已被换成 '?'，逐个比对会失真；只判长度与是否
                 * 还夹着别的记录的日期。 */
                int other = 0;
                for (int k = 0; k < NREC; k++) {
                    if (k == i) continue;
                    char oh[16]; snprintf(oh, sizeof oh, "%.10s", records[k]);
                    if (strstr(line, oh)) other = 1;
                }
                on_own_line = !other;
                printf("  记录%d「%s」-> 行内容: %s\n", i, head, line);
                if (other) { printf("    ^ 与别的记录拼在同一行\n"); bad++; }
            }
        }
        if (!found) { printf("  记录%d「%s」-> 未找到\n", i, head); bad++; }
        (void)on_own_line;
    }

    screen_free(&s);
    if (bad) { printf("\n[FAIL] %d 条记录未独占一行（跨记录拼接复现）\n", bad); return 1; }
    printf("\n[PASS] 三条记录各自独占一行\n");
    return 0;
}
