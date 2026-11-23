#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_anim_frame.py — 切标签「滑入动画」重写出来的帧必须是合法 VT 流。

为什么要有这一条：新建/切换 pane 那一瞬间（`anim` 开着）整帧会被
`anim_slide_fill` / `anim_slide_rewrite` 重写一遍再发出去。重写里只要丢掉一个字符
（最典型的是某个行定位 `ESC[r;cH` 的 ESC），宿主就会把剩下的 `[4;1H` 当正文打出来
——用户看到的正是「新建一个跑不存在命令的 pane 时，屏幕上闪一下 ANSI 代码，随即消失」。
`anim = off` 就不再闪，所以这条路必须单独钉住。

做法与 verify_screen_state.py 同一套：从 `src/render.c` 里抽出真实的
`anim_esc` / `anim_chunk_has_text` / `anim_slide_fill` / `anim_slide_rewrite` 四段
源码，配上 `src/utf8.c` 编译成小程序，喂真实形状的整帧，逐个位移 d 重写后检查：
  1) 每一段 ESC 序列都完整（`ESC[...` 必须有终止字节）；
  2) 正文里不许出现「没有 ESC 的 `[数字;数字H`」；
  3) 重写不会把字节数算错（`*posp`/`*endp` 必须落在合法边界上）。

变异自证：把 `anim_slide_fill` 里拷贝序列的那句 `SLIDE_PUTN(out + i, L)` 改成
`SLIDE_PUTN(out + i, L - 1)`（模拟丢一个字符），本脚本必须报错。
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


def extract_func(text, prefix):
    idx = text.find(prefix)
    if idx == -1:
        sys.exit("FAIL: %s not found" % prefix)
    brace = text.find("{", idx)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[idx:i + 1]
    sys.exit("FAIL: unbalanced braces for %s" % prefix)


def extract_macro(text, name):
    idx = text.find("#define " + name)
    if idx == -1:
        sys.exit("FAIL: macro %s not found" % name)
    end = text.find("while (0)", idx)
    if end == -1:
        sys.exit("FAIL: macro %s unterminated" % name)
    end = text.find("\n", end)
    return text[idx:end]


HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "utf8.h"

static char *g_slide_buf;
static int g_slide_cap;

__EXTRACTED__

static int failures;

/* 一帧是不是合法 VT 流；返回 0 = 合法，否则返回出错位置的说明用下标 */
static int bad_index(const char *b, int n, char *why, int whycap) {
    int i = 0;
    while (i < n) {
        if (b[i] != '\x1b') {
            /* 正文里出现「[数字;数字H」= 某个 ESC 被吃掉了 */
            if (b[i] == '[' && i + 2 < n) {
                int j = i + 1, digits = 0;
                while (j < n && b[j] >= '0' && b[j] <= '9') { j++; digits++; }
                if (digits > 0 && j < n && b[j] == ';') {
                    snprintf(why, whycap, "bare CSI at %d", i);
                    return i;
                }
            }
            i++;
            continue;
        }
        if (i + 1 >= n) { snprintf(why, whycap, "lone ESC at end"); return i; }
        if (b[i + 1] == '[') {
            int j = i + 2;
            while (j < n && !((unsigned char)b[j] >= 0x40 && (unsigned char)b[j] <= 0x7e)) j++;
            if (j >= n) { snprintf(why, whycap, "unfinished CSI at %d", i); return i; }
            i = j + 1;
        } else if (b[i + 1] == ']') {
            int j = i + 2;
            while (j < n && b[j] != '\x07') j++;
            if (j >= n) { snprintf(why, whycap, "unfinished OSC at %d", i); return i; }
            i = j + 1;
        } else {
            snprintf(why, whycap, "ESC + %02x at %d", (unsigned char)b[i + 1], i);
            return i;
        }
    }
    return 0;
}

/* 造一帧真实形状：逐行 CUP + SGR + 正文（满宽），末尾标签栏，再跟光标段。 */
static int make_frame(char *out, int cap, int rows, int cols, int with_cjk, int tail_mode) {
    int n = 0;
    const char *cjk = "\xe4\xb8\xad\xe6\x96\x87";   /* 中文 */
    for (int r = 2; r <= rows + 1 && n < cap - 400; r++) {
        n += snprintf(out + n, cap - n, "\x1b[%d;1H\x1b[0m\x1b[0;37;40m", r);
        int used = 0;
        if (with_cjk && (r % 3) == 0) {
            n += snprintf(out + n, cap - n, "  %s", cjk);
            used = 2 + 4;
            while (used < cols - 6 && n < cap - 40) { out[n++] = 'a'; used++; }
            if (used + 4 <= cols) { memcpy(out + n, cjk, 6); n += 6; used += 4; }
            else { n += snprintf(out + n, cap - n, "    "); used += 4; }
        } else {
            while (used < cols - 4) { out[n++] = (char)('0' + used % 10); used++; }
            n += snprintf(out + n, cap - n, "abcd");
            used += 4;
        }
        while (used < cols) { out[n++] = ' '; used++; }
        if (tail_mode) n += snprintf(out + n, cap - n, "\x1b[K");
    }
    /* 标签栏（不参与位移） */
    n += snprintf(out + n, cap - n, "\x1b[1;1H\x1b[0m\x1b[0;37;40m termux [cmd] [+]%*s", cols - 18, "");
    /* 光标段 */
    n += snprintf(out + n, cap - n, "\x1b[%d;%dH\x1b[?25h", rows - 2, cols / 2);
    return n;
}

static void run_case(int cols, int rows, int with_cjk, int tail_mode, int d) {
    static char frame[1 << 16];
    static char copy[1 << 18];
    int len = make_frame(frame, (int)sizeof frame, rows, cols, with_cjk, tail_mode);
    memcpy(copy, frame, (size_t)len);
    int pos = len, cursor = len - 16;      /* 光标段 16 字节：CUP + ?25h */
    int ok = anim_slide_rewrite(copy, (int)sizeof copy, &pos, &cursor, rows, cols, d);
    if (!ok) return;                        /* 装不下 ⇒ 这一帧不动画，合法 */
    char why[128] = {0};
    int bad = bad_index(copy, pos, why, (int)sizeof why);
    if (bad) {
        printf("[FAIL] cols=%d rows=%d cjk=%d tail=%d d=%d: %s\n", cols, rows, with_cjk, tail_mode, d, why);
        printf("       上下文: ");
        for (int i = (bad > 40 ? bad - 40 : 0); i < bad + 30 && i < pos; i++) {
            unsigned char c = (unsigned char)copy[i];
            if (c == 0x1b) printf("\\e");
            else if (c < 0x20) printf("\\x%02x", c);
            else putchar(c);
        }
        printf("\n");
        failures++;
    }
    if (cursor < 0 || cursor > pos) {
        printf("[FAIL] cols=%d rows=%d d=%d: cursor=%d pos=%d 越界\n", cols, rows, d, cursor, pos);
        failures++;
    }
}

int main(int argc, char **argv) {
    int cols = 120, rows = 29;
    if (argc > 1) {
        /* --frame <file>：拿真机抓到的整帧（render_dump.log 里的一帧）当输入 */
        FILE *f = fopen(argv[1], "rb");
        if (!f) { printf("打不开 %s\n", argv[1]); return 2; }
        static char body[1 << 18];
        int n = (int)fread(body, 1, sizeof body - 1, f);
        fclose(f);
        body[n] = 0;
        /* 光标段 = 末尾「最后一个 CUP + ?25h」，按最后一处 \e[ 数字;数字 H 切 */
        int cut = -1;
        for (int i = 0; i + 3 < n; i++) {
            if (body[i] == '\x1b' && body[i + 1] == '[') {
                int j = i + 2, dg = 0;
                while (j < n && body[j] >= '0' && body[j] <= '9') { j++; dg++; }
                if (dg > 0 && j < n && body[j] == ';') {
                    j++;
                    while (j < n && body[j] >= '0' && body[j] <= '9') j++;
                    if (j < n && body[j] == 'H') cut = i;
                }
            }
        }
        if (cut < 0) { printf("帧里找不到光标段\n"); return 2; }
        for (int dd = -cols; dd <= cols; dd++) {
            if (dd == 0) continue;
            static char copy[1 << 18];
            memcpy(copy, body, (size_t)n);
            int pos = n, cursor = cut;
            int ok = anim_slide_rewrite(copy, (int)sizeof copy, &pos, &cursor, rows, cols, dd);
            if (!ok) continue;
            char why[128] = {0};
            int bad = bad_index(copy, pos, why, (int)sizeof why);
            if (bad) {
                printf("[FAIL] real-frame d=%d: %s\n", dd, why);
                failures++;
            }
        }
        printf(failures ? "ANIM-FRAME FAILED (%d)\n" : "ANIM-FRAME PASSED\n", failures);
        return failures ? 1 : 0;
    }
    for (int cjk = 0; cjk <= 1; cjk++)
        for (int tail = 0; tail <= 1; tail++)
            for (int d = -cols; d <= cols; d++) {
                if (d == 0) continue;
                run_case(cols, rows, cjk, tail, d);
            }
    printf(failures ? "ANIM-FRAME FAILED (%d)\n" : "ANIM-FRAME PASSED\n", failures);
    return failures ? 1 : 0;
}
"""


def main() -> int:
    render_c = open(os.path.join(SRC, "render.c"), encoding="utf-8").read()
    pieces = [
        extract_macro( render_c, "SLIDE_PUTF"),
        extract_macro( render_c, "SLIDE_PUTN"),
        extract_func(render_c, "static int anim_esc("),
        extract_func(render_c, "static int anim_chunk_has_text("),
        extract_func(render_c, "static int anim_slide_fill("),
        extract_func(render_c, "static int anim_slide_rewrite("),
    ]
    body = "\n\n".join(pieces)
    src = HARNESS.replace("__EXTRACTED__", body)
    print("=== 切标签滑入动画：重写后的帧必须是合法 VT 流 (verify_anim_frame.py) ===")
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(td, exist_ok=True)
        with open(os.path.join(td, "h.c"), "w", encoding="utf-8") as f:
            f.write(src)
        exe = os.path.join(td, "h.bin")
        cp = subprocess.run(
            ["gcc", "-O1", "-Wall", "-Wextra", "-I" + INC,
             os.path.join(td, "h.c"), os.path.join(SRC, "utf8.c"), "-o", exe],
            encoding="utf-8", errors="replace")
        if cp.returncode != 0:
            print("FAIL: 无法编译动画 harness", file=sys.stderr)
            return 1
        args = [exe]
        if len(sys.argv) > 2 and sys.argv[1] == "--frame":
            args.append(sys.argv[2])
        run = subprocess.run(args, encoding="utf-8", errors="replace")
        print(run.stdout or "")
        return 0 if run.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
