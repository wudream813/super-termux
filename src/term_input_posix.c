/* ---------------------------------------------------------------------------
 * term_input_posix.c —— 把宿主终端的字节流解析成 Windows 形状的 INPUT_RECORD。
 *
 * 为什么值得单独一个文件：这是 POSIX 移植里唯一「语义」上不能照搬的部分。引擎的
 * keymap.c / input.c 完全按 Windows 的「虚拟键码 + 修饰键状态 + UnicodeChar」写，
 * 例如上下分屏绑的是 VKEY_SHIFT(VK_OEM_MINUS)、命令面板绑 CHR(':')。所以转义序列
 * 必须被翻译成同样的三元组，keymap 那张表才能一个字不改地继续用。
 *
 * 覆盖的编码：
 *   CSI  cursor      \e[A/B/C/D  \e[1;5C（带修饰）  \eOA/B/C/D（应用模式）
 *   CSI  editing     \e[H \e[F \e[1~..\e[8~  \e[Z（Shift+Tab）
 *   CSI  function    \e[11~..\e[34~  \eOP/Q/R/S
 *   SGR  mouse       \e[<b;x;yM / \e[<b;x;ym（1006）
 *   X10  mouse       \e[M + 3 字节（1000，兜底）
 *   OSC / DCS        整段吃掉（宿主终端的响应，比如 \e]0;标题\a）
 *   控制字符          \x01..\x1A -> Ctrl+字母；\x7f -> VK_BACK；\r \t \b
 *   可打印字符        解码 UTF-8，平面外拆成代理对（引擎有 g_high_surrogate 配套）
 *
 * 裸 ESC 的歧义（单按 ESC vs 转义序列的开头）用「短轮询」解决：缓冲区里只剩一个
 * \x1b 时再 poll 一次，有后续字节就继续当序列解析，没有就发 VK_ESCAPE。
 * ------------------------------------------------------------------------- */
#include "platform.h"
#include "types.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define IBUF_CAP 16384
static unsigned char g_ib[IBUF_CAP];
static int g_ib_len = 0;

/* 鼠标按键状态：Windows 的 dwButtonState 是「当前按住的键集合」，而 SGR 报的是
 * 单次按下/抬起，所以要自己维护。 */
static DWORD g_btn_state = 0;
static DWORD g_last_press_btn = 0;
static ULONGLONG g_last_press_ms = 0;
static int g_last_press_x = -1, g_last_press_y = -1;

extern volatile sig_atomic_t g_winch;   /* 定义在 platform_posix.c */

static void push_key(INPUT_RECORD *r, WORD vk, WCHAR ch, DWORD ctrl) {
    memset(r, 0, sizeof(*r));
    r->EventType = KEY_EVENT;
    r->Event.KeyEvent.bKeyDown = TRUE;      /* handle_key 只处理按下，抬起不发 */
    r->Event.KeyEvent.wRepeatCount = 1;
    r->Event.KeyEvent.wVirtualKeyCode = vk;
    r->Event.KeyEvent.uChar.UnicodeChar = ch;
    r->Event.KeyEvent.dwControlKeyState = ctrl;
}

/* ASCII -> (虚拟键码, 是否 Shift)。必须准确：keymap 里有 VKEY_SHIFT(VK_OEM_MINUS)
 * 这类同时看键码和 Shift 状态的绑定（中文输入法的 `_` 那条 bug #26 就是这么来的）。 */
static void ascii_vk(unsigned char c, WORD *vk, DWORD *ctrl) {
    *ctrl = 0;
    if (c >= 'a' && c <= 'z') { *vk = (WORD)(c - 32); return; }
    if (c >= 'A' && c <= 'Z') { *vk = (WORD)c; *ctrl = SHIFT_PRESSED; return; }
    if (c >= '1' && c <= '9') { *vk = (WORD)c; return; }
    if (c == '0')             { *vk = (WORD)'0'; return; }
    switch (c) {
    case '`':  *vk = VK_OEM_3; break;
    case '~':  *vk = VK_OEM_3;      *ctrl = SHIFT_PRESSED; break;
    case '!':  *vk = (WORD)'1';     *ctrl = SHIFT_PRESSED; break;
    case '@':  *vk = (WORD)'2';     *ctrl = SHIFT_PRESSED; break;
    case '#':  *vk = (WORD)'3';     *ctrl = SHIFT_PRESSED; break;
    case '$':  *vk = (WORD)'4';     *ctrl = SHIFT_PRESSED; break;
    case '%':  *vk = (WORD)'5';     *ctrl = SHIFT_PRESSED; break;
    case '^':  *vk = (WORD)'6';     *ctrl = SHIFT_PRESSED; break;
    case '&':  *vk = (WORD)'7';     *ctrl = SHIFT_PRESSED; break;
    case '*':  *vk = (WORD)'8';     *ctrl = SHIFT_PRESSED; break;
    case '(':  *vk = (WORD)'9';     *ctrl = SHIFT_PRESSED; break;
    case ')':  *vk = (WORD)'0';     *ctrl = SHIFT_PRESSED; break;
    case '-':  *vk = VK_OEM_MINUS;  break;
    case '_':  *vk = VK_OEM_MINUS;  *ctrl = SHIFT_PRESSED; break;
    case '=':  *vk = VK_OEM_PLUS;   break;
    case '+':  *vk = VK_OEM_PLUS;   *ctrl = SHIFT_PRESSED; break;
    case '[':  *vk = VK_OEM_4;      break;
    case '{':  *vk = VK_OEM_4;      *ctrl = SHIFT_PRESSED; break;
    case ']':  *vk = VK_OEM_6;      break;
    case '}':  *vk = VK_OEM_6;      *ctrl = SHIFT_PRESSED; break;
    case '\\': *vk = VK_OEM_5;      break;
    case '|':  *vk = VK_OEM_5;      *ctrl = SHIFT_PRESSED; break;
    case ';':  *vk = VK_OEM_1;      break;
    case ':':  *vk = VK_OEM_1;      *ctrl = SHIFT_PRESSED; break;
    case '\'': *vk = VK_OEM_7;      break;
    case '"':  *vk = VK_OEM_7;      *ctrl = SHIFT_PRESSED; break;
    case ',':  *vk = VK_OEM_COMMA;  break;
    case '<':  *vk = VK_OEM_COMMA;  *ctrl = SHIFT_PRESSED; break;
    case '.':  *vk = VK_OEM_PERIOD; break;
    case '>':  *vk = VK_OEM_PERIOD; *ctrl = SHIFT_PRESSED; break;
    case '/':  *vk = VK_OEM_2;      break;
    case '?':  *vk = VK_OEM_2;      *ctrl = SHIFT_PRESSED; break;
    case ' ':  *vk = VK_SPACE;      break;
    default:   *vk = 0;             break;
    }
}

/* CSI 的修饰参数：\e[1;5C 里的 5 -> Ctrl。约定是 (值-1) 的位掩码。 */
static DWORD csi_mod(int p) {
    int m = p - 1;
    DWORD d = 0;
    if (m & 1) d |= SHIFT_PRESSED;
    if (m & 2) d |= LEFT_ALT_PRESSED;
    if (m & 4) d |= LEFT_CTRL_PRESSED;
    if (m & 8) d |= LEFT_ALT_PRESSED;      /* Meta 也当 Alt */
    return d;
}

/* 一条鼠标事件。b = 原始按钮码，x/y = 1 基，release = SGR 的 'm'。 */
static void push_mouse(INPUT_RECORD *r, int b, int x, int y, int release) {
    memset(r, 0, sizeof(*r));
    r->EventType = MOUSE_EVENT;
    MOUSE_EVENT_RECORD *m = &r->Event.MouseEvent;
    m->dwMousePosition.X = (SHORT)(x - 1);   /* 引擎用 0 基；Y=0 是标签栏那一行 */
    m->dwMousePosition.Y = (SHORT)(y - 1);
    DWORD ctrl = 0;
    if (b & 4)  ctrl |= SHIFT_PRESSED;
    if (b & 8)  ctrl |= LEFT_ALT_PRESSED;
    if (b & 16) ctrl |= LEFT_CTRL_PRESSED;
    m->dwControlKeyState = ctrl;

    if (b & 64) {                            /* 滚轮 */
        short delta = (b & 1) ? -120 : 120;  /* 上正下负，与 Windows 一致 */
        m->dwEventFlags = MOUSE_WHEELED;
        m->dwButtonState = ((DWORD)((unsigned short)delta) << 16) | g_btn_state;
        return;
    }
    int btn = b & 3;
    DWORD wb = (btn == 0) ? FROM_LEFT_1ST_BUTTON_PRESSED
             : (btn == 1) ? FROM_LEFT_2ND_BUTTON_PRESSED
             : (btn == 2) ? RIGHTMOST_BUTTON_PRESSED : 0;
    if (b & 32) {                            /* 按住移动 / 无键移动 */
        m->dwEventFlags = MOUSE_MOVED;
        m->dwButtonState = g_btn_state;
        return;
    }
    if (release || btn == 3) {               /* 抬起（X10 用 btn==3 表示抬起） */
        g_btn_state &= ~wb;
        m->dwEventFlags = 0;
        m->dwButtonState = g_btn_state;
        return;
    }
    /* 按下：Windows 会把 500ms 内同位置的同键第二次按下报成 DOUBLE_CLICK，
     * 引擎靠它做双击选词，这里补齐同样的语义。 */
    ULONGLONG now = GetTickCount64();
    int dbl = (wb != 0 && wb == g_last_press_btn &&
               x == g_last_press_x && y == g_last_press_y &&
               g_last_press_ms != 0 && now - g_last_press_ms < 500);
    g_btn_state |= wb;
    m->dwEventFlags = dbl ? DOUBLE_CLICK : 0;
    m->dwButtonState = g_btn_state;
    g_last_press_btn = wb;
    g_last_press_x = x; g_last_press_y = y;
    g_last_press_ms = dbl ? 0 : now;         /* 双击之后重新计时，避免连判 */
}

/* 跳过 OSC / DCS：吃到 BEL(0x07) 或 ST(ESC \)。返回消费字节数，0 = 数据不足。 */
static int skip_until_st(const unsigned char *p, int n) {
    for (int i = 2; i < n; i++) {
        if (p[i] == 0x07) return i + 1;
        if (p[i] == 0x1b && i + 1 < n && p[i + 1] == '\\') return i + 2;
        if (p[i] == 0x9c) return i + 1;      /* 8 位 ST */
    }
    if (n > 4096) return n;                  /* 兜底：超长就整段丢掉 */
    return 0;
}

static int parse_csi(const unsigned char *p, int n, INPUT_RECORD *tmp, int *cnt) {
    int i = 2;
    int priv = 0;
    if (i < n && (p[i] == '<' || p[i] == '=' || p[i] == '>' || p[i] == '?')) priv = p[i++];
    int par[8]; int npar = 0; int cur = -1;
    for (;;) {
        if (i >= n) return 0;                          /* 序列没到终止字节，要更多输入 */
        if (i > 64) return 2;                          /* 垃圾：吃掉 "\e[" 重新同步 */
        unsigned char d = p[i];
        if (d >= '0' && d <= '9') { cur = (cur < 0 ? 0 : cur) * 10 + (d - '0'); i++; continue; }
        if (d == ';') { if (npar < 8) par[npar++] = cur; cur = -1; i++; continue; }
        if (d >= 0x20 && d <= 0x2f) { i++; continue; }  /* 中间字节，忽略 */
        break;
    }
    if (cur >= 0 || npar > 0) { if (npar < 8) par[npar++] = cur; }
    unsigned char fin = p[i];
    int used = i + 1;
    int p0 = (npar >= 1 && par[0] >= 0) ? par[0] : 0;
    DWORD mod = (npar >= 2 && par[1] > 0) ? csi_mod(par[1]) : 0;

    /* ---- SGR 鼠标（\e[<b;x;yM / m）---- */
    if (priv == '<' && (fin == 'M' || fin == 'm')) {
        if (npar < 3) return used;
        *cnt = 1;
        push_mouse(tmp, par[0], par[1], par[2], fin == 'm');
        return used;
    }
    /* ---- X10 鼠标（\e[M + 3 个 +32 的字节）---- */
    if (priv == 0 && fin == 'M' && npar == 0) {
        if (i + 3 >= n) return 0;
        *cnt = 1;
        push_mouse(tmp, p[i + 1] - 32, p[i + 2] - 32, p[i + 3] - 32, 0);
        return i + 4;
    }
    if (priv == '?' || priv == '=' || priv == '>') return used;   /* 模式设置等，忽略 */

    *cnt = 1;
    switch (fin) {
    case 'A': push_key(tmp, VK_UP,    0, mod); return used;
    case 'B': push_key(tmp, VK_DOWN,  0, mod); return used;
    case 'C': push_key(tmp, VK_RIGHT, 0, mod); return used;
    case 'D': push_key(tmp, VK_LEFT,  0, mod); return used;
    case 'H': push_key(tmp, VK_HOME,  0, mod); return used;
    case 'F': push_key(tmp, VK_END,   0, mod); return used;
    case 'Z': push_key(tmp, VK_TAB,  L'\t', SHIFT_PRESSED); return used;
    case 'E': push_key(tmp, VK_NUMPAD5, 0, mod); return used;
    case '~': {
        WORD vk = 0; WCHAR ch = 0;
        switch (p0) {
        case 1: case 7:  vk = VK_HOME;   break;
        case 2:          vk = VK_INSERT; break;
        case 3:          vk = VK_DELETE; break;
        case 4: case 8:  vk = VK_END;    break;
        case 5:          vk = VK_PRIOR;  break;
        case 6:          vk = VK_NEXT;   break;
        case 11: vk = VK_F1;  break;
        case 12: vk = VK_F2;  break;
        case 13: vk = VK_F3;  break;
        case 14: vk = VK_F4;  break;
        case 15: vk = VK_F5;  break;
        case 17: vk = VK_F6;  break;
        case 18: vk = VK_F7;  break;
        case 19: vk = VK_F8;  break;
        case 20: vk = VK_F9;  break;
        case 21: vk = VK_F10; break;
        case 23: vk = VK_F11; break;
        case 24: vk = VK_F12; break;
        case 25: vk = VK_F13; break;
        case 26: vk = VK_F14; break;
        case 28: vk = VK_F15; break;
        case 29: vk = VK_F16; break;
        case 31: vk = VK_F17; break;
        case 32: vk = VK_F18; break;
        case 33: vk = VK_F19; break;
        case 34: vk = VK_F20; break;
        case 200: case 201: *cnt = 0; return used;   /* 括号粘贴标记，忽略 */
        default: *cnt = 0; return used;
        }
        push_key(tmp, vk, ch, mod);
        return used;
    }
    default:
        *cnt = 0;    /* 不认识的序列：吃掉，别当成按键 */
        return used;
    }
}

static int parse_ss3(const unsigned char *p, int n, INPUT_RECORD *tmp, int *cnt) {
    if (n < 3) return 0;
    *cnt = 1;
    switch (p[2]) {
    case 'A': push_key(tmp, VK_UP,    0, 0); return 3;
    case 'B': push_key(tmp, VK_DOWN,  0, 0); return 3;
    case 'C': push_key(tmp, VK_RIGHT, 0, 0); return 3;
    case 'D': push_key(tmp, VK_LEFT,  0, 0); return 3;
    case 'H': push_key(tmp, VK_HOME,  0, 0); return 3;
    case 'F': push_key(tmp, VK_END,   0, 0); return 3;
    case 'P': push_key(tmp, VK_F1,    0, 0); return 3;
    case 'Q': push_key(tmp, VK_F2,    0, 0); return 3;
    case 'R': push_key(tmp, VK_F3,    0, 0); return 3;
    case 'S': push_key(tmp, VK_F4,    0, 0); return 3;
    default:  *cnt = 0; return 3;
    }
}

/* 一个 UTF-8 字符 -> 1 或 2 条按键记录（平面外拆代理对）。返回消费字节数。 */
static int parse_utf8_key(const unsigned char *p, int n, INPUT_RECORD *tmp, int *cnt) {
    unsigned char c = p[0];
    unsigned int cp4; int extra;
    if (c < 0x80)              { cp4 = c;       extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp4 = c & 0x1Fu; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp4 = c & 0x0Fu; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp4 = c & 0x07u; extra = 3; }
    else { *cnt = 1; push_key(tmp, 0, 0xFFFD, 0); return 1; }
    if (n < extra + 1) return 0;                     /* 多字节字符还没收全 */
    for (int k = 1; k <= extra; k++) {
        if ((p[k] & 0xC0) != 0x80) { *cnt = 1; push_key(tmp, 0, 0xFFFD, 0); return 1; }
        cp4 = (cp4 << 6) | (unsigned int)(p[k] & 0x3Fu);
    }
    if (cp4 > 0x10FFFF) cp4 = 0xFFFD;
    if (cp4 > 0xFFFF) {
        unsigned int v = cp4 - 0x10000u;
        *cnt = 2;
        push_key(&tmp[0], 0, (WCHAR)(0xD800u + (v >> 10)), 0);
        push_key(&tmp[1], 0, (WCHAR)(0xDC00u + (v & 0x3FFu)), 0);
    } else {
        WORD vk; DWORD ctl;
        ascii_vk((unsigned char)cp4, &vk, &ctl);       /* 非 ASCII 时 vk=0，引擎走 CHR 匹配 */
        *cnt = 1;
        push_key(tmp, (cp4 < 0x80) ? vk : 0, (WCHAR)cp4, (cp4 < 0x80) ? ctl : 0);
    }
    return extra + 1;
}

/* 解析一条。返回消费字节数；0 = 数据不足；-1 = 缓冲区里只有一个裸 ESC。 */
static int parse_one(const unsigned char *p, int n, INPUT_RECORD *tmp, int *cnt) {
    *cnt = 0;
    if (n <= 0) return 0;
    unsigned char c = p[0];

    if (c == 0x1b) {
        if (n == 1) return -1;                        /* 可能是裸 ESC，交给调用方 */
        if (p[1] == '[') return parse_csi(p, n, tmp, cnt);
        if (p[1] == 'O') return parse_ss3(p, n, tmp, cnt);
        if (p[1] == ']' || p[1] == 'P' || p[1] == 'X' || p[1] == '^' || p[1] == '_')
            return skip_until_st(p, n);
        if (p[1] == '\\') return 2;                   /* 单独的 ST，忽略 */
        if (p[1] == 0x1b) { *cnt = 1; push_key(tmp, VK_ESCAPE, 0x1b, 0); return 1; }
        /* Alt + 单字符 */
        if (p[1] == 0x7f) { *cnt = 1; push_key(tmp, VK_BACK, 0, LEFT_ALT_PRESSED); return 2; }
        INPUT_RECORD t2[2]; int c2 = 0;
        int used = parse_utf8_key(p + 1, n - 1, t2, &c2);
        if (used <= 0) return 0;
        for (int k = 0; k < c2 && k < 2; k++) {
            tmp[k] = t2[k];
            tmp[k].Event.KeyEvent.dwControlKeyState |= LEFT_ALT_PRESSED;
        }
        *cnt = c2;
        return used + 1;
    }

    if (c == '\r' || c == '\n') { *cnt = 1; push_key(tmp, VK_RETURN, L'\r', 0); return 1; }
    if (c == '\t')              { *cnt = 1; push_key(tmp, VK_TAB,    L'\t', 0); return 1; }
    if (c == 0x7f || c == 0x08) { *cnt = 1; push_key(tmp, VK_BACK, 0x08,   0); return 1; }
    if (c == 0x00)              { *cnt = 1; push_key(tmp, VK_SPACE, 0, LEFT_CTRL_PRESSED); return 1; }
    if (c >= 0x01 && c <= 0x1a) {                     /* Ctrl + 字母 */
        *cnt = 1; push_key(tmp, (WORD)('A' + c - 1), (WCHAR)c, LEFT_CTRL_PRESSED);
        return 1;
    }
    if (c >= 0x1c && c <= 0x1f) {                     /* Ctrl + \ ] ^ _ */
        WORD vk = (c == 0x1c) ? VK_OEM_5 : (c == 0x1d) ? VK_OEM_6
              : (c == 0x1e) ? (WORD)'6' : VK_OEM_MINUS;
        *cnt = 1; push_key(tmp, vk, (WCHAR)c, LEFT_CTRL_PRESSED);
        return 1;
    }
    return parse_utf8_key(p, n, tmp, cnt);
}

/* 丢掉还没解析完的残留字节。切模式 / 恢复控制台之后要调，
 * 否则半截转义序列会粘到下一段输入上。测试也靠它在用例之间复位。 */
void plat_input_flush(void) { g_ib_len = 0; }

int plat_console_read(INPUT_RECORD *out, int cap, int timeout_ms) {
    if (cap <= 0) return 0;
    int n = 0;

    /* 已有缓冲就先解析，没有就等输入。 */
    if (g_ib_len == 0) {
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
        if (pr < 0 && errno != EINTR) return 0;
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            ssize_t r = read(STDIN_FILENO, g_ib, IBUF_CAP);
            if (r > 0) g_ib_len = (int)r;
        }
    }

    int pos = 0;
    while (pos < g_ib_len && n < cap) {
        INPUT_RECORD tmp[2]; int cnt = 0;
        int used = parse_one(g_ib + pos, g_ib_len - pos, tmp, &cnt);
        if (used == -1) {
            /* 只剩一个 ESC：短轮询确认它是不是某个序列的开头。 */
            struct pollfd pfd;
            pfd.fd = STDIN_FILENO; pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, 15);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                if (g_ib_len < IBUF_CAP) {
                    ssize_t r = read(STDIN_FILENO, g_ib + g_ib_len, (size_t)(IBUF_CAP - g_ib_len));
                    if (r > 0) { g_ib_len += (int)r; continue; }   /* 重新解析 */
                }
            }
            push_key(&out[n++], VK_ESCAPE, 0x1b, 0);
            pos += 1;
            continue;
        }
        if (used == 0) break;                        /* 数据不足，留到下一轮 */
        for (int k = 0; k < cnt && n < cap; k++) out[n++] = tmp[k];
        pos += used;
    }

    /* 消费掉的挪走。 */
    if (pos > 0) {
        if (pos >= g_ib_len) g_ib_len = 0;
        else { memmove(g_ib, g_ib + pos, (size_t)(g_ib_len - pos)); g_ib_len -= pos; }
    }

    /* SIGWINCH：补一条窗口尺寸变化事件，让主循环走 handle_resize()。 */
    if (n < cap && g_winch) {
        g_winch = 0;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].EventType = WINDOW_BUFFER_SIZE_EVENT;
        n++;
    }
    return n;
}
