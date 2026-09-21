/* ---------------------------------------------------------------------------
 * main_posix.c —— Linux / macOS 的入口与控制台主循环。
 *
 * 与 src/main.c（Windows）的对应关系：
 *   GetStdHandle / SetConsoleMode      ->  plat_console_init（termios 原始模式）
 *   GetConsoleScreenBufferInfo         ->  plat_console_size（TIOCGWINSZ）
 *   WINDOW_BUFFER_SIZE_EVENT           ->  SIGWINCH（由 term_input_posix.c 转成事件）
 *   WaitForSingleObject+ReadConsoleInputW -> plat_console_read（poll + 转义序列解析）
 *   WriteConsoleA                      ->  write(STDOUT_FILENO)
 *   SetConsoleCtrlHandler              ->  SIGTERM/SIGHUP + atexit 恢复 termios
 *
 * 全局状态、toast、dump 函数在 src/globals.c，两边共用。
 * ------------------------------------------------------------------------- */
#include "common.h"
#include "platform.h"
#include "types.h"
#include "screen.h"
#include "utf8.h"
#include "vt.h"
#include "config.h"
#include "pane.h"
#include "render.h"
#include "input.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "split.h"

#include <signal.h>
#include <unistd.h>

extern int g_dump_enabled;      /* 定义在 src/globals.c */

void host_write(const char *s, int len) {
    int off = 0;
    while (off < len) {
        ssize_t w = write(STDOUT_FILENO, s + off, (size_t)(len - off));
        if (w <= 0) break;
        off += (int)w;
    }
}

static void host_printf(const char *fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        host_write(buf, len);
    }
}

static void handle_resize(void) {
    int nc = 0, nt = 0;
    if (plat_console_size(&nc, &nt) != 0) return;
    if (nc < 4) nc = 4;
    if (nt < 2) nt = 2;
    int nr = nt - 1;
    if (nc == g_mux.host_cols && nt == g_mux.total_host_rows) return;

    EnterCriticalSection(&g_mux.cs);
    g_mux.host_cols = nc; g_mux.total_host_rows = nt; g_mux.host_rows = nr;
    int pane_cols = nc;
    int split_now = split_is_split();
    for (int i = 0; i < g_mux.pane_count; i++) if (g_mux.panes[i].active) {
        if (!split_now) {
            screen_resize(&g_mux.panes[i].screen, pane_cols, nr);
            if (g_mux.panes[i].scroll_offset > g_mux.panes[i].screen.hist_lines)
                g_mux.panes[i].scroll_offset = g_mux.panes[i].screen.hist_lines;
        }
        g_mux.panes[i].screen.detect_count = 0;
    }
    LeaveCriticalSection(&g_mux.cs);

    /* 分屏下各 pane 的尺寸由分屏矩形决定（渲染帧里的 pane_resize_to 会统一调整）；
     * 非分屏 pane 直接按整屏尺寸通知 pty。 */
    for (int i = 0; i < g_mux.pane_count; i++) if (g_mux.panes[i].active) {
        if (!split_now && g_mux.panes[i].process != NULL_HANDLE &&
            (g_mux.panes[i].conpty_cols != pane_cols || g_mux.panes[i].conpty_rows != nr)) {
            plat_proc_resize(g_mux.panes[i].process, g_mux.panes[i].pipe_in,
                             g_mux.panes[i].pipe_out, pane_cols, nr);
            g_mux.panes[i].conpty_cols = pane_cols;
            g_mux.panes[i].conpty_rows = nr;
        }
    }
    g_mux.needs_redraw = 1;
}

/* 移植期调试开关：TERMUX_PORT_DEBUG=<文件> 时，把每条 INPUT_RECORD 和当时
 * 的 prefix_mode 追加写进去。键盘链路出问题时（「按键没反应」）这是唯一能
 * 区分「字节没翻译对」还是「引擎没接住」的手段，所以留在代码里。 */
static FILE *g_port_dbg = NULL;
static void port_dbg(const char *fmt, ...) {
    if (!g_port_dbg) {
        const char *f = getenv("TERMUX_PORT_DEBUG");
        if (!f || !*f) return;
        g_port_dbg = fopen(f, "a");
        if (!g_port_dbg) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_port_dbg, fmt, ap);
    va_end(ap);
    fflush(g_port_dbg);
}

static void handle_input(void) {
    INPUT_RECORD rec[128];
    ULONGLONG last_render = 0;
    while (g_mux.running) {
        int wait_ms = g_mux.needs_redraw ? 8 : 25;
        int cnt = plat_console_read(rec, 128, wait_ms);
        int has_input = cnt > 0;
        for (int i = 0; i < cnt; i++) {
            if (rec[i].EventType == KEY_EVENT) {
                port_dbg("KEY vk=0x%02X ctrl=0x%08lX uc=U+%04X down=%d prefix=%d\n",
                         (unsigned)rec[i].Event.KeyEvent.wVirtualKeyCode,
                         (unsigned long)rec[i].Event.KeyEvent.dwControlKeyState,
                         (unsigned)(rec[i].Event.KeyEvent.uChar.UnicodeChar & 0xFFFF),
                         (int)rec[i].Event.KeyEvent.bKeyDown, g_mux.prefix_mode);
                handle_key(&rec[i].Event.KeyEvent);
                port_dbg("    after: prefix=%d redraw=%d panes=%d\n",
                         g_mux.prefix_mode, g_mux.needs_redraw, g_mux.pane_count);
            } else if (rec[i].EventType == MOUSE_EVENT) handle_mouse(&rec[i].Event.MouseEvent);
            else if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) handle_resize();
        }

        /* toast 到期自动消失。 */
        if (g_toast_until && GetTickCount64() >= g_toast_until) {
            g_toast_until = 0;
            g_toast_msg[0] = 0;
            g_mux.needs_redraw = 1;
        }

        /* 各 hover 预览的计时（与 Windows 侧一致）。 */
        if (g_hover_preview_pane >= 0 && !g_hover_preview_active &&
            GetTickCount64() - g_hover_preview_start >= 1500) {
            g_hover_preview_active = 1; g_mux.needs_redraw = 1;
        }
        if (g_hover_chooser_idx >= 0 && !g_hover_chooser_active &&
            GetTickCount64() - g_hover_chooser_start >= 1000) {
            g_hover_chooser_active = 1; g_mux.needs_redraw = 1;
        }
        if (g_hover_settings_name_idx >= 0 && !g_hover_settings_name_active &&
            GetTickCount64() - g_hover_settings_name_start >= 1000) {
            g_hover_settings_name_active = 1; g_mux.needs_redraw = 1;
        }
        if (g_hover_settings_cmd_idx >= 0 && !g_hover_settings_cmd_active &&
            GetTickCount64() - g_hover_settings_cmd_start >= 1000) {
            g_hover_settings_cmd_active = 1; g_mux.needs_redraw = 1;
        }

        reap_dead_panes();
        if (g_mux.active_pane < 0 || !g_mux.panes[g_mux.active_pane].active) {
            int f = -1;
            for (int i = 0; i < g_mux.pane_count; i++) if (g_mux.panes[i].active) { f = i; break; }
            if (f >= 0) g_mux.active_pane = f; else { g_mux.running = 0; break; }
        }
        if (g_mux.needs_redraw) {
            ULONGLONG now = GetTickCount64();
            if (has_input || (now - last_render >= 12)) {
                /* 搜索开着时新到的终端输出要先重算匹配再画（每帧最多一次）。 */
                search_refresh_live();
                port_dbg("RENDER 开始 needs_redraw=%d has_input=%d now=%llu\n",
                         g_mux.needs_redraw, has_input, (unsigned long long)now);
                render_screen();
                port_dbg("RENDER 结束 needs_redraw=%d\n", g_mux.needs_redraw);
                last_render = now;
            }
        }
    }
}

/* 被信号打断时也要把 termios 和备用屏还原，否则用户的 shell 会留在原始模式里
 * （不回显、不换行），看起来像「终端坏了」。 */
static void restore_console(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    host_write("\x1b[?1003l\x1b[?1006l\x1b[?1049l\x1b[?25h\x1b[0m", 33);
    plat_console_shutdown();
}

static void on_term_signal(int sig) {
    (void)sig;
    InterlockedExchange(&g_mux.running, 0);
}

int main(void) {
    memset(&g_mux, 0, sizeof(g_mux));
    InitializeCriticalSection(&g_mux.cs);

    if (plat_console_init(1) != 0) {
        fprintf(stderr, "termux: 需要在终端里运行（stdin/stdout 都得是 tty）\n");
        DeleteCriticalSection(&g_mux.cs);
        return 1;
    }
    atexit(restore_console);

    int cols = 0, trows = 0;
    if (plat_console_size(&cols, &trows) != 0 || cols <= 0 || trows <= 0) {
        fprintf(stderr, "termux: 取不到终端尺寸\n");
        restore_console();
        DeleteCriticalSection(&g_mux.cs);
        return 1;
    }
    g_mux.host_cols = cols < 4 ? 4 : cols;
    g_mux.total_host_rows = trows < 2 ? 2 : trows;
    g_mux.host_rows = g_mux.total_host_rows - 1;

    g_dump_enabled = getenv("TERMUX_DUMP") != NULL;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    /* SIGINT 不拦：原始模式下 Ctrl+C 是 0x03 字节，要原样转发给前台 shell。 */
    signal(SIGPIPE, SIG_IGN);

    load_config();

    /* 备用屏 + 鼠标追踪（1003 = 任何移动都报，1006 = SGR 扩展坐标）。
     * 关掉括号粘贴：引擎不处理 200~/201~，让它当普通文本进来更省心。 */
    if (g_mouse_enabled)
        host_printf("\x1b[?1049h\x1b[?2004l\x1b[?1003h\x1b[?1006h\x1b[2J\x1b[H\x1b[?25l");
    else
        host_printf("\x1b[?1049h\x1b[?2004l\x1b[2J\x1b[H\x1b[?25l");

    g_mux.running = 1;
    split_reset();
    int first = create_pane();
    if (first < 0) {
        host_printf("\x1b[31m启动失败：无法创建窗格（检查 $SHELL 是否可执行）\x1b[0m\r\n");
        Sleep(3000);
        restore_console();
        DeleteCriticalSection(&g_mux.cs);
        return 1;
    }
    split_init_tab(first);
    g_mux.active_pane = first;
    if (g_default_startup == 1) g_mux.help_mode = 1;
    g_mux.needs_redraw = 1;
    render_screen();
    handle_input();
    for (int i = 0; i < g_mux.pane_count; i++) close_pane(i);

    restore_console();
    render_cleanup();
    DeleteCriticalSection(&g_mux.cs);
    printf("Bye!\n");
    return 0;
}
