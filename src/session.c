/* ---------------------------------------------------------------------------
 * v2.3.0：会话快照（`[general] session = on`）。记录格式与设计说明见 include/session.h。
 *
 * 三条要紧的取舍：
 *  1) 存的是【逻辑行】（软换行折下来的续行先并回去），不是屏幕物理行 —— 这样下次
 *     用不同宽度打开时历史由引擎自己 reflow，不会出现「一行被腰斩」或者半个汉字
 *     （见 screen.h 里 line_wrap 那段注释，本仓库为这类问题翻过好几次车）。
 *  2) 灌回去走 screen_process_output —— 与真机输出同一条路：软换行标记、宽字符、
 *     滚动都归引擎算，本模块不自己维护环形缓冲（自己再算一遍就是第二个真相）。
 *  3) 读档失败一律当「没有快照」，中途出错就停在这：宁可少恢复，不让启动挂掉。
 * ------------------------------------------------------------------------- */

#include "session.h"

#include "common.h"
#include "config.h"
#include "pane.h"
#include "platform.h"
#include "screen.h"
#include "split.h"
#include "types.h"
#include "utf8.h"
#include "vt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>          /* time() —— POSIX 那边被别的头顺带拉进来了，MinGW 不会：
                            * 少这一行就是 implicit-function-declaration，Windows 作业直接红。 */

#define SESS_MAX_BYTES      (6 * 1024 * 1024)   /* 快照文件上限：更大的直接不认 */
#define SESS_MAX_LINES      4000                /* 每个窗格最多存这么多逻辑行 */
#define SESS_MAX_LINE_BYTES 8000                /* 单条逻辑行的字节上限 */
#define SESS_PATH_NAME      L"termux.session"
#define SESS_HOME_NAME      L".termux.session"

/* 恢复时写在窗格最前面那行说明的前缀。写的和读的要认的是同一个串 ⇒ 只在这里写一次
 * （sess_feed 拼的就是「SESS_MARK_HEAD + 「N 行；…）」，存盘时按同一前缀把它剔掉）。 */
#define SESS_MARK_HEAD      "\xe2\x94\x80\xe2\x94\x80 \xe4\xb8\x8a\xe6\xac\xa1\xe4\xbc\x9a\xe8\xaf\x9d\xe7\x9a\x84\xe5\x8e\x86\xe5\x8f\xb2\xef\xbc\x88\xe5\x85\xb1 "
#define SESS_MARK_TAIL      " \xe8\xa1\x8c\xef\xbc\x9b\xe8\xbf\x9b\xe7\xa8\x8b\xe6\xb2\xa1\xe6\x9c\x89\xe7\x95\x99\xe5\x9c\xa8\xe5\x90\x8e\xe5\x8f\xb0\xef\xbc\x89\xe2\x94\x80\xe2\x94\x80" 

/* ---- 追加式小缓冲：写满上限就停止追加（宁少不错，绝不越界） ---- */
typedef struct { char *p; int len, cap; } SBuf;

static void sbuf_add(SBuf *b, const char *s, int n) {
    if (n <= 0 || b->len >= SESS_MAX_BYTES || b->len + n > SESS_MAX_BYTES) return;
    if (b->len + n > b->cap) {
        int nc = b->cap ? b->cap * 2 : 16384;
        while (nc < b->len + n && nc < SESS_MAX_BYTES) nc *= 2;
        if (nc > SESS_MAX_BYTES) nc = SESS_MAX_BYTES;
        if (nc < b->len + n) return;
        char *np = (char *)realloc(b->p, (size_t)nc + 1);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sbuf_str(SBuf *b, const char *s) { sbuf_add(b, s, (int)strlen(s)); }

static void sbuf_fmt(SBuf *b, const char *fmt, ...) {
    char tmp[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    sbuf_add(b, tmp, n);
}

/* 路径与 ini 用同一套定位（exe 旁边优先，其次主目录下的点文件），见 config_sibling_path。 */
static void sess_path(WCHAR *out, int out_len, int for_write) {
    config_sibling_path(out, out_len, for_write, SESS_PATH_NAME, SESS_HOME_NAME);
}

/* =========================================================================
 * 存
 * ========================================================================= */

/* 一个物理行的单元格 → 一行 UTF-8 文本（行尾空白去掉）。
 * NUL 与 C0 控制符当空格；落单的代理项丢掉 —— 直接扔给 WideCharToMultiByte 会让
 * 整行转换失败或者变成一串 '?'（Windows 的 WCHAR 是 UTF-16，一个 emoji 占两格；
 * POSIX 侧 WCHAR 是 32 位，那两个判断自然不成立，不必 #ifdef）。
 * 返回写入 out 的字节数（不含结尾 NUL）。 */
static int sess_row_text(ScreenBuffer *s, int ph, char *out, int out_max) {
    ScreenLine *ln = &s->lines[ph];
    out[0] = 0;
    int used = ln->used;
    if (used > s->cols) used = s->cols;
    if (used > ln->len) used = ln->len;          /* len = cells[] 的容量（见 screen_init） */
    if (used <= 0) return 0;
    WCHAR *w = (WCHAR *)malloc((size_t)(used + 1) * sizeof(WCHAR));
    if (!w) return 0;
    int n = 0;
    for (int i = 0; i < used; i++) {
        WCHAR c = ln->cells[i].Char.UnicodeChar;
        WCHAR pv = (i > 0) ? ln->cells[i - 1].Char.UnicodeChar : 0;
        int prev_high = (pv >= 0xD800 && pv <= 0xDBFF);
        /* 宽字符的【次格】不是内容（BMP 宽字符的次格写 0、emoji 写低代理 —— 判据与
         * screen.c 的 cell_is_wide_trail 同一条）。v2.3.0 第一版把它当空格，于是
         * 「汉」存成「汉 」，中文历史每存一轮胀一倍、恢复出来还满屏夹空格
         * （实测：说明行被存成「上 次 会 话」）⇒ 必须跳过。 */
        if (c >= 0xDC00 && c <= 0xDFFF && prev_high) continue;
        if (c == 0 && !prev_high && is_wide_cp((unsigned int)pv)) continue;
        if (c == 0 || c < 0x20 || c == 0x7F) { w[n++] = L' '; continue; }
        if (c >= 0xD800 && c <= 0xDBFF) {        /* 高代理：后面紧跟低代理才成对留下 */
            WCHAR d = (i + 1 < used) ? ln->cells[i + 1].Char.UnicodeChar : 0;
            if (d >= 0xDC00 && d <= 0xDFFF) { w[n++] = c; w[n++] = d; i++; }
            else w[n++] = L' ';
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) { w[n++] = L' '; continue; }   /* 落单的低代理 */
        w[n++] = c;
    }
    while (n > 0 && w[n - 1] == L' ') n--;       /* 行尾空白不算内容 */
    int got = 0;
    if (n > 0) {
        int need = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
        if (need > 0) {
            /* 缓冲区按【转换要的长度】开，再按 out_max 截：直接把 need 夹到装不下的
             * 长度再叫它转，WideCharToMultiByte 会返回 0（insufficient buffer），
             * 整行文本反而一个字都存不下来 —— 超长行应当截断，不该消失。 */
            char *dst = (char *)malloc((size_t)need + 1);
            if (dst) {
                int g = WideCharToMultiByte(CP_UTF8, 0, w, n, dst, need, NULL, NULL);
                if (g > 0) {
                    if (g > 0 && dst[g - 1] == 0) g--;    /* 两侧的实现都可能把结尾 NUL 计进长度 */
                    if (g > out_max - 1) g = out_max - 1;
                    memcpy(out, dst, (size_t)g);
                    got = g;
                }
                free(dst);
            }
        }
    }
    out[got] = 0;
    free(w);
    return got;
}

/* 从最后一行往前数逻辑行起点，最多 max 条，把起点（rel 行号）按正序写进 out[]。
 * 反向数 + 上限 ⇒ 代价与上限成正比，跟 scrollback 有多大无关（50 万行也一样快）。 */
static int sess_collect_starts(ScreenBuffer *s, int *out, int max) {
    int cnt = 0;
    for (int rel = s->rows - 1; rel >= -s->hist_lines && cnt < max; rel--)
        if (!s->line_wrap[screen_phys_row(s, rel)]) out[cnt++] = rel;
    for (int i = 0; i < cnt / 2; i++) {          /* 倒着数出来的，翻正 */
        int a = out[i];
        out[i] = out[cnt - 1 - i];
        out[cnt - 1 - i] = a;
    }
    return cnt;
}

/* 一个窗格的历史 → 若干条 `D ...`。两条剪枝：
 *  - 屏底部那几行空行不是「历史」（新窗格一屏空着，恢复出来只是让人以为上次会话真有
 *    那么多空行）⇒ 记下「最后一条非空行结束的位置」，收尾剪掉尾巴 ⇒ 空历史 = 零条 D
 *    记录 = 恢复时连说明行都不写；
 *  - 上一次恢复时那句说明行是【我们自己写的】，不是用户的历史 ⇒ 别再存回去，
 *    否则每开一次会话就多一行说明，档位会一直长。 */
static void sess_write_pane(ScreenBuffer *s, SBuf *b, char *tmp, int tmp_max) {
    int *starts = (int *)malloc((size_t)(SESS_MAX_LINES + 1) * sizeof(int));
    if (!starts) return;
    int n = sess_collect_starts(s, starts, SESS_MAX_LINES);
    int keep = b->len;
    int mlen = (int)strlen(SESS_MARK_HEAD);
    for (int i = 0; i < n; i++) {
        int to = (i + 1 < n) ? starts[i + 1] - 1 : s->rows - 1;
        int used = 0, head = 1, mark = 0;
        for (int rel = starts[i]; rel <= to; rel++) {
            int r = sess_row_text(s, screen_phys_row(s, rel), tmp, tmp_max);
            if (r <= 0) continue;
            if (head) {
                head = 0;
                if (r >= mlen && memcmp(tmp, SESS_MARK_HEAD, (size_t)mlen) == 0) { mark = 1; break; }
                sbuf_str(b, "D ");                 /* 确认这条要留，才动 b */
            }
            if (used + r > SESS_MAX_LINE_BYTES) break;
            sbuf_add(b, tmp, r);
            used += r;
        }
        if (mark) continue;                         /* 一条都没写出去，无需回滚 */
        if (used > 0) { sbuf_str(b, "\n"); keep = b->len; }
    }
    if (keep < b->len) { b->len = keep; if (b->p) b->p[keep] = 0; }
    free(starts);
}

static void sess_write_tree(SplitNode *nd, int node, SBuf *b, int depth) {
    if (node < 0 || depth > 24 || b->len >= SESS_MAX_BYTES) { sbuf_str(b, "p"); return; }
    if (nd[node].leaf) { sbuf_str(b, "p"); return; }
    sbuf_fmt(b, "%c%d", nd[node].dir == SPLIT_V ? 'v' : 'h', nd[node].frac_pct);
    sess_write_tree(nd, nd[node].a, b, depth + 1);
    sess_write_tree(nd, nd[node].b, b, depth + 1);
}

static void sess_leaf_list(SplitNode *nd, int node, int *out, int *cnt, int cap, int depth) {
    if (node < 0 || *cnt >= cap || depth > 24) return;
    if (nd[node].leaf) { out[(*cnt)++] = nd[node].pane_idx; return; }
    sess_leaf_list(nd, nd[node].a, out, cnt, cap, depth + 1);
    sess_leaf_list(nd, nd[node].b, out, cnt, cap, depth + 1);
}

/* 把【此刻还活着】的窗格与布局排成记录文本。返回 tab 数（0 = 此刻没东西可存）。
 * 「此刻还活着」很关键：窗格的 ScreenBuffer 是在 close_pane() 里 screen_free() 掉的，
 * 也就是说等主循环退出时历史早没了 —— 所以采集必须挂在窗格关闭的那一刻（见 session_note_now），
 * 退出时只做一次落盘。判据用 screen.lines != NULL 而不是 pane.active：reap 里 active 先被清 0，
 * 而那一刻屏还在。 */
static int sess_render(SBuf *b, int *leaves) {   /* b 必须是调用方新清零的 SBuf */
    char *tmp = (char *)malloc((size_t)SESS_MAX_LINE_BYTES + 256);
    if (!tmp) return 0;

    sbuf_str(b, "s 1\n");
    sbuf_fmt(b, "u %lld\n", (long long)time(NULL));

    int tabs = 0;
    if (leaves) *leaves = 0;
    EnterCriticalSection(&g_mux.cs);
    SplitNode *nd = split_nodes();
    for (int i = 0; i < g_mux.pane_count && b->len < SESS_MAX_BYTES; i++) {
        Pane *p = &g_mux.panes[i];
        if (!p->screen.lines || p->is_split_child || p->is_settings || p->is_about) continue;
        int root = split_root_for_tab(i);
        if (root < 0) continue;
        int lvs[MAX_PANES];
        int nl = 0;
        sess_leaf_list(nd, root, lvs, &nl, MAX_PANES, 0);
        if (nl < 1) continue;
        int focus = -1;
        for (int k = 0; k < nl; k++)
            if (lvs[k] == g_mux.active_pane) focus = k;
        sbuf_str(b, "T ");
        sbuf_fmt(b, "%d ", p->color);
        sess_write_tree(nd, root, b, 0);
        sbuf_str(b, "\n");
        for (int k = 0; k < nl; k++) {
            sbuf_str(b, "P ");
            sbuf_fmt(b, "%d\n", (k == focus) ? 1 : 0);
            if (lvs[k] >= 0 && lvs[k] < g_mux.pane_count)
                sess_write_pane(&g_mux.panes[lvs[k]].screen, b, tmp, SESS_MAX_LINE_BYTES);
        }
        tabs++;
    }
    LeaveCriticalSection(&g_mux.cs);
    free(tmp);
    return tabs;
}

/* ---- 内存里留着「最富」的一份：窗格一个一个关掉时，后一次采集只会更少，
 *      所以只在 tab 数不减少、字节数不减少时才替换（同宽时新的赢：还活着的窗格
 *      可能又多打了几百行）。 ---- */
static SBuf g_sess_keep;
static int g_sess_keep_tabs;
static int g_sess_keep_leaves;
static int g_sess_suppress;        /* 恢复期间别顺手把半成品记进 keep */

/* 先数一遍「现在有几个 tab、几个窗格」（只看结构，不碰文本），便宜到可以每次调用都做。
 * 作用是给下面的采集把关：主循环退出时是【一个窗格一个窗格关过来】的，每次都全量
 * 重排一遍文本就是 O(窗格²)，16 格 ×4000 行能卡住退出；结构已经比留着的那份少了，
 * 内容不可能更「富」⇒ 直接不看文本。 */
static void sess_count_live(int *tabs, int *leaves) {
    SplitNode *nd = split_nodes();
    *tabs = 0;
    *leaves = 0;
    for (int i = 0; i < g_mux.pane_count; i++) {
        Pane *p = &g_mux.panes[i];
        if (!p->screen.lines || p->is_split_child || p->is_settings || p->is_about) continue;
        int root = split_root_for_tab(i);
        if (root < 0) continue;
        int lvs[MAX_PANES], nl = 0;
        sess_leaf_list(nd, root, lvs, &nl, MAX_PANES, 0);
        if (nl < 1) continue;
        (*tabs)++;
        *leaves += nl;
    }
}

void session_note_now(void) {
    if (!g_session_persist || g_sess_suppress) return;
    if (g_sess_keep.p) {
        int tabs, leaves;
        EnterCriticalSection(&g_mux.cs);
        sess_count_live(&tabs, &leaves);
        LeaveCriticalSection(&g_mux.cs);
        if (tabs < g_sess_keep_tabs || leaves < g_sess_keep_leaves) return;
    }
    SBuf b;
    memset(&b, 0, sizeof(b));
    int nl = 0;
    int tabs = sess_render(&b, &nl);
    if (tabs < 1 || b.len <= 0 || !b.p) { free(b.p); return; }
    /* 「更富」= tab 数不少、字节数不少（同分时新的一份赢：还活着的窗格可能又多打了几百行）。 */
    if (g_sess_keep.p &&
        (tabs < g_sess_keep_tabs || b.len < g_sess_keep.len)) { free(b.p); return; }
    free(g_sess_keep.p);
    g_sess_keep = b;
    g_sess_keep_tabs = tabs;
    g_sess_keep_leaves = nl;
}

int session_save(void) {
    if (!g_session_persist) return 0;
    session_note_now();      /* 窗格还活着的话此刻再采一次（多半已是同一份） */
    if (!g_sess_keep.p || g_sess_keep.len <= 0) return 0;
    SBuf *b = &g_sess_keep;

    WCHAR path[MAX_PATH] = { 0 };
    sess_path(path, MAX_PATH, 1);
    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        /* exe 旁边写不了（Program Files 之类）⇒ 退到主目录，和 ini 同一个兜底 */
        const WCHAR *prof = plat_user_home();
        if (prof) {
            WCHAR alt[MAX_PATH] = { 0 };
            _snwprintf(alt, MAX_PATH - 1, L"%s" TERMUX_PATH_SEP_S SESS_HOME_NAME, prof);
            f = _wfopen(alt, L"wb");
        }
    }
    int ok = 0;
    if (f) {
        ok = (fwrite(b->p, 1, (size_t)b->len, f) == (size_t)b->len) ? 1 : 0;
        if (fclose(f) != 0) ok = 0;
    }
    free(g_sess_keep.p);                 /* 落盘后就地松开（别等进程退出，窗格多的时候这份东西不小） */
    g_sess_keep.p = NULL;
    g_sess_keep.len = g_sess_keep.cap = 0;
    g_sess_keep_tabs = g_sess_keep_leaves = 0;
    return ok;
}

/* =========================================================================
 * 恢复
 * ========================================================================= */

static int sess_default_shell(WCHAR *out, int out_cells) {
    const WCHAR *d;
#ifdef _WIN32
    d = TERMUX_DEFAULT_SHELL_W;
#else
    d = plat_default_shell();
#endif
    if (!d || !d[0]) return 0;
    int i = 0;                       /* 手抄一遍：MSVC 把 wcsncpy 标了 C4996 */
    while (d[i] && i < out_cells - 1) { out[i] = d[i]; i++; }
    out[i] = 0;
    return 1;
}

/* 数树里有几个叶子，顺带验语法（先验再动手，免得半路上创建出一堆空标签页）。
 * 返回叶子数；0 = 语法不对 / 超上限。budget 是「还容得下几个窗格」。 */
static int sess_tree_count(const char **pp, int depth, int budget) {
    if (depth > 24 || budget <= 0) return 0;
    char c = **pp;
    if (c == 'p') { (*pp)++; return 1; }
    if (c != 'v' && c != 'h') return 0;
    (*pp)++;
    int pct = 0, any = 0;
    while (**pp >= '0' && **pp <= '9') { pct = pct * 10 + (**pp - '0'); (*pp)++; any = 1; }
    if (!any || pct < 5 || pct > 95) return 0;
    int a = sess_tree_count(pp, depth + 1, budget);
    if (a <= 0) return 0;
    int b = sess_tree_count(pp, depth + 1, budget - a);
    if (b <= 0) return 0;
    return a + b;
}

/* 按树把 node（当前持有 cur_pane 的叶子）展开；新建出来的窗格依次记进 leaves[]。 */
static int sess_expand(int node, int cur_pane, const char **pp, int *leaves, int *nl,
                       SplitNode *nd) {
    char c = **pp;
    if (c == 'p') {
        (*pp)++;
        if (*nl >= MAX_PANES) return 0;
        leaves[(*nl)++] = cur_pane;
        return 1;
    }
    if (c != 'v' && c != 'h') return 0;
    (*pp)++;
    int pct = 0, any = 0;
    while (**pp >= '0' && **pp <= '9') { pct = pct * 10 + (**pp - '0'); (*pp)++; any = 1; }
    if (!any || pct < 5 || pct > 95) return 0;
    if (g_mux.pane_count >= MAX_PANES) return 0;
    WCHAR sh[256];
    memset(sh, 0, sizeof(sh));
    if (!sess_default_shell(sh, (int)(sizeof(sh) / sizeof(sh[0])))) return 0;
    int np = create_pane_shell(sh);
    if (np < 0) return 0;
    if (split_do(node, (c == 'v') ? SPLIT_V : SPLIT_H, np) < 0) { close_pane(np); return 0; }
    g_mux.panes[np].is_split_child = 1;
    nd[node].frac_pct = pct;
    if (!sess_expand(nd[node].a, cur_pane, pp, leaves, nl, nd)) return 0;
    if (!sess_expand(nd[node].b, np, pp, leaves, nl, nd)) return 0;
    return 1;
}

/* 文件先切成一行一条记录：解析时前进/退回都只是动下标，不用跟游标较劲。 */
typedef struct { char type; const char *body; } SRec;

static int sess_index_lines(char *raw, SRec **out) {
    int cap = 64, n = 0;
    SRec *r = (SRec *)malloc((size_t)cap * sizeof(*r));
    if (!r) { *out = NULL; return -1; }
    for (char *p = raw; *p; ) {
        char *s = p;
        while (*p && *p != '\n') p++;
        char *e = p;
        if (*p == '\n') *p++ = 0;
        while (e > s && e[-1] == '\r') *--e = 0;
        if (e == s) continue;
        const char *body = (const char *)s + 1;
        if (body < e && *body == ' ') body++;
        if (n >= cap) {
            int nc = cap * 2;
            SRec *nr = (SRec *)realloc(r, (size_t)nc * sizeof(*r));
            if (!nr) break;
            r = nr;
            cap = nc;
        }
        r[n].type = s[0];
        r[n].body = body;
        n++;
    }
    *out = r;
    return n;
}

/* 把某个窗格的快照文本灌回屏幕缓冲。锁只在这一句上（create_pane_shell 内部也要拿
 * 锁，见 wincompat.h 里那句「必须可重入」—— 可重入不等于该套着拿）。 */
static void sess_feed(Pane *p, SBuf *hist, int lines) {
    if (!p || !p->active || (lines <= 0 && hist->len <= 0)) return;
    SBuf f;
    memset(&f, 0, sizeof(f));
    sbuf_str(&f, "\x1b[m");                      /* 先清掉残留样式，别把整段历史染色 */
    if (lines > 0) sbuf_fmt(&f, "%s%d%s\r\n", SESS_MARK_HEAD, lines, SESS_MARK_TAIL);
    sbuf_add(&f, hist->p ? hist->p : "", hist->len);
    EnterCriticalSection(&g_mux.cs);
    screen_process_output(&p->screen, f.p, f.len);
    LeaveCriticalSection(&g_mux.cs);
    p->scroll_offset = 0;
    free(f.p);
}

int session_restore(void) {
    if (!g_session_persist) return 0;

    WCHAR path[MAX_PATH] = { 0 };
    sess_path(path, MAX_PATH, 0);
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        const WCHAR *prof = plat_user_home();
        if (!prof) return 0;
        WCHAR alt[MAX_PATH] = { 0 };
        _snwprintf(alt, MAX_PATH - 1, L"%s" TERMUX_PATH_SEP_S SESS_HOME_NAME, prof);
        f = _wfopen(alt, L"rb");
        if (!f) return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > SESS_MAX_BYTES) { fclose(f); return 0; }
    char *raw = (char *)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return 0; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;
    if (rd == 0) { free(raw); return 0; }

    /* 从现在起会动窗格（建 tab / 失败回滚都走 close_pane）⇒ 这段时间不许采集，
     * 免得把「恢复到一半」当成一份快照存下去。suppress 必须和这里的每一条 return
     * 一一对应地解开：v2.3.0 第一版把它设在上面对 `done:` 之外的好几个 return 之前，
     * 结果「本次没有快照」就把标志永久卡住，退出时的采集全被吞掉（实测存不出文件）。 */
    g_sess_suppress = 1;
    /* 声明必须全部排在第一条 goto 之前：跳进作用域会绕过初始化（gcc 的
     * -Wmaybe-uninitialized 就是抓这个的），done: 里读到的是栈垃圾。 */
    SRec *recs = NULL;
    int nrec = 0, ri = 0, tabs = 0, focus_pane = -1;
    nrec = sess_index_lines(raw, &recs);
    if (nrec < 1 || !recs) goto done;
    if (recs[ri].type != 's' || recs[ri].body[0] != '1') goto done;   /* 版本不认 ⇒ 整档不认 */
    ri++;
    while (ri < nrec) {
        if (recs[ri].type != 'T') { ri++; continue; }             /* u / 认不出的：跳过 */
        const char *tk = recs[ri].body;
        int color = 0;
        while (*tk >= '0' && *tk <= '9') { color = color * 10 + (*tk - '0'); tk++; }
        if (color < 0 || color > 8) color = 0;
        while (*tk == ' ') tk++;
        const char *chk = tk;
        if (sess_tree_count(&chk, 0, MAX_PANES) < 1 || *chk) { ri++; continue; }  /* 树坏了：这个 tab 不要 */
        ri++;

        int anchor;
        if (tabs == 0) {
            anchor = g_mux.active_pane;                            /* 启动时已经建好的那一个 */
            if (anchor < 0 || anchor >= g_mux.pane_count) goto done;
            SplitNode *nd = split_nodes();
            int root0 = split_root_for_tab(anchor);
            if (root0 < 0 || !nd[root0].leaf) goto done;           /* tab 0 不该有分屏 */
        } else {
            if (g_mux.pane_count >= MAX_PANES) goto done;
            WCHAR sh[256];
            memset(sh, 0, sizeof(sh));
            if (!sess_default_shell(sh, (int)(sizeof(sh) / sizeof(sh[0])))) goto done;
            anchor = create_pane_shell(sh);
            if (anchor < 0) goto done;
            split_init_tab(anchor);
        }
        g_mux.panes[anchor].color = color;
        int root = split_root_for_tab(anchor);
        if (root < 0) goto done;

        int leaves[MAX_PANES];
        int nl = 0;
        const char *tc = tk;
        if (!sess_expand(root, anchor, &tc, leaves, &nl, split_nodes()) || nl < 1) goto done;

        for (int k = 0; k < nl && ri < nrec; k++) {
            int flag = 0;
            if (recs[ri].type == 'P') {
                flag = recs[ri].body[0] == '1' ? 1 : 0;
                ri++;
            }
            if (flag) focus_pane = leaves[k];
            SBuf h;
            memset(&h, 0, sizeof(h));
            int lines = 0;
            while (ri < nrec && recs[ri].type == 'D') {
                if (recs[ri].body[0]) { sbuf_str(&h, recs[ri].body); sbuf_str(&h, "\r\n"); }
                else sbuf_str(&h, "\r\n");                        /* 空行也要占一行 */
                lines++;
                ri++;
            }
            sess_feed(&g_mux.panes[leaves[k]], &h, lines);
            free(h.p);
        }
        tabs++;
    }
done:
    g_sess_suppress = 0;
    free(recs);
    free(raw);
    if (tabs < 1) return 0;
    if (focus_pane >= 0 && focus_pane < g_mux.pane_count) switch_pane(focus_pane);
    g_mux.needs_redraw = 1;
    return tabs;
}
