/* ---------------------------------------------------------------------------
 * 滚动条拖动的鼠标捕获 harness（bug #28，2026-09-20）。
 *
 * 用户报：「滚动时光标移到另一个窗格会直接变成移动另一个窗格的滚动条」。
 *
 * 根因：滚动条拖动只有 g_sb_dragging / g_sb_grab_offset 两个状态，**没记是哪个
 * pane 在拖**。而 handle_split_mouse() 的窗格命中那一步，鼠标按着划过另一个窗格
 * 就会 switch_pane() 切焦点 —— 之后的 move 事件里 p = &g_mux.panes[active_pane]
 * 已经是新窗格，于是拿【原窗格算出来的 grab_offset】去拖【新窗格】的滚动条。
 *
 * 修法：拖动期间分屏层不切焦点（鼠标捕获），并把 pane 记进 g_sb_drag_pane。
 * 分隔条拖动本来就有同样的捕获（handle_split_mouse 第 1 步直接 return 1）。
 *
 * ⚠ 判据能成立的前提：tests/render_harness_shims.c 的 switch_pane() 必须真的改
 *   g_mux.active_pane。它原来是空实现 —— 那样这个 bug 在也测不出来（假绿）。
 *
 * 构建见 tests/verify_sb_drag.sh。
 * ------------------------------------------------------------------------- */
#include "common.h"
#include "types.h"
#include "screen.h"
#include "split.h"
#include "input.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void ck(const char *n, int cond) {
    if (!cond) { printf("[FAIL] %s\n", n); failures++; }
    else       printf("[ok]   %s\n", n);
}

static void feed(ScreenBuffer *s, const char *txt) {
    screen_process_output(s, txt, (int)strlen(txt));
}

/* 喂 rows 行，每行正好 cols 宽（硬换行），多出来的滚进 scrollback。 */
static void fill(ScreenBuffer *s, int cols, int rows) {
    char line[1024];
    for (int r = 0; r < rows; r++) {
        char body[512];
        int bn = snprintf(body, sizeof body, "R%03d ", r);
        while (bn < cols - 3) body[bn++] = '.';
        body[bn++] = 'Z'; body[bn++] = 'Z'; body[bn] = 0;
        snprintf(line, sizeof line, "%s\r\n", body);
        feed(s, line);
    }
}

#define LBTN FROM_LEFT_1ST_BUTTON_PRESSED

static void mev(MOUSE_EVENT_RECORD *me, int x, int y, DWORD flags, DWORD btn) {
    memset(me, 0, sizeof *me);
    me->dwMousePosition.X = (SHORT)x;
    me->dwMousePosition.Y = (SHORT)y;
    me->dwEventFlags = flags;
    me->dwButtonState = btn;
}

int main(void) {
    memset(&g_mux, 0, sizeof g_mux);
    g_mux.running = 1;
    g_mux.hOut = (HANDLE)(long)1;
    g_mux.host_cols = 80;
    g_mux.host_rows = 20;
    g_mux.total_host_rows = 21;
    g_mux.pane_count = 2;
    g_mouse_enabled = 1;

    /* 两个窗格：左 39 列、右 40 列（V 均分 80 列的结果），各 20 行模型，
     * 各喂 60 行 -> 有 scrollback，滚动条画得出来。 */
    int wc[2] = { 39, 40 };
    for (int i = 0; i < 2; i++) {
        if (!screen_init(&g_mux.panes[i].screen, wc[i], 20)) {
            printf("screen_init 失败\n");
            return 1;
        }
        g_mux.panes[i].active = 1;
        g_mux.panes[i].conpty_cols = wc[i];
        g_mux.panes[i].conpty_rows = 20;
        fill(&g_mux.panes[i].screen, wc[i], 60);
    }
    g_mux.active_pane = 0;

    split_reset();
    split_init_tab(0);
    ck("#28 左右分屏成功", split_split_active(SPLIT_V, 1) == 1);
    g_mux.active_pane = 0;

    PaneRect rs[MAX_PANES];
    memset(rs, 0, sizeof rs);
    split_layout(split_active_root(), 0, 0, g_mux.host_cols, g_mux.host_rows, split_nodes(), rs);
    ck("#28 两个窗格都有矩形", rs[0].valid && rs[1].valid);
    ck("#28 右窗格在左边窗格右侧", rs[1].oc0 > rs[0].oc0 + rs[0].ocols);
    ck("#28 两个窗格都有可回看历史",
       g_mux.panes[0].screen.hist_lines > 0 && g_mux.panes[1].screen.hist_lines > 0);

    /* 左窗格滚动条在 pane 本地最右列；控制台坐标 x = 本地列 + oc0，y = 本地行 + 1。 */
    int sb_x = rs[0].oc0 + g_mux.panes[0].screen.cols - 1;
    int in_p1_x = rs[1].oc0 + 5;      /* 右窗格内部的一列 */
    ck("#28 滚动条列落在左窗格内", sb_x >= rs[0].oc0 && sb_x < rs[0].oc0 + rs[0].ocols);

    MOUSE_EVENT_RECORD me;

    /* ---- 1) 在左窗格滚动条上按下 ---- */
    mev(&me, sb_x, 11, 0, LBTN);
    handle_mouse(&me);
    ck("#28 按下后进入滚动条拖动", g_sb_dragging == 1);
    ck("#28 拖动锁定在 pane 0", g_sb_drag_pane == 0);
    ck("#28 焦点仍是 pane 0", g_mux.active_pane == 0);
    /* 按下那一下就会把滑块跳到点击处，scroll_offset 已经不是 0 了；后面要比的是
     * 「有没有继续跟着动」，所以先把这个基准记下来。 */
    int vo_after_press = g_mux.panes[0].scroll_offset;

    /* ---- 2) 按着鼠标划进右窗格：焦点绝不能被切走 ---- */
    mev(&me, in_p1_x, 14, MOUSE_MOVED, LBTN);
    handle_mouse(&me);
    ck("#28 划过右窗格后焦点仍在 pane 0（没被 switch_pane）", g_mux.active_pane == 0);
    ck("#28 右窗格 scroll_offset 没被动过", g_mux.panes[1].scroll_offset == 0);
    ck("#28 拖动仍在进行", g_sb_dragging == 1);

    /* ---- 3) 继续在右窗格里移动：右窗格的条必须纹丝不动 ---- */
    mev(&me, in_p1_x, 18, MOUSE_MOVED, LBTN);
    handle_mouse(&me);
    ck("#28 右窗格里继续拖，右窗格 scroll_offset 仍是 0", g_mux.panes[1].scroll_offset == 0);
    ck("#28 焦点仍在 pane 0", g_mux.active_pane == 0);

    /* ---- 4) 划回左窗格：拖动应当照常作用于 pane 0 ---- */
    mev(&me, sb_x, 18, MOUSE_MOVED, LBTN);
    handle_mouse(&me);
    ck("#28 划回左窗格后 pane 0 的条真的在动",
       g_mux.panes[0].scroll_offset != vo_after_press);
    ck("#28 右窗格依旧没被碰", g_mux.panes[1].scroll_offset == 0);

    /* ---- 5) 在右窗格上方松手：拖动结束，焦点仍不切 ---- */
    mev(&me, in_p1_x, 18, 0, 0);
    handle_mouse(&me);
    ck("#28 松手后拖动结束", g_sb_dragging == 0);
    ck("#28 松手后 g_sb_drag_pane 复位", g_sb_drag_pane == -1);
    ck("#28 松手时鼠标在右窗格，焦点仍未被切走", g_mux.active_pane == 0);

    /* ---- 6) 捕获没有把正常的点击切焦点搞坏 ---- */
    mev(&me, in_p1_x, 10, 0, LBTN);
    handle_mouse(&me);
    ck("#28 不在拖动时，点右窗格正常切焦点", g_mux.active_pane == 1);

    /* ---- 7) 在窗格【外面】松手也必须结束拖动（否则 g_sb_dragging 会卡住，从此
     *        分屏层再也不切焦点）。旧代码的重置分支在坐标裁剪之后，走不到。
     *        注意不能用标签栏那一行：handle_mouse 在 my==0 时就走标签栏分支返回了
     *        （src/input.c:3835），事件根本到不了分屏层。用分隔线那一列 —— 它不
     *        属于任何窗格，handle_split_mouse 第 3 步会「点在边框上，吞掉」。 */
    int div_x = rs[0].oc0 + rs[0].ocols;
    g_mux.active_pane = 0;
    mev(&me, sb_x, 11, 0, LBTN);
    handle_mouse(&me);
    ck("#28 再次按下进入拖动", g_sb_dragging == 1);
    mev(&me, div_x, 12, 0, 0);     /* 分隔线那一列，任何窗格之外 */
    handle_mouse(&me);
    ck("#28 在窗格外松手后拖动结束", g_sb_dragging == 0);
    mev(&me, in_p1_x, 10, 0, LBTN);
    handle_mouse(&me);
    ck("#28 窗格外松手之后仍能正常切焦点", g_mux.active_pane == 1);

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nSB DRAG TESTS PASSED\n");
    return 0;
}
