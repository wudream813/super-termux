/* conpty_repro.c — 在真实 Windows ConPTY 上复现「拖动分屏后历史撕裂」。
 *
 * 与 Linux 侧 replay 的根本区别：字节流来自真实 conhost，不是我手工构造的
 * fixture。流程：
 *   1. CreatePseudoConsole 起一个窄窗格（默认 16 列）跑 cmd /c dir
 *   2. 把 conhost 的输出喂给真实的 screen.c / vt.c
 *   3. ResizePseudoConsole 模拟拖动分隔条（窄 -> 宽）
 *   4. 检查末态有没有撕裂（同一条 dir 记录被拆成多行 / 行内大段填充空格）
 *
 * 编译（在 Windows 上，原生 MinGW）：
 *   gcc -O1 -Wall -Wextra -Iinclude src/screen.c src/vt.c src/utf8.c \
 *       conpty_repro.c -o conpty_repro.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "screen.h"
#include "vt.h"
#include "types.h"

/* screen.c 需要的全局量 */
int g_scrollback_lines = 9001;
MuxState g_mux;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_match_count = 0;
int g_search_match_cur = -1;
int g_search_active = 0;


/* conhost 那一端的管道句柄，spawn 之后再关 */
static HANDLE g_pty_r = NULL, g_pty_w = NULL;

/* 拖动过程中每一步的错位采样 */
static int g_drag_full[64], g_drag_nonempty[64], g_drag_cols[64], g_drag_n = 0;

/* ---- ConPTY 管道辅助 ---- */
static HRESULT make_pty(int cols, int rows, HPCON *hpc, HANDLE *hin, HANDLE *hout) {
    HANDLE ci_r = NULL, ci_w = NULL, co_r = NULL, co_w = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&ci_r, &ci_w, &sa, 0)) return HRESULT_FROM_WIN32(GetLastError());
    if (!CreatePipe(&co_r, &co_w, &sa, 0)) return HRESULT_FROM_WIN32(GetLastError());
    COORD sz = { (SHORT)cols, (SHORT)rows };
    HRESULT hr = CreatePseudoConsole(sz, ci_r, co_w, 0, hpc);
    if (FAILED(hr)) { CloseHandle(ci_r); CloseHandle(co_w); return hr; }
    /* 注意：ci_r / co_w 是 conhost 那一端的句柄，必须在 CreateProcess 之后才关。
     * 这里先留着，由调用方在 spawn 完成后关闭（见 main 里的 g_pty_r / g_pty_w）。
     * 过早关闭会让 conhost 拿不到有效的输入/输出端，cmd 启动即退出
     * （实测 exitcode=0、dir 命令写进去后 drain=0）。 */
    g_pty_r = ci_r;
    g_pty_w = co_w;
    *hin = ci_w;   /* 我们写入 -> conhost 的 stdin */
    *hout = co_r;  /* conhost 的 stdout -> 我们读取 */
    return S_OK;
}

static int spawn_cmd(HPCON hpc, const WCHAR *cmdline, HANDLE *hproc) {
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    SIZE_T need = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &need);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
        GetProcessHeap(), 0, need);
    if (!si.lpAttributeList) return 0;
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &need)) return 0;
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof(hpc), NULL, NULL))
        return 0;
    /* 必须显式清掉标准句柄：ConPTY 模式下子进程的输出应当只走伪控制台，
     * 但 hStdOutput 为 NULL 时 Windows 会回退到父进程的 stdout，导致 conhost
     * 的输出同时写进我们的管道和父进程控制台 —— 实测 dir 的几百行结果出现在
     * 本程序的 stdout 重定向文件里（3493 行），却从未作为一次 ReadFile 返回，
     * 于是模型 hist 恒为 0。设成 INVALID_HANDLE_VALUE 断掉这条回退路径。 */
    si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
    si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    WCHAR cl[512];
    _snwprintf(cl, 512, L"%s", cmdline);
    if (!CreateProcessW(NULL, cl, NULL, NULL, FALSE,
            EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi))
        return 0;
    CloseHandle(pi.hThread);
    *hproc = pi.hProcess;
    return 1;
}

/* 读光 conhost 当前可读的输出，喂进模型。返回读到的字节数。
 * ConPTY 的输出端不能用 PeekNamedPipe 探长度（实测恒为 0，导致 drain 空转、
 * 模型全空）。改用「后台线程阻塞 ReadFile + 主线程按截止时刻收工」：
 * 线程把读到的字节直接喂进模型，主线程到点就返回。 */
static volatile LONG g_feed_seq = 0;

/* ptymin.exe 已验证：主线程直接阻塞 ReadFile 能正常拿到 conhost 输出。
 * 之前的后台线程版本里 dir 输出没进模型（hist 恒 0、输出泄漏到 SSH stdout），
 * 怀疑是线程与主线程并发访问 ScreenBuffer 造成的，这里去掉线程。 */
typedef struct { HANDLE h; ScreenBuffer *s; volatile LONG total; volatile LONG stop; volatile LONG dumped; volatile LONG fed; volatile LONG seq; } DrainCtx;
static DWORD WINAPI drain_thread(LPVOID p) {
    DrainCtx *d = (DrainCtx *)p;
    char buf[65536];
    {
        DWORD n = 0;
        if (!ReadFile(d->h, buf, sizeof buf, &n, NULL) || n == 0) return 0;
        /* 头 96 字节按十六进制+可见字符转储，确认 conhost 到底发了什么 */
        /* 转储第 3 批（dir 输出开始的那批），看 conhost 在 16 列下的折行模式 */
        if (InterlockedIncrement(&g_feed_seq) == 4) {
            printf("[raw] n=%lu 全量:\n", (unsigned long)n);
            for (DWORD i = 0; i < n; i++) {
                printf("%02x ", (unsigned char)buf[i]);
                if ((i & 15) == 15) printf("\n");
            }
            printf("\n[txt] |");
            for (DWORD i = 0; i < n; i++) {
                unsigned char c = (unsigned char)buf[i];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf("|\n");
            fflush(stdout);
        }
        int hy0 = d->s->hist_lines, cy0 = d->s->cursor_y, cx0 = d->s->cursor_x;
        screen_process_output(d->s, buf, (int)n);
        printf("[feed] n=%-6lu hist %d->%-4d cursor (%d,%d)->(%d,%d) cols=%d\n",
               (unsigned long)n, hy0, d->s->hist_lines, cy0, cx0,
               d->s->cursor_y, d->s->cursor_x, d->s->cols);
        fflush(stdout);
        InterlockedAdd(&d->total, (LONG)n);
    }
    return 0;
}
/* 同步读：用一个「读一次就返回」的短命线程 + WaitForSingleObject 超时，
 * 这样既能拿到阻塞 ReadFile 的数据，又不会在没数据时永久卡住。
 * 每次调用读到「超时窗口内没有新数据」为止。 */
static long g_seq_base = 0;
static long drain(HANDLE hout, ScreenBuffer *s, int timeout_ms, long cap) {
    (void)cap;
    long total = 0;
    DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
    while ((int)(deadline - GetTickCount()) > 0) {
        DrainCtx d;
        g_seq_base += 0;  /* seq 由 InterlockedIncrement 全局累加，见下 */
        d.h = hout; d.s = s; d.total = 0; d.stop = 0; d.dumped = 1; d.fed = 1; d.seq = g_seq_base;
        HANDLE th = CreateThread(NULL, 0, drain_thread, &d, 0, NULL);
        if (!th) break;
        /* 给线程 300ms 拿一批数据；拿不到就收工 */
        if (WaitForSingleObject(th, 300) == WAIT_TIMEOUT) {
            d.stop = 1;
            CancelSynchronousIo(th);
            WaitForSingleObject(th, 1000);
            CloseHandle(th);
            if (d.total == 0) break;   /* 这一轮没数据，drain 结束 */
        } else {
            CloseHandle(th);
        }
        total += d.total;
        if (d.total == 0) break;
    }
    return total;
}

/* ---- 末态检查 ---- */
static int row_has(ScreenBuffer *s, int rel, const char *needle, char *out, int outsz) {
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) return 0;
    ScreenLine *ln = &s->lines[pr];
    int n = 0;
    for (int x = 0; x < ln->used && n < outsz - 1; x++) {
        WCHAR c = ln->cells[x].Char.UnicodeChar;
        if (c == 0) continue;
        out[n++] = (c < 128) ? (char)c : '?';
    }
    out[n] = 0;
    return strstr(out, needle) != NULL;
}

int main(int argc, char **argv) {
    /* 输出到管道时 stdout 是全缓冲的；程序卡在阻塞 ReadFile 上时缓冲区不会
     * flush，看起来就像「没有任何输出」。改成行缓冲，并在每个阶段打点。 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[stage] enter main\n"); fflush(stdout);
    int narrow = (argc > 1) ? atoi(argv[1]) : 16;
    int wide   = (argc > 2) ? atoi(argv[2]) : 92;
    int rows   = (argc > 3) ? atoi(argv[3]) : 29;
    /* argv[5] = 拖动宽度序列编号，用于扫不同拖动形态 */
    int seqid  = (argc > 5) ? atoi(argv[5]) : 0;

    memset(&g_mux, 0, sizeof g_mux);
    ScreenBuffer s;
    memset(&s, 0, sizeof s);
    printf("[stage] screen_init...\n"); fflush(stdout);
    if (!screen_init(&s, narrow, rows)) { printf("[fail] screen_init\n"); return 2; }
    printf("[stage] screen_init ok cols=%d rows=%d total=%d\n", s.cols, s.rows, s.total_lines); fflush(stdout);

    HPCON hpc = NULL;
    HANDLE hin = NULL, hout = NULL;
    printf("[stage] CreatePseudoConsole...\n"); fflush(stdout);
    HRESULT hr = make_pty(narrow, rows, &hpc, &hin, &hout);
    if (FAILED(hr)) { printf("[fail] CreatePseudoConsole 0x%08lx\n", (unsigned long)hr); return 2; }
    printf("[stage] pty ok hin=%p hout=%p 请求尺寸=%dx%d\n", (void*)hin, (void*)hout, narrow, rows); fflush(stdout);
    /* 确认 conhost 实际采用的尺寸：ResizePseudoConsole 返回 S_OK 说明尺寸被接受 */
    {
        COORD chk = { (SHORT)narrow, (SHORT)rows };
        HRESULT rhr = ResizePseudoConsole(hpc, chk);
        printf("[stage] ResizePseudoConsole(%dx%d) = 0x%08lx\n", narrow, rows, (unsigned long)rhr);
        fflush(stdout);
    }

    HANDLE hproc = NULL;
    printf("[stage] spawn cmd...\n"); fflush(stdout);
    /* 交互式 cmd（裸 cmd.exe 或 /k）在 ConPTY 下拿不到 win32-input-mode 格式的
     * 输入会立刻退出（实测 exitcode=0、写入 dir 后 drain=0）。banner 里的
     * ESC[?9001h 就是 win32-input-mode，普通 \r 不被识别。
     * 改用一次性命令：dir 先跑完产生真实折行内容，再做 resize。
     * /c 后面用 & pause 之外的方式保持进程活着以便 resize —— 这里直接让 dir
     * 输出足够多的行，进程结束后 ConPTY 仍会保留屏幕内容供 resize 重绘。 */
    if (!spawn_cmd(hpc, L"cmd.exe /c dir C:\\Windows\\System32\\*.dll", &hproc)) {
        printf("[fail] spawn cmd err=%lu\n", (unsigned long)GetLastError()); return 2; }
    printf("[stage] cmd spawned hproc=%p\n", (void*)hproc); fflush(stdout);
    /* 现在才关 conhost 端的句柄：子进程已经继承了它们 */
    CloseHandle(g_pty_r);
    CloseHandle(g_pty_w);

    printf("ConPTY 复现：窄=%d 宽=%d 行=%d\n", narrow, wide, rows);

    /* 1. 等 banner */
    long nb = drain(hout, &s, 12000, 16 << 20);
    printf("banner drain 读到 %ld 字节; cols=%d hist=%d cursor=(%d,%d)\n",
           nb, s.cols, s.hist_lines, s.cursor_y, s.cursor_x);
    printf("[stage] banner drain 完成\n"); fflush(stdout);

    /* 2. 在窄窗格里跑 dir（产生会被折行的长行） */
    /* dir 由 spawn 时的命令行执行，这里只负责读光输出 */
    (void)hin;
    long nd = drain(hout, &s, 12000, 16 << 20);
    printf("dir 后（窄 %d 列）: drain=%ld hist=%d cursor=(%d,%d)\n",
           narrow, nd, s.hist_lines, s.cursor_y, s.cursor_x);
    fflush(stdout);

    /* 记录窄屏下 kernel32 出现在几个物理行 */
    int narrow_hits = 0;
    for (int r = -s.hist_lines; r < s.rows; r++) {
        char b[256];
        if (row_has(&s, r, "kernel32", b, sizeof b)) narrow_hits++;
    }
    printf("  窄屏下含 kernel32 的物理行数 = %d\n", narrow_hits);

    /* 3. 模拟拖动分隔条。真机 screen_resize_trace.log 显示一次拖动产生 172 次
     * resize（来回拖、每帧都 resize），不是单向 8 步。
     *
     * 关键：必须复刻真机 pane_resize_to 的门控行为 —— 拖动期间本地
     * screen_resize 每帧都做，但 ResizePseudoConsole 被 split_drag_active() 跳过，
     * conpty 宽度保持不动，松手后第一帧才补发一次。
     * 早先这个循环每步都发 ResizePseudoConsole，测的是「conhost 同步跟随」，
     * 与真机行为不符，所以复现不出「拖动中错位、松手才恢复」。 */
    {
        /* 多组拖动形态。seq0 是原来的 15↔92 来回；其余覆盖「拖到极窄」和
         * 「快速小幅抖动」，用来扫「历史直接消失」到底在什么宽度下触发。 */
        const int seq0[] = { 15, 16, 15, 20, 16, 30, 15, 45, 16, 60, 15, 76, 16, 92,
                             30, 16, 50, 15, 70, 20, 92 };
        const int seq1[] = { 4, 5, 4, 6, 4, 8, 5, 10, 4, 12, 6, 4, 8, 16, 4, 30, 92 };
        const int seq2[] = { 2, 3, 2, 4, 2, 3, 2, 5, 2, 4, 2, 6, 2, 92 };
        const int seq3[] = { 16, 15, 16, 15, 16, 15, 16, 15, 16, 15, 16, 15, 16, 92 };
        const int seq4[] = { 92, 4, 92, 4, 92, 4, 92, 4, 92 };
        const int *seq = seq0;
        int nseq = (int)(sizeof seq0 / sizeof seq0[0]);
        switch (seqid) {
            case 1: seq = seq1; nseq = (int)(sizeof seq1 / sizeof seq1[0]); break;
            case 2: seq = seq2; nseq = (int)(sizeof seq2 / sizeof seq2[0]); break;
            case 3: seq = seq3; nseq = (int)(sizeof seq3 / sizeof seq3[0]); break;
            case 4: seq = seq4; nseq = (int)(sizeof seq4 / sizeof seq4[0]); break;
            default: break;
        }
        printf("[seq] id=%d nseq=%d\n", seqid, nseq);
        fflush(stdout);
        if (nseq > 64) nseq = 64;
        int conpty_cols = narrow;   /* 拖动期间冻结，模拟 conpty_cols 不更新 */
        /* FREEZE_MODEL=1 复刻修复后的 pane_resize_to：拖动中本地 screen_resize
         * 也跳过，两侧一起冻结。FREEZE_MODEL=0 是修复前的行为（只冻结 conhost）。 */
        int freeze_model = (argc > 4) ? atoi(argv[4]) : 0;
        printf("[mode] freeze_model=%d\n", freeze_model);
        fflush(stdout);
        for (int i = 0; i < nseq; i++) {
            int c = seq[i];
            if (!freeze_model) screen_resize(&s, c, rows);   /* 本地模型：修复前每帧跟随 */
            /* conhost：拖动中【不】resize，只读它按旧宽度产生的输出 */
            drain(hout, &s, 120, 4 << 20);
            /* 拖动中的错位指标：模型已按新宽度 reflow，但行内容还是 conhost 按
             * conpty_cols 排版的。表现为「物理行写满到 model_cols，而它本该是
             * 一条 conpty_cols 宽的行的片段」。统计写满整行的物理行占比。 */
            {
                int full = 0, nonempty = 0;
                for (int r = -s.hist_lines; r < s.rows && r < 0; r++) {
                    int pr = screen_phys_row(&s, r);
                    if (pr < 0 || pr >= s.total_lines || !s.lines || !s.lines[pr].cells) continue;
                    ScreenLine *ln = &s.lines[pr];
                    int used = ln->used > s.cols ? s.cols : ln->used;
                    while (used > 0 && ln->cells[used - 1].Char.UnicodeChar == L' ') used--;
                    if (used <= 0) continue;
                    nonempty++;
                    if (used >= s.cols) full++;
                }
                if (i < 64) {
                    g_drag_full[i] = full;
                    g_drag_nonempty[i] = nonempty;
                    g_drag_cols[i] = c;
                    g_drag_n = i + 1;
                }
            }
            if (i % 7 == 0) {
                printf("[drag] step %d model_cols=%d conpty_cols=%d hist=%d%s\n",
                       i, c, conpty_cols, s.hist_lines,
                       c != conpty_cols ? "  <-- 宽度错位" : "");
                fflush(stdout);
            }
            /* 每一步都记录 hist，捕捉「历史直接消失」发生的确切时机 */
            /* 用户报的症状是「往上翻不到旧内容」——能翻多少由 scroll_limit 决定，
             * 不是 hist_lines。拖动中模型宽度冻结，但渲染用的是
             * min(rc->cols, s->cols)，所以必须按【渲染宽度】算 limit 才是用户
             * 实际能翻到的量。这里同时打两种宽度下的 limit。 */
            {
                int lim_model = screen_scroll_limit(&s);
                int lim_render = 0;
                {
                    int h = screen_reflow_height(&s, c);
                    lim_render = h - rows;
                    if (lim_render < 0) lim_render = 0;
                }
                printf("[hist] step%-3d model_cols=%-3d hist=%-5d height=%-5d lim_model=%-5d lim_render(w=%d)=%-5d\n",
                       i, c, s.hist_lines, screen_reflow_height(&s, s.cols),
                       lim_model, c, lim_render);
            }
            fflush(stdout);
        }
        /* 松手：第一帧一次性补发（本地 reflow + conhost resize） */
        screen_resize(&s, wide, rows);
        COORD rsz = { (SHORT)wide, (SHORT)rows };
        ResizePseudoConsole(hpc, rsz);
        conpty_cols = wide;
        printf("\n=== 拖动过程错位采样（model_cols vs 写满整行的历史行数）===\n");
        for (int i = 0; i < g_drag_n; i++)
            printf("  step%-3d cols=%-3d 写满整行=%-4d / 非空历史行=%-4d\n",
                   i, g_drag_cols[i], g_drag_full[i], g_drag_nonempty[i]);
        fflush(stdout);
    }
    screen_resize(&s, wide, rows);
    COORD fsz = { (SHORT)wide, (SHORT)rows };
    ResizePseudoConsole(hpc, fsz);
    drain(hout, &s, 3000, 8 << 20);

    printf("拖宽后: cols=%d hist=%d\n", s.cols, s.hist_lines);

    /* 4. 检查撕裂 */
    int wide_hits = 0, torn = 0;
    printf("\n=== 末态含 kernel32 的行 ===\n");
    for (int r = -s.hist_lines; r < s.rows; r++) {
        char b[512];
        if (row_has(&s, r, "kernel32", b, sizeof b)) {
            wide_hits++;
            int pr = screen_phys_row(&s, r);
            int wrap = (s.line_wrap && pr >= 0) ? s.line_wrap[pr] : -1;
            printf("  rel%+04d wrap=%d |%s|\n", r, wrap, b);
            /* 撕裂特征：这一行不是完整的 dir 记录（缺日期或缺大小） */
            if (!strstr(b, "kernel32.dll")) torn++;
        }
    }

    printf("\n窄屏物理行=%d  宽屏物理行=%d  疑似撕裂=%d\n", narrow_hits, wide_hits, torn);
    fflush(stdout);

    /* 真机症状的量化指标：CUP 跳列产生的填充空格会在拖宽后变成行内大段空格。
     * 统计末态所有行里「连续 >=6 个空格」的出现次数 —— 真实 dir 输出的对齐
     * 空格最多 4-5 个（日期与大小之间），>=6 基本只可能来自 CUP 填充。 */
    {
        int runs = 0, maxrun = 0, lines_hit = 0;
        for (int r = -s.hist_lines; r < s.rows; r++) {
            int pr = screen_phys_row(&s, r);
            if (pr < 0 || pr >= s.total_lines || !s.lines || !s.lines[pr].cells) continue;
            ScreenLine *ln = &s.lines[pr];
            int cur = 0, hit = 0;
            for (int x = 0; x < ln->used; x++) {
                if (ln->cells[x].Char.UnicodeChar == L' ') {
                    cur++;
                } else {
                    if (cur >= 6) { runs++; hit = 1; if (cur > maxrun) maxrun = cur; }
                    cur = 0;
                }
            }
            if (cur >= 6) { runs++; hit = 1; if (cur > maxrun) maxrun = cur; }
            if (hit) lines_hit++;
        }
        printf("填充空格指标: 连续>=6空格段数=%d  最长=%d  受影响行数=%d\n",
               runs, maxrun, lines_hit);
        fflush(stdout);
    }

    /* 5. 完整可见区转储，供人工核对 */
    printf("\n=== 末态可见区 ===\n");
    for (int r = 0; r < s.rows; r++) {
        char b[512];
        int pr = screen_phys_row(&s, r);
        if (pr < 0 || pr >= s.total_lines || !s.lines || !s.lines[pr].cells) continue;
        ScreenLine *ln = &s.lines[pr];
        int n = 0;
        for (int x = 0; x < ln->used && n < 500; x++) {
            WCHAR c = ln->cells[x].Char.UnicodeChar;
            if (c == 0) continue;
            b[n++] = (c < 128) ? (char)c : '?';
        }
        b[n] = 0;
        printf("  r%-3d |%s|\n", r, b);
    }

    CloseHandle(hproc);
    ClosePseudoConsole(hpc);
    CloseHandle(hin);
    CloseHandle(hout);
    screen_free(&s);
    return torn ? 1 : 0;
}
