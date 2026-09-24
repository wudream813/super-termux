#ifndef WIN_TERMUX_TYPES_H
#define WIN_TERMUX_TYPES_H

#include "common.h"

// VT Parser States
enum {
    ST_NORMAL = 0,
    ST_ESC,           // ESC received
    ST_ESC_INTER,     // ESC intermediate bytes
    ST_CSI_ENTRY,     // CSI entry
    ST_CSI_PARAM,     // CSI parameters
    ST_CSI_INTER,     // CSI intermediate
    ST_CSI_IGNORE,    // CSI ignore rest
    ST_OSC_STRING,    // OSC string
    ST_DCS_ENTRY,     // DCS entry
    ST_DCS_PARAM,     // DCS parameters
    ST_DCS_INTER,     // DCS intermediate
    ST_DCS_PASSTHROUGH, // DCS passthrough
    ST_DCS_IGNORE,    // DCS ignore
    ST_SOS_STRING,    // SOS/PM/APC string
};

typedef struct {
    CHAR_INFO *cells;
    WORD *fg_rgb;
    WORD *bg_rgb;
    unsigned char *rgb_valid;
    int len;          /* 四个数组的实际分配宽度（容量） */
    int used;         /* 本行由终端输出实际写到的最右 cell+1；保留行尾真实空格 */
} ScreenLine;

typedef struct {
    ScreenLine *lines;
    int cols, rows, total_lines, scroll_top;
    unsigned char *line_wrap;  /* 并行于环形缓冲：每物理行 1=该行是上一物理行因自动
                                * 折行（软换行）折下来的续行；0=硬换行后的新行。
                                * v1.8.47：历史渲染据此把物理行合并成逻辑行并 reflow。 */
    int cursor_x, cursor_y, cursor_visible;
    WORD current_attr;
    int fg_color, bg_color, bold, underline, reverse_video;

    // VT parser state
    int state;
    char param_buf[256];
    int param_len;
    char inter_buf[16];
    int inter_len;
    int osc_num;
    char osc_buf[512];
    int osc_len;
    int osc_sep;

    int saved_cx, saved_cy;
    CHAR_INFO *alt_buffer;
    int in_alt_screen, alt_scroll_top;
    int origin_mode, auto_wrap, wraparound_pending;
    int scroll_region_top, scroll_region_bottom;
    int app_cursor_keys, app_keypad;
    int mouse_tracking, mouse_sgr, bracketed_paste, win32_input_mode;
    char tab_stops[512];
    char response_buf[256];
    int response_len;

    unsigned utf8_state, utf8_cp;
    int pane_index;
    int detect_col, detect_count;

    int fg_r, fg_g, fg_b, bg_r, bg_g, bg_b;
    int fg_rgb_on, bg_rgb_on;
    WORD *alt_fg_rgb, *alt_bg_rgb;
    unsigned char *alt_rgb_valid;
    int hist_lines;
    int alt_hist_lines;
    int cr_pending;   /* 上一个逻辑文本控制是否为 CR；OSC 标题不打断 CRLF */
    /* ConPTY 在【屏幕最后一行的末列】上的定位标记，-1 表示没有。
     *
     * conhost 窄屏折行续写有两种字节形态（09-17 真机 pane0 流实测）：
     *   (1) <写满一行> CR LF ESC[<下一行>;<末列>H <剩余内容>     —— 偏移 1327 等 52 处
     *   (2) <写满一行> ESC[<末行>;<越界列>H CR LF <剩余内容>     —— 偏移 1589/1899/3258
     * 形态 (1) 的续写首字符落在被续写行的末列，随后自动折行，screen_put_cp 的
     * screen_newline + screen_mark_softwrap 会把新行正确标成续行，无需额外规则。
     * 形态 (2) 不同：那个 CUP 只是 conhost 把「已折到下一行」的内部光标收回末列，
     * 紧跟的 CR LF 在底行触发【整屏滚动】，续写文本落在滚出来的新底行上 —— 而
     * screen_scroll_up 会把新底行的 line_wrap 清 0，于是这条记录被拆成两半
     * （实测 24 列下 bin / Ext2Fsd / Vape 三条：<DIR> 那半与名字那半分家）。
     * cup_eol_row 记下形态 (2) 的 CUP 落点行，供随后那个 LF 判定「滚动出来的新
     * 底行是上一行的续行」。只在 CUP 落到 (rows-1, cols-1) 时置位，任何其它输入
     * （可打印字符、别的 CSI/ESC、别的 C0）立即清掉，判定窗口极窄。 */
    int cup_eol_row;
    int repaint_candidate; /* 收到 DECTCEM hide，等待 HOME 确认 ConPTY 整屏重绘 */
    int repaint_active;    /* ConPTY viewport 重绘中：底边 CRLF 不得写入 scrollback */
    /* resize 后的第一次整屏重绘：conhost 增高时不回收滚动历史、只在下方补空行，
     * 所以重绘只带「它 viewport 里的那几行」，比窗格矮一截。repaint_snap 保存重绘前
     * 的可见行，重绘结束后用它把内容顶回底行（见 screen_repaint_reanchor）。 */
    int resize_repaint_pending;
    /* 同一次 resize 已经「让过」几趟重绘。conhost 的 resize 重绘常常是两趟：
     * 第一趟以 ESC[<rows>;1H ESC[?25h 收尾（光标停在最后一行 ⇒ 无事可做），
     * 第二趟才把光标停在提示符下面一行、并在下方补一串 ESC[K 空行 —— 真正需要
     * 重锚定的是第二趟。2026-09-20 真机 render_dump.log 实测：左窗格 61→5→92，
     * 第一趟就把 pending 吃掉，第二趟拿不到快照，24 行 dir 列表被空行覆盖。
     * 计数用来给「让过」封顶，避免 pending 一直挂着让后面无关的重绘误触发。 */
    int resize_repaint_pass;
    ScreenLine *repaint_snap;
    /* 与 repaint_snap 一一对应的软换行续行标志。2026-09-17：早先快照只存 cells，
     * reanchor 补回顶部那几行时无条件把 line_wrap 清 0，于是 conhost 重绘后整片
     * 可见区的续行标记丢失 —— 拖宽 reflow 无法把它们并回逻辑行，dir 记录被拆成
     * 「<DIR>」+ 一行纯空格 + 「   arena」这种三行（用户报的「有些地方多出了空格」）。
     * 打点实测：[CLR repaint顶部] dp=90..107 配 scroll_top=90 正好覆盖 rel 0..17。 */
    unsigned char *repaint_snap_wrap;
    int repaint_snap_rows;
    int repaint_snap_cols;
    /* 重绘前本地内容流的行数（hist + 可见区里到最后一条非空行为止）。conhost 把更老
     * 的行滚进它自己的滚动缓冲后，重绘只带最新那几行；拿它和这个数一比就知道该不该
     * 重新锚定、该下移几行（见 screen_repaint_reanchor）。 */
    int repaint_snap_content;
} ScreenBuffer;

typedef struct {
    int start_col, end_col, pane_idx;
    int close_start, close_end;
} PaneTabInfo;

typedef struct {
    int active;
    HPCON hpc;
    int conpty_cols, conpty_rows;   /* 上次 ResizePseudoConsole 下发的尺寸（v1.8.52：
                                    * 避免每帧同尺寸重复下发触发 ConPTY 整屏重绘） */
    HANDLE pipe_in, pipe_out, process, thread, read_thread;
    ScreenBuffer screen;
    char title[64];
    char full_title[256];
    int scroll_offset;
    int color;
    int is_settings;
    int is_about;
    int is_split_child;   /* 分屏子窗格：不作为独立标签页出现在标签栏 */
    int exited_hold;
    DWORD exit_code;
    /* v1.8.47：历史 reflow 视图缓存（渲染历史滚动时，逻辑行按当前窗格宽重排后
     * 的可见网格）。rf_rows/rf_cols 为网格尺寸，rf_grid 行主序 RGlyph；仅在向上
     * 回看（scroll_offset>0 且非 alt 屏）时有效，每帧由渲染侧重建。 */
    int rf_rows, rf_cols, rf_valid, rf_n;  /* rf_n=视口顶部连续历史(reflow)行数 */
    void *rf_grid;
    WCHAR input_history[256];
    int input_history_len;
    int input_history_pos;
} Pane;

typedef struct {
    /* v2.1.2：32 字节只放得下 10 个汉字，菜单项名字稍长就被从中间切断——不仅显示截断，
     * 悬停气泡里也是断的（snprintf 按字节截，尾巴还会留半个 UTF-8 序列变成 '?'）。 */
    char name[64];
    char cmd[256];
    char workdir[256];
    int color;          /* 启动默认标签颜色：0 = 跟随默认(蓝)，1-8 = 指定色 */
} ChooserItem;

typedef struct {
    int abs_y;
    int start_x;
    int end_x;
} SearchMatch;

typedef struct {
    int page;
    int selection;
    int scroll;
    int query_len;
    int query_pos;
    int focus;
    char query[64];
} PaletteViewState;

typedef struct {
    Pane panes[MAX_PANES];
    int pane_count, active_pane;
    volatile LONG running;
    int host_cols, host_rows, total_host_rows;
    HANDLE hOut, hIn;
    CRITICAL_SECTION cs;
    int needs_redraw, prefix_mode;
    int help_mode;
    int help_scroll;
    int chooser_mode;
    int custom_cmd_mode;
    char custom_cmd_buf[128];
    int custom_cmd_len;
    int custom_cmd_pos;
    int ctx_mode;
    int ctx_pane;
    int rename_mode;
    char rename_buf[64];
    int rename_len;
    int rename_pos;
    int settings_mode;
    int settings_sel;
    int settings_edit_idx;
    int settings_edit_field;
    char settings_edit_name[32];
    int settings_edit_name_len;
    int settings_edit_name_pos;
    char settings_edit_cmd[256];
    int settings_edit_cmd_len;
    int settings_edit_cmd_pos;
    int palette_mode;
    int palette_page;
    PaletteViewState palette_stack[PALETTE_STACK_MAX];
    int palette_stack_len;
    int palette_sel;
    char palette_query[64];
    int palette_query_len;
    int palette_query_pos;
    int palette_scroll;
    int palette_focus;
    int palette_field;
    int confirm_exit_mode;   /* confirm_on_exit = true 时的退出确认弹窗 */
    int confirm_close_mode;  /* confirm_on_close = true 时的关闭窗格/标签确认弹窗 */
    int palette_edit_idx;
    int palette_edit_new;
    DWORD orig_in_mode, orig_out_mode;
    UINT orig_cp, orig_input_cp;
    PaneTabInfo tab_info[MAX_PANES + 3];
    int tab_count;
} MuxState;

// Global State Externs
extern MuxState g_mux;
extern int g_pop_anchor_x;
extern int g_mouse_x, g_mouse_y;
extern int g_mouse_prev_in_tabbar;
extern WCHAR g_high_surrogate;
extern WCHAR g_orig_title[256];

extern int g_hover_preview_pane;
extern DWORD64 g_hover_preview_start;
extern int g_hover_preview_active;
extern int g_hover_chooser_idx;
extern DWORD64 g_hover_chooser_start;
extern int g_hover_chooser_active;
extern int g_hover_settings_name_idx;
extern DWORD64 g_hover_settings_name_start;
extern int g_hover_settings_name_active;
extern int g_hover_settings_cmd_idx;
extern DWORD64 g_hover_settings_cmd_start;
extern int g_hover_settings_cmd_active;

extern int g_sb_dragging;
extern int g_sb_grab_offset;
/* 滚动条拖动锁定在哪个 pane 上（-1 = 没在拖）。拖动期间分屏层不得切焦点，
 * 否则鼠标划过另一个窗格就会变成拖那个窗格的滚动条（2026-09-20 用户报）。 */
extern int g_sb_drag_pane;

/* 瞬时警告提示（toast）：分屏空间不足等操作失败时，在屏幕底部中央短暂显示一条
 * 黄字消息。g_toast_until 为 GetTickCount64() 过期时刻（0=无提示）。 */
extern char g_toast_msg[96];
extern DWORD64 g_toast_until;
void toast_show(const char *msg, unsigned int ms);

// Copy Mode & Selection
extern int g_copy_mode;
extern int g_copy_sel_active;
extern int g_copy_cx, g_copy_cy;
extern int g_copy_end_x;   /* 选区端点（键盘=主格光标；鼠标=原始列） */
extern int g_copy_anchor_x, g_copy_anchor_abs_y;
extern int g_copy_block;   /* 1 = 矩形（框）选区，0 = 行内连续选区 */
extern int g_copy_quick;   /* Shift/Alt 点选发起的临时复制会话 */
/* 复制 / 搜索这两个模态属于某一个 pane：切到别的标签页必须先收回，
 * 否则两个标签页会同时响应同一套按键。-1 = 当前没有模态。 */
extern int g_ui_mode_pane;
extern int g_mouse_selecting;
extern int g_mouse_sel_sx, g_mouse_sel_s_abs_y;
extern int g_mouse_sel_ex, g_mouse_sel_e_abs_y;

// Scrollback History Search
extern SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
extern int g_search_match_count;
extern int g_search_match_cur;
extern int g_search_mode;
extern int g_search_active;
extern char g_search_buf[64];
extern int g_search_len, g_search_pos;
/* 终端来了新数据、搜索匹配需要重算。由 pane 读线程置位，主循环在渲染前消费一次
 * （每帧最多重扫一次，避免每个 ReadFile 分块都扫一遍整个滚动缓冲）。 */
extern int g_search_dirty;

// Diagnostic functions
void dump_pane_bytes(int pane_idx, const char *data, int len);
void dump_render_output(const char *out, int len, int pcols, int prows, int hcols, int hrows);
void dump_delta_output(const char *data, int delta_len, int full_len);
void log_mouse_event(const char *tag, const MOUSE_EVENT_RECORD *me);
void host_write(const char *data, int len);

#endif // WIN_TERMUX_TYPES_H
