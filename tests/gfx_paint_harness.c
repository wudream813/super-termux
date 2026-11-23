/* tests/gfx_paint_harness.c —— 渲染层缺陷的定点回归（v2.3.7）。
 *
 * 为什么单独一格：这些只有把 render.c 真正跑起来、看它【写出去的字节】才验得到，
 * 而 verify_pane_palette.py 的 T18 要真 pty + 400 秒。这里用 tests/stub 的 Win32
 * 替身把渲染路径搬进普通进程，几百毫秒就能跑完。
 *
 * 判据：
 *   1. 图形直通后光标归位：帧序必须是 帧体 → 图片 → 光标。图片带绝对 CUP 且不
 *      归位，顺序一颠倒宿主光标就停在图片锚点。
 *   2. visible_now / render_pane_should_wake()：分屏里非活动但可见的窗格要能点
 *      重绘，zoom 盖住的 / 别的标签页的不点。
 *   3. 分屏逐格渲染时 emoji 的次格（低代理）不写字节（否则右半格被空格擦掉）。
 *   4. 单窗格无历史时不画滚动条；有历史时照常画。
 *
 * 编译（见 verify_gfx_paint.py）：
 *   gcc -Itests/stub -Iinclude src/{render,split,framediff,theme,screen,vt,utf8,keymap,
 *       input,config,cliphtml}.c tests/render_harness_shims.c tests/gfx_paint_harness.c
 */
#include "common.h"
#include "types.h"
#include "screen.h"
#include "vt.h"
#include "split.h"
#include "render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int g_stub_cols, g_stub_rows;
extern char *g_stub_out;
extern int g_stub_out_len, g_stub_out_cap;
extern int g_split_zoom;        /* src/input.c：zoom 只画活动窗格 */

static char outbuf[1 << 20];
static int failures = 0;

static void ck(int cond, const char *name) {
    if (cond) printf("  ok   %s\n", name);
    else { printf("  FAIL %s\n", name); failures++; }
}

static void feed(ScreenBuffer *s, const char *txt) {
    screen_process_output(s, txt, (int)strlen(txt));
}

/* 渲染一帧并把输出从头攒起；返回这一帧的字节数。 */
static int frame(void) {
    g_stub_out_len = 0;
    render_screen();
    return g_stub_out_len;
}

static int ends_with(const char *hay, const char *tail) {
    size_t h = strlen(hay), t = strlen(tail);
    return h >= t && memcmp(hay + h - t, tail, t) == 0;
}

/* 光标段：CUP + 显示光标。单窗格路径的位置是 (cursor_y + 2, cursor_x + 1)。 */
static void cursor_seq(char *buf, size_t cap, int cy, int cx) {
    snprintf(buf, cap, "\x1b[%d;%dH\x1b[?25h", cy + 2, cx + 1);
}

int main(void) {
    static const char SIX[] = "\x1bP0;1;0q#0;2;0;0;0#0~~\x1b\\";
    char cur[64];

    memset(&g_mux, 0, sizeof g_mux);
    g_mux.running = 1;
    g_mux.hOut = (HANDLE)(long)1;
    g_mux.host_cols = 60;
    g_mux.host_rows = 12;
    g_mux.total_host_rows = g_mux.host_rows + 1;
    g_stub_cols = g_mux.host_cols;
    g_stub_rows = g_mux.host_rows;
    g_stub_out = outbuf;
    g_stub_out_cap = (int)sizeof outbuf;

    if (!screen_init(&g_mux.panes[0].screen, 60, 12)) {
        printf("screen_init 失败\n");
        return 1;
    }
    g_mux.panes[0].active = 1;
    g_mux.active_pane = 0;
    g_mux.pane_count = 1;

    /* ---- 1) 图片在帧尾转发，光标必须【最后】落地 ---- */
    feed(&g_mux.panes[0].screen, "hello\r\n");
    feed(&g_mux.panes[0].screen, SIX);
    frame();
    ck(strstr(outbuf, SIX) != NULL, "sixel 原样转发到宿主");
    cursor_seq(cur, sizeof cur, g_mux.panes[0].screen.cursor_y,
               g_mux.panes[0].screen.cursor_x);
    ck(ends_with(outbuf, cur), "整帧末尾是 shell 光标段（图片之后光标已归位）");
    {
        const char *img = strstr(outbuf, SIX);
        const char *last = outbuf + strlen(outbuf) - strlen(cur);
        ck(img != NULL && img < last, "图片字节在光标段之前发出（帧体 → 图片 → 光标）");
    }

    /* ---- 2) graphics = off：一条都不转发，光标照常 ---- */
    g_graphics_relay = 0;
    feed(&g_mux.panes[0].screen, SIX);
    frame();
    ck(strstr(outbuf, "\x1bP0;1;0q") == NULL, "graphics=off 时不转发图片");
    cursor_seq(cur, sizeof cur, g_mux.panes[0].screen.cursor_y,
               g_mux.panes[0].screen.cursor_x);
    ck(ends_with(outbuf, cur), "graphics=off 时光标仍在帧尾");
    g_graphics_relay = 1;

    /* ---- 2b) 单窗格：没有可回看的行时不画滚动条（v2.3.7）---- */
    g_mouse_x = g_mux.host_cols - 1;   /* 鼠标贴着右缘：dist == 0 */
    g_mouse_y = 2;
    frame();
    /* 无历史时整条都是「滑块」：热态滑块画的是带底色的空格（095 色），不是 │。
     * 两种形态都不能出现。 */
    ck(strstr(outbuf, "\xe2\x94\x82") == NULL &&
       strstr(outbuf, "048;2;225;235;250") == NULL,
       "单窗格无历史：鼠标靠近右缘也不画滚动条（不盖住正文最后一列）");
    for (int i = 0; i < 40; i++) {     /* 反向对照：有历史时必须照常画 */
        char ln[32];
        snprintf(ln, sizeof ln, "line %d\r\n", i);
        feed(&g_mux.panes[0].screen, ln);
    }
    frame();
    ck(strstr(outbuf, "\xe2\x94\x82") != NULL,
       "单窗格有历史：鼠标靠近右缘照常画滚动条");
    g_mouse_x = -1; g_mouse_y = -1;

    /* ---- 3) visible_now / render_pane_should_wake ---- */
    if (!screen_init(&g_mux.panes[1].screen, 28, 5)) {
        printf("screen_init(pane1) 失败\n");
        return 1;
    }
    g_mux.panes[1].active = 1;
    g_mux.pane_count = 2;
    g_mux.active_pane = 0;
    split_reset();
    split_init_tab(0);
    {
        int root = split_root_for_tab(0);
        if (root < 0 || split_do(root, SPLIT_V, 1) < 0) {
            printf("split_do 失败\n");
            return 1;
        }
    }

    frame();
    ck(g_mux.panes[0].visible_now && g_mux.panes[1].visible_now,
       "分屏：两块窗格本帧都画了 -> visible_now 都是 1");
    ck(render_pane_should_wake(1) == 1, "非活动但可见的窗格来了输出 -> 点重绘");
    ck(render_pane_should_wake(0) == 1, "活动窗格 -> 点重绘");

    g_split_zoom = 1;
    frame();
    ck(g_mux.panes[1].visible_now == 0, "zoom：被盖住的窗格 visible_now = 0");
    ck(render_pane_should_wake(1) == 0, "zoom 时被盖住的窗格不白点重绘");
    g_split_zoom = 0;

    g_mux.active_pane = 1;
    frame();
    ck(render_pane_should_wake(1) == 1, "活动窗格恒为 1（与 visible_now 无关）");

    /* 别的标签页：把 pane1 标成不可见（切走后下一帧就会这样），再看判据 */
    g_mux.panes[1].visible_now = 0;
    ck(render_pane_should_wake(1) == 1, "活动窗格在别的标签页也算数（本来就是它）");
    g_mux.active_pane = 0;
    ck(render_pane_should_wake(1) == 0, "非活动 + 本帧没画 -> 不点重绘（别的标签页 / 隐藏）");

    /* ---- 4) 分屏逐格渲染：emoji 的次格（低代理）不得被写空格 ----
     * 模型里 emoji 是高低代理两格；显示端由主格那 4 个字节一次画出两列。分屏
     * 路径逐格定位，轮到低代理那格时若走「代理 -> 空格」兜底，就会在 emoji 的
     * 第二列盖一个空格，把右半擦掉（整屏路径早就用 x++ 跳过次格）。 */
    {
        static const char EMOJI[] = "\xF0\x9F\x98\x80";   /* 😀 U+1F600 */
        g_mouse_x = -1; g_mouse_y = -1;                   /* 不画滚动条，别干扰列号 */
        feed(&g_mux.panes[0].screen, "\x1b[2J\x1b[H");
        feed(&g_mux.panes[0].screen, EMOJI);
        frame();
        ck(strstr(outbuf, EMOJI) != NULL, "分屏：emoji 主格被画出来（4 字节原样）");
        PaneRect rects[MAX_PANES];
        memset(rects, 0, sizeof rects);
        int root = split_active_root();
        if (root >= 0)
            split_layout(root, 0, 0, g_mux.host_cols, g_mux.host_rows, split_nodes(), rects);
        if (rects[0].valid) {
            char cup[32];
            /* px=1 是 emoji 的次格：cc = c0 + 1，CUP 列 = cc + 1 */
            snprintf(cup, sizeof cup, "\x1b[%d;%dH", rects[0].r0 + 2, rects[0].c0 + 2);
            ck(strstr(outbuf, cup) == NULL,
               "分屏：emoji 次格没有写任何字节（右半格不被空格擦掉）");
        }
    }

    screen_free(&g_mux.panes[0].screen);
    screen_free(&g_mux.panes[1].screen);

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nGFX-PAINT CHECKS PASSED\n");
    return 0;
}
