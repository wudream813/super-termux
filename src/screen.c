#include "screen.h"
#include "config.h"
#include <stdlib.h>

/* ---- resize 诊断 trace（TERMUX_DUMP=1 时启用，另见 main.c 的 dump 开关）----
 * 分屏拖动宽度时逐帧 resize，本地 reflow 与 ConPTY 重发交错，真机症状（历史
 * 顶部空白行 / 历史消失）无法在纯 screen 级 harness 复现。这里把每次 resize
 * 前后的尺寸 / hist / 环首尾行内容记到 screen_resize_trace.log，用于分辨是
 * reflow 逻辑错误还是上游状态（ConPTY 交错）已污染。 */
static int screen_trace_on(void) {
    static int init = 0, on = 0;
    if (!init) { on = getenv("TERMUX_DUMP") ? 1 : 0; init = 1; }
    return on;
}
static void screen_trace_row(FILE *f, ScreenBuffer *s, int rel) {
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) {
        fprintf(f, "  rel%+04d <noline>\n", rel);
        return;
    }
    ScreenLine *ln = &s->lines[pr];
    fprintf(f, "  rel%+04d w%d |", rel, s->line_wrap ? !!s->line_wrap[pr] : 0);
    for (int x = 0; x < ln->len && x < 64; x++) {
        unsigned c = (unsigned)ln->cells[x].Char.UnicodeChar;
        if (c == 0) fputc('#', f);
        else if (c == L' ') fputc('.', f);
        else if (c < 128) fputc((char)c, f);
        else fputc('?', f);
    }
    fputc('|', f);
    fputc('\n', f);
}
/* 全量 wrap 图 + 最长连续续行段（2026-09-17，bug #16 诊断）。
 *
 * 上面那几行样本只覆盖「环首 3 行 + 历史末 3 行 + 第 0 行 + 末行」，中间整段省略。
 * 而用户 2026-09-17 报的形态是【一整段】历史并成一条逻辑行：内容顺序完好、词被从
 * 中间切开（Music -> Mu|sic、Saved Games20|26-07-25），说明这一段里每一行的
 * line_wrap 都是 1，reflow 把它们并成一条后按新宽度重折。级联发生在历史【中间】，
 * 样本行照不到，所以下一批日志即使发回来也定位不了 —— 这里补全量图。
 *
 * 纯诊断：只读 line_wrap / used，不写任何状态，不改渲染与 reflow 行为。
 * 输出：
 *   wrapmap  每行一个字符（'1'=该行是上一行的软换行续行），每 64 个换一段并标 rel
 *   run      最长连续 '1' 段的长度、起始 rel，以及该段并成的逻辑行总字符数
 *            （>> 单条记录宽度即为级联；正常折行的长行不会跨记录） */
static void screen_trace_wrapmap(FILE *f, ScreenBuffer *s) {
    if (!s->line_wrap) return;
    int best_len = 0, best_rel = 0, run = 0, run_start = 0;
    int best_glyphs = 0;
    fprintf(f, "  wrapmap:\n");
    int i = 0;
    for (int rel = -s->hist_lines; rel < s->rows; rel++, i++) {
        int pr = screen_phys_row(s, rel);
        int w = (pr >= 0 && pr < s->total_lines && s->line_wrap[pr]) ? 1 : 0;
        if (i % 64 == 0) fprintf(f, "    rel%+05d ", rel);
        fputc(w ? '1' : '0', f);
        if (i % 64 == 63 || rel == s->rows - 1) fputc('\n', f);
        if (w) {
            if (run == 0) run_start = rel;
            run++;
            if (run > best_len) { best_len = run; best_rel = run_start; }
        } else run = 0;
    }
    /* 最长段并成的逻辑行有多少字符：段首的上一行（段起点）到段末，逐行取 used。 */
    if (best_len > 0 && s->lines) {
        int glyphs = 0;
        for (int rel = best_rel - 1; rel < best_rel - 1 + best_len + 1; rel++) {
            int pr = screen_phys_row(s, rel);
            if (pr < 0 || pr >= s->total_lines || !s->lines[pr].cells) continue;
            int u = s->lines[pr].used;
            if (u > s->cols) u = s->cols;
            if (u > 0) glyphs += u;
        }
        best_glyphs = glyphs;
    }
    fprintf(f, "  run: 最长连续续行段=%d 起于 rel%+05d 并成逻辑行约 %d 字符（cols=%d）\n",
            best_len, best_rel, best_glyphs, s->cols);
}

static void screen_trace_ring(const char *tag, ScreenBuffer *s, int nc, int nr, int hist_pre) {
    if (!screen_trace_on()) return;
    FILE *f = fopen("screen_resize_trace.log", "a");
    if (!f) return;
    fprintf(f, "--- [%s] %s %dx%d -> %dx%d  hist %d -> %d  scroll_top=%d  limit=%d  height=%d\n",
            tag, s->in_alt_screen ? "ALT" : "MAIN", s->cols, s->rows, nc, nr,
            hist_pre, s->hist_lines, s->scroll_top,
            s->in_alt_screen ? 0 : screen_scroll_limit(s),
            s->in_alt_screen ? 0 : screen_reflow_height(s, nc > 0 ? nc : s->cols));
    screen_trace_wrapmap(f, s);
    int shown = 0;
    for (int j = 0; j < 3 && -s->hist_lines + j < 0; j++) { screen_trace_row(f, s, -s->hist_lines + j); shown++; }
    if (s->hist_lines - shown > 6) fprintf(f, "  ... (hist 中间 %d 行略) ...\n", s->hist_lines - shown - 3);
    for (int j = 1; j <= 3 && s->hist_lines >= j; j++) screen_trace_row(f, s, -j);
    screen_trace_row(f, s, 0);
    screen_trace_row(f, s, s->rows - 1);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * 一行的四个并行数组：cells / fg_rgb / bg_rgb / rgb_valid
 *
 * 它们必须永远一起分配、一起搬运、一起清空。历史上已经两次栽在「改了前三个、
 * 漏掉最后一个」上（v1.5.0 的 screen_scroll_up 漏拷 rgb_valid、v1.8.10 的
 * screen_resize alt 屏迁移只搬了每行第 0 列）。所有搬运统一收口到下面这几个
 * 辅助函数，以后再加第五个并行数组也只需要改这里。
 * ------------------------------------------------------------------------- */
static void line_free(ScreenLine *ln) {
    free(ln->cells);
    free(ln->fg_rgb);
    free(ln->bg_rgb);
    free(ln->rgb_valid);
    ln->cells = NULL;
    ln->fg_rgb = NULL;
    ln->bg_rgb = NULL;
    ln->rgb_valid = NULL;
    ln->len = 0;
    ln->used = 0;
}

/* 把一整行填成空白（不碰分配状态）。 */
static void line_fill_blank(ScreenLine *ln, int n, WORD attr) {
    if (!ln->cells) return;
    ln->used = 0;
    for (int j = 0; j < n; j++) {
        ln->cells[j].Char.UnicodeChar = L' ';
        ln->cells[j].Attributes = attr;
        if (ln->fg_rgb) ln->fg_rgb[j] = RGB565_WHITE;
        if (ln->bg_rgb) ln->bg_rgb[j] = RGB565_BLACK;
        if (ln->rgb_valid) ln->rgb_valid[j] = 0;
    }
}

/* 分配一行并填成空白；任一数组失败就整行回滚并返回 0。 */
static int line_alloc(ScreenLine *ln, int n, WORD attr) {
    ln->cells = (CHAR_INFO *)malloc(n * sizeof(CHAR_INFO));
    ln->fg_rgb = (WORD *)malloc(n * sizeof(WORD));
    ln->bg_rgb = (WORD *)malloc(n * sizeof(WORD));
    ln->rgb_valid = (unsigned char *)calloc(n, 1);
    if (!ln->cells || !ln->fg_rgb || !ln->bg_rgb || !ln->rgb_valid) {
        line_free(ln);
        return 0;
    }
    ln->len = n;
    line_fill_blank(ln, n, attr);
    return 1;
}

/* 逐行搬运：四个数组各拷 n 个元素。 */
static void line_copy(ScreenLine *dst, const ScreenLine *src, int n) {
    if (!dst->cells || !src->cells || n <= 0) return;
    memcpy(dst->cells, src->cells, n * sizeof(CHAR_INFO));
    if (dst->fg_rgb && src->fg_rgb) memcpy(dst->fg_rgb, src->fg_rgb, n * sizeof(WORD));
    if (dst->bg_rgb && src->bg_rgb) memcpy(dst->bg_rgb, src->bg_rgb, n * sizeof(WORD));
    if (dst->rgb_valid && src->rgb_valid) memcpy(dst->rgb_valid, src->rgb_valid, n * sizeof(unsigned char));
    dst->used = src->used < n ? src->used : n;
}

/* alt 屏是扁平数组，按「行起点下标」搬运同样的四份数据。 */
static void alt_row_copy(CHAR_INFO *dc, WORD *dfr, WORD *dbr, unsigned char *dv, int di,
                         const CHAR_INFO *sc, const WORD *sfr, const WORD *sbr, const unsigned char *sv, int si,
                         int n) {
    if (n <= 0) return;
    if (dc && sc) memcpy(&dc[di], &sc[si], n * sizeof(CHAR_INFO));
    if (dfr && sfr) memcpy(&dfr[di], &sfr[si], n * sizeof(WORD));
    if (dbr && sbr) memcpy(&dbr[di], &sbr[si], n * sizeof(WORD));
    if (dv && sv) memcpy(&dv[di], &sv[si], n * sizeof(unsigned char));
}

int screen_ensure_line(ScreenBuffer *s, int pr) {
    if (!s->lines || pr < 0 || pr >= s->total_lines) return 0;
    if (s->lines[pr].cells) return 1;

    return line_alloc(&s->lines[pr], s->cols, s->current_attr ? s->current_attr : 0x07);
}

int screen_init(ScreenBuffer *s, int cols, int rows) {
    memset(s, 0, sizeof(*s));
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    s->cols = cols;
    s->rows = rows;
    s->total_lines = rows + g_scrollback_lines;
    s->cup_eol_row = -1;   /* 无待判定的「底行末列 CUP」；0 是合法行号 */
    s->current_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    s->fg_color = 7;
    s->bg_color = 0;

    s->lines = (ScreenLine *)calloc(s->total_lines, sizeof(ScreenLine));
    s->line_wrap = (unsigned char *)calloc(s->total_lines, 1);
    s->alt_buffer = (CHAR_INFO *)malloc(rows * cols * sizeof(CHAR_INFO));
    s->alt_fg_rgb = (WORD *)malloc(rows * cols * sizeof(WORD));
    s->alt_bg_rgb = (WORD *)malloc(rows * cols * sizeof(WORD));
    s->alt_rgb_valid = (unsigned char *)calloc(rows * cols, 1);

    if (!s->lines || !s->line_wrap || !s->alt_buffer || !s->alt_fg_rgb || !s->alt_bg_rgb || !s->alt_rgb_valid) {
        screen_free(s);
        return 0;
    }

    for (int i = 0; i < rows * cols; i++) {
        s->alt_fg_rgb[i] = RGB565_WHITE;
        s->alt_bg_rgb[i] = RGB565_BLACK;
        s->alt_buffer[i].Char.UnicodeChar = L' ';
        s->alt_buffer[i].Attributes = s->current_attr;
    }

    // Allocate initial visible rows
    for (int r = 0; r < rows; r++) {
        screen_ensure_line(s, r);
    }

    s->scroll_top = 0;
    s->cursor_visible = 1;
    s->auto_wrap = 1;
    s->scroll_region_top = 0;
    s->scroll_region_bottom = rows - 1;

    for (int i = 0; i < cols && i < 512; i += 8)
        s->tab_stops[i] = 1;
    return 1;
}

void screen_free(ScreenBuffer *s) {
    if (s->lines) {
        for (int i = 0; i < s->total_lines; i++) line_free(&s->lines[i]);
        free(s->lines);
        s->lines = NULL;
    }
    free(s->line_wrap); s->line_wrap = NULL;
    free(s->alt_buffer); s->alt_buffer = NULL;
    free(s->alt_fg_rgb); s->alt_fg_rgb = NULL;
    free(s->alt_bg_rgb); s->alt_bg_rgb = NULL;
    free(s->alt_rgb_valid); s->alt_rgb_valid = NULL;
    screen_repaint_snapshot_free(s);
    s->resize_repaint_pending = 0;
    s->resize_repaint_pass = 0;
}

CHAR_INFO *screen_cell(ScreenBuffer *s, int row, int col) {
    if (row < 0 || col < 0 || col >= s->cols) return NULL;
    if (s->in_alt_screen) {
        if (row >= s->rows) return NULL;
        return &s->alt_buffer[row * s->cols + col];
    }
    if (row >= s->rows) return NULL;
    int pr = screen_phys_row(s, row);
    if (!screen_ensure_line(s, pr)) return NULL;
    return &s->lines[pr].cells[col];
}

WORD build_attr(ScreenBuffer *s) {
    int fg = s->fg_color, bg = s->bg_color;
    if (s->reverse_video) { int t = fg; fg = bg; bg = t; }
    static const WORD ctab[16] = {
        0,
        FOREGROUND_RED,
        FOREGROUND_GREEN,
        FOREGROUND_RED | FOREGROUND_GREEN,
        FOREGROUND_BLUE,
        FOREGROUND_RED | FOREGROUND_BLUE,
        FOREGROUND_GREEN | FOREGROUND_BLUE,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
        FOREGROUND_INTENSITY,
        FOREGROUND_RED | FOREGROUND_INTENSITY,
        FOREGROUND_GREEN | FOREGROUND_INTENSITY,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
        FOREGROUND_BLUE | FOREGROUND_INTENSITY,
        FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
        FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
    };
    /* v1.8.15 修复：ctab[] 里存的是「前景」位标志（FOREGROUND_*，值 0..0xF），
     * 背景需要把同样的颜色位左移 4 位到 BACKGROUND_* 位置（如红 0x04<<4=0x40）。
     * 旧代码写成 (ctab[bg] >> 4) << 4——前景值右移 4 位恒为 0，导致所有走
     * 16/256 色 attr 的程序内容背景位永远是 0（黑背景）。colortool -c 的色块
     * 用的正是 ESC[48;5;Nm（量化进 attr），因此背景整片丢失。真彩（48;2;..）
     * 走 bg_rgb_on 分支不经这里，所以 win-termux 自身 UI 与真彩程序不受影响。 */
    WORD a = ctab[fg & 15] | (ctab[bg & 15] << 4);
    if (s->bold) a |= FOREGROUND_INTENSITY;
    if (s->underline) a |= COMMON_LVB_UNDERSCORE;
    return a;
}

void screen_write_cell(ScreenBuffer *s, int row, int col, WCHAR ch, WORD attr) {
    if (row < 0 || col < 0 || col >= s->cols) return;
    unsigned char v = (s->fg_rgb_on ? 1 : 0) | (s->bg_rgb_on ? 2 : 0);
    if (s->in_alt_screen) {
        if (row >= s->rows) return;
        s->alt_buffer[row * s->cols + col].Char.UnicodeChar = ch;
        s->alt_buffer[row * s->cols + col].Attributes = attr;
        if (s->alt_fg_rgb) {
            s->alt_fg_rgb[row * s->cols + col] = s->fg_rgb_on ? rgb565(s->fg_r, s->fg_g, s->fg_b) : RGB565_WHITE;
            s->alt_bg_rgb[row * s->cols + col] = s->bg_rgb_on ? rgb565(s->bg_r, s->bg_g, s->bg_b) : RGB565_BLACK;
            if (s->alt_rgb_valid) s->alt_rgb_valid[row * s->cols + col] = v;
        }
    } else {
        int pr = screen_phys_row(s, row);
        if (screen_ensure_line(s, pr)) {
            s->lines[pr].cells[col].Char.UnicodeChar = ch;
            s->lines[pr].cells[col].Attributes = attr;
            s->lines[pr].fg_rgb[col] = s->fg_rgb_on ? rgb565(s->fg_r, s->fg_g, s->fg_b) : RGB565_WHITE;
            s->lines[pr].bg_rgb[col] = s->bg_rgb_on ? rgb565(s->bg_r, s->bg_g, s->bg_b) : RGB565_BLACK;
            s->lines[pr].rgb_valid[col] = v;
            if (col + 1 > s->lines[pr].used) s->lines[pr].used = col + 1;
        }
    }
}

/* 擦除不是“输出空格”：保留 used 左侧真实尾随空格，但被擦除到行尾时收缩有效
 * 范围。不能直接循环 screen_write_cell，否则 ESC[K 会把整行 used 错标成 cols。 */
void screen_erase_range(ScreenBuffer *s, int row, int sx, int ex, WORD attr) {
    if (!s || row < 0 || row >= s->rows) return;
    if (sx < 0) sx = 0;
    if (ex >= s->cols) ex = s->cols - 1;
    if (sx > ex) return;
    if (s->in_alt_screen) {
        for (int x = sx; x <= ex; x++) screen_write_cell(s, row, x, L' ', attr);
        return;
    }
    int pr = screen_phys_row(s, row);
    if (!screen_ensure_line(s, pr)) return;
    ScreenLine *ln = &s->lines[pr];
    int old_used = ln->used;
    for (int x = sx; x <= ex; x++) screen_write_cell(s, row, x, L' ', attr);
    if (ex >= old_used - 1 && sx < old_used) ln->used = sx;
    else ln->used = old_used;
}

void screen_scroll_up(ScreenBuffer *s, int top, int bottom, int count) {
    if (count <= 0) return;
    if (top < 0) top = 0;
    if (bottom >= s->rows) bottom = s->rows - 1;
    if (bottom < top) return;
    if (count > bottom - top + 1) count = bottom - top + 1;

    if (s->in_alt_screen) {
        for (int i = top; i <= bottom - count; i++) {
            alt_row_copy(s->alt_buffer, s->alt_fg_rgb, s->alt_bg_rgb, s->alt_rgb_valid, i * s->cols,
                         s->alt_buffer, s->alt_fg_rgb, s->alt_bg_rgb, s->alt_rgb_valid, (i + count) * s->cols,
                         s->cols);
        }
        for (int i = bottom - count + 1; i <= bottom; i++)
            for (int j = 0; j < s->cols; j++)
                screen_write_cell(s, i, j, L' ', s->current_attr);
        return;
    }

    if (top == 0 && bottom == s->rows - 1) {
        int old_hist = s->hist_lines;
        s->hist_lines += count;
        int hist_cap = s->total_lines - s->rows;
        if (s->hist_lines > hist_cap) s->hist_lines = hist_cap;
        int dropped = (old_hist + count) - s->hist_lines;

        int pi = s->pane_index;
        if (pi >= 0 && pi < MAX_PANES && g_mux.panes[pi].active) {
            if (g_mux.panes[pi].scroll_offset > 0) {
                g_mux.panes[pi].scroll_offset += count;
                if (g_mux.panes[pi].scroll_offset > s->hist_lines)
                    g_mux.panes[pi].scroll_offset = s->hist_lines;
            }
            if (pi == g_mux.active_pane && g_search_active && g_search_match_count > 0 && dropped > 0) {
                int new_count = 0;
                int new_cur = -1;
                for (int m = 0; m < g_search_match_count; m++) {
                    g_search_matches[m].abs_y -= dropped;
                    if (g_search_matches[m].abs_y >= 0) {
                        if (m == g_search_match_cur) new_cur = new_count;
                        g_search_matches[new_count++] = g_search_matches[m];
                    }
                }
                g_search_match_count = new_count;
                /* BUG-9 (v1.8.11): 当前停留的匹配被滚出缓冲时，以前会跳到「最新」的
                 * 那一条，浏览位置从最老一端直接弹到最新一端。剔除总是从最老的一端
                 * 发生，所以存活项里的 index 0 恰好就是「原当前项之后最近的一条」。 */
                if (new_cur >= 0) {
                    g_search_match_cur = new_cur;
                } else if (new_count > 0) {
                    g_search_match_cur = 0;
                } else {
                    g_search_match_cur = -1;
                }
                if (g_search_match_count == 0) {
                    g_search_active = 0;
                }
            }
        }

        for (int c = 0; c < count; c++) {
            int pr = screen_phys_row(s, s->rows + c);
            if (s->lines && s->lines[pr].cells) {
                line_fill_blank(&s->lines[pr], s->cols, s->current_attr);
            }
            /* 该物理槽进入可见区作新行，软换行续行标志清零（它将是硬换行新行，
             * 直到屏幕 put 逻辑再次自动折行时由 screen_mark_softwrap 标记）。 */
            if (s->line_wrap) s->line_wrap[pr] = 0;
        }
        s->scroll_top = (s->scroll_top + count) % s->total_lines;
    } else {
        // Partial scroll
        for (int i = top; i <= bottom - count; i++) {
            int dst_pr = screen_phys_row(s, i);
            int src_pr = screen_phys_row(s, i + count);
            if (s->lines && s->lines[src_pr].cells) {
                screen_ensure_line(s, dst_pr);
                line_copy(&s->lines[dst_pr], &s->lines[src_pr], s->cols);
            } else if (s->lines && s->lines[dst_pr].cells) {
                line_fill_blank(&s->lines[dst_pr], s->cols, s->current_attr);
            }
        }
        for (int i = bottom - count + 1; i <= bottom; i++) {
            int pr = screen_phys_row(s, i);
            if (s->lines && s->lines[pr].cells) {
                line_fill_blank(&s->lines[pr], s->cols, s->current_attr);
            }
        }
    }
}

void screen_scroll_viewport_up(ScreenBuffer *s, int count) {
    if (!s || s->in_alt_screen || count <= 0 || s->rows <= 0) return;
    if (count > s->rows) count = s->rows;
    for (int y = 0; y < s->rows - count; y++) {
        int dp = screen_phys_row(s, y);
        int sp = screen_phys_row(s, y + count);
        if (s->lines && s->lines[sp].cells) {
            screen_ensure_line(s, dp);
            line_copy(&s->lines[dp], &s->lines[sp], s->cols);
        } else if (s->lines && s->lines[dp].cells) {
            line_fill_blank(&s->lines[dp], s->cols, s->current_attr);
        }
        if (s->line_wrap) s->line_wrap[dp] = s->line_wrap[sp];
    }
    for (int y = s->rows - count; y < s->rows; y++) {
        int pr = screen_phys_row(s, y);
        screen_ensure_line(s, pr);
        line_fill_blank(&s->lines[pr], s->cols, s->current_attr);
        if (s->line_wrap) s->line_wrap[pr] = 0;
    }
}

/* 整行是否空白（未分配的槽也算空白）。 */
static int screen_row_blank(ScreenBuffer *s, int rel) {
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) return 1;
    ScreenLine *ln = &s->lines[pr];
    int n = ln->len < s->cols ? ln->len : s->cols;
    for (int x = 0; x < n; x++) {
        WCHAR c = ln->cells[x].Char.UnicodeChar;
        if (c != 0 && c != L' ') return 0;
    }
    return 1;
}

void screen_repaint_snapshot_free(ScreenBuffer *s) {
    if (!s || !s->repaint_snap) return;
    for (int i = 0; i < s->repaint_snap_rows; i++) line_free(&s->repaint_snap[i]);
    free(s->repaint_snap);
    free(s->repaint_snap_wrap);
    s->repaint_snap = NULL; s->repaint_snap_wrap = NULL;
    s->repaint_snap_rows = 0; s->repaint_snap_cols = 0;
    s->repaint_snap_content = 0;
}

/* 整屏重绘开始前，把当前可见行存一份。 */
void screen_repaint_snapshot(ScreenBuffer *s) {
    if (!s || !s->lines) return;
    screen_repaint_snapshot_free(s);
    s->repaint_snap = (ScreenLine *)calloc((size_t)s->rows, sizeof(ScreenLine));
    if (!s->repaint_snap) return;
    /* wrap 标志必须和 cells 一起存：reanchor 要用它原样补回，否则补回的行会被当成
     * 硬换行新行，拖宽后就并不回原来的逻辑行。分配失败则整份快照作废（宁可少补）。 */
    s->repaint_snap_wrap = (unsigned char *)calloc((size_t)s->rows, 1);
    if (!s->repaint_snap_wrap) { screen_repaint_snapshot_free(s); return; }
    s->repaint_snap_rows = s->rows; s->repaint_snap_cols = s->cols;
    /* 记住重绘前本地内容流有多少行。这是「该不该重新锚定」的判据，比「有没有滚动
     * 历史」和「光标是否在底行」都准确：内容正好铺满窗格时 hist_lines == 0；而窗格刚
     * 被拖高时 cursor_y 也不再等于 rows-1 —— 两种情况下 conhost 的重绘照样会把本地还
     * 可见的顶部内容覆盖掉（真机日志：120x4 时 height=4，一次重绘后 height=1）。 */
    {
        int last = -1;
        for (int y = s->rows - 1; y >= 0; y--)
            if (!screen_row_blank(s, y)) { last = y; break; }
        s->repaint_snap_content = s->hist_lines + last + 1;
    }
    WORD fill_attr = s->current_attr ? s->current_attr : 0x07;
    for (int y = 0; y < s->rows; y++) {
        int pr = screen_phys_row(s, y);
        /* line_copy 要求 dst->cells 已分配，否则它会静默返回 —— 快照行必须先 alloc。 */
        if (!line_alloc(&s->repaint_snap[y], s->cols, fill_attr)) {
            s->repaint_snap_rows = y;      /* 截到已成功的那几行，宁可少补也不越界 */
            return;
        }
        if (s->lines[pr].cells) line_copy(&s->repaint_snap[y], &s->lines[pr], s->cols);
        s->repaint_snap_wrap[y] = (s->line_wrap && s->line_wrap[pr]) ? 1 : 0;
    }
}

/* resize 之后的第一次 ConPTY 整屏重绘结束时重新锚定。
 *
 * 真机症状（termux_dump.log 实测）：拖大分隔条后提示符停在半屏、下面一大片空行，
 * 而且滚动历史被一路吃掉（hist 26 → 0）。原因是 conhost 在窗口【增高】时不把滚动
 * 历史拉回来、只在下方补空行，所以它回给我们的整屏重绘只有「h 行内容 + (rows-h) 行
 * 空白」，末尾还把光标绝对定位在提示符那一行（真机是 ESC[11;26H，而窗格高 21 行）。
 * 照实画 ⇒ 提示符停在第 h 行、下面 rows-h 行空白；更糟的是下一次 resize 的 reflow
 * 只扫到光标行为止（scan_end），那 rows-h 行空白不算内容，于是 hist = tcount - nr
 * 每拖一次就少 rows-h 行。
 *
 * 处理：重绘结束后，若光标以下全是空白，就把重绘写进来的内容整体下移 tail 行、让它
 * 的底边（提示符）正好落在最后一行，上面空出来的 tail 行用【重绘前的可见行】补回 ——
 * 那正是 conhost 没发给我们的、更老的那几行。hist_lines 不动：内容一行都没丢，
 * 视图仍是连续的（等价于 Windows Terminal 长高时往上多显示历史）。 */
/* Plan B 开关（见 analysis/拖动错乱-根因与三种可选行为.md §19.5 杠杆 B）：
 * 关掉「重绘后把提示符顶回底行」的底部重锚定，让 conhost 的重绘照实落地。
 * 这正是 Windows Terminal 的立场 —— 它不把内容钉在底部，而是让自己的 viewport
 * 去对齐 conpty 认为的样子。microsoft/terminal PR #4354 明确指出「把内容钉在
 * 底部 + conpty 整屏重发」正是滚动缓冲被覆盖的成因。
 *
 * TERMUX_REANCHOR=0 关 / =1 开；未设置时用编译期默认（Plan B 的二进制定义
 * TERMUX_NO_REANCHOR，默认关）。做成运行时开关是为了在同一个二进制上交叉对比
 * A / B，不必重编。 */
static int screen_reanchor_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("TERMUX_REANCHOR");
#ifdef TERMUX_NO_REANCHOR
        cached = 0;
#else
        cached = 1;
#endif
        if (v && *v) cached = (atoi(v) != 0);
    }
    return cached;
}

/* big 的文本（去尾随空格后）是否以 sml 的文本结尾，且 big 严格更长。
 *
 * 用途：conhost 自己的 reflow【不跨它 scrollback / viewport 的边界合并逻辑行】，
 * 所以它 resize 重绘的顶行往往只是本地那条完整记录的后半截。2026-09-20 真机
 * （uploads/render_dump.log，左窗格 61→5→92）：
 *     本地 reflow : 「2026-09-05  14:14             5,047 洛谷题解_未命名.md」
 *     conhost 重绘: 「题解_未命名.md」
 * 照实覆盖就会把前缀删掉。这种情况下保留本地那行。
 *
 * 宽字符在本地占 2 格、次格 UnicodeChar==0，两边都要跳过（口径必须一致，
 * 否则含汉字的行永远比不上 —— 同一个坑在 screen_row_text_eq 里已经栽过一次）。 */
static int line_ends_with_longer(const ScreenLine *big, const ScreenLine *sml) {
    if (!big || !sml || !big->cells || !sml->cells) return 0;
    int aw = big->used < big->len ? big->used : big->len;
    int bw = sml->used < sml->len ? sml->used : sml->len;
    int ai = aw - 1, bi = bw - 1;
    while (ai >= 0 && big->cells[ai].Char.UnicodeChar == L' ') ai--;
    while (bi >= 0 && sml->cells[bi].Char.UnicodeChar == L' ') bi--;
    int matched = 0;
    while (bi >= 0) {
        WCHAR bc = sml->cells[bi].Char.UnicodeChar;
        if (bc == 0) { bi--; continue; }
        while (ai >= 0 && big->cells[ai].Char.UnicodeChar == 0) ai--;
        if (ai < 0 || big->cells[ai].Char.UnicodeChar != bc) return 0;
        ai--; bi--; matched++;
    }
    if (matched <= 0) return 0;
    while (ai >= 0) {
        WCHAR ac = big->cells[ai].Char.UnicodeChar;
        if (ac != 0 && ac != L' ') return 1;      /* big 前面还有真内容 ⇒ 严格更长 */
        ai--;
    }
    return 0;
}

static void screen_trace_reanchor(ScreenBuffer *s, const char *why) {
    if (!screen_trace_on()) return;
    FILE *f = fopen("screen_resize_trace.log", "a");
    if (!f) return;
    fprintf(f, "--- [REANCHOR] %s  cols=%d rows=%d cur=(%d,%d) snap_content=%d snap_rows=%d snap_cols=%d hist=%d\n",
            why, s->cols, s->rows, s->cursor_x, s->cursor_y,
            s->repaint_snap_content, s->repaint_snap_rows, s->repaint_snap_cols, s->hist_lines);
    fclose(f);
}

int screen_repaint_reanchor(ScreenBuffer *s) {
    if (!s || s->in_alt_screen || s->rows <= 0 || !s->lines) return 0;
    /* Plan B：不做底部重锚定。快照仍要释放，否则内存一直挂着。 */
    if (!screen_reanchor_enabled()) { screen_trace_reanchor(s, "off(Plan B)"); screen_repaint_snapshot_free(s); return 0; }
    /* tail<=0 必须【最先】判，而且要连快照一起留住。
     *
     * conhost 的 resize 重绘在真机上是两趟（uploads/render_dump.log 2026-09-20，
     * 左窗格 61→5→92，termux_dump.log pane0 偏移 5884 / 6076）：
     *   趟 1: ESC[?25l ESC[H <5 行内容> ESC[29;1H ESC[?25h ESC[?25l
     *   趟 2: ESC[H <同样 5 行 + ESC[K> <24 趟 ESC[K CR LF> ESC[5;26H ESC[?25h
     * 趟 1 的光标停在最后一行 ⇒ tail==0、这趟本来就无事可做；但旧实现在这里
     * 连同 resize_repaint_pending 一起消费掉了，趟 2 的 ESC[H 于是不再取快照，
     * 重锚定拿不到「重绘前的可见行」，只能眼睁睁看着 24 行列表被 ESC[K 抹平。
     * 所以这里返回 1：快照和 pending 都留着，交给下一趟。 */
    if (s->rows - (s->cursor_y + 1) <= 0) { screen_trace_reanchor(s, "tail<=0 keep-for-next-pass"); return 1; }
    /* 只有【重绘前内容一直铺满到最后一行】才需要重新锚定（area = 整个窗格）：
     * 那种情形下 conhost 少发的那几行一定是更老的内容，重绘内容该贴窗格底边，上面
     * 空出的行用重绘前的可见行补回。
     *
     * 内容不足一屏时（repaint_snap_content < rows，新开的 cmd / 内容被拖窄后折行仍
     * 不满屏）【一律不动作】：此时内容是顶对齐的，conhost 少发几行只是它自己少画了，
     * 照实覆盖即可。曾经在这里用 area = min(rows, content) 继续下移，结果把重绘内容
     * 下移 (content-rep) 行、顶部又用快照补回同样的行 —— 同几行出现两遍、末尾几行被
     * 挤掉。真机左右分屏实测：左窗格 39 列时 banner 在第 2、3、9 行重复出现三次，
     * (c) 行和提示符整个消失（用户报的「渲染出现严重故障」）。
     *
     * 注意判据不能用 hist_lines>0：内容正好铺满窗格时 hist_lines == 0，而 conhost 把
     * 更老的行滚进它自己的滚动缓冲后只回画「提示符在顶 + 下方空白」，照实画会把本地
     * 还可见的顶部内容覆盖掉（真机日志：120x4 时 height=4，一次重绘后 height=1）。 */
    if (s->repaint_snap_content < s->rows) { screen_trace_reanchor(s, "snap_content<rows"); screen_repaint_snapshot_free(s); return 0; }
    int rep = s->cursor_y + 1;               /* conhost 这次画了几行内容 */
    int tail = s->rows - rep;
    if (tail <= 0) { screen_trace_reanchor(s, "tail<=0"); screen_repaint_snapshot_free(s); return 0; }
    for (int y = s->cursor_y + 1; y < s->rows; y++)
        if (!screen_row_blank(s, y)) { screen_trace_reanchor(s, "below-not-blank"); screen_repaint_snapshot_free(s); return 0; }  /* 下方有真实内容 */
    if (!s->repaint_snap || s->repaint_snap_rows != s->rows || s->repaint_snap_cols != s->cols) {
        screen_trace_reanchor(s, "no-snapshot"); screen_repaint_snapshot_free(s); return 0;
    }
    /* 可见内容整体下移 tail 行（倒序搬，源不会先被覆盖）；掉出底边的都是空白行。 */
    for (int y = s->rows - 1; y >= tail; y--) {
        int dp = screen_phys_row(s, y), sp = screen_phys_row(s, y - tail);
        if (!screen_ensure_line(s, dp)) { screen_repaint_snapshot_free(s); return 0; }
        /* 只有搬进来的是【重绘顶行】(y == tail) 时才可能被 conhost 截断：那是唯一
         * 跨它 scrollback/viewport 边界的一行。本地那行更完整就整行保留（连
         * line_wrap 一起），不要用后半截覆盖掉前缀。 */
        if (y == tail && y < s->repaint_snap_rows && s->repaint_snap[y].cells &&
            line_ends_with_longer(&s->repaint_snap[y], &s->lines[sp])) {
            /* 注意这里必须【从快照拷回来】而不是 continue：此刻 dp 那行已经被
             * conhost 的 ESC[K 抹成空白了，原地不动只会留一个空行。 */
            line_copy(&s->lines[dp], &s->repaint_snap[y], s->cols);
            if (s->line_wrap)
                s->line_wrap[dp] = (s->repaint_snap_wrap && y < s->repaint_snap_rows)
                                 ? s->repaint_snap_wrap[y] : 0;
            screen_trace_reanchor(s, "keep-longer-local-row");
            continue;
        }
        if (s->lines[sp].cells) line_copy(&s->lines[dp], &s->lines[sp], s->cols);
        else line_fill_blank(&s->lines[dp], s->cols, s->current_attr);
        if (s->line_wrap) s->line_wrap[dp] = s->line_wrap[sp];
    }
    /* 顶部空出的 tail 行 = 重绘前的最上面 tail 行（conhost 这次没发的那几行）。 */
    for (int y = 0; y < tail; y++) {
        int dp = screen_phys_row(s, y);
        if (!screen_ensure_line(s, dp)) { screen_repaint_snapshot_free(s); return 0; }
        if (s->repaint_snap[y].cells) line_copy(&s->lines[dp], &s->repaint_snap[y], s->cols);
        else line_fill_blank(&s->lines[dp], s->cols, s->current_attr);
        /* 原样恢复快照里的续行标志。这里曾经写死 0，理由是「补回的是重绘前的顶部行、
         * 应当是硬换行新行」—— 错了：那几行完全可能是上一行软换行折下来的续行，
         * 清 0 会让拖宽后的 reflow 把它们和上一行拆开（2026-09-17 真机复现）。 */
        if (s->line_wrap)
            s->line_wrap[dp] = (s->repaint_snap_wrap && y < s->repaint_snap_rows)
                             ? s->repaint_snap_wrap[y] : 0;
    }
    s->cursor_y += tail;
    if (s->cursor_y > s->rows - 1) s->cursor_y = s->rows - 1;
    screen_trace_reanchor(s, "applied");
    screen_repaint_snapshot_free(s);
    return 2;
}

void screen_scroll_down(ScreenBuffer *s, int top, int bottom, int count) {
    if (count <= 0) return;
    if (top < 0) top = 0;
    if (bottom >= s->rows) bottom = s->rows - 1;
    if (bottom < top) return;
    if (count > bottom - top + 1) count = bottom - top + 1;

    if (s->in_alt_screen) {
        for (int i = bottom; i >= top + count; i--) {
            alt_row_copy(s->alt_buffer, s->alt_fg_rgb, s->alt_bg_rgb, s->alt_rgb_valid, i * s->cols,
                         s->alt_buffer, s->alt_fg_rgb, s->alt_bg_rgb, s->alt_rgb_valid, (i - count) * s->cols,
                         s->cols);
        }
        for (int i = top; i < top + count && i <= bottom; i++) {
            for (int j = 0; j < s->cols; j++)
                screen_write_cell(s, i, j, L' ', s->current_attr);
        }
        return;
    }

    if (top == 0 && bottom == s->rows - 1) {
        s->hist_lines -= count;
        if (s->hist_lines < 0) s->hist_lines = 0;
        int pi = s->pane_index;
        if (pi >= 0 && pi < MAX_PANES && g_mux.panes[pi].active) {
            if (g_mux.panes[pi].scroll_offset > 0) {
                g_mux.panes[pi].scroll_offset -= count;
                if (g_mux.panes[pi].scroll_offset < 0)
                    g_mux.panes[pi].scroll_offset = 0;
            }
        }
        s->scroll_top = (s->scroll_top - count % s->total_lines + s->total_lines) % s->total_lines;
        for (int c = 0; c < count; c++) {
            int pr = screen_phys_row(s, c);
            if (s->lines && s->lines[pr].cells) {
                line_fill_blank(&s->lines[pr], s->cols, s->current_attr);
            }
        }
    } else {
        for (int i = bottom; i >= top + count; i--) {
            int dst_pr = screen_phys_row(s, i);
            int src_pr = screen_phys_row(s, i - count);
            if (s->lines && s->lines[src_pr].cells) {
                screen_ensure_line(s, dst_pr);
                line_copy(&s->lines[dst_pr], &s->lines[src_pr], s->cols);
            } else if (s->lines && s->lines[dst_pr].cells) {
                line_fill_blank(&s->lines[dst_pr], s->cols, s->current_attr);
            }
        }
        for (int i = top; i < top + count && i <= bottom; i++) {
            int pr = screen_phys_row(s, i);
            if (s->lines && s->lines[pr].cells) {
                line_fill_blank(&s->lines[pr], s->cols, s->current_attr);
            }
        }
    }
}

void screen_newline(ScreenBuffer *s) {
    if (s->cursor_y >= s->scroll_region_bottom)
        screen_scroll_up(s, s->scroll_region_top, s->scroll_region_bottom, 1);
    else if (s->cursor_y < s->rows - 1)
        s->cursor_y++;
}

/* 标记「光标所在物理行」为上一行的软换行续行（自动折行折下来的，不是硬换行）。
 * v1.8.47：历史 reflow 据此把相邻物理行合并回同一条逻辑行。在 vt.c 的自动折行
 * 路径（wraparound_pending 触发的 newline）调用；普通回车 / LF / IND 不调用。 */
void screen_mark_softwrap(ScreenBuffer *s) {
    if (!s->line_wrap || s->in_alt_screen) return;
    int pr = screen_phys_row(s, s->cursor_y);
    if (pr >= 0 && pr < s->total_lines) s->line_wrap[pr] = 1;
}

void detect_conpty_width(ScreenBuffer *s, int written_len) {
    (void)written_len;
    if (s->in_alt_screen) return;
    if (s->detect_count >= 5) return;
    if (s->cols >= 1000) return;

    if (s->cursor_y > 0 && s->cursor_x == 0) {
        int prev_r = s->cursor_y - 1;
        int last_char = -1;
        for (int c = s->cols - 1; c >= 0; c--) {
            CHAR_INFO *ci = screen_cell(s, prev_r, c);
            if (ci && ci->Char.UnicodeChar != L' ') {
                last_char = c;
                break;
            }
        }
        if (last_char >= s->cols - 1) {
            s->detect_count++;
            if (s->detect_count >= 3) {
                int new_cols = s->cols + 8;
                if (new_cols <= 500) {
                    screen_resize(s, new_cols, s->rows);
                    s->detect_count = 0;
                }
            }
        }
    }
}



/* 宽字符在屏幕缓冲里占两个相邻 CHAR_INFO 单元格。这些吸附/步进函数按【单元格
 * 列号】索引，必须用 CHAR_INFO*（步长 = sizeof(CHAR_INFO)，含 Char+Attributes），
 * 而不能取 &cells[0].Char.UnicodeChar 当 WCHAR*——那样按 sizeof(WCHAR) 步长索引，
 * 会读到相邻单元格的 Attributes，列号全错位（历史 bug：→ 要按两下才跨过汉字）。
 * LCC() 取一行里第 i 个单元格的字符。 */
#define LCC(line, i) ((line)[(i)].Char.UnicodeChar)

/* 选区边界「吸附到完整字符」。宽字符（中文/全角/宽符号占两列；emoji 等
 * non-BMP 字符以高/低代理对占两格）在屏幕上跨两列，选区端点若正好落在它
 * 中间，会出现「选中半个字」——高亮只盖一半、复制结果也容易错位。
 * 规则（只看一行的单元格缓冲，便于纯函数回归）：
 *   - 右端点在某个宽字符的【主格】上（BMP 宽字符本格、或 emoji 高代理），
 *     右扩 1 格把次格也纳入；
 *   - 左端点落在某个宽字符的【次格】上（BMP 宽字符的占位 0、或 emoji 低代理），
 *     左退 1 格把主格也纳入。
 * 返回调整后的端点，夹紧到 [0, ncols-1]。 */
int snap_right_to_char(const CHAR_INFO *line, int ncols, int x) {
    if (x < 0 || !line) return x;
    if (x < ncols) {
        WCHAR c = LCC(line, x);
        int wide_lead =
            ((c >= 0xD800 && c <= 0xDBFF) && x + 1 < ncols &&
             LCC(line, x + 1) >= 0xDC00 && LCC(line, x + 1) <= 0xDFFF) ? 1
            : (!(c >= 0xD800 && c <= 0xDFFF) && is_wide_cp((unsigned int)c));
        if (wide_lead && x + 1 < ncols) return x + 1;
    }
    return x;
}

int snap_left_to_char(const CHAR_INFO *line, int ncols, int x) {
    (void)ncols;
    if (x <= 0 || !line) return x;
    WCHAR c = LCC(line, x);
    /* emoji 低代理：主格在 x-1（高代理） */
    if (c >= 0xDC00 && c <= 0xDFFF) {
        if (x - 1 >= 0 && LCC(line, x - 1) >= 0xD800 && LCC(line, x - 1) <= 0xDBFF)
            return x - 1;
        return x;
    }
    /* BMP 宽字符的占位格：本格 ch==0、左邻是宽字符主格 */
    if (c == 0) {
        WCHAR prev = LCC(line, x - 1);
        if (!(prev >= 0xD800 && prev <= 0xDBFF) && is_wide_cp((unsigned int)prev))
            return x - 1;
    }
    return x;
}

/* 单元格 k 是否为宽字符的【次格】（占位）：BMP 宽字符写 0、emoji 写低代理。
 * lead[out]（非 NULL）返回该宽字符主格列号。 */
static int cell_is_wide_trail(const CHAR_INFO *line, int k, int *lead) {
    if (k < 1) return 0;
    WCHAR c = LCC(line, k);
    if (c >= 0xDC00 && c <= 0xDFFF &&
        LCC(line, k - 1) >= 0xD800 && LCC(line, k - 1) <= 0xDBFF) {
        if (lead) *lead = k - 1;
        return 1;
    }
    if (c == 0) {
        WCHAR p = LCC(line, k - 1);
        if (!(p >= 0xD800 && p <= 0xDBFF) && is_wide_cp((unsigned int)p)) {
            if (lead) *lead = k - 1;
            return 1;
        }
    }
    return 0;
}

/* 复制模式里按字符移动光标：一次跨过整个宽字符（中文/全角/emoji），光标永远
 * 停在【主格】上，不停在次格（占位）。dir=+1 向右、-1 向左；line 为该行单元格。
 * 规则：右移 = 右侧下一个主格（宽字符主格跨 2 列）；左移 = 左边最近的主格
 * （左邻若为次格，则该宽字符主格在再左一列）。 */
int copy_step_char(const CHAR_INFO *line, int ncols, int x, int dir) {
    if (!line || dir == 0) return x;
    if (x < 0) x = 0;
    if (dir > 0) {
        if (x >= ncols) return ncols - 1;
        WCHAR c = LCC(line, x);
        int emoji_lead = (c >= 0xD800 && c <= 0xDBFF && x + 1 < ncols &&
                          LCC(line, x + 1) >= 0xDC00 && LCC(line, x + 1) <= 0xDFFF);
        int wide_lead  = !(c >= 0xD800 && c <= 0xDFFF) && is_wide_cp((unsigned int)c);
        int step = (emoji_lead || wide_lead) ? 2 : 1;
        int nx = x + step;
        return nx < ncols ? nx : ncols - 1;
    } else {
        if (x <= 0) return 0;
        int lead;
        if (cell_is_wide_trail(line, x - 1, &lead))
            return lead;          /* 左邻是次格 -> 落到它的宽字符主格 */
        return x - 1;             /* 左邻本身就是主格（窄字符/空格/宽主格） */
    }
}

/* 复制模式里把【光标所在列】整字化：无论鼠标点选/拖动还是上下移动，光标都不
 * 许停在宽字符的次格（半个汉字/全角/emoji）上。若 x 落在次格，则退到它所属宽
 * 字符的主格；落在主格或窄字符上则原样返回。返回值夹紧到 [0, ncols-1]。 */
int copy_cursor_to_lead(const CHAR_INFO *line, int ncols, int x) {
    if (!line) return x;
    if (x < 0) return 0;
    if (x >= ncols) return ncols > 0 ? ncols - 1 : 0;
    int lead;
    if (cell_is_wide_trail(line, x, &lead)) return lead;
    return x;
}

void cell_truecolor(ScreenBuffer *s, int row, int col, int ar, WORD *out_f, WORD *out_b, int *out_fv, int *out_bv) {    *out_f = RGB565_WHITE; *out_b = RGB565_BLACK;
    *out_fv = 0; *out_bv = 0;
    if (row < 0 || col < 0 || col >= s->cols) return;
    if (s->in_alt_screen) {
        if (row >= s->rows || !s->alt_fg_rgb) return;
        unsigned char v = s->alt_rgb_valid ? s->alt_rgb_valid[row * s->cols + col] : 0;
        *out_fv = (v & 1) ? 1 : 0;
        *out_bv = (v & 2) ? 1 : 0;
        if (*out_fv || *out_bv) {
            *out_f = s->alt_fg_rgb[row * s->cols + col];
            *out_b = s->alt_bg_rgb[row * s->cols + col];
        }
        return;
    }
    int pr = (ar >= 0) ? ar : screen_phys_row(s, row);
    if (pr >= 0 && pr < s->total_lines && s->lines && s->lines[pr].cells) {
        unsigned char v = s->lines[pr].rgb_valid ? s->lines[pr].rgb_valid[col] : 0;
        *out_fv = (v & 1) ? 1 : 0;
        *out_bv = (v & 2) ? 1 : 0;
        *out_f = s->lines[pr].fg_rgb[col];
        *out_b = s->lines[pr].bg_rgb[col];
    }
}

/* ---- v1.8.47：滚动历史的逻辑行 reflow 视图 --------------------------------
 * 物理环形缓冲里每物理行可能是「上一行自动折行折下来的续行」（line_wrap==1）。
 * 渲染滚动历史时，把相邻物理行按 wrap 标志合并回逻辑行，再按【当前视口宽】重新
 * 折行：窄视口把一条长逻辑行折成多行完整显示、拖宽折回一行，内容不丢不重。
 *
 * 实现（正序、追加式，避免倒序/游标底填的行号错误）：
 *   1) 自老→新扫描物理行（最老历史 -hist_lines 到最新可见 rows-1），按 line_wrap
 *      把续行并入当前逻辑行（段内正序、段间尾接），硬换行结束一条逻辑行；
 *   2) 每条逻辑行按视口宽宽字符感知折行，折出的显示行【正向追加】到动态数组
 *      vrows（老在上、新在下）；
 *   3) 向上回看 vo：视口显示显示行 [total-vo-rows .. total-vo-1]（total-vo-1 为
 *      视口底部 = 距今 vo 个显示行之前的那行），拷入 out[y][x]。
 * 只扫描到「视口底部对应逻辑行」即可停止（更老内容不入视口），时间与视口大小
 * 线性相关、与总历史量无关。 */

static int reflow_glyph_w(WCHAR ch) {
    if (ch >= 0xDC00 && ch <= 0xDFFF) return 0;   /* emoji 低代理（次格） */
    if (ch >= 0xD800 && ch <= 0xDBFF) return 2;   /* emoji 高代理（主格） */
    if (ch == 0) return 0;                          /* BMP 宽字符次格占位 */
    return is_wide_cp((unsigned int)ch) ? 2 : 1;
}

/* 把一条逻辑行 g[0..n-1]（老→新）按宽 w 折行，折出的每条显示行【正向】写入 cb
 * （cb 收到该显示行的 RGlyph 段，返回非 0 表示停止）。空行硬放避免超宽 glyph
 * 死循环。 */
static int reflow_append_rows(const RGlyph *g, int n, int w,
                              int (*cb)(const RGlyph *line, int cnt, void *ud),
                              void *ud) {
    if (w < 1) w = 1;
    int col = 0, seg_start = 0;
    int k = 0;
    while (k < n) {
        WCHAR ch = g[k].ci.Char.UnicodeChar;
        int gw = reflow_glyph_w(ch);
        int adv = (gw == 2 && k + 1 < n) ? 2 : 1;
        if (gw > 0 && col > 0 && col + gw > w) {
            /* 当前显示行放不下（且行非空）：先把 seg_start..k-1 作为一条显示行发出 */
            if (cb(g + seg_start, k - seg_start, ud)) return 1;
            seg_start = k;
            col = 0;
        }
        if (gw > 0) col += gw;
        k += adv;
    }
    /* 末段（含空逻辑行 n==0 时发一条空行）。 */
    if (n == 0) {
        RGlyph blank; blank.ci.Char.UnicodeChar = L' '; blank.ci.Attributes = 0x07;
        blank.fg = RGB565_WHITE; blank.bg = RGB565_BLACK; blank.v = 0;
        if (cb(&blank, 0, ud)) return 1;
    } else if (cb(g + seg_start, n - seg_start, ud)) {
        return 1;
    }
    return 0;
}

/* 一条逻辑行的前 n 个 glyph 按宽度 w 折行会占到第几条显示行（1 起）。折行判据与
 * reflow_append_rows 完全一致，用于 resize 后把光标映射到新的显示行。 */
static int reflow_rows_for(const RGlyph *g, int n, int w) {
    if (w < 1) w = 1;
    int rows = 1, col = 0;
    for (int k = 0; k < n; ) {
        WCHAR ch = g[k].ci.Char.UnicodeChar;
        int gw = reflow_glyph_w(ch);
        int adv = (gw == 2 && k + 1 < n) ? 2 : 1;
        if (gw > 0 && col > 0 && col + gw > w) { rows++; col = 0; }
        if (gw > 0) col += gw;
        k += adv;
    }
    return rows;
}

/* cb 上下文：把每条显示行追加到动态数组（行主序，每行 width 列）。 */
typedef struct {
    RGlyph *buf;
    int cap;        /* 已分配【行数】 */
    int count;      /* 已追加行数 */
    int width;
} RfAcc;

static int reflow_acc_cb(const RGlyph *line, int cnt, void *ud) {
    RfAcc *a = (RfAcc *)ud;
    if (a->count >= a->cap) {
        int ncap = a->cap ? a->cap * 2 : 64;
        RGlyph *nb = (RGlyph *)realloc(a->buf, (size_t)ncap * a->width * sizeof(RGlyph));
        if (!nb) return 1;
        a->buf = nb; a->cap = ncap;
    }
    RGlyph *dst = a->buf + (size_t)a->count * a->width;
    for (int x = 0; x < a->width; x++) {
        dst[x].ci.Char.UnicodeChar = L' ';
        dst[x].ci.Attributes = 0x07;
        dst[x].fg = RGB565_WHITE; dst[x].bg = RGB565_BLACK; dst[x].v = 0;
    }
    int m = cnt < a->width ? cnt : a->width;
    for (int x = 0; x < m; x++) dst[x] = line[x];
    a->count++;
    return 0;
}

/* 物理行参与逻辑 reflow 的有效 cell 数。
 * line_wrap 标在“下一物理行是本行的软换行续行”上，因此若 rel+1 为续行，本行的
 * 整个旧视口宽度都是逻辑布局的一部分。旧代码无条件裁掉每个物理行尾空格，会把
 * 自动折行边界前用户真实输出的空格一起删除；resize 后单词/列对齐因而粘连。
 * 硬换行末段仍裁掉终端为屏幕宽度补的空白，避免每行都被误当成满宽内容。 */
static int screen_row_reflow_len(ScreenBuffer *s, int rel) {
    if (!s || !s->lines) return 0;
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || pr >= s->total_lines || !s->lines[pr].cells) return 0;
    ScreenLine *ln = &s->lines[pr];
    int len = ln->used;
    if (len > s->cols) len = s->cols;
    /* 兼容旧缓冲/测试夹具中直接写 cells、尚无 used 元数据的行。 */
    if (len <= 0) {
        len = ln->len;
        if (len > s->cols) len = s->cols;
        while (len > 0 && ln->cells[len - 1].Char.UnicodeChar == L' ') len--;
    }
    return len;
}

/* ---- 内容区间边界 ------------------------------------------
 * 纯空白逻辑行分两种：a) 夹在两条内容之间的真实空行（程序/CRLF 输出产生的空行，
 * 实时屏幕可见，回看历史时也应保留——否则「历史里换行不见了」）；b) 首条内容之上
 * / 末条内容之下的空行（命令行下方未用可见区、历史环顶的空行），不属于内容，计高度
 * 与渲染时都应跳过（否则滚到最顶时视图上方垫一片空白）。
 * 返回首条非空逻辑行的最老物理 rel（*top）与末条非空逻辑行的最新物理 rel（*bot），
 * 无内容返回 0。物理 rel：-hist..-1 为历史、0..rows-1 为可见区。 */
static int screen_content_span(ScreenBuffer *s, int *top_rel, int *bot_rel) {
    int hist = s->hist_lines > 0 ? s->hist_lines : 0;
    int cap = s->total_lines - s->rows;
    if (hist > cap) hist = cap;
    int top = 0, bot = 0;
    int tset = 0;         /* top 是否已定（历史 rel 为负，不能用 -1 当哨兵） */
    int gs = 0;           /* 当前逻辑行（按 line_wrap 合并的段链）最老物理行 */
    int g_has = 0;        /* 当前逻辑行是否有非空字符 */
    int opened = 0;
    for (int rel = -hist; rel < s->rows; rel++) {
        int pr = screen_phys_row(s, rel);
        if (pr < 0 || pr >= s->total_lines) continue;
        int len = screen_row_reflow_len(s, rel);
        /* 空物理行（从未写字符，cells 可能为 NULL）也是真实一行：按 wrap 参与逻辑
         * 行分组，作为 len=0 的空逻辑行处理，content_span 只关心它是否隔开内容。 */
        int wr = (s->line_wrap && s->line_wrap[pr]) ? 1 : 0;
        if (!opened) { opened = 1; gs = rel; g_has = 0; }
        else if (wr == 0) {   /* 前一条逻辑行结束于 rel-1 */
            if (g_has) {
                if (!tset) { top = gs; tset = 1; }
                bot = rel - 1;
            }
            gs = rel; g_has = 0;
        }
        if (len > 0) g_has = 1;
    }
    if (opened && g_has) {
        if (!tset) { top = gs; tset = 1; }
        bot = s->rows - 1;
    }
    if (!tset) return 0;
    *top_rel = top; *bot_rel = bot;
    return 1;
}

/* ---- v1.8.52：reflow 内容「显示行」高度 ----------------------------------
 * screen_reflow_view 把「历史 + 可见」按逻辑行重排成显示行、底部锚定。滚动上限若按
 * 物理 hist_lines 算，会允许 vo 滚过真正内容末尾——视图顶部出现一大片空白（用户看到
 * 「历史开头是空行」/ resize 后「历史没了」）。这里算出回看会产生的显示行总数（夹在
 * 首末内容之间的真实空行各占一行，首末内容之外的空白不占），供滚动上限用
 * （见 screen_scroll_limit 与 reflow_view 内部的 vo 自限）。语义必须与 reflow_view
 * 完全一致：软换行合并逻辑行、内容间空行占位、每条逻辑行按 width 折出的显示行数求和。 */
int screen_reflow_height(ScreenBuffer *s, int width) {
    if (!s || !s->line_wrap || s->in_alt_screen || width < 1) return 0;
    /* O(内容) 无分配：按 reflow 同规则数逻辑行的显示行。连续物理行（软换行链）属于
     * 同一逻辑行，游标列跨段延续；段内行尾空格先去掉（与 reflow 拼 log 一致）。 */
    int top = 0, bot = 0;
    int have = screen_content_span(s, &top, &bot);
    int total = 0;
    int line_open = 0, lhas = 0, lrows = 1, lcol = 0;
    int gs = 0;             /* 当前逻辑行的最老物理 rel */
    int hist = s->hist_lines > 0 ? s->hist_lines : 0;
    int cap = s->total_lines - s->rows;
    if (hist > cap) hist = cap;
    for (int rel = -hist; rel < s->rows; rel++) {
        int pr = screen_phys_row(s, rel);
        if (pr < 0 || pr >= s->total_lines) continue;
        int len = screen_row_reflow_len(s, rel);
        int wr = (s->line_wrap && s->line_wrap[pr]) ? 1 : 0;
        if (wr == 0) {
            /* 前一条逻辑行结束：内容计折行数，内容间的空行占 1 行（leading/trailing
             * 空行由 content_span 区间排除）。 */
            if (line_open) {
                if (lhas) total += lrows;
                else if (have && gs >= top && gs <= bot) total += 1;
            }
            line_open = 1; gs = rel; lhas = 0; lrows = 1; lcol = 0;
        } else if (!line_open) {
            line_open = 1; gs = rel; lhas = 0; lrows = 1; lcol = 0;   /* 最老一段即续行（丢头） */
        }
        for (int x = 0; x < len; x++) {
            WCHAR c = s->lines[pr].cells[x].Char.UnicodeChar;
            int gw = reflow_glyph_w(c);
            if (gw > 0) {
                if (lcol > 0 && lcol + gw > width) { lrows++; lcol = 0; }
                lcol += gw;
                lhas = 1;
            }
        }
    }
    if (line_open) {
        if (lhas) total += lrows;
        else if (have && gs >= top && gs <= bot) total += 1;
    }
    return total;
}

int screen_scroll_limit(ScreenBuffer *s) {
    if (!s) return 0;
    int h = screen_reflow_height(s, s->cols);
    int lim = h - s->rows;
    return lim > 0 ? lim : 0;
}

int screen_reflow_view(ScreenBuffer *s, int vo, int rows, int width, RGlyph *out) {
    if (!s || !s->line_wrap || s->in_alt_screen || rows <= 0 || width <= 0 || !out) return 0;
    if (vo < 0) vo = 0;
    /* vo 自限：滚过内容末尾就不该再出现顶部空白（v1.8.52）。 */
    {
        int h = screen_reflow_height(s, width);
        int lim = h - rows;
        if (lim < 0) lim = 0;
        if (vo > lim) vo = lim;
    }

    /* grid 高 rows+vo：grid[rows-1] 是视口底部（vo=0 时的最新显示行），
     * grid[0..rows-1] 恒为输出视口；向上回看 vo 时，扫描会跳过最新 vo 行、把更老
     * 内容填进 grid[0..]，grid[rows..rows+vo-1] 为预留的跳过区（不输出）。 */
    int total = rows + vo;
    RGlyph *grid = (RGlyph *)malloc((size_t)total * width * sizeof(RGlyph));
    if (!grid) return 0;
    for (int i = 0; i < total * width; i++) {
        grid[i].ci.Char.UnicodeChar = L' '; grid[i].ci.Attributes = 0x07;
        grid[i].fg = RGB565_WHITE; grid[i].bg = RGB565_BLACK; grid[i].v = 0;
    }

    /* 每条逻辑行先用 RfAcc 正向折出显示行（老在上、新在下），再倒序写入 grid。 */
    RfAcc acc; acc.buf = NULL; acc.cap = 0; acc.count = 0; acc.width = width;

    int logcap = width > 256 ? width : 256;
    RGlyph *logrow = (RGlyph *)malloc((size_t)logcap * sizeof(RGlyph));
    if (!logrow) { free(grid); free(acc.buf); return 0; }
    int logn = 0;
    /* cur 起点 = total-1：最新显示行（如命令行）落 grid[total-1]；跳过的最新 vo
     * 行落 grid[rows..total-1]（不输出），视口内容落 grid[0..rows-1]。填满 grid[0]
     * 后更老内容无槽位，停止扫描。 */
    int cur = total - 1;
    int stop = 0;

    /* 处理当前累积的逻辑行（logrow 为【老→新】）：正向折行到 acc，再把显示行
     * 从最新到最老写入 grid[cur]、grid[cur-1]…。
     * 空逻辑行（logn==0，纯空白行）分两种：夹在两条内容之间的真实空行占 1 个显示行
     * （keep_blank=1，回看历史时空行可见、与实时屏幕一致，不丢「换行」）；首条内容
     * 之上 / 末条内容之下的空白（命令行下方未用可见区等）不落位，历史内容才底部锚定、
     * 随 vo 上移到最顶也不垫空白。区间边界见 screen_content_span。 */
    #define RF_FLUSH_LOGROW() do {                                             \
        if (logn != 0 || keep_blank) {                                         \
            acc.count = 0;                                                     \
            reflow_append_rows(logrow, logn, width, reflow_acc_cb, &acc);      \
            for (int _i = acc.count - 1; _i >= 0 && !stop; _i--) {            \
                if (cur < 0) { stop = 1; break; }                             \
                for (int x = 0; x < width; x++)                               \
                    grid[cur * width + x] = acc.buf[_i * width + x];           \
                cur--;                                                         \
            }                                                                  \
        }                                                                      \
        logn = 0;                                                              \
    } while (0)

    /* 自底向上扫描：可见底部 rows-1 → 最老历史 -hist_lines。含可见区物理行，
     * 这样历史与实时内容统一 reflow、边界连续（修 v1.8.49 两套坐标系错位）。 */
    int top = 0, bot = 0;
    int have = screen_content_span(s, &top, &bot);
    int keep_blank = 0;
    for (int rel = s->rows - 1; rel >= -s->hist_lines && !stop; rel--) {
        int pr = screen_phys_row(s, rel);
        if (pr < 0 || pr >= s->total_lines) continue;
        /* 空物理行（从未写字符、cells==NULL）也是真实一行，按 len=0 空逻辑行参与
         * 分组：wrap0 处结束上一条逻辑行；落位时由 keep_blank 决定是否显示。 */
        int len = screen_row_reflow_len(s, rel);
        int wrap = (s->line_wrap && s->line_wrap[pr]) ? 1 : 0;

        /* 段前插：本行段（扫描上更老）插到 logrow 开头，保持 logrow 老→新。 */
        if (logn + len > logcap) {
            while (logn + len > logcap) logcap *= 2;
            RGlyph *nl = (RGlyph *)realloc(logrow, (size_t)logcap * sizeof(RGlyph));
            if (!nl) { free(logrow); free(acc.buf); free(grid); return 0; }
            logrow = nl;
        }
        if (len > 0) {
            if (logn > 0) memmove(logrow + len, logrow, (size_t)logn * sizeof(RGlyph));
            for (int x = 0; x < len; x++) {
                RGlyph g;
                g.ci = s->lines[pr].cells[x];
                g.fg = s->lines[pr].fg_rgb ? s->lines[pr].fg_rgb[x] : RGB565_WHITE;
                g.bg = s->lines[pr].bg_rgb ? s->lines[pr].bg_rgb[x] : RGB565_BLACK;
                g.v  = s->lines[pr].rgb_valid ? s->lines[pr].rgb_valid[x] : 0;
                logrow[x] = g;
            }
            logn += len;
        }
        if (!wrap) {
            /* 本行是该逻辑行起点（最老段）：整条逻辑行完整，落位。空行是否保留
             * 看该逻辑行最老 rel 是否落在内容区间内。 */
            keep_blank = (logn == 0 && have && rel >= top && rel <= bot);
            RF_FLUSH_LOGROW();
        }
    }
    /* 环顶最老一段：扫描到 -hist_lines 仍未遇到 wrap0（续行丢头）时整条在此收尾；
     * 空行同样只有落在内容区间内才保留。 */
    if (!stop && logn > 0) {
        keep_blank = 0;
        RF_FLUSH_LOGROW();
    } else if (!stop && logn == 0 && have && -s->hist_lines >= top && -s->hist_lines <= bot) {
        keep_blank = 1;
        RF_FLUSH_LOGROW();
    }
    #undef RF_FLUSH_LOGROW

    free(logrow);
    free(acc.buf);

    /* grid 底部锚定：grid[total-1] 是最新显示行，越老越靠上。向上回看 vo 行 = 跳过
     * 最新 vo 个显示行，视口底 = grid[total-1-vo]，顶 = 再上 rows-1 行。 */
    int bottom_idx = total - 1 - vo;
    for (int y = 0; y < rows; y++) {
        int gi = bottom_idx - (rows - 1 - y);
        for (int x = 0; x < width; x++) {
            if (gi >= 0 && gi < total) out[y * width + x] = grid[gi * width + x];
        }
    }

    free(grid);
    return 1;
}

/* ---- ConPTY resize 后整屏重绘对齐 ---------------------------------------
 * ConPTY 在 resize 后（以及随后反复整屏重绘时）按【自身滚动缓冲】的顶部把可见屏
 * 重画给本进程；其顶行常比本地 reflow 环的可见顶行深若干行（真机 fixture 实测：
 * 120→59 收窄后重绘顶行是 "4"，而本地 rel0 还是 "3"）。若把重绘直接逐行落下，
 * 顶行会落在本地 rel>0 处、把本地还显示着的顶部内容覆盖吞行。
 *
 * 喂解析器前对每个输入块先调用本函数：若重绘的【所有非空行】与本地环从 rel=k
 * (k>=1) 起的行逐行一致，说明只是 ConPTY 视口更深，就把环前滚 k 行对齐
 * （hist_lines +k、scroll_top 前进 k，内容零改动），随后重绘逐行覆盖的都是相同
 * 内容，不再吞行。顶行即本地 rel0 或非重绘块则不动。
 *
 * 两条硬约束（都是真机 bug 换来的）：
 *  - 只接受【正向】偏移。旧实现取 |rel| 最小、允许 rel<0（重绘顶行落在本地历史
 *    里时也转环），那会把 hist_lines 削短、把较新的可见行整批覆盖掉——用户报的
 *    「每拖宽一列历史就短一截」。ConPTY 只会把行滚【出】视口，不会把历史拉回来，
 *    所以负向匹配一定是误判，不动作。
 *  - 必须【整块逐行】吻合，不能只比首行。旧实现只比重绘首行、取 |rel| 最小者；
 *    dir 式输出里同名行反复出现（真机 termux_dump.log 在可见区出现多次），单行
 *    匹配必然认错位置，把重绘写到错误偏移、生成新的重复行，下一次 align 又去匹配
 *    这条新重复行——自我放大。用户报的「历史重复」：真机重放内容流 40 行变 56 行、
 *    同一文件名出现 13 次。
 * 还有一条：重绘最后一行是空白（conhost 增高时下方补空行的「短重绘」）时不动作，
 * 那种情形由 screen_repaint_reanchor 按底边对齐，转环只会把内容重复写一遍。
 * 返回：0=非重绘块（无 ESC[H）；1=已滚动对齐；2=归顶重绘但无需对齐。 */
/* screen.c 保持无跨模块依赖：此处内联一个最小 UTF-8 解码器供 repaint 对齐比较用。 */
static unsigned int screen_utf8_cp(const char *s, int n, int *adv) {
    unsigned char c = (unsigned char)s[0];
    int i, need = 0; unsigned int cp = 0;
    if (c < 0x80) { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else { *adv = 1; return 0xFFFD; }
    if (1 + need > n) { *adv = 1; return 0xFFFD; }
    for (i = 1; i <= need; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) { *adv = 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *adv = 1 + need;
    return cp;
}

/* 取重绘里从 p 起的下一行文本：剥掉 CSI/OSC，制表符当空格，去首尾空白后写进 out。
 * 返回下一行的起点；数据到尾返回 -1。*n = 0 表示这是 conhost 补的空白行。
 * ConPTY 重绘逐行写「行文本 ESC[K CRLF」，最后一行可能只有 ESC[K 没有 CRLF。 */
static int repaint_next_row(const char *data, int len, int p, WCHAR *out, int cap, int *n) {
    unsigned char seg[4096];
    int segn = 0;
    int k = p;
    while (k < len && segn < 4090) {
        unsigned char c = (unsigned char)data[k];
        if (c == '\r') {                       /* CRLF：吃掉后面的 LF */
            k++;
            if (k < len && data[k] == '\n') k++;
            break;
        }
        if (c == '\n') { k++; break; }
        if (c == 0x1b) {
            if (k + 1 < len && data[k + 1] == '[') {
                int j = k + 2;
                while (j < len && !((unsigned char)data[j] >= 0x40 && (unsigned char)data[j] <= 0x7E)) j++;
                if (j >= len) { k = len; break; }
                k = j + 1;
                continue;                      /* 剥掉 CSI（含 ESC[K / SGR） */
            }
            if (k + 1 < len && data[k + 1] == ']') {
                k += 2;
                while (k < len && data[k] != 0x07) k++;
                if (k < len) k++;
                continue;                      /* 剥掉 OSC */
            }
            k += 2;
            continue;
        }
        seg[segn++] = (c == '\t') ? ' ' : c;
        k++;
    }
    int a = 0;
    while (a < segn && (seg[a] == ' ' || seg[a] == '\t')) a++;
    int b = segn;
    while (b > a && (seg[b - 1] == ' ' || seg[b - 1] == '\t')) b--;
    int wn = 0;
    int q = a;
    while (q < b && wn < cap) {
        int adv = 0;
        unsigned int cp = screen_utf8_cp((const char *)seg + q, b - q, &adv);
        if (adv <= 0) break;
        q += adv;
        if (cp < 0x10000) out[wn++] = (WCHAR)cp;
        else {
            out[wn++] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10));
            if (wn < cap) out[wn++] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        }
    }
    *n = wn;
    if (k >= len && segn == 0) return -1;      /* 数据到尾且这行没内容 */
    return k;
}

/* 本地 rel 行的文本（去首尾空白）是否与 w[0..wn) 逐字相同。 */
static int screen_row_text_eq(ScreenBuffer *s, int rel, const WCHAR *w, int wn) {
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) return 0;
    ScreenLine *ln = &s->lines[pr];
    int nl = ln->len;
    int a = 0;
    while (a < nl && ln->cells[a].Char.UnicodeChar == L' ') a++;
    int b = nl;
    while (b > a && ln->cells[b - 1].Char.UnicodeChar == L' ') b--;
    /* 宽字符在本地占 2 格、次格 UnicodeChar==0；而 repaint_next_row 给出的 wn 是
     * 【码点数】。旧实现直接拿 (b-a) 比 wn、再逐格比 w[x]，于是任何含宽字符的行都
     * 必然不等，差值正好等于该行的宽字符个数（本地实测：v3 最后一次重绘 k=19 时
     * rel=23「523,582 洛谷题解_未命名.html」本地 56 格 vs wn 49，差 7 = 7 个汉字，
     * 两边打印出的文本逐字相同）。后果是 screen_repaint_align 的 k 循环在真机
     * dir 输出上永远匹配不到，短重绘只能一路交给 reanchor 兜底。 */
    int x = a, i = 0;
    while (x < b) {
        WCHAR ch = ln->cells[x].Char.UnicodeChar;
        if (ch == 0) { x++; continue; }        /* 跳过宽字符次格 */
        if (i >= wn || ch != w[i]) return 0;
        i++; x++;
    }
    return i == wn;
}

int screen_repaint_align(ScreenBuffer *s, const char *data, int len) {
    if (!s || s->in_alt_screen || len <= 0) return 0;
    /* 1) 找首个「光标归顶」CSI：ESC[H / ESC[1;1H / ESC[;H（行=1 列=1）。
     *    之前跳过 cursor 显隐 / resize 通知等 CSI 与 OSC。 */
    int homed = -1;
    int i = 0;
    while (i < len) {
        unsigned char b = (unsigned char)data[i];
        if (b != 0x1b) { i++; continue; }
        if (i + 1 >= len) break;
        if (data[i + 1] == '[') {
            int j = i + 2; int row = 0, col = 0, cur = 0, fin = 0;
            while (j < len) {
                char c = data[j];
                if (c >= '0' && c <= '9') { if (!cur) row = row * 10 + (c - '0'); else col = col * 10 + (c - '0'); j++; continue; }
                if (c == ';') { cur = 1; j++; continue; }
                if (c == '?' || c == ' ') { j++; continue; }
                fin = c; j++; break;
            }
            if (fin == 'H' || fin == 'f') {
                int rr = row ? row : 1, cc = col ? col : 1;
                if (rr == 1 && cc == 1) { homed = j; }
            }
            i = j;
            if (homed >= 0) break;
            continue;
        }
        if (data[i + 1] == ']') { i += 2; while (i < len && data[i] != 0x07) i++; if (i < len) i++; continue; }
        i += 2;
    }
    if (homed < 0) return 0;
    /* 2) 先看重绘的最后一行有没有内容。conhost 在窗口【增高】时不把滚动历史拉回来、
     *    只在下方补空行，这种「短重绘」的底部对齐由 screen_repaint_reanchor 负责
     *    （把内容整体下移、顶部用重绘前的可见行补回），此时转环只会把内容重复写一遍。
     *    只有重绘一直写到最后一行（conhost 视口是满的）时，它的顶行才可能比本地
     *    可见顶行深，需要下面的正向对齐。 */
    {
        int p = homed, last_blank = 1, any = 0;
        while (p >= 0 && p < len) {
            WCHAR rl[8];
            int rn = 0;
            int np = repaint_next_row(data, len, p, rl, 8, &rn);
            last_blank = (rn == 0);
            any = 1;
            p = np;
        }
        if (!any) return 2;
        /* 「短重绘」（conhost 这次没写到最后一行）以前一律 return 2 撒手，交给
         * screen_repaint_reanchor 做底部对齐。这在 reanchor 开着时是对的：两边都
         * 动手会把同一批内容搬两遍。
         *
         * 但 reanchor 关掉时（Plan B，TERMUX_REANCHOR=0）两边就都不管了 —— conhost
         * 的短重绘从本地第 0 行写起，而它那一屏对应的时间流位置比本地第 0 行晚得多，
         * 于是本地可见区被整片覆盖。真机症状就是用户报的「历史被截断」；本地可复现：
         * tests/fixtures_pane0_stream_v3.bin，STEPS=212:59,1153:19,7589:80,8238:103，
         * ALIGN=1 TERMUX_REANCHOR=0 末态少 38 条内容行（整份 C:\ 列表尾部 +
         * 整份 Downloads 列表）。
         *
         * 所以只在 reanchor 真的会接手时才撒手；否则继续走下面的正向对齐 —— 那才是
         * Windows Terminal 的做法：不钉底部，而是把本地视口挪到 conpty 认为的位置，
         * 被挤掉的行进历史而不是被覆盖。 */
        if (last_blank && screen_reanchor_enabled()) return 2;
    }
    /* 3) 从最小的正向偏移 k 起试：要求重绘的【每一条非空行】都与本地 rel=k+j 一致
     *    （rel 超出本地最新行的那些行不比——conhost 的行数可能比本地多）。取第一个
     *    全吻合的 k。k 只到 rows-1：再大就等于把整个可见区推成历史，没有意义。 */
    int best = 0;
    for (int k = 1; k <= s->rows - 1 && !best; k++) {
        int p = homed, j = 0, matched = 0, ok = 1;
        while (p >= 0 && p < len) {
            WCHAR rl[256];
            int rn = 0;
            int np = repaint_next_row(data, len, p, rl, 256, &rn);
            if (rn > 0) {
                if (k + j <= s->rows - 1) {
                    if (!screen_row_text_eq(s, k + j, rl, rn)) { ok = 0; break; }
                    matched++;
                }
            }
            j++;
            p = np;
            if (j > 4096) break;
        }
        if (ok && matched > 0) best = k;
    }
    if (best > 0) {
        s->hist_lines += best;
        if (s->hist_lines > s->total_lines - s->rows) s->hist_lines = s->total_lines - s->rows;
        s->scroll_top = (s->scroll_top + best) % s->total_lines;
        if (s->scroll_top < 0) s->scroll_top += s->total_lines;
        return 1;
    }
    return 2;   /* 归顶重绘已见，但没有整块吻合的正向偏移：不动作 */
}

/* ---- reflow-on-resize 实现 ----------------------------------------------- */

/* 临时环形区：容量 = 新 total_lines；存“逻辑行按新宽度折出的显示行”，满则丢最老
 * （与滚动缓冲容量固定、丢最老一致——重排期间内容不会重复也不会凭空增长）。 */
typedef struct {
    RGlyph **rows;        /* rows[slot] 一行 width 格（行内已铺满空格） */
    unsigned char *first; /* first[slot]=1 = 该显示行是某逻辑行的首行 */
    int *used;            /* 该显示行真实使用的 cell 数（含输出的空格） */
    int cap, head, count, width;
} RfRing;

static int rfring_init(RfRing *r, int cap, int width) {
    r->cap = cap; r->width = width; r->head = 0; r->count = 0;
    r->rows = (RGlyph **)calloc((size_t)cap, sizeof(RGlyph *));
    r->first = (unsigned char *)calloc((size_t)cap, 1);
    r->used = (int *)calloc((size_t)cap, sizeof(int));
    if (!r->rows || !r->first || !r->used) {
        free(r->rows); free(r->first); free(r->used);
        r->rows = NULL; r->first = NULL; r->used = NULL;
        return 0;
    }
    return 1;
}
static void rfring_free(RfRing *r) {
    if (r->rows) {
        for (int i = 0; i < r->cap; i++) free(r->rows[i]);
        free(r->rows);
    }
    free(r->first);
    free(r->used);
    r->rows = NULL; r->first = NULL; r->used = NULL;
}
static void rfring_add(RfRing *r, const RGlyph *row, int firstflag, int used) {
    int slot;
    if (r->count < r->cap) {
        slot = (r->head + r->count) % r->cap;
        if (!r->rows[slot]) {
            r->rows[slot] = (RGlyph *)malloc((size_t)r->width * sizeof(RGlyph));
            if (!r->rows[slot]) return;
        }
        r->count++;
    } else {
        slot = r->head;
        r->head = (r->head + 1) % r->cap;
    }
    memcpy(r->rows[slot], row, (size_t)r->width * sizeof(RGlyph));
    r->first[slot] = (unsigned char)firstflag;
    r->used[slot] = used;
}

/* reflow_append_rows 回调：把一条显示行（铺满 width 格）塞进 RfRing。 */
typedef struct {
    RfRing *ring;
    RGlyph *line;
    int width;
    int first_done;   /* 当前逻辑行是否已落过首行（决定后续行 wrap=1） */
} RfSink;
static int reflow_sink_cb(const RGlyph *seg, int cnt, void *ud) {
    RfSink *sk = (RfSink *)ud;
    for (int x = 0; x < sk->width; x++) {
        sk->line[x].ci.Char.UnicodeChar = L' ';
        sk->line[x].ci.Attributes = 0x07;
        sk->line[x].fg = RGB565_WHITE; sk->line[x].bg = RGB565_BLACK; sk->line[x].v = 0;
    }
    int m = cnt < sk->width ? cnt : sk->width;
    for (int x = 0; x < m; x++) sk->line[x] = seg[x];
    rfring_add(sk->ring, sk->line, sk->first_done ? 0 : 1, m);
    sk->first_done = 1;
    return 0;
}

/* 从 RGlyph 显示行拷进一行（四个并行数组一并写）。 */
static void line_store_rglyph(ScreenLine *ln, const RGlyph *row, int w, int used) {
    if (!ln->cells || !row) return;
    ln->used = used < w ? used : w;
    for (int x = 0; x < w; x++) {
        ln->cells[x].Char.UnicodeChar = row[x].ci.Char.UnicodeChar;
        ln->cells[x].Attributes = row[x].ci.Attributes;
        if (ln->fg_rgb) ln->fg_rgb[x] = row[x].fg;
        if (ln->bg_rgb) ln->bg_rgb[x] = row[x].bg;
        if (ln->rgb_valid) ln->rgb_valid[x] = row[x].v;
    }
}

static int screen_resize_reflow(ScreenBuffer *s, int nc, int nr) {
    int nt = nr + g_scrollback_lines;
    WORD fill_attr = s->current_attr ? s->current_attr : 0x07;

    ScreenLine *nl = (ScreenLine *)calloc(nt, sizeof(ScreenLine));
    unsigned char *nwrap = (unsigned char *)calloc(nt, 1);
    CHAR_INFO *na = (CHAR_INFO *)calloc((size_t)nr * nc, sizeof(CHAR_INFO));
    WORD *nafr = (WORD *)malloc((size_t)nr * nc * sizeof(WORD));
    WORD *nabr = (WORD *)malloc((size_t)nr * nc * sizeof(WORD));
    unsigned char *nav = (unsigned char *)calloc((size_t)nr * nc, 1);
    if (!nl || !nwrap || !na || !nafr || !nabr || !nav) {
        free(nl); free(nwrap); free(na); free(nafr); free(nabr); free(nav);
        return 0;
    }
    for (int i = 0; i < nr * nc; i++) {
        nafr[i] = RGB565_WHITE; nabr[i] = RGB565_BLACK;
        na[i].Char.UnicodeChar = L' '; na[i].Attributes = fill_attr;
    }

    /* 1) 合并逻辑行 + 按新宽度折行，落进 RfRing。自老→新扫历史与可见。 */
    RfRing ring;
    memset(&ring, 0, sizeof(ring));
    if (!rfring_init(&ring, nt, nc)) {
        for (int i = 0; i < nt; i++) line_free(&nl[i]);
        free(nl); free(nwrap); free(na); free(nafr); free(nabr); free(nav);
        return 0;
    }
    RfSink sk;
    sk.ring = &ring; sk.width = nc; sk.first_done = 1;
    sk.line = (RGlyph *)malloc((size_t)(nc > 16 ? nc : 16) * sizeof(RGlyph));
    if (!sk.line) {
        rfring_free(&ring);
        for (int i = 0; i < nt; i++) line_free(&nl[i]);
        free(nl); free(nwrap); free(na); free(nafr); free(nabr); free(nav);
        return 0;
    }
    RGlyph *log = NULL; int logcap = 0, logn = 0;
    int line_open = 0;
    int oom = 0;
    int old_hist = s->hist_lines > 0 ? s->hist_lines : 0;
    if (old_hist > s->total_lines - s->rows) old_hist = s->total_lines - s->rows;
    /* 「顶部空区」探测：无滚动历史时，可见区首行内容之前的一串空白物理行只是
     * 放大/清屏后屏幕未填满的空区，不是内容、更不是该进滚动缓冲的空白行。这里
     * 跳过它们，否则下一次 shrink 会把它们当历史空白行收进内容流（放大后缩小，
     * 历史顶部凭空多出空白行）。一旦扫到任何内容，其后所有行（含刻意空行）都是
     * 真内容。 */
    int empty_top = (old_hist == 0) ? 1 : 0;
    int saw_content = 0;
    /* resize 只扫描真实内容跨度，不能把可见内容下方未使用的屏幕填充行搬进历史。
     * 上下分屏首次把 29 行缩到 14 行时，旧代码把 banner 后面的 15 个空白屏幕行
     * 也落入 ring，得到 hist=15；ConPTY 随后重绘 banner，于是最上方出现重复。
     * 夹在内容之间的真实空行仍位于 content span 内，不受影响。游标所在空行也保留，
     * 以免正在输入前的垂直位置在 resize 时上跳。 */
    int span_top = 0, span_bot = -1;
    int have_span = screen_content_span(s, &span_top, &span_bot);
    int scan_end = have_span ? span_bot + 1 : 0;
    /* 历史本身由 rel=-old_hist..-1 完整扫描；可见屏幕则无论是否已有历史，
     * 都只能扫描到真实内容/游标末端。若 old_hist>0 时强行扫满 s->rows，
     * 上方 pane 下方未使用的 padding 会在每次拖动分隔线时被塞进时间流，
     * 形成提示符后的多余空行，并不断扰乱 hist/scroll_limit。内容跨度内部的
     * 真实空行仍完整保留。 */
    if (s->cursor_y + 1 > scan_end) scan_end = s->cursor_y + 1;
    if (scan_end > s->rows) scan_end = s->rows;
    (void)span_top;

    /* 光标必须跟着内容一起 reflow：记下「老光标行之前已折出的显示行数」
     * （cursor_rows）与「光标在自己那条逻辑行内的显示行偏移」（cursor_intra）。
     * 不重算的话 resize 后光标停在旧行号上（内容已按新宽度整体移位），下一条
     * 输出就会写进已有内容行、把它覆盖掉（放大窗格后最明显）。 */
    int cursor_rows = -1, cursor_intra = 0;
    for (int rel = -old_hist; rel < scan_end && !oom; rel++) {
        int pr = screen_phys_row(s, rel);
        if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) continue;
        ScreenLine *ln = &s->lines[pr];
        int len = screen_row_reflow_len(s, rel);
        int wr = (s->line_wrap && s->line_wrap[pr]) ? 1 : 0;
        /* 纯增高（nc==cols && nr>rows）不做特殊处理：内容是一条时间流，屏幕变高
         * 就是「最新 nr 条显示行可见、其余留在历史」，hist 自然缩小 nr-rows 行
         * （= v1.8.51 的 pull 语义）。旧实现在历史与旧可见区之间插入 nr-rows 个
         * 空逻辑行来「保住 old_hist」，代价是这些空行落在内容跨度【内部】：被
         * screen_reflow_height 计成内容行，每把窗格拖高 k 行就往滚动历史里永久塞
         * k 个空行，同时抬高 scroll_limit（翻到顶还有一片空白）。 */
        if (len == 0 && wr == 0 && empty_top && !saw_content) continue; /* 顶部空区 */
        if (len > 0 || wr) saw_content = 1;
        if (wr == 0) {
            if (line_open) {           /* 上一条逻辑行完整，折行落位 */
                sk.first_done = 0;
                reflow_append_rows(log, logn, nc, reflow_sink_cb, &sk);
                logn = 0;
            }
            line_open = 1;             /* 本行是新逻辑行首行（空白行也是） */
        } else if (!line_open) {
            line_open = 1;             /* 历史最老一段即续行（被淘汰丢头）的兜底 */
        }
        /* 此处 ring.count = 光标所在逻辑行【之前】的显示行数（上一条逻辑行刚折完
         * 落位），log[0..logn-1] = 光标所在逻辑行在本物理行之前已累积的 glyph。 */
        if (cursor_rows < 0 && rel == s->cursor_y) {
            int upto = logn + (s->cursor_x < len ? s->cursor_x : len);
            cursor_rows = ring.count;
            cursor_intra = (log && upto > 0) ? reflow_rows_for(log, upto, nc) - 1 : 0;
        }
        if (len > 0) {
            if (logn + len > logcap) {
                while (logn + len > logcap) logcap = logcap ? logcap * 2 : 512;
                RGlyph *ng = (RGlyph *)realloc(log, (size_t)logcap * sizeof(RGlyph));
                if (!ng) { oom = 1; break; }
                log = ng;
            }
            for (int x = 0; x < len; x++) {
                RGlyph g;
                g.ci = ln->cells[x];
                g.fg = ln->fg_rgb ? ln->fg_rgb[x] : RGB565_WHITE;
                g.bg = ln->bg_rgb ? ln->bg_rgb[x] : RGB565_BLACK;
                g.v = ln->rgb_valid ? ln->rgb_valid[x] : 0;
                log[logn++] = g;
            }
        }
    }
    if (!oom && line_open) {
        sk.first_done = 0;
        reflow_append_rows(log, logn, nc, reflow_sink_cb, &sk);
        logn = 0;
    }
    free(log);
    free(sk.line);

    if (oom) {
        rfring_free(&ring);
        for (int i = 0; i < nt; i++) line_free(&nl[i]);
        free(nl); free(nwrap); free(na); free(nafr); free(nabr); free(nav);
        return 0;
    }

    /* 2) 落位：可见区 = 最新 nr 条显示行（rel 0..nr-1，物理槽 0..nr-1），其上为历史。
     *    内容槽 j（0..ring.count-1，老→新）。 */
    int tcount = ring.count;
    int hist = tcount > nr ? tcount - nr : 0;
    /* 光标在内容流里的显示行下标（相对内容首行）。扫描中途 OOM 时兜底到内容末尾。 */
    if (cursor_rows < 0) cursor_rows = tcount;
    int cur_off = cursor_rows + cursor_intra;
    /* 内容不足一屏时的起始 rel：顶对齐（base = 0）。
     * 这一支只在 tcount < nr 时走到，而 tcount < nr 就意味着这次 resize 之后
     * hist 归 0（内容整屏放得下）——此时 ConPTY 的整屏重绘也是顶对齐的（conhost
     * 增高时把内容留在原处、只在下方补空行），两边必须一致，否则每次拖动都会看到
     * 内容在「底对齐的一帧」和「顶对齐的重绘」之间跳。
     * 「有历史时提示符必须贴底」由 screen_repaint_reanchor() 保证：有历史 ⇒ 内容
     * 一定超过一屏 ⇒ 走下面 tcount >= nr 那支（可见区 = 最新 nr 行，天然底对齐），
     * 重绘结束后再把 conhost 少发的那几行补回顶部。 */
    int base = 0;
    if (tcount >= nr) {
        for (int j = 0; j < tcount; j++) {
            int slot = (ring.head + j) % ring.cap;
            int rel = j - hist;
            int phys = rel >= 0 ? rel : rel + nt;   /* 历史区落在环尾 nt-hist..nt-1 */
            if (!nl[phys].cells) {
                if (!line_alloc(&nl[phys], nc, fill_attr)) continue;
            }
            line_store_rglyph(&nl[phys], ring.rows[slot], nc, ring.used[slot]);
            if (ring.first[slot]) nwrap[phys] = 0; else nwrap[phys] = 1;
        }
    } else {
        for (int j = 0; j < tcount; j++) {
            int slot = (ring.head + j) % ring.cap;
            int phys = base + j;
            if (!nl[phys].cells) {
                if (!line_alloc(&nl[phys], nc, fill_attr)) continue;
            }
            line_store_rglyph(&nl[phys], ring.rows[slot], nc, ring.used[slot]);
            if (ring.first[slot]) nwrap[phys] = 0; else nwrap[phys] = 1;
        }
    }
    /* 可见区其余槽也分配成空白（渲染前可能被直接读取）。 */
    for (int y = 0; y < nr; y++)
        if (!nl[y].cells) line_alloc(&nl[y], nc, fill_attr);
    /* 光标落到新布局里同一条内容行上：内容首行 rel = (tcount>=nr ? -hist : base)，
     * 再加上光标在内容流里的显示行下标。 */
    {
        int ncur = (tcount >= nr ? -hist : base) + cur_off;
        if (ncur < 0) ncur = 0;
        if (ncur > nr - 1) ncur = nr - 1;
        s->cursor_y = ncur;
    }
    rfring_free(&ring);

    /* 3) 换旧为新。 */
    if (s->lines) {
        for (int i = 0; i < s->total_lines; i++) line_free(&s->lines[i]);
        free(s->lines);
    }
    free(s->line_wrap);
    free(s->alt_buffer); free(s->alt_fg_rgb); free(s->alt_bg_rgb); free(s->alt_rgb_valid);
    s->lines = nl;
    s->line_wrap = nwrap;
    s->alt_buffer = na;
    s->alt_fg_rgb = nafr;
    s->alt_bg_rgb = nabr;
    s->alt_rgb_valid = nav;
    s->cols = nc;
    s->rows = nr;
    s->total_lines = nt;
    s->hist_lines = hist;
    /* 环头必须归零。上面的落位是【绝对布局】：可见区 = 物理槽 0..nr-1、历史 =
     * 物理槽 nt-hist..nt-1，而 screen_phys_row() 是 (scroll_top+rel)%total_lines，
     * 渲染 / 复制 / 搜索 / reflow_view 全部经它取行。保留旧的 scroll_top（v1.8.52
     * 曾这么做，想「不把回看视图跳回底部」）等于让环头与布局错位 scroll_top 行：
     * 历史与可见屏整体平移、最上面几行从历史里消失、底部多一行空白，且后续输出
     * 写进错误的物理槽覆盖已有内容。回看位置不在 scroll_top 上——它在
     * g_mux.panes[i].scroll_offset（pane_resize_to 已用 screen_scroll_limit 夹过），
     * 这里复位环头不影响用户的回看位置。 */
    s->scroll_top = 0;
    if (s->alt_hist_lines > nt - nr) s->alt_hist_lines = nt - nr;
    if (s->cursor_x >= nc) s->cursor_x = nc - 1;
    if (s->cursor_y >= nr) s->cursor_y = nr - 1;
    s->scroll_region_top = 0; s->scroll_region_bottom = nr - 1;
    s->wraparound_pending = 0;
    memset(s->tab_stops, 0, sizeof(s->tab_stops));
    for (int i = 0; i < nc && i < 512; i += 8) s->tab_stops[i] = 1;
    return 1;
}

/* ---- alt 屏（vim/htop 等）resize：沿用 v1.8.11 BUG-8 后的物理行迁移 ---- */
static int screen_resize_legacy(ScreenBuffer *s, int nc, int nr) {
    if (nc == s->cols && nr == s->rows) return 1;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    int nt = nr + g_scrollback_lines;

    ScreenLine *nl = (ScreenLine *)calloc(nt, sizeof(ScreenLine));
    unsigned char *nwrap = (unsigned char *)calloc(nt, 1);   /* v1.8.47 软换行标志 */
    CHAR_INFO *na = (CHAR_INFO *)calloc(nr * nc, sizeof(CHAR_INFO));
    WORD *nafr = (WORD *)malloc(nr * nc * sizeof(WORD));
    WORD *nabr = (WORD *)malloc(nr * nc * sizeof(WORD));
    unsigned char *nav = (unsigned char *)calloc(nr * nc, 1);

    if (!nl || !nwrap || !na || !nafr || !nabr || !nav) {
        free(nl); free(nwrap); free(na); free(nafr); free(nabr); free(nav);
        return 0;
    }
    for (int i = 0; i < nr * nc; i++) {
        nafr[i] = RGB565_WHITE; nabr[i] = RGB565_BLACK;
        na[i].Char.UnicodeChar = L' '; na[i].Attributes = s->current_attr ? s->current_attr : 0x07;
    }

    int cc = nc < s->cols ? nc : s->cols;
    int cr = nr < s->rows ? nr : s->rows;
    int nst = 0;
    /* v1.8.45 #6：历史行可能比新窗格宽（收窄后保留旧宽内容），其实际宽度存在
     * ScreenLine.len；见下方历史迁移。可见行按新宽 nc，ConPTY resize 后会重排。 */

    int old_hist = s->hist_lines;
    int old_cap = s->total_lines - s->rows;
    if (old_hist > old_cap) old_hist = old_cap;
    if (old_hist > nt - nr) old_hist = nt - nr;

    /* ---- 行迁移（底部锚定；放大时把行提回可见区的行退出历史）----
     * resize 只改变可见窗口大小，绝不凭空新增/删除历史——历史只由真实滚动产生。
     * 可见区内容【底部对齐】：命令提示符（屏幕底部）始终留在底部。
     * 锚点偏移 k = nr - rows（缩小 k<0、放大 k>0、纯宽变 k=0）：
     *   新可见行 y 的旧逻辑行 src = y - k（src>=0 为旧可见行、src<0 为旧历史）。
     *  - 缩小（k<0）：y=0 取旧可见 -k 行，被裁的顶部 |k| 行【不滚入历史】——
     *    ConPTY resize 后会按新高度重新输出可见内容，把可见行移入历史会与
     *    ConPTY 的重发重复（v1.8.45 修复：上下分屏调大小后历史重复）。
     *  - 放大（k>0）：底部 rows 行取旧可见行，顶部 k 行从旧历史最新行回补
     *    （src<0）。
     *  - v1.8.51：被回补进可见区的历史行【必须同时从历史里移除】。旧实现放大后
     *    new_hist 不变，同一批行既留在历史区、又出现在可见区顶部（内容两份）；
     *    v1.8.50 整窗 reflow 把「历史 + 可见」当作一条单调世界流扫描，这份副本
     *    会让回看时同一段输出重复显示、甚至时间倒退（拖分屏条把窗格拉高后最明显）。
     *    故放大时先算 pull = min(k, old_hist)（从历史提回可见区的行数），
     *    new_hist = old_hist - pull；历史迁移只保留真正还在滚动缓冲里的行，
     *    被提走的行不再属于历史。内容总数 old_hist+rows 在放大前后守恒。
     * 历史行数恒为 min(old_hist, newcap)，且放大时再减去 pull。 */
    WORD fill_attr = s->current_attr ? s->current_attr : 0x07;
    int newcap = nt - nr;
    int k = nr - s->rows;                          /* 锚点偏移：缩小<0、放大>0 */
    int pull = 0;                                  /* 放大时从历史提回可见区的行数 */
    if (k > 0 && old_hist > 0) {
        pull = k < old_hist ? k : old_hist;
        if (pull > newcap) pull = newcap;
    }
    int new_hist = old_hist - pull;
    if (new_hist > newcap) new_hist = newcap;
    if (new_hist < 0) new_hist = 0;

    /* 取「旧布局逻辑行 src」的物理行：src>=0 为旧可见行、src<0 为旧历史第 -src 新；
     * 越界/不存在返回 -1。 */
    #define RESIZE_OLD_ROW(src) ( \
        ((src) >= 0 && (src) < s->rows) ? screen_phys_row(s, (src)) : \
        ((src) < 0 && -(src) >= 1 && -(src) <= old_hist) ? screen_phys_row(s, (src)) : -1)

    /* 迁移新可见区（rel 0..nr-1）：src = y - k（底部锚定）。src<0 时从旧历史
     * 回补（仅放大会发生），取不到则空白。 */
    for (int y = 0; y < nr; y++) {
        int new_r = (nst + y) % nt;
        int src = y - k;
        int old_r = RESIZE_OLD_ROW(src);
        if (old_r >= 0 && old_r < s->total_lines && s->lines && s->lines[old_r].cells) {
            if (line_alloc(&nl[new_r], nc, fill_attr))
                line_copy(&nl[new_r], &s->lines[old_r], cc);
        } else {
            line_alloc(&nl[new_r], nc, fill_attr);
        }
    }
    /* 迁移新历史区（rel -1..-new_hist，-1 最新）：新历史第 h 行（-1 最新）恒对应
     * 旧历史第 -(h+pull) 行——放大的 pull 行已被提回可见区、不再属于历史，历史区
     * 整体只保留真正滚出过可见区的内容（v1.8.51，见上）。被裁的可见行不滚入历史
     * （缩小分支，见上）。 */
    for (int h = 1; h <= new_hist; h++) {
        /* 新历史第 h 行（-1 最新）位于新可见首行 nst 的「上 h 行」：
         * (nst - h) 环回。历史在循环缓冲里排在可见区之前；注意必须先减再取模
         * （- 优先级高于 % 会错算成 nst-(h%nt)）。 */
        int new_r = ((nst - h) % nt + nt) % nt;
        int old_h = h + pull;
        int old_r = (old_h <= old_hist) ? screen_phys_row(s, -old_h) : -1;
        if (old_r >= 0 && old_r < s->total_lines && s->lines && s->lines[old_r].cells) {
            /* v1.8.45 #6：历史行完整保留——目标行宽度取 max(新窗格宽 nc, 源行
             * 实际 len)。收窄时源历史行 len=旧宽 > nc，整行右侧内容（命令/输出）
             * 全部保留，不截断；之后拖宽回来即可完整显示。 */
            int src_len = s->lines[old_r].len > 0 ? s->lines[old_r].len : s->cols;
            int dst_w = nc > src_len ? nc : src_len;
            if (line_alloc(&nl[new_r], dst_w, fill_attr))
                line_copy(&nl[new_r], &s->lines[old_r], src_len);
            if (s->line_wrap) nwrap[new_r] = s->line_wrap[old_r];  /* 续行关系随行走 */
        }
    }
    /* 可见区迁移：同样把旧物理行的续行标志带过来（src>=0 旧可见、src<0 旧历史
     * 回补）。 */
    for (int y = 0; y < nr; y++) {
        int new_r = (nst + y) % nt;
        int src = y - k;
        int old_r = RESIZE_OLD_ROW(src);
        if (old_r >= 0 && s->line_wrap) nwrap[new_r] = s->line_wrap[old_r];
    }
    #undef RESIZE_OLD_ROW
    (void)cr;

    // Migrate alt buffer
    /* BUG-8 (v1.8.11): 这里以前 rgb_valid 只搬了每行第 0 列（其余三个数组都搬了
     * cc 个），alt 屏一改窗口大小整屏真彩色就退化成 16 色。现在四个数组统一走
     * alt_row_copy()，不可能再漏。 */
    for (int y = 0; y < cr && y < s->rows; y++) {
        alt_row_copy(na, nafr, nabr, nav, y * nc,
                     s->alt_buffer, s->alt_fg_rgb, s->alt_bg_rgb, s->alt_rgb_valid, y * s->cols,
                     cc);
    }

    if (s->lines) {
        for (int i = 0; i < s->total_lines; i++) line_free(&s->lines[i]);
        free(s->lines);
    }
    free(s->line_wrap);
    free(s->alt_buffer); free(s->alt_fg_rgb); free(s->alt_bg_rgb); free(s->alt_rgb_valid);

    s->lines = nl;
    s->line_wrap = nwrap;
    s->alt_buffer = na;
    s->alt_fg_rgb = nafr;
    s->alt_bg_rgb = nabr;
    s->alt_rgb_valid = nav;
    s->cols = nc;
    s->rows = nr;
    s->total_lines = nt;
    /* 尺寸扩大（拖分屏条 / 拉大窗口）时，新扩出的列默认是「无真彩、纯黑底」空白，
     * 而窗格内 shell 已绘制区域是 ConPTY 的真彩色背景（深色但非纯黑），两者交界会
     * 显出一条颜色不同的带（拖条时「右侧多一块背景」）。标准终端 resize 会把旧行
     * 右缘的背景向右延展，因此这里把每行已迁移部分最右格的属性/前景/背景真彩复制
     * 给新增列（字符保持空格），新增行没有可继承的旧格则维持默认。 */
    if (nc > cc) {
        for (int idx = 0; idx < nt; idx++) {
            ScreenLine *ln = &nl[idx];
            if (!ln->cells || ln->len < nc) continue;
            /* 背景延展只填补「旧窗口宽度之外」的新列（x 从旧宽 cc 起）。但历史
             * 宽行（v1.8.45 #6 保留了旧宽内容，len 可能来自更宽的历史）其 cc..nc-1
             * 列已是迁移过来的真实内容，绝不能填空白覆盖——这种行整体跳过：它的
             * 内容列本就填满，无需延展。判据：该行是从更宽源迁移来的历史行。 */
            int is_wide_hist = 0;
            /* 历史区物理行：新可见首行 nst 之上（环回）。简单判据——宽行的 len
             * 大于本次新窗格宽 nc 时必然含旧宽内容；这里 len>=nc 且来自历史则跳过。
             * 为稳妥，仅对【可见区】行做延展（可见行宽度恒=nc 且 cc 左侧为真实内容，
             * cc 右侧是新空列）；历史行 len>=nc 说明是保留的宽行，整行跳过。 */
            int phys = idx;
            int rel = (phys - nst % nt + nt) % nt;   /* 相对新可见首行：0..nr-1 可见 */
            if (rel >= nr) is_wide_hist = 1;          /* 环回区 = 历史 */
            if (is_wide_hist) continue;
            int src = cc - 1;
            for (int x = cc; x < nc; x++) {
                ln->cells[x].Char.UnicodeChar = L' ';
                ln->cells[x].Attributes = ln->cells[src].Attributes;
                if (ln->fg_rgb) ln->fg_rgb[x] = ln->fg_rgb[src];
                if (ln->bg_rgb) ln->bg_rgb[x] = ln->bg_rgb[src];
                if (ln->rgb_valid) ln->rgb_valid[x] = ln->rgb_valid[src];
            }
        }
    }
    s->scroll_top = nst;
    s->hist_lines = new_hist;
    if (s->alt_hist_lines > nt - nr) s->alt_hist_lines = nt - nr;
    if (s->cursor_x >= nc) s->cursor_x = nc - 1;
    if (s->cursor_y >= nr) s->cursor_y = nr - 1;
    s->scroll_region_top = 0; s->scroll_region_bottom = nr - 1;
    s->wraparound_pending = 0;
    memset(s->tab_stops, 0, sizeof(s->tab_stops));
    for (int i = 0; i < nc && i < 512; i += 8) s->tab_stops[i] = 1;
    return 1;
}
/* ---------------------------------------------------------------------------
 * screen_resize：窗口 / pane 尺寸变化。
 *
 * 两条路径：
 *   - alt 屏（vim/htop 等全屏程序）：走 screen_resize_legacy()（v1.8.11 BUG-8
 *     之后沿用至今的物理行迁移；全屏程序 resize 后由 ConPTY 整屏重绘）。
 *   - 普通主屏：走 screen_resize_reflow()。v1.8.52 起改为【本地逻辑行 reflow】：
 *     尺寸（宽度 / 高度）变化时，把「滚动历史 + 可见区」按 line_wrap 合并回逻辑行
 *     （一条逻辑行 = 一次硬换行界定的内容），再按【新宽度】重新折行、重建物理环。
 *
 * 为什么要这么做（根因链）：
 *   旧实现收窄/分屏时历史行保留旧宽度、可见行却截断到新宽度，行内容随后又被
 *   ConPTY 按新宽重绘——line_wrap 停留在旧宽度下的角色（过期的软换行标志）。
 *   v1.8.50 的整窗 reflow 按 line_wrap 合并，遇到过期标志就把行拼错/断错，
 *   在历史/实时边界附近出现「该折行没折行（拼成一行）」和「多出空白行」。
 *   本地 reflow 保证尺寸变化后每一行的宽度恒为新宽、line_wrap 与内容一一对应：
 *   内容（逻辑行）完整保留、不重复、不丢，只按新宽度重新折行。
 *   hist_lines 语义 = 「可见区上方（滚动缓冲里）的显示行数」，收窄时同一批内容
 *   占用更多物理行、历史行数随之自然增多（与真实终端一致）。
 * ------------------------------------------------------------------------- */

int screen_resize(ScreenBuffer *s, int nc, int nr) {
    if (nc == s->cols && nr == s->rows) return 1;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    int hist_pre = s->hist_lines;
    int trace = !s->in_alt_screen && screen_trace_on();
    /* resize 后 conhost 会回一次整屏重绘；那次重绘结束时要把提示符重新顶到底行。
     * 旧快照是按旧尺寸存的，作废。 */
    screen_repaint_snapshot_free(s);
    s->resize_repaint_pass = 0;
    if (!s->in_alt_screen) s->resize_repaint_pending = 1;
    if (trace) screen_trace_ring("PRE ", s, nc, nr, hist_pre);
    int r = 0;
    if (s->in_alt_screen) r = screen_resize_legacy(s, nc, nr);
    else r = screen_resize_reflow(s, nc, nr);
    if (trace) screen_trace_ring("POST", s, nc, nr, hist_pre);
    return r;
}

