/* tests/test_gfx_relay.c —— v2.3.6：图形协议直通的状态机回归。
 *
 * 为什么单独一格：真正的转发（锁内取出 + 帧尾 host_write）在 tests/verify_pane_palette.py
 * 的 T18 里用真 pty 验；但那一格要 400 秒、还依赖 libvterm（缺了就 SKIP）。而这一格
 * 只管「采集」这一段：认出 sixel(DCS q) / kitty(APC G) / iTerm2(OSC 1337 + File=) 三种
 * 序列并整段原样入队，其余一律不入队。这一段出的错都是「静默消失」型的（本项目第一版
 * 就因为状态号跳到一个不存在的 case 上，kitty 整条没了），编译期什么都看不出来，
 * 却能在几毫秒里被下面这几行断言抓住。
 *
 * 不链 src/config.c：与 tests/cascade_probe.c 同一套源文件（screen/vt/utf8/theme），
 * 配置项在这里按需定义。
 */
#include "common.h"
#include "types.h"
#include "config.h"
#include "screen.h"
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_scrollback_lines = 1000;
MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0, g_search_match_cur = -1, g_search_active = 0;

static int g_ok = 0, g_fail = 0;
static void ck(int cond, const char *name) {
    if (cond) { g_ok++; printf("  ok   %s\n", name); }
    else      { g_fail++; printf("  FAIL %s\n", name); }
}

static ScreenBuffer S;

/* 把队列清空、状态机复位。每个子用例前后都要做一次，否则上一例的残留会把下一例蒙对。 */
static void gfx_reset(void) {
    for (int k = 0; k < GFX_RELAY_QUEUE; k++) {
        free(S.gfx_q[k].p);
        S.gfx_q[k].p = NULL; S.gfx_q[k].len = 0;
    }
    S.gfx_qn = 0;
    S.gfx_state = 0; S.gfx_cur_len = 0; S.gfx_oversize = 0;
    S.gfx_sawfile = 0; S.gfx_escpend = 0;
}

static void feed(const char *b, int n) { screen_process_output(&S, b, n); }
static void feed_str(const char *b) { feed(b, (int)strlen(b)); }

/* 逐字节喂：真实读路径每次 read 到的边界是不定的，一条序列被切在任何位置都必须等价。 */
static void feed_bytes(const char *b) {
    int n = (int)strlen(b);
    for (int i = 0; i < n; i++) feed(b + i, 1);
}

/* 队列第 k 条是否恰为 b（逐字节相同、且没有多余条目混进来） */
static int item_is(int k, const char *b) {
    int n = (int)strlen(b);
    if (k < 0 || k >= S.gfx_qn) return 0;
    if (!S.gfx_q[k].p || S.gfx_q[k].len != n) return 0;
    return memcmp(S.gfx_q[k].p, b, (size_t)n) == 0;
}

/* 队列里恰好一条、且与原字节完全一致 ⇒ 1 */
static int one_identical(const char *b) {
    int n = (int)strlen(b);
    if (S.gfx_qn != 1) return 0;
    if (!S.gfx_q[0].p || S.gfx_q[0].len != n) return 0;
    return memcmp(S.gfx_q[0].p, b, (size_t)n) == 0;
}

/* rel 行（0 基，视口内）上的文本。用来确认「畸形序列没被当文字画出来」。 */
static int row_text(int rel, char *out, int cap) {
    int pr = screen_phys_row(&S, rel);
    int n = 0;
    out[0] = 0;
    if (pr < 0 || pr >= S.total_lines) return 0;
    ScreenLine *ln = &S.lines[pr];
    if (!ln->cells) return 0;
    int w = ln->used < ln->len ? ln->used : ln->len;
    if (w > S.cols) w = S.cols;
    for (int x = 0; x < w && n < cap - 5; x++) {
        WCHAR ch = ln->cells[x].Char.UnicodeChar;
        if (ch == 0) continue;
        if (ch < 0x80) out[n++] = (char)ch;
        else if (ch < 0x800) { out[n++] = (char)(0xC0 | (ch >> 6)); out[n++] = (char)(0x80 | (ch & 0x3F)); }
        else { out[n++] = (char)(0xE0 | (ch >> 12)); out[n++] = (char)(0x80 | ((ch >> 6) & 0x3F));
               out[n++] = (char)(0x80 | (ch & 0x3F)); }
    }
    out[n] = 0;
    return n;
}

int main(void) {
    static const char SIX[] = "\x1bP0;1;0q\"1;1;11;13#5;2;97;11;23#5!11~-!11~\x1b\\";
    static const char SIXBEL[] = "\x1bP4;23q#0;2;0;0;0#3;2;245;0;0#0!16~\x07";
    static const char KIT[] = "\x1b_Ga=T,f=24,i=7,m=0;iVBORw0KGgoAAAANSUhEUg\x1b\\";
    static const char KITBEL[] = "\x1b_Ga=d,d=I,i=7\x07";
    static const char ITE[] = "\x1b]1337;File=inline=1;size=8;width=100;aGVsbG8td29ybGQ=\x07";
    static const char ITE2[] = "\x1b]1337;File=inline=1:iVBORw0KGgo\x1b\\";
    static const char TITLE[] = "\x1b]0;bash on host\x07";
    static const char RQSS[] = "\x1bP0$r1;1$\x1b\\";          /* DECRQSS：也是 DCS，但不是 sixel */
    static const char HALF[] = "\x1bP0;1;0q\"1;1;11;13#5;2;97;11;23";  /* 没有结尾 */
    static const char BRK[] = "\x1bP0;1;0q#0;2;0;0;0\x1b[1;1H 中途插了个 CSI"; /* 采集途中出现别的 ESC */
    char big[9000];
    char row[512];

    if (!screen_init(&S, 100, 24)) { printf("screen_init 失败\n"); return 1; }
    g_graphics_relay = 1;
    g_graphics_max_kb = 32768;

    /* --- 1. 三种协议都要整段原样入队 ---------------------------------------- */
    gfx_reset(); feed_str(SIX);      ck(one_identical(SIX),    "sixel(DCS q, ST 收尾) 原样入队");
    gfx_reset(); feed_str(SIXBEL);    ck(one_identical(SIXBEL), "sixel(BEL 收尾) 原样入队");
    gfx_reset(); feed_str(KIT);       ck(one_identical(KIT),    "kitty(APC G, ST 收尾) 原样入队");
    gfx_reset(); feed_str(KITBEL);    ck(one_identical(KITBEL), "kitty(BEL 收尾) 原样入队");
    gfx_reset(); feed_str(ITE);       ck(one_identical(ITE),    "iTerm2(OSC 1337 File=, BEL) 原样入队");
    gfx_reset(); feed_str(ITE2);      ck(one_identical(ITE2),   "iTerm2(OSC 1337 File=, ST) 原样入队");
    /* kitty 多块传输：本项目不重组，每块各发各的 —— 关键是每块都得原样到宿主。 */
    gfx_reset();
    {
        const char *c1 = "\x1b_Ga=T,m=1;iVBORw0K\x1b\\";
        const char *c2 = "\x1b_Gm=0;AAAA\x1b\\";
        feed_str(c1); feed_str(c2);
        ck(S.gfx_qn == 2 && item_is(0, c1) && item_is(1, c2), "kitty 分块：两块各入队（本项不重组，原样交给宿主）");
    }

    /* --- 2. 切断在任意字节边界都必须等价 ------------------------------------ */
    gfx_reset();
    {   /* 先完整喂一次拿基准长度，再逐字节喂一次比对 */
        int n = (int)strlen(SIX);
        feed_bytes(SIX);
        ck(one_identical(SIX), "同一条 sixel 逐字节喂入，结果与整块喂一致");
        ck(n == S.gfx_q[0].len, "长度对得上");
    }
    gfx_reset(); feed_bytes(KIT);  ck(one_identical(KIT),   "kitty 逐字节喂入也能完整入队");
    gfx_reset(); feed_bytes(ITE);  ck(one_identical(ITE),   "iTerm2 逐字节喂入也能完整入队");

    /* --- 3. 认不出的一律不入队，且绝不发半截 -------------------------------- */
    gfx_reset(); feed_str(TITLE);   ck(S.gfx_qn == 0, "窗口标题 OSC 0 不入队");
    gfx_reset(); feed_str(RQSS);    ck(S.gfx_qn == 0, "DECRQSS 这类非 sixel 的 DCS 不入队");
    gfx_reset(); feed_str(HALF);    ck(S.gfx_qn == 0, "半截 DCS 不入队（宁可不发，也不发半张图）");
    gfx_reset(); feed_str(HALF); feed_str(SIX);
    /* 半截没有结尾，后面那条 sixel 的起始 ESC 会被当成「采集中途冒出别的 ESC」⇒
     * 两段一起丢。这是有意的：认不准就什么都不发（发出半张图会把宿主的 sixel
     * 解析器停在半截上，之后每一行文字都被它当数据吃掉 —— 比看不到图糟得多）。*/
    ck(S.gfx_qn == 0, "半截 DCS 之后紧跟一条完整 sixel：两段都丢，宁缺毋滥");
    gfx_reset(); feed_str(HALF); feed_str("\x1b\\");
    ck(S.gfx_qn == 1, "半截 DCS 后来一个 ST：合成一条【有结尾的】DCS 发出去");
    {
        char *p = S.gfx_q[0].p; int n = S.gfx_q[0].len;
        ck(n >= 2 && p[0] == 0x1b && p[1] == 'P' && p[n-2] == 0x1b && p[n-1] == '\\',
           "入队的每一条都以 ST/BEL 收尾（绝不发没有结尾的 DCS）");
    }
    gfx_reset(); feed_str(BRK);     ck(S.gfx_qn == 0, "采集中途冒出别的 ESC ⇒ 整段丢");
    gfx_reset(); feed_str("\x1b_Xa=1;whatever\x1b\\"); ck(S.gfx_qn == 0, "非 kitty 的 APC 不入队");
    gfx_reset(); feed_str("\x1b]1337;NotFile=1\x07"); ck(S.gfx_qn == 0, "1337 但没有 File= ⇒ 不当图片");

    /* 大载荷但不是图片（没有 File=）：必须靠 4KB 那道上限收手，不能一路吃到底 */
    big[0] = 0;
    memcpy(big, "\x1b]1337;Junk=", 12);
    memset(big + 12, 'A', 6000);
    big[6012] = 0;
    gfx_reset(); feed_str(big);     ck(S.gfx_qn == 0, "1337 里 4KB 还认不出 File= ⇒ 放弃（不吃上亿字节）");

    /* --- 4. 大小上限（ini: graphics_max，单位 KB）-------------------------- */
    g_graphics_max_kb = 1;      /* 1 KB：下面这条约 9 KB，必须整个被丢掉 */
    memcpy(big, "\x1b_Ga=T,f=100,m=0;", 17);
    big[17] = '\x89';
    memset(big + 18, 'A', (size_t)sizeof(big) - 19);
    big[sizeof(big) - 1] = 0;
    gfx_reset(); feed_str(big);     ck(S.gfx_qn == 0, "超过 graphics_max 的 kitty 整段丢弃");
    gfx_reset(); feed_str(SIX);     ck(one_identical(SIX), "同上限下的小 sixel 照常入队");
    g_graphics_max_kb = 32768;

    /* --- 5. 开关关掉 ⇒ 一条都不收，且不能把文字带坏 ------------------------ */
    g_graphics_relay = 0;
    gfx_reset(); feed_str(SIX); feed_str(KIT); feed_str(ITE);
    ck(S.gfx_qn == 0, "graphics = off：三条都不入队");
    ck(S.gfx_state == 0, "graphics = off：状态机停在空闲（不会关到一半卡在采集中）");
    g_graphics_relay = 1;

    /* --- 6. 队列有上限，满了不阻塞不崩、留前面那些 ------------------------- */
    gfx_reset();
    for (int i = 0; i < GFX_RELAY_QUEUE + 6; i++) { feed_str(SIX); feed_str("\r\n"); }
    ck(S.gfx_qn == GFX_RELAY_QUEUE, "连喂 22 条：队列停在 16，不崩不阻塞");
    ck(S.gfx_qn > 0 && item_is(0, SIX), "满队时保住的是最先收到的那些完整条目");

    /* --- 7. 采集坐标（render 拿它换算宿主行）-------------------------------- */
    gfx_reset();
    feed_str("\x1b[2J\x1b[1;1H");        /* 光标归位：坐标断言与前面用例留下的位置无关 */
    feed_str("abc\r\n\r\n");
    feed_str(SIX);
    ck(S.gfx_qn == 1 && S.gfx_q[0].y == 2 && S.gfx_q[0].x == 0, "坐标记成「采集时的光标行列」");
    ck(S.gfx_q[0].hist == S.hist_lines, "顺手记下当时的历史行数（换算 host_row 要用）");

    /* --- 8. 畸形序列不能变成屏幕上的乱码 ------------------------------------ */
    gfx_reset();
    feed_str("\x1b[2J\x1b[1;1H");
    feed_str("MARK-");
    feed_str(HALF);
    feed_str("END\r\n");
    row_text(0, row, sizeof(row));
    ck(strstr(row, "MARK-") != NULL, "半截 DCS 之前的文字完好");
    ck(strstr(row, "#5;2;97") == NULL && strstr(row, "0;1;0q") == NULL,
       "半截 DCS 的载荷没被当文字画出来（主解析器照旧把它连同后文吞进 DCS，与真终端一致）");
    feed_str("\x1b\\");            /* 终于给了 ST：DCS 结束，后面恢复正常 */
    feed_str("AFTER\r\n");
    row_text(0, row, sizeof(row));
    ck(strstr(row, "AFTER") != NULL, "补上 ST 之后，同一行上的新文字正常显示");

    gfx_reset();
    feed_str(SIX);
    row_text(0, row, sizeof(row));
    ck(strstr(row, "P0;1;0q") == NULL && strstr(row, "#5;2;97") == NULL,
       "正常 sixel 也不会漏进屏幕文本（要么转发、要么丢弃，绝不重画）");

    gfx_reset();
    screen_free(&S);
    printf("gfx-relay: %d ok / %d FAIL\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
