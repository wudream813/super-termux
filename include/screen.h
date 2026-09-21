#ifndef WIN_TERMUX_SCREEN_H
#define WIN_TERMUX_SCREEN_H

#include "common.h"
#include "types.h"
#include "utf8.h"

int screen_init(ScreenBuffer *s, int cols, int rows);
void screen_free(ScreenBuffer *s);
int screen_resize(ScreenBuffer *s, int nc, int nr);
int screen_ensure_line(ScreenBuffer *s, int pr);

static inline int screen_phys_row(ScreenBuffer *s, int rel_row) {
    int r = (s->scroll_top + rel_row) % s->total_lines;
    if (r < 0) r += s->total_lines;
    return r;
}

static inline int screen_to_abs_row(ScreenBuffer *s, int cy, int vo) {
    if (s->in_alt_screen) return cy;
    return s->hist_lines - vo + cy;
}

CHAR_INFO *screen_cell(ScreenBuffer *s, int row, int col);
void screen_write_cell(ScreenBuffer *s, int row, int col, WCHAR ch, WORD attr);
void screen_erase_range(ScreenBuffer *s, int row, int sx, int ex, WORD attr);
void screen_scroll_up(ScreenBuffer *s, int top, int bottom, int count);
/* ConPTY viewport repaint 用：移动可见行但绝不写入/删除 scrollback。 */
void screen_scroll_viewport_up(ScreenBuffer *s, int count);
void screen_repaint_snapshot(ScreenBuffer *s);      /* 整屏重绘前存一份可见行 */
void screen_repaint_snapshot_free(ScreenBuffer *s);
/* 重绘后把提示符重新顶到底行。返回值：
 *   2 = 已重锚定（快照已释放，本次 resize 的重绘算处理完了）
 *   1 = 没动作，但【快照与 pending 都保留】：这趟重绘一直写到最后一行（tail<=0），
 *       是 conhost 两趟重绘的第一趟，真正要处理的是下一趟
 *   0 = 没动作，快照已释放（条件不成立 / 重锚定被关掉） */
int screen_repaint_reanchor(ScreenBuffer *s);
void screen_scroll_down(ScreenBuffer *s, int top, int bottom, int count);
void screen_newline(ScreenBuffer *s);
void screen_mark_softwrap(ScreenBuffer *s);  /* 标记当前行=软换行续行（v1.8.47 reflow） */
int screen_reflow_height(ScreenBuffer *s, int width); /* reflow 内容显示行总数（v1.8.52） */
int screen_scroll_limit(ScreenBuffer *s);             /* 回看 vo 上限（内容末贴视口顶），0=无 */

/* reflow 视图里的一个显示单元（字符 + 16 色属性 + 真彩）。 */
typedef struct {
    CHAR_INFO ci;
    WORD fg, bg;
    unsigned char v;
} RGlyph;

/* 生成滚动历史的 reflow 视图：把物理行按软换行标志合并成逻辑行、按 width 重新
 * 折行，返回向上回看 vo 个显示行时、视口 rows×width 的内容到 out（行主序）。
 * 返回有效行数；alt 屏/无 wrap 标志时返回 0（调用方回退到物理行直取）。 */
int screen_reflow_view(ScreenBuffer *s, int vo, int rows, int width, RGlyph *out);
/* 本地 reflow resize 后 ConPTY 会整屏重绘（重绘顶行按其自身滚动缓冲对齐）。若重绘的
 * 所有非空行与本地环从 rel=k(k>=1) 起逐行一致，把环前滚 k 行使 ConPTY 视口与本地对齐
 * （内容不变、历史行数增长），避免重绘覆盖吞行。命中并调整返回 1，否则 0/2。 */
int screen_repaint_align(ScreenBuffer *s, const char *data, int len);
void detect_conpty_width(ScreenBuffer *s, int written_len);
WORD build_attr(ScreenBuffer *s);
void cell_truecolor(ScreenBuffer *s, int row, int col, int ar, WORD *out_f, WORD *out_b, int *out_fv, int *out_bv);

/* 选区边界吸附到完整字符（避免选中半个宽字符）。line 为一行的 CHAR_INFO 单元格
 * （真实缓冲步长，勿传 WCHAR*），ncols 为宽度；返回夹紧/吸附后的列号。 */
int snap_right_to_char(const CHAR_INFO *line, int ncols, int x);
int snap_left_to_char(const CHAR_INFO *line, int ncols, int x);
/* 复制模式按字符移动：一次跨过整个宽字符，dir=+1 右 / -1 左。 */
int copy_step_char(const CHAR_INFO *line, int ncols, int x, int dir);
/* 复制模式光标列整字化：落在宽字符次格（半个字）则退到其主格。 */
int copy_cursor_to_lead(const CHAR_INFO *line, int ncols, int x);

#endif // WIN_TERMUX_SCREEN_H
