#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_wide_wrap.py

v1.8.31 回归：宽字符（中文/全角）在「只剩最后一列」放不下而强制换行时，
旧行最后一列必须被清成普通空格，不能残留脏内容（旧字符、宽字次格 0 或主格）。

历史 bug：vt.c screen_put_cp 在 wide && cursor_x >= cols-1 时直接换行，
旧行末列（cursor_x == cols-1）没写任何东西。ConPTY 重绘时那一格会保留上一帧
脏内容——若恰好是宽字次格（ch==0 且左邻是宽字主格），snap_left_to_char 会把它
误判成宽字符次格而把选区左沿左退一列；块选经过这条「因汉字换行」的行之后，
后续窄字符行的高亮/复制整体错位一列。

本脚本链接【真实 src/screen.c + src/vt.c + src/utf8.c】，喂 UTF-8：先填满到
最后一列（cursor_x == cols-1），再喂一个汉字，断言旧行末列被清成空格。
变异：删掉 vt.c 换行分支里清末列的那行 screen_write_cell(... L' ' ...)，用例立即失败。
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
INC = os.path.join(ROOT, "include")
STUB = os.path.join(ROOT, "tests", "stub")

# 用 tests/stub 的 windows.h 替身；补齐 screen.c/vt.c 需要而 stub 未提供的类型。
STUB_EXTRA = r"""
typedef wchar_t WCHAR;
typedef struct { union { WCHAR UnicodeChar; char AsciiChar; } Char; unsigned short Attributes; } CHAR_INFO;
typedef struct { long dummy; } CRITICAL_SECTION;
typedef struct { unsigned long dwEventMask; } MOUSE_EVENT_RECORD;
#ifndef COMMON_LVB_UNDERSCORE
#define COMMON_LVB_UNDERSCORE 0x8000
#endif
#ifndef FOREGROUND_RED
#define FOREGROUND_RED 4
#define FOREGROUND_GREEN 2
#define FOREGROUND_BLUE 1
#define FOREGROUND_INTENSITY 8
#endif
"""

# types.h extern 的全局符号（screen.c/vt.c 引用）。
GLOBALS = r"""
#include "common.h"
#include "types.h"
int g_scrollback_lines = 10000;
MuxState g_mux;
int g_search_active;
int g_search_match_count;
int g_search_match_cur;
SearchMatch g_search_matches[MAX_SEARCH_MATCHES];
int g_search_case_sensitive;
"""

HARNESS = r"""
#include "screen.h"
#include "vt.h"
#include <stdio.h>

static int failures = 0;
static void ck(const char *n, int cond) {
    if (!cond) { printf("[FAIL] %s\n", n); failures++; }
    else       { printf("[ok]   %s\n", n); }
}

/* v1.8.52：统计历史区（rel<0）空白行 / 内容行数量。 */
static int hist_blank(ScreenBuffer *s) {
    int n = 0;
    for (int rel = -s->hist_lines; rel < 0; rel++) {
        int pr = screen_phys_row(s, rel);
        int any = 0;
        ScreenLine *l = &s->lines[pr];
        for (int x = 0; x < l->len; x++)
            if (l->cells[x].Char.UnicodeChar != L' ') { any = 1; break; }
        if (!any) n++;
    }
    return n;
}
static int hist_content(ScreenBuffer *s) {
    int n = 0;
    for (int rel = -s->hist_lines; rel < 0; rel++) {
        int pr = screen_phys_row(s, rel);
        int any = 0;
        ScreenLine *l = &s->lines[pr];
        for (int x = 0; x < l->len; x++)
            if (l->cells[x].Char.UnicodeChar != L' ') { any = 1; break; }
        if (any) n++;
    }
    return n;
}

/* 某行是否全空白（未分配的槽渲染为空白，也算空白）。 */
static int row_is_blank(ScreenBuffer *s, int rel) {
    int pr = screen_phys_row(s, rel);
    if (pr < 0 || pr >= s->total_lines || !s->lines || !s->lines[pr].cells) return 1;
    ScreenLine *l = &s->lines[pr];
    for (int x = 0; x < l->len; x++) {
        WCHAR c = l->cells[x].Char.UnicodeChar;
        if (c != L' ' && c != 0) return 0;
    }
    return 1;
}

/* 40x6 屏幕先输出 8 行把光标顶到底行，再喂 tail（命令结束的字节序列 + 提示符），
 * 返回「提示符上一行」是否仍是空行。 */
static int blank_kept(const char *tail) {
    ScreenBuffer s;
    screen_init(&s, 40, 6);
    for (int i = 1; i <= 8; i++) {
        char b[64]; snprintf(b, sizeof b, "LINE-%d\r\n", i);
        screen_process_output(&s, b, (int)strlen(b));
    }
    screen_process_output(&s, tail, (int)strlen(tail));
    int ok = row_is_blank(&s, s.cursor_y - 1);
    screen_free(&s);
    return ok;
}

int main(void) {
    /* 宽 12 列。先写满 12 个窄字符（列0-11 都有真实内容，末列也是真实字符，
       模拟 ConPTY 重绘前的脏缓冲），此时 cursor 触发 wraparound_pending；
       再喂一个汉字：窄字符换行到第 2 行？不——更准的做法是直接把末列弄脏再让
       宽字在 cols-1 强制换行。做法：写 11 个窄字符到列0-10，手动把列11 写成
       脏宽字次格 0（像 ConPTY 半写残留），再喂汉字强制换行。 */
    ScreenBuffer s; screen_init(&s, 12, 6);
    screen_process_output(&s, "0123456789a", 11);   /* 列0-10，cursor=11=cols-1 */
    screen_write_cell(&s, 0, 11, 0, 7);              /* 末列脏残格：宽字次格 0 */
    screen_process_output(&s, "\xe6\xb1\x89", 3);   /* 汉 U+6C49 强制换行 */

    CHAR_INFO *last_prev = screen_cell(&s, 0, 11);
    CHAR_INFO *new0 = screen_cell(&s, 1, 0);
    CHAR_INFO *new1 = screen_cell(&s, 1, 1);
    ck("宽字换行：旧行末列(列11)被清成空格", last_prev && last_prev->Char.UnicodeChar == L' ');
    ck("宽字换行：新行列0 是汉字主格", new0 && new0->Char.UnicodeChar == 0x6C49);
    ck("宽字换行：新行列1 是次格占位0", new1 && new1->Char.UnicodeChar == 0);
    /* 旧行末列绝不能是宽字次格（ch==0），否则 snap 会把它误当宽字次格左退一列。 */
    ck("宽字换行：旧行末列不是宽字次格(0)", last_prev && last_prev->Char.UnicodeChar != 0);

    /* 同样验证 cursor 停在 cols-2（宽字要写 cols-2/cols-1 正好放下）时不应误清：
       写 10 个窄字符（cursor 到 10 == cols-2），汉字占列10/11 正好，不换行。 */
    ScreenBuffer s2; screen_init(&s2, 12, 4);
    screen_process_output(&s2, "0123456789", 10);
    screen_process_output(&s2, "\xe6\xb1\x89", 3);
    CHAR_INFO *f10 = screen_cell(&s2, 0, 10);
    CHAR_INFO *f11 = screen_cell(&s2, 0, 11);
    ck("正好放下：汉字主格在列10", f10 && f10->Char.UnicodeChar == 0x6C49);
    ck("正好放下：汉字次格在列11(0占位)", f11 && f11->Char.UnicodeChar == 0);

    /* ---- v1.8.52 追加：ConPTY 裸 LF vs CRLF 空行语义（真实回放定位的幻影空行）----
     * ConPTY 在输出流里夹带成串【裸 LF】（其 9001 行内部缓冲的滚动/留位标记），
     * 本地若按普通 LF 一律滚动，cmd 长输出的滚动历史会逐行多出幻影空白行。
     * 规则：CRLF 里的 LF 是真实行尾（必滚）；裸 LF 若底行仍是空白则吸收（只留位）。
     * 以下用例：裸 LF 标记不产生空行；真实空行（CRLF CRLF）必须保留。 */
    {
        ScreenBuffer s; screen_init(&s, 40, 6);
        for (int i = 1; i <= 20; i++) {
            char b[64]; snprintf(b, sizeof b, "Line %d\r\n", i);
            screen_process_output(&s, b, (int)strlen(b));
        }
        ck("纯 CRLF 长输出：历史无幻影空行", hist_blank(&s) == 0);
        ck("纯 CRLF 长输出：历史全部是内容行", hist_content(&s) == s.hist_lines && s.hist_lines == 15);
        screen_free(&s);
    }
    {
        ScreenBuffer s; screen_init(&s, 40, 6);
        screen_process_output(&s, "Alpha\r\n\r\nBeta\r\n\r\nGamma\r\n", (int)strlen("Alpha\r\n\r\nBeta\r\n\r\nGamma\r\n"));
        for (int i = 0; i < 14; i++) screen_process_output(&s, "fill\r\n", 6);
        ck("真实空行(CRLF CRLF)：空白行保留 >=2", hist_blank(&s) >= 2);
        ck("真实空行(CRLF CRLF)：内容行不丢", hist_content(&s) >= 8);
        screen_free(&s);
    }
    /* ---- 「命令结束后少一个空行」回归（真机报的） --------------------------
     * cmd/ConPTY 在命令结束时发出的空行有 4 种字节形态，其中两种【没有 CR】。
     * 曾经为了消除 ConPTY 留位标记造成的幻影空行，用过「光标在底行且底行空白就
     * 吸收裸 LF」的规则；它会把下面 A / B 两种真实空行一起吃掉，真机症状就是
     * 「命令结束后少了一个空行」。幻影空行现在由 repaint_active 负责（见下一组），
     * 不再靠猜裸 LF —— 加回吸收这 4 条立刻红。 */
    ck("A 命令结束空行: ESC]0;标题 BEL + 裸LF + 提示符 -> 保留",
       blank_kept("\x1b]0;C:\\work\x07\nC:\\work>"));
    ck("B 命令结束空行: 裸LF + 提示符 -> 保留",
       blank_kept("\nC:\\work>"));
    ck("C 命令结束空行: CR LF CR LF + 提示符 -> 保留",
       blank_kept("\r\n\r\nC:\\work>"));
    ck("D 命令结束空行: CR + ESC]0;标题 BEL + LF + 提示符 -> 保留",
       blank_kept("\r\x1b]0;C:\\work\x07\nC:\\work>"));

    /* ---- 幻影空行的正主：整屏重绘（repaint_active）不得把尾部空行塞进历史 ----
     * ConPTY 重绘 = ESC[?25l + ESC[H + 逐行「文本 ESC[K CRLF」。重绘按行遍历
     * viewport，底边的 CRLF 必须走 screen_scroll_viewport_up（只移可见行），
     * 否则每拖一次分隔线就往 scrollback 里塞一批空行。 */
    {
        ScreenBuffer s; screen_init(&s, 40, 6);
        for (int i = 1; i <= 12; i++) {
            char b[64]; snprintf(b, sizeof b, "row%d\r\n", i);
            screen_process_output(&s, b, (int)strlen(b));
        }
        int h0 = s.hist_lines;
        char rp[512]; int n = 0;
        n += sprintf(rp + n, "\x1b[?25l\x1b[H");
        for (int i = 1; i <= 8; i++) n += sprintf(rp + n, "row%d\x1b[K\r\n", i);
        screen_process_output(&s, rp, n);
        ck("整屏重绘(repaint_active)：底边 CRLF 不进滚动历史", s.hist_lines == h0);
        screen_free(&s);
    }

    /* ---- 满行后的「延迟换行」何时作废（v2.3.7，conhost 实测） ----------------
     * 写满 120 列后光标处于延迟换行态。conhost 的【擦除类】控制序列（EL/ED/ECH）
     * 会清掉这个状态：下一条可见字符写回末列（覆盖），而不是折到下一行。
     * SGR / 私有模式 / BEL 不清 —— 下一条字符照常折行。
     * 这一列之差会让 120 列满行的流整屏内容错开一行（真机：mdt 滚动的状态行错位），
     * 所以四条都钉住（前三条 conhost 实测 B 在 (0,119)，第四条在 (1,0)）。 */
    {
        const char *ctl[] = {"\x1b[K", "\x1b[16X", "\x1b[J"};
        const char *name[] = {"EL", "ECH", "ED"};
        for (int i = 0; i < 3; i++) {
            ScreenBuffer s; screen_init(&s, 120, 6);
            char buf[256];
            int n = 0;
            memset(buf, 'a', 120); n = 120;
            memcpy(buf + n, ctl[i], strlen(ctl[i])); n += (int)strlen(ctl[i]);
            buf[n++] = 'B';
            screen_process_output(&s, buf, n);
            CHAR_INFO *c = screen_cell(&s, 0, 119);
            char det[160];
            snprintf(det, sizeof det, "满行 + %s 清延迟换行：B 覆盖末列（B 在 (%d,%d)，%s）",
                     name[i], s.cursor_y, s.cursor_x,
                     c && c->Char.UnicodeChar == 'B' ? "末列是 B" : "末列不是 B");
            ck(det, c && c->Char.UnicodeChar == 'B' && s.cursor_y == 0);
            screen_free(&s);
        }
        {
            ScreenBuffer s; screen_init(&s, 120, 6);
            char buf[256];
            int n = 0;
            memset(buf, 'a', 120); n = 120;
            memcpy(buf + n, "\x1b[38;2;1;2;3m", 13); n += 13;
            buf[n++] = 'B';
            screen_process_output(&s, buf, n);
            CHAR_INFO *c = screen_cell(&s, 1, 0);
            char det[160];
            snprintf(det, sizeof det, "满行 + SGR 不清延迟换行：B 折到下一行（B 在 (%d,%d)）",
                     s.cursor_y, s.cursor_x);
            ck(det, c && c->Char.UnicodeChar == 'B');
            screen_free(&s);
        }
    }


    /* ---- 右缘「填充空格」不能吃掉真实内容里的空格（v2.3.8） ------------------
     * 老规则：宽字符占满右缘（左邻是宽字次格 ch==0、光标 >= cols-2）时紧跟在后面
     * 的空格一律丢弃。它修好了 ConPTY 的排版填充空格，但真实内容也会落在这个位置：
     * mdt 把「，」摆在 116-117 列、真空格在 118 列、再往后是 SGR + 边框字符。
     * 丢掉那一个空格 ⇒ 整行短一列 ⇒ 右缘边框落到 118 列、下一行开头挤到上一行末尾，
     * 逐行累积成用户看到的「右侧滚动条/状态行错位」（2026-11-22 vttrace：22 处）。
     * 新规则：先当普通空格写，下一个事件再定性 —— 可打印字符（可夹 SGR）= 内容，
     * 保留；换行/光标移动/擦除 = 排版填充，撤回。
     * 判据必须两头都钉：A 保住内容空格，B/C/D 保住「填充空格被丢弃」。 */
    {
        /* A：mdt 形态 —— 空格后面跟 SGR 再跟可打印字符，必须保留。 */
        ScreenBuffer s; screen_init(&s, 120, 6);
        char buf[512]; int n = 0;
        memset(buf, 'a', 116); n = 116;                     /* 0..115 */
        n += (int)snprintf(buf + n, sizeof buf - n, "\xe4\xb8\xad");  /* 中 116-117 */
        buf[n++] = ' ';                                     /* 真空格 118 */
        n += (int)snprintf(buf + n, sizeof buf - n, "\x1b[38;2;1;2;3m");
        buf[n++] = 'B';                                     /* 119 */
        screen_process_output(&s, buf, n);
        CHAR_INFO *sp = screen_cell(&s, 0, 118);
        CHAR_INFO *b119 = screen_cell(&s, 0, 119);
        int used = s.lines[screen_phys_row(&s, 0)].used;
        char det[200];
        snprintf(det, sizeof det,
                 "A 右缘真空格 + SGR + 字符：空格保留、不挤短整行（used=%d 空格=%d B在(%d,%d)）",
                 used, sp ? (int)sp->Char.UnicodeChar : -1, s.cursor_y, s.cursor_x);
        ck(det, sp && sp->Char.UnicodeChar == L' ' &&
                b119 && b119->Char.UnicodeChar == 'B' && s.cursor_y == 0 && used == 120);
        screen_free(&s);
    }
    {
        /* B：ConPTY 形态 —— 空格后面直接换行，必须撤回（used 不涨）。
         * 注意 LF 本身不回到行首（VT 语义），所以 Z 落在第 118 列；
         * 判据只看「填充空格有没有把 used 撑到 119」。 */
        ScreenBuffer s; screen_init(&s, 120, 6);
        char buf[512]; int n = 0;
        memset(buf, 'a', 116); n = 116;
        n += (int)snprintf(buf + n, sizeof buf - n, "\xe4\xb8\xad");
        buf[n++] = ' ';
        buf[n++] = '\n';
        buf[n++] = 'Z';
        screen_process_output(&s, buf, n);
        int used0 = s.lines[screen_phys_row(&s, 0)].used;
        CHAR_INFO *z = screen_cell(&s, 1, 118);
        char det[200];
        snprintf(det, sizeof det,
                 "B 右缘填充空格 + LF：撤回填充空格、整行不多占地（used=%d Z在(%d,%d)）",
                 used0, s.cursor_y, s.cursor_x);
        ck(det, used0 == 118 && z && z->Char.UnicodeChar == 'Z' && s.cursor_y == 1);
        screen_free(&s);
    }
    {
        /* C：空格后面跟 EL —— 同属排版填充，撤回后 used 只剩宽字符那一格。 */
        ScreenBuffer s; screen_init(&s, 120, 6);
        char buf[512]; int n = 0;
        memset(buf, 'a', 116); n = 116;
        n += (int)snprintf(buf + n, sizeof buf - n, "\xe4\xb8\xad");
        buf[n++] = ' ';
        memcpy(buf + n, "\x1b[K", 3); n += 3;
        buf[n++] = 'B';
        screen_process_output(&s, buf, n);
        int used0 = s.lines[screen_phys_row(&s, 0)].used;
        CHAR_INFO *b118 = screen_cell(&s, 0, 118);
        char det[200];
        snprintf(det, sizeof det,
                 "C 右缘填充空格 + EL：撤回（used=%d B在(%d,%d)）",
                 used0, s.cursor_y, s.cursor_x);
        /* EL 会清掉 118..119，然后 B 写在 118（撤回把光标放回 118）。 */
        ck(det, used0 == 119 && b118 && b118->Char.UnicodeChar == 'B' && s.cursor_y == 0);
        screen_free(&s);
    }
    {
        /* D：填充空格落在末列（cols-1）时也要撤回，且不留延迟换行。
         * 后面接可打印字符时它是内容（见 A），所以这里必须跟换行。 */
        ScreenBuffer s; screen_init(&s, 120, 6);
        char buf[512]; int n = 0;
        memset(buf, 'a', 117); n = 117;                     /* 0..116 */
        n += (int)snprintf(buf + n, sizeof buf - n, "\xe4\xb8\xad");  /* 117-118 */
        buf[n++] = ' ';                                     /* 119 */
        buf[n++] = '\n';
        screen_process_output(&s, buf, n);
        int used0 = s.lines[screen_phys_row(&s, 0)].used;
        char det[200];
        snprintf(det, sizeof det,
                 "D 末列填充空格 + LF：撤回、不留延迟换行、不折行（used=%d y=%d pending=%d）",
                 used0, s.cursor_y, s.wraparound_pending);
        ck(det, used0 == 119 && s.cursor_y == 1 && !s.wraparound_pending);
        screen_free(&s);
    }
    {
        /* E：远离右缘的真实空格不受影响（「洛谷 题解」这类）。 */
        ScreenBuffer s; screen_init(&s, 120, 6);
        char buf[256]; int n = 0;
        memset(buf, 'a', 40); n = 40;
        buf[n++] = ' ';
        buf[n++] = 'B';
        screen_process_output(&s, buf, n);
        CHAR_INFO *sp = screen_cell(&s, 0, 40);
        char det[160];
        snprintf(det, sizeof det, "E 行中真实空格照旧保留（空格=%d）",
                 sp ? (int)sp->Char.UnicodeChar : -1);
        ck(det, sp && sp->Char.UnicodeChar == L' ');
        screen_free(&s);
    }

    printf("\nWIDE-WRAP CHECKS PASSED\n");
    return 0;
}
"""


def main() -> int:
    print("=== 宽字符强制换行清旧行末格验证 (verify_wide_wrap.py) ===")
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(td, exist_ok=True)
        # 组装一个含补充类型的 stub windows.h。
        stub_win = os.path.join(STUB, "windows.h")
        with open(stub_win, encoding="utf-8") as f:
            stub_txt = f.read()
        # 保证 WCHAR / CHAR_INFO 等存在（stub 已含 WCHAR；仅追加缺的类型，避免重复定义）。
        extra = ""
        if "CHAR_INFO" not in stub_txt:
            extra += STUB_EXTRA
        with open(os.path.join(td, "windows.h"), "w", encoding="utf-8") as f:
            f.write(stub_txt + "\n" + extra)
        for name in ("shellapi.h", "process.h"):
            srcp = os.path.join(STUB, name)
            if os.path.exists(srcp):
                with open(srcp, encoding="utf-8") as f, open(os.path.join(td, name), "w", encoding="utf-8") as o:
                    o.write(f.read())
        with open(os.path.join(td, "globs.c"), "w", encoding="utf-8") as f:
            f.write(GLOBALS)
        with open(os.path.join(td, "h.c"), "w", encoding="utf-8") as f:
            f.write(HARNESS)
        exe = os.path.join(td, "h.bin")
        cp = subprocess.run(
            ["gcc", "-O1", "-Wall", "-Wextra", "-I" + td, "-I" + INC,
             os.path.join(td, "h.c"), os.path.join(td, "globs.c"),
             os.path.join(SRC, "screen.c"), os.path.join(SRC, "vt.c"),
             os.path.join(SRC, "utf8.c"), "-o", exe, "-lm"],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        if cp.returncode != 0:
            print(cp.stderr, file=sys.stderr)
            print("FAIL: 宽字符换行 harness 无法编译", file=sys.stderr)
            return 1
        run = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", errors="replace")
        print(run.stdout)
        if run.returncode != 0 or "[FAIL]" in run.stdout:
            print(run.stderr, file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
