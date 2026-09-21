/* test_term_input_posix.c —— 输入翻译器的单元测试。
 *
 * 为什么要有这个：POSIX 版整条键盘链路是「终端字节流 -> parse_one -> INPUT_RECORD
 * -> keymap.spec_match -> 动作」。中间任何一环错，表现都是「按键没反应」，
 * 从外面看不出是哪一环。这个测试直接把字节喂给 parse_one 的唯一入口
 * plat_console_read()，把翻译结果打出来，于是：
 *   - 键位没反应到底是「字节没翻译对」还是「keymap 没匹配上」一目了然；
 *   - 改 term_input_posix.c 之后能立刻知道有没有把已有映射改坏。
 *
 * 做法：把测试字节写进一根管道，把读端 dup2 到 fd 0，再调 plat_console_read。
 * 这样跑的就是生产代码本身，不是替身。
 *
 * 编译（见 Makefile 的 unittest-posix-input 目标）：
 *   cc -O1 -Iinclude tests/test_term_input_posix.c src/term_input_posix.c \
 *      -o /tmp/tti && /tmp/tti
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "wincompat.h"

int plat_console_read(INPUT_RECORD *out, int cap, int timeout_ms);
void plat_input_flush(void);

/* platform_posix.c 里的 SIGWINCH 标志；这里不链那个文件，自己给一个定义。 */
volatile sig_atomic_t g_winch = 0;

static int g_fail = 0;

static const char *ctrl_names(DWORD st) {
    static char b[64];
    b[0] = 0;
    if (st & LEFT_CTRL_PRESSED)  strcat(b, "CTRL ");
    if (st & LEFT_ALT_PRESSED)   strcat(b, "ALT ");
    if (st & SHIFT_PRESSED)      strcat(b, "SHIFT ");
    if (st & RIGHT_CTRL_PRESSED) strcat(b, "RCTRL ");
    if (!b[0]) strcpy(b, "- ");
    return b;
}

static void feed(const char *bytes, int len) {
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); _exit(2); }
    ssize_t w = write(fds[1], bytes, (size_t)len);
    if (w != len) { fprintf(stderr, "pipe write short\n"); _exit(2); }
    close(fds[1]);
    dup2(fds[0], STDIN_FILENO);
    close(fds[0]);
}

/* 把一次输入翻译成记录并打印。返回记录数。 */
static int run_n(const char *label, const char *bytes, int len,
                 INPUT_RECORD *out, int cap) {
    plat_input_flush();                 /* 上一个用例的残留不能污染这一个 */
    feed(bytes, len);
    int n = plat_console_read(out, cap, 60);
    printf("%-26s -> %d 条\n", label, n);
    for (int i = 0; i < n; i++) {
        INPUT_RECORD *r = &out[i];
        if (r->EventType == KEY_EVENT) {
            printf("      KEY   vk=0x%02X sc=0x%02X down=%d ctrl=%s unicode=U+%04X\n",
                   (unsigned)r->Event.KeyEvent.wVirtualKeyCode,
                   (unsigned)r->Event.KeyEvent.wVirtualScanCode,
                   (int)r->Event.KeyEvent.bKeyDown,
                   ctrl_names(r->Event.KeyEvent.dwControlKeyState),
                   (unsigned)(r->Event.KeyEvent.uChar.UnicodeChar & 0xFFFF));
        } else if (r->EventType == MOUSE_EVENT) {
            printf("      MOUSE x=%d y=%d btn=0x%04X flags=0x%04X wheel=%d\n",
                   (int)r->Event.MouseEvent.dwMousePosition.X,
                   (int)r->Event.MouseEvent.dwMousePosition.Y,
                   (unsigned)r->Event.MouseEvent.dwButtonState,
                   (unsigned)r->Event.MouseEvent.dwEventFlags,
                   (int)(short)HIWORD(r->Event.MouseEvent.dwButtonState));
        } else if (r->EventType == WINDOW_BUFFER_SIZE_EVENT) {
            printf("      RESIZE %dx%d\n",
                   (int)r->Event.WindowBufferSizeEvent.dwSize.X,
                   (int)r->Event.WindowBufferSizeEvent.dwSize.Y);
        } else {
            printf("      type=%d\n", (int)r->EventType);
        }
    }
    return n;
}

/* 长度用 strlen 算，免得手写数字数错一个字节（\e[<0;10;5M 是 10 字节不是 9）。 */
static int run(const char *label, const char *bytes, INPUT_RECORD *out, int cap) {
    return run_n(label, bytes, (int)strlen(bytes), out, cap);
}

/* 断言：第 i 条是 KEY 且 vk / ctrl / unicode 都符合。 */
static void ck_key(const char *label, INPUT_RECORD *out, int n, int i,
                   int vk, DWORD ctrl, int uc) {
    int ok = (i < n) && out[i].EventType == KEY_EVENT &&
             out[i].Event.KeyEvent.wVirtualKeyCode == (WORD)vk &&
             (out[i].Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED |
                                                         LEFT_ALT_PRESSED |
                                                         SHIFT_PRESSED)) == ctrl &&
             (out[i].Event.KeyEvent.uChar.UnicodeChar & 0xFFFF) == (unsigned)uc;
    if (ok) { printf("[ok]   %s\n", label); return; }
    printf("[FAIL] %s  (期望 vk=0x%02X ctrl=0x%X U+%04X；实际 ",
           label, vk, (unsigned)ctrl, (unsigned)uc);
    if (i < n && out[i].EventType == KEY_EVENT)
        printf("vk=0x%02X ctrl=0x%X U+%04X",
               (unsigned)out[i].Event.KeyEvent.wVirtualKeyCode,
               (unsigned)out[i].Event.KeyEvent.dwControlKeyState,
               (unsigned)(out[i].Event.KeyEvent.uChar.UnicodeChar & 0xFFFF));
    else
        printf("第 %d 条不是 KEY（共 %d 条）", i, n);
    printf(")\n");
    g_fail++;
}

static void ck_mouse(const char *label, INPUT_RECORD *out, int n, int i,
                     int x, int y, DWORD flags) {
    int ok = (i < n) && out[i].EventType == MOUSE_EVENT &&
             out[i].Event.MouseEvent.dwMousePosition.X == (SHORT)x &&
             out[i].Event.MouseEvent.dwMousePosition.Y == (SHORT)y &&
             out[i].Event.MouseEvent.dwEventFlags == (DWORD)flags;
    if (ok) { printf("[ok]   %s\n", label); return; }
    printf("[FAIL] %s (期望 %d,%d flags=0x%X)\n", label, x, y, (unsigned)flags);
    g_fail++;
}

int main(void) {
    INPUT_RECORD out[64];
    int n;

    printf("=== term_input_posix 翻译测试 ===\n\n");

    /* ---- 1. Ctrl+B 前缀键：必须翻译成 'B' + LEFT_CTRL_PRESSED，
     *         否则 keymap.c:325 的 spec_match(&g_prefix,...) 抓不到。 ---- */
    n = run("Ctrl+B (0x02)", "\x02", out, 64);
    ck_key("Ctrl+B -> vk='B' + CTRL", out, n, 0, 'B', LEFT_CTRL_PRESSED, 0x02);

    /* ---- 2. 分屏键 '-'：keymap 里是 CHR('-')，只看 UnicodeChar。 ---- */
    n = run("'-' (0x2d)", "-", out, 64);
    ck_key("'-' -> U+002D 无修饰", out, n, 0, VK_OEM_MINUS, 0, '-');

    n = run("'_' (0x5f, 需 SHIFT)", "_", out, 64);
    ck_key("'_' -> U+005F + SHIFT", out, n, 0, VK_OEM_MINUS, SHIFT_PRESSED, '_');

    /* ---- 3. 一串普通字符 ---- */
    n = run("\"ls -l\\r\"", "ls -l\r", out, 64);
    ck_key("第 1 条 = 'l'", out, n, 0, 'L', 0, 'l');
    ck_key("第 6 条(下标 5) = CR", out, n, 5, VK_RETURN, 0, '\r');

    /* ---- 4. 方向键 ---- */
    n = run("ESC [ A (上)", "\x1b[A", out, 64);
    ck_key("上箭头 -> VK_UP", out, n, 0, VK_UP, 0, 0);

    n = run("ESC [ 1 ; 5 C (Ctrl+右)", "\x1b[1;5C", out, 64);
    ck_key("Ctrl+右 -> VK_RIGHT + CTRL", out, n, 0, VK_RIGHT, LEFT_CTRL_PRESSED, 0);

    /* ---- 5. Backspace / Delete / Home ---- */
    n = run("0x7f", "\x7f", out, 64);
    ck_key("0x7f -> VK_BACK (UnicodeChar 0x08，与 Windows 一致)", out, n, 0, VK_BACK, 0, 0x08);

    n = run("ESC [ 3 ~ (Delete)", "\x1b[3~", out, 64);
    ck_key("Delete -> VK_DELETE", out, n, 0, VK_DELETE, 0, 0);

    n = run("ESC [ H (Home)", "\x1b[H", out, 64);
    ck_key("Home -> VK_HOME", out, n, 0, VK_HOME, 0, 0);

    /* ---- 6. SGR 鼠标：坐标是 1-based，引擎要 0-based ---- */
    n = run("SGR 按下 (10,5)", "\x1b[<0;10;5M", out, 64);
    ck_mouse("SGR 按下 -> (9,4)", out, n, 0, 9, 4, 0);

    n = run("SGR 滚轮上", "\x1b[<64;3;3M", out, 64);
    ck_mouse("滚轮 -> MOUSE_WHEELED", out, n, 0, 2, 2, MOUSE_WHEELED);
    if (n > 0 && out[0].EventType == MOUSE_EVENT) {
        int d = (int)(short)HIWORD(out[0].Event.MouseEvent.dwButtonState);
        if (d == 120) printf("[ok]   滚轮 delta = +120\n");
        else { printf("[FAIL] 滚轮 delta 期望 120，实际 %d\n", d); g_fail++; }
    }

    /* ---- 7. UTF-8 中文 ---- */
    n = run("UTF-8 \"中\"", "\xe4\xb8\xad", out, 64);
    ck_key("中文 -> U+4E2D", out, n, 0, 0, 0, 0x4E2D);

    /* ---- 8. OSC 序列要整段吃掉，不能漏成按键 ---- */
    n = run("OSC 0 ; x BEL", "\x1b]0;title\x07", out, 64);
    if (n == 0) printf("[ok]   OSC 序列被整段丢弃（0 条）\n");
    else { printf("[FAIL] OSC 序列漏出 %d 条记录\n", n); g_fail++; }

    /* ---- 9. 自证：把 '-' 的期望改成 '+'，必须失败 ---- */
    {
        int saved = g_fail;
        n = run("自证：'-' 假装是 '+'", "-", out, 64);
        ck_key("(自证) '-' 期望成 '+'", out, n, 0, VK_OEM_PLUS, 0, '+');
        if (g_fail == saved + 1) {
            printf("[ok]   自证有效（错的期望被抓到了）\n");
            g_fail = saved;              /* 这条 FAIL 是故意造出来的，不计入成绩 */
        } else {
            printf("[FAIL] 自证无效，断言是死的\n");
            g_fail = saved + 1;
        }
    }

    printf("\n%s\n", g_fail ? "INPUT TRANSLATOR: FAILED" : "INPUT TRANSLATOR PASSED");
    return g_fail ? 1 : 0;
}
