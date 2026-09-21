/* ---------------------------------------------------------------------------
 * 渲染 harness —— 在 Linux 下直接跑 render.c 的真实渲染路径。
 *
 * 为什么需要它：拖动分屏导致的错乱（bug #14）此前只能靠真机日志反推，改一版
 * 猜一版。这个 harness 把 render_screen() 搬到本地，可以自己设终端尺寸、自己
 * 喂内容、自己看输出，A/B 对比锚定策略。
 *
 * 它复刻的是生产路径里最关键的一点：**拖动中模型被冻结**（bug #12 的
 * freeze_model 决定）。所以 harness 只改 g_mux.host_cols/rows（split_layout
 * 据此算出更窄的 PaneRect），而不调 pane_resize_to()，窗格模型尺寸保持原样。
 * 这正是 render.c:2559 `cols = min(rc->cols, s->cols)` 会截断的场景。
 *
 * 构建（见 tests/render_harness.sh）：
 *   gcc -Itests/stub -Iinclude src/{render,split,framediff,theme,screen,vt,utf8,
 *   keymap,input,config,main,cliphtml}.c tests/render_harness_shims.c \
 *       tests/render_harness.c -o /tmp/rh
 * ------------------------------------------------------------------------- */
#include "common.h"
#include "types.h"
#include "screen.h"
#include "vt.h"
#include "split.h"
#include "render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* stub 状态（定义在 render_harness_shims.c，声明在 tests/stub/windows.h） */
extern int g_stub_cols, g_stub_rows;
extern char *g_stub_out;
extern int g_stub_out_len, g_stub_out_cap;

static char outbuf[1 << 20];

static void feed(ScreenBuffer *s, const char *txt) {
    screen_process_output(s, txt, (int)strlen(txt));
}

/* 造 rows 行内容，每行形如 "|<行号>|<填充>|<尾标>"，行宽恰好 cols。
 * 尾标用可见字符，渲染后被截断就能一眼看出来。 */
/* content_cols > 模型宽时，写进去的行会被终端自动折行，line_wrap 置 1 ——
 * 这才是真机上「长命令输出」的形态，也是松手后 reflow 真正要处理的东西。 */
static void fill_rows_w(ScreenBuffer *s, int model_cols, int content_cols, int rows) {
    char line[1024];
    for (int r = 0; r < rows; r++) {
        char body[512];
        int bn = snprintf(body, sizeof body, "R%02d ", r);
        while (bn < content_cols - 3) body[bn++] = '.';
        body[bn++] = 'Z'; body[bn++] = 'Z'; body[bn] = 0;
        if (content_cols <= model_cols) {
            /* 内容装得下一行：用 CUP 绝对定位，得到干净的硬换行。 */
            snprintf(line, sizeof line, "\x1b[%d;1H%s", r + 1, body);
        } else {
            /* 内容比模型宽，必须让终端【自然】自动折行，line_wrap 才会被正确
             * 标成续行。这里绝不能用 CUP 固定行号 —— 一条 60 列的行在 22 列模型
             * 下占 3 个物理行，按 r+1 定位会让相邻行互相覆盖（2026-09-15 我自己
             * 就踩了这个坑，凭空造出「10 行被合并成 5 行」的假象）。 */
            snprintf(line, sizeof line, "%s\r\n", body);
        }
        feed(s, line);
    }
}

static void fill_rows(ScreenBuffer *s, int cols, int rows) {
    fill_rows_w(s, cols, cols, rows);
}

/* 宽字符版：复刻中文路径提示符（用户环境是中文 Windows）。
 * 每行形如  C:\\用户\\D00。。。。\\下载>  —— 含全角字符，占两格，折行判据与
 * 纯 ASCII 不同，是最容易出「错位」的形态。
 * 项目约定：中文一律用 raw UTF-8 字面量，不要转成 \xNN（转义写错过一次，
 * 结果是屏幕上出现字面的 "\xe7"）。 */
static void fill_rows_cjk(ScreenBuffer *s, int content_cols, int rows) {
    char line[1024];
    for (int r = 0; r < rows; r++) {
        char body[512];
        int bn = snprintf(body, sizeof body, "C:\\用户\\D%02d", r);
        /* 全角句点填充：每个占 2 列 */
        while (bn + 3 < content_cols - 8) bn += snprintf(body + bn, sizeof body - (size_t)bn, "。");
        snprintf(body + bn, sizeof body - (size_t)bn, "\\下载>");
        snprintf(line, sizeof line, "%s\r\n", body);
        feed(s, line);
    }
}

/* 列对齐敏感内容：每行 = 2 列缩进 + N 个全角字符 + 一个 ASCII 标记 '|'。
 * 若宽字符宽度算错（把全角当 1 列，或次格处理不当），'|' 就会左右偏，
 * 多行之间的 '|' 也不再对齐 —— 正是用户报的「整行横向偏移」。 */
static void fill_rows_align(ScreenBuffer *s, int wide_cnt, int rows) {
    char line[1024];
    for (int r = 0; r < rows; r++) {
        char body[512];
        int bn = snprintf(body, sizeof body, "%02d  ", r);   /* 行号 + 2 列缩进 */
        for (int k = 0; k < wide_cnt; k++)
            bn += snprintf(body + bn, sizeof body - (size_t)bn, "中");
        snprintf(body + bn, sizeof body - (size_t)bn, "|tail");
        snprintf(line, sizeof line, "%s\r\n", body);
        feed(s, line);
    }
}

int main(int argc, char **argv) {
    int host_cols = (argc > 1) ? atoi(argv[1]) : 120;
    int host_rows = (argc > 2) ? atoi(argv[2]) : 25;
    int narrow    = (argc > 3) ? atoi(argv[3]) : 66;   /* 拖动后的宿主宽 */
    int model_cols = (argc > 4) ? atoi(argv[4]) : 59;  /* 冻结的窗格模型宽 */
    int hist_lines = (argc > 5) ? atoi(argv[5]) : 0;   /* 额外喂多少行历史 */
    int scroll_off = (argc > 6) ? atoi(argv[6]) : 0;   /* 回看多少行 */
    /* 松手后把模型同步到的宽度（生产中 pane_resize_to -> screen_resize ->
     * screen_resize_reflow）。0 = 不做松手帧。 */
    int release_cols = (argc > 7) ? atoi(argv[7]) : 0;
    /* 每行内容宽度。大于 model_cols 就会自动折行（软换行），复刻真机上长命令
     * 输出的形态；默认等于 model_cols，即全是硬换行。 */
    int content_cols = (argc > 8) ? atoi(argv[8]) : 0;
    if (content_cols <= 0) content_cols = model_cols;
    int cjk_mode = (argc > 9) ? atoi(argv[9]) : 0;   /* 1 = 用中文路径内容 */

    memset(&g_mux, 0, sizeof g_mux);
    g_mux.running = 1;
    g_mux.hOut = (HANDLE)(long)1;
    g_mux.host_cols = host_cols;
    g_mux.host_rows = host_rows;
    g_mux.total_host_rows = host_rows + 1;

    /* 两个窗格，模型尺寸 model_cols x 20（分屏后每个窗格的实际大小） */
    int pane_rows = (host_rows - 1) / 2 - 2;
    if (pane_rows < 3) pane_rows = 3;
    for (int i = 0; i < 2; i++) {
        if (!screen_init(&g_mux.panes[i].screen, model_cols, pane_rows)) {
            printf("screen_init 失败\n");
            return 1;
        }
        g_mux.panes[i].active = 1;
        /* 先喂 hist_lines 行历史：它们会滚出可见区进 scrollback。
         * 行号从 0 连续编到 hist_lines+pane_rows-1，便于核对行序。 */
        if (hist_lines > 0) {
            char line[512];
            for (int r = 0; r < hist_lines; r++) {
                char body[256];
                int bn = snprintf(body, sizeof body, "R%03d ", r);
                while (bn < model_cols - 3) body[bn++] = '.';
                body[bn++] = 'Z'; body[bn++] = 'Z'; body[bn] = 0;
                snprintf(line, sizeof line, "%s\r\n", body);
                feed(&g_mux.panes[i].screen, line);
            }
        }
        if (cjk_mode == 2) fill_rows_align(&g_mux.panes[i].screen, 6, pane_rows);
        else if (cjk_mode) fill_rows_cjk(&g_mux.panes[i].screen, content_cols, pane_rows);
        else          fill_rows_w(&g_mux.panes[i].screen, model_cols, content_cols, pane_rows);
        g_mux.panes[i].scroll_offset = scroll_off;
    }
    g_mux.pane_count = 2;
    g_mux.active_pane = 0;

    /* 上下分屏树 */
    split_reset();
    split_init_tab(0);
    int root = split_root_for_tab(0);
    if (root < 0) { printf("split_init_tab 失败\n"); return 1; }
    if (split_do(root, SPLIT_H, 1) < 0) { printf("split_do 失败\n"); return 1; }

    /* 诊断：这些行是用 CUP 逐行写入的硬换行，line_wrap 应全为 0。
     * 若 reflow 仍把它们折断，说明折行没看 line_wrap —— 那就是「内容重复/
     * 行序混乱」的根因。 */
    {
        ScreenBuffer *s0 = &g_mux.panes[0].screen;
        printf("模型: cols=%d rows=%d hist=%d\n", s0->cols, s0->rows, s0->hist_lines);
        int wrapped = 0;
        for (int y = 0; y < s0->rows && y < 20; y++)
            if (s0->line_wrap && s0->line_wrap[y]) wrapped++;
        printf("可见区 line_wrap=1 的行数 = %d（期望 0，全是 CUP 硬换行）\n", wrapped);
        printf("reflow 后显示行数: 48列=%d  59列=%d\n",
               screen_reflow_height(s0, 48), screen_reflow_height(s0, 59));
        printf("scroll_offset=%d\n", scroll_off);
    }

    g_stub_out = outbuf;
    g_stub_out_cap = (int)sizeof outbuf;

    /* --- 第一帧：未拖动 --- */
    g_mux.host_cols = host_cols;
    g_stub_out_len = 0;
    render_screen();
    printf("=== 未拖动：host_cols=%d model_cols=%d ===\n", host_cols, model_cols);
    printf("输出 %d 字节；含尾标 ZZ 的行数 = %d\n",
           g_stub_out_len, (int)(strstr(outbuf, "ZZ") ? 1 : 0));
    /* 打印最后一条可见内容行，看是否完整 */
    {
        const char *p = strrchr(outbuf, 'Z');
        if (p) {
            int back = 0;
            while (back < 70 && p - back > outbuf && p[-back] != '\x1b') back++;
            printf("尾部片段: [%.*s]\n", back + 1, p - back);
        }
    }

    /* --- 第二帧：拖动变窄（只改 host，模型冻结）--- */
    g_mux.host_cols = narrow;
    g_stub_out_len = 0;
    render_screen();
    printf("\n=== 拖动变窄：host_cols=%d（模型仍 %d）===\n", narrow, model_cols);
    printf("输出 %d 字节；含尾标 ZZ = %s\n",
           g_stub_out_len, strstr(outbuf, "ZZ") ? "是" : "否（被截断）");
    {
        const char *p = strrchr(outbuf, 'Z');
        if (p) {
            int back = 0;
            while (back < 70 && p - back > outbuf && p[-back] != '\x1b') back++;
            printf("尾部片段: [%.*s]\n", back + 1, p - back);
        } else {
            /* 没有 Z，打印最后一行的原始片段 */
            int n = g_stub_out_len < 120 ? g_stub_out_len : 120;
            printf("末 %d 字节: [%.*s]\n", n, n, outbuf + g_stub_out_len - n);
        }
    }

    /* 把两帧原始输出落盘，交给 Python 终端仿真解析成网格。
     * （生产里 dump_render_output 由 main.c 的渲染循环调用，harness 不链
     * main.c，所以在这里自己写。） */
    if (getenv("TERMUX_DUMP")) {
        FILE *f = fopen("harness_frames.log", "wb");
        if (f) {
            g_mux.host_cols = host_cols;
            g_stub_out_len = 0; render_screen();
            fprintf(f, "FRAME tag=wide hcols=%d len=%d\n", host_cols, g_stub_out_len);
            fwrite(outbuf, 1, (size_t)g_stub_out_len, f); fputc('\n', f);

            g_mux.host_cols = narrow;
            g_stub_out_len = 0; render_screen();
            fprintf(f, "FRAME tag=narrow hcols=%d len=%d\n", narrow, g_stub_out_len);
            fwrite(outbuf, 1, (size_t)g_stub_out_len, f); fputc('\n', f);

            if (release_cols > 0) {
                /* 松手：生产中 pane_resize_to() 会调 screen_resize()，其内部走
                 * screen_resize_reflow() 把模型按新宽度重排。shim 里 pane_resize_to
                 * 是空的，所以在这里直接调 screen_resize 复刻同一条路径。 */
                for (int i = 0; i < 2; i++) {
                    screen_resize(&g_mux.panes[i].screen, release_cols, pane_rows);
                    g_mux.panes[i].scroll_offset = 0;
                }
                g_stub_out_len = 0; render_screen();
                fprintf(f, "FRAME tag=released hcols=%d len=%d\n",
                        g_mux.host_cols, g_stub_out_len);
                fwrite(outbuf, 1, (size_t)g_stub_out_len, f); fputc('\n', f);
                printf("\n三帧已写入 harness_frames.log（含松手后重排，模型 -> %d 列）\n",
                       release_cols);
            } else {
                printf("\n两帧已写入 harness_frames.log\n");
            }
            fclose(f);
        }
    }

    for (int i = 0; i < 2; i++) screen_free(&g_mux.panes[i].screen);
    return 0;
}
