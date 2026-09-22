#!/usr/bin/env python3
"""verify_copy_wide.py

v1.8.18 回归：复制中文 / 全角等宽字符时，占位格不能被当成空格。

中文（如"保" U+4FDD）在终端占两列，vt 写入时主格写字、次格写 0 占位。
旧复制代码把 ch==0 的占位格一律当空格（text_ch=' '），导致
"保留所有权利" 复制成 "保 留 所 有 权 利"（每字后多一个空格）。

两层验证：
  (1) 从【真实 src/input.c】抽取 copy_cell_is_wide_spacer()，断言：
      中文/全角后的 0 占位格 -> 1（跳过）；普通空格、真实空格、non-BMP
      emoji 低代理、行首 0、普通 ASCII 后 0 -> 0。
  (2) 链接【真实 src/vt.c、screen.c、cliphtml.c】，喂 UTF-8 中文串到真实
      vt 解析，按 input.c 复制逻辑生成 CF_HTML，断言 HTML 文本与纯文本里
      中文之间无多余空格。
变异：把 copy_cell_is_wide_spacer 改成恒返回 0（旧行为），(2) 立即失败。
"""
import os
import re
import subprocess
import sys
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 无 -fsanitize）
#   统一收在 tests/hbuild.py；降级时会自己往 stderr 打 [SKIP-SANITIZER]。
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402

import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
INPUT_C = os.path.join(ROOT, "src", "input.c")
SRC = os.path.join(ROOT, "src")
INC = os.path.join(ROOT, "include")

# ---- (1) 纯函数单测：复制 input.c 的 copy_cell_is_wide_spacer 判定 ----
UNIT_HARNESS = r"""
#include <stdio.h>
#include <wchar.h>
typedef unsigned short WCHAR;
int is_wide_cp(unsigned int cp);
%(func)s
static int failures = 0;
static void ck(const char *name, int got, int want) {
    if (got != want) { printf("[FAIL] %s: got %d want %d\n", name, got, want); failures++; }
    else             { printf("[ok]   %s: %d\n", name, got); }
}
int main(void) {
    /* 保 U+4FDD 宽；其后占位 0 -> 1 */
    ck("中文'保'后的0占位格->跳过", copy_cell_is_wide_spacer(0, 0x4FDD), 1);
    ck("全角'＠'U+FF20后的0占位格->跳过", copy_cell_is_wide_spacer(0, 0xFF20), 1);
    ck("平假名'あ'U+3042后的0占位格->跳过", copy_cell_is_wide_spacer(0, 0x3042), 1);
    /* 真实空格 ch=' '（非0）-> 0 */
    ck("普通空格ch=0x20->不跳过", copy_cell_is_wide_spacer(0x20, 0x4FDD), 0);
    /* ASCII 'A' 后的空 0（非宽字符左邻）-> 0（那是真正的行内空白） */
    ck("ASCII 'A'后的0->不跳过(真空格)", copy_cell_is_wide_spacer(0, 'A'), 0);
    /* non-BMP emoji 主格是高代理 U+D83D（不是 0，走代理对路径），次格低代理 ch!=0 */
    ck("高代理左邻(emoji)->不跳过", copy_cell_is_wide_spacer(0xDC68, 0xD83D), 0);
    ck("高代理左邻且本格0(不会发生)->不跳过", copy_cell_is_wide_spacer(0, 0xD83D), 0);
    /* 左邻为 0（行首/连续空）-> 0 */
    ck("左邻为0->不跳过", copy_cell_is_wide_spacer(0, 0), 0);
    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nUNIT CHECKS PASSED\n");
    return 0;
}
"""

# ---- (2) 端到端：真实 vt + screen + cliphtml，喂中文串，走复制逻辑 ----
E2E_HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "screen.h"
#include "vt.h"
#include "cliphtml.h"
/* 与 input.c 修复后的复制逻辑一致（宽占位格 skip；cliphtml 跳过 skip 格） */
int main(void) {
    ScreenBuffer s; memset(&s,0,sizeof s); screen_init(&s,40,4); s.pane_index=-1;
    const char *utf8 = " 保留所有权利";   /* 1 前导空格 + 6 个汉字 */
    screen_process_output(&s, utf8, (int)strlen(utf8));

    int pr = screen_phys_row(&s, 0);
    ClipHtmlCell cells[64];
    /* ★ 必须先 cliphtml_init。cliphtml_frag_begin() 直接往 b 里 buf_puts，
     * 它【不】负责初始化 —— b->data / b->cap 是调用方的责任（生产代码
     * src/input.c:1504 就是先 cliphtml_init 再 frag_begin）。这里原来漏了，
     * b 是未初始化的栈变量，buf_reserve 就去 realloc 一个栈上的垃圾指针：
     * Linux 上新栈页恰好是 0（realloc(NULL,..) == malloc）所以侥幸过，
     * macOS 栈上是垃圾，ASan 直接报 "attempting free on address which was
     * not malloc()-ed"。CI 的 macOS 作业抓出来的。 */
    ClipHtmlBuf h; cliphtml_init(&h); cliphtml_frag_begin(&h);
    /* 纯文本收集：与 input.c 一致，宽占位格 continue，CJK 用 '?' 占位计数 */
    char text[256]; int tl=0;
    int x0=0, x1=39, valid=x0-1;
    for (int x=x0;x<=x1;x++){
        ClipHtmlCell*hc=&cells[x]; memset(hc,0,sizeof*hc);
        CHAR_INFO*c=&s.lines[pr].cells[x];
        WCHAR ch=c->Char.UnicodeChar;
        WCHAR prev= x>0 ? s.lines[pr].cells[x-1].Char.UnicodeChar : 0;
        if (copy_cell_is_wide_spacer(ch, prev)) { hc->skip=1; continue; }
        WCHAR tc=(ch!=0)?ch:L' ';
        if (tc!=L' '){ valid=x; if(tl<250) text[tl++]=(tc<128)?(char)tc:'C'; }
        hc->ch=(unsigned short)tc;
    }
    /* 纯文本应只有 7 个有效字符：1 前导空格不计 valid，6 个 CJK = 'C'x6，无中间空格 */
    int ccount=0, midsp=0;
    for (int i=0;i<tl;i++){ if(text[i]=='C')ccount++; if(text[i]==' ')midsp++; }
    cliphtml_frag_row(&h,cells,x0,valid);
    cliphtml_finalize(&h);

    /* 统计生成 HTML 里行的字符数（span 外 + span 内可见字符）。我们关心：
       中文之间没有被插入空格。cliphtml 跳过 skip 格，所以总可见字符数应 =
       1 前导空格 + 6 汉字（汉字在 stub 里按非 ASCII 直接转义为 '?'，这里用
       真实 UTF-8：cell ch 是真实 WCHAR，frag_row 会按 UTF-8 输出多字节）。 */
    /* 直接从最终 HTML 取 <pre>...</pre> 内容，统计其中的空格数。 */
    const char* pre=strstr(h.data,"<pre");
    const char* gt=strchr(pre,'>');
    const char* end=strstr(gt+1,"</pre>");
    int len=(int)(end-(gt+1));
    int spaces=0, cjk=0;
    for (const char*p=gt+1;p<end;p++){
        unsigned char u=(unsigned char)*p;
        if (*p==' ') spaces++;
        if (u>=0xE0 && u<=0xEF) { /* UTF-8 3-byte = CJK */ cjk++; }
    }
    printf("HTML 片段长度=%d, 空格数=%d, CJK三字节首字节数=%d\n",len,spaces,cjk);
    /* 期望：前导 1 个空格 + 6 个汉字（各 3 字节）= 19 字节；空格仅 1 个。 */
    if (spaces != 1) { printf("[FAIL] 多余空格：期望1(前导)，实际%d\n",spaces); return 1; }
    if (cjk != 6)    { printf("[FAIL] CJK字数：期望6，实际%d\n",cjk); return 1; }
    printf("[ok]   复制中文无插入空格：'%s' -> 空格1 + CJK6\n", utf8);

    /* 纯文本路径：宽占位格不进 wbuf，6 个 CJK 字符，无中间空格 */
    if (ccount != 6) { printf("[FAIL] 纯文本 CJK 字数：期望6 实际%d\n",ccount); return 1; }
    if (midsp != 0) { printf("[FAIL] 纯文本中间多余空格 %d\n",midsp); return 1; }
    printf("[ok]   纯文本无中间空格（6 CJK，0 额外空格）\n");
    printf("\nE2E WIDE-COPY CHECKS PASSED\n");
    screen_free(&s);
    return 0;
}
"""


def extract_func(src, name):
    idx = src.find("static int " + name)
    if idx < 0:
        idx = src.find(name)
    brace = src.find("{", idx)
    depth = 0
    i = brace
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[idx:i + 1]
        i += 1
    raise SystemExit("花括号不配对: " + name)


def run_cc(cfile, out, extra_objs=(), cxx=False, extra_inc=()):
    cc = "g++" if cxx else "gcc"
    inc_args = []
    for d in extra_inc:
        inc_args += ["-I", d]
    cmd = [cc, "-O1", "-g", *hbuild.sanitize_flags(),
           "-Wall", "-Wextra"] + inc_args + ["-I", INC, cfile, "-o", out] + list(extra_objs)
    if cxx:
        cmd += ["-x", "c++"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("编译失败")
    r = subprocess.run([out], capture_output=True, text=True,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    return r


def main():
    src = open(INPUT_C, encoding="utf-8").read()
    func = extract_func(src, "copy_cell_is_wide_spacer")

    with tempfile.TemporaryDirectory() as td:
        # stub windows.h（utf8.c 用 is_wide_cp，不依赖 windows；screen/vt 需要）
        stubdir = os.path.join(td, "stub")
        os.makedirs(stubdir)
        open(os.path.join(stubdir, "windows.h"), "w").write(
            "#pragma once\n#ifndef STUB_WINDOWS_H\n#define STUB_WINDOWS_H\n"
            "#include <stddef.h>\n#include <wchar.h>\n"
            "typedef unsigned short WORD; typedef unsigned long DWORD; typedef long LONG;\n"
            "typedef short SHORT; typedef wchar_t WCHAR; typedef unsigned char BYTE;\n"
            "typedef void* HANDLE; typedef int BOOL; typedef unsigned int UINT; typedef int INT;\n"
            "typedef unsigned long long DWORD64; typedef void* LPVOID; typedef void* HWND;\n"
            "typedef const wchar_t* LPCWSTR; typedef wchar_t* LPWSTR;\n"
            "typedef void* HPCON; typedef struct { volatile long v; } CRITICAL_SECTION;\n"
            "typedef struct _COORD { SHORT X; SHORT Y; } COORD;\n"
            "typedef struct _SMALL_RECT { SHORT Left,Top,Right,Bottom; } SMALL_RECT;\n"
            "typedef struct _CHAR_INFO { union { WCHAR UnicodeChar; char AsciiChar; } Char; WORD Attributes; } CHAR_INFO;\n"
            "typedef struct _KEY_EVENT_RECORD { int bKeyDown; WORD wRepeatCount; WORD wVirtualKeyCode; WORD wVirtualScanCode; union { WCHAR UnicodeChar; char AsciiChar; } uChar; DWORD dwControlKeyState; } KEY_EVENT_RECORD;\n"
            "typedef struct _MOUSE_EVENT_RECORD { COORD dwMousePosition; DWORD dwButtonState; DWORD dwControlKeyState; DWORD dwEventFlags; } MOUSE_EVENT_RECORD;\n"
            "#define TRUE 1\n#define FALSE 0\n#ifndef NULL\n#define NULL 0\n#endif\n"
            "#define FOREGROUND_BLUE 0x1\n#define FOREGROUND_GREEN 0x2\n#define FOREGROUND_RED 0x4\n#define FOREGROUND_INTENSITY 0x8\n"
            "#define COMMON_LVB_UNDERSCORE 0x8000\n#define COMMON_LVB_REVERSE_VIDEO 0x4000\n"
            "#endif\n")
        for h in ["shellapi.h", "process.h", "windowsx.h", "winuser.h", "wincon.h",
                  "processthreadsapi.h", "handleapi.h", "fileapi.h", "synchapi.h", "namedpipeapi.h"]:
            open(os.path.join(stubdir, h), "w").write('#pragma once\n#include "windows.h"\n')
        # globals stub
        glob = os.path.join(td, "globals.c")
        open(glob, "w").write(
            '#include "config.h"\n#include "types.h"\n'
            'int g_scrollback_lines=10000; MuxState g_mux;\n'
            'SearchMatch g_search_matches[MAX_SEARCH_MATCHES];\n'
            'int g_search_active=0,g_search_match_count=0,g_search_match_cur=0,g_mouse_enabled=1;\n'
            'ChooserItem g_chooser_items[MAX_CHOOSER_ITEMS]; int g_chooser_item_count=0;\n'
            'int g_settings_nav=0,g_settings_field=0,g_settings_table_sel=0,g_default_startup=0,g_copy_on_select=0,g_confirm_on_exit=0;\n')

        def obj(name):
            of = os.path.join(td, name + ".o")
            p = subprocess.run(
                ["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
                 "-I", stubdir, "-I", INC, "-c",
                 os.path.join(SRC, name + ".c"), "-o", of],
                capture_output=True, text=True)
            if p.returncode != 0:
                print(p.stderr)
                raise SystemExit(f"编译 {name} 失败")
            return of

        objs = [obj("screen"), obj("vt"), obj("utf8"), obj("cliphtml")]
        gobj = os.path.join(td, "globals.o")
        p = subprocess.run(["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
                            "-I", stubdir, "-I", INC, "-c", glob, "-o", gobj],
                           capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stderr)
            raise SystemExit("globals 编译失败")

        # (1) 纯函数单测（只需 utf8 提供 is_wide_cp）
        u1 = os.path.join(td, "unit.c")
        open(u1, "w").write(UNIT_HARNESS.replace("%(func)s", func))
        r = run_cc(u1, os.path.join(td, "unit"), extra_objs=[obj("utf8")])
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            raise SystemExit(1)

        # (2) 端到端（screen+vt+utf8+cliphtml+globals），注入真实判定函数
        u2 = os.path.join(td, "e2e.c")
        e2e = E2E_HARNESS.replace('int main(void) {', func + '\nint main(void) {', 1)
        open(u2, "w").write(e2e)
        r = run_cc(u2, os.path.join(td, "e2e"), extra_objs=objs + [gobj],
                   extra_inc=[stubdir])
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            raise SystemExit(1)

    print("[OK] verify_copy_wide passed")


if __name__ == "__main__":
    main()
