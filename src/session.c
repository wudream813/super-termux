/* ---------------------------------------------------------------------------
 * v2.3.0：会话快照（`[general] session = on`）。记录格式与设计说明见 include/session.h。
 *
 * 三条要紧的取舍：
 *  1) 存的是【逻辑行】（软换行折下来的续行先并回去），不是屏幕物理行 —— 这样下次
 *     用不同宽度打开时历史由引擎自己 reflow，不会出现「一行被腰斩」或者半个汉字
 *     （见 screen.h 里 line_wrap 那段注释，本仓库为这类问题翻过好几次车）。
 *  2) 灌回去走 screen_process_output —— 与真机输出同一条路：软换行标记、宽字符、
 *     滚动都归引擎算，本模块不自己维护环形缓冲（自己再算一遍就是第二个真相）。
 *  3) 读档失败一律当「没有快照」，中途出错就停在这：宁可少恢复，不让启动挂掉。
 * ------------------------------------------------------------------------- */

#include "session.h"

#include "common.h"
#include "config.h"
#include "pane.h"
#include "platform.h"
#include "screen.h"
#include "split.h"
#include "types.h"
#include "utf8.h"
#include "vt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>          /* time() —— POSIX 那边被别的头顺带拉进来了，MinGW 不会：
                            * 少这一行就是 implicit-function-declaration，Windows 作业直接红。 */
#ifndef _WIN32
#include <dirent.h>        /* v2.3.2：exe 目录里找最新那份 *.session */
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define SESS_MAX_BYTES      (6 * 1024 * 1024)   /* 快照文件上限：更大的直接不认 */
#define SESS_MAX_LINES      4000                /* 每个窗格最多存这么多逻辑行 */
#define SESS_MAX_LINE_BYTES 8000                /* 单条逻辑行的字节上限 */
#define SESS_VER_TEXT     "2"                 /* 快照格式版本；1 = 纯文本（无 ANSI、无转义） */
#define SESS_PATH_NAME      L"termux.session"
#define SESS_HOME_NAME      L".termux.session"

/* 恢复时写在窗格最前面那行说明的前缀。写的和读的要认的是同一个串 ⇒ 只在这里写一次
 * （sess_feed 拼的就是「SESS_MARK_HEAD + 「N 行；…）」，存盘时按同一前缀把它剔掉）。 */
#define SESS_MARK_HEAD      "\xe2\x94\x80\xe2\x94\x80 \xe4\xb8\x8a\xe6\xac\xa1\xe4\xbc\x9a\xe8\xaf\x9d\xe7\x9a\x84\xe5\x8e\x86\xe5\x8f\xb2\xef\xbc\x88\xe5\x85\xb1 "

/* v2.3.2：session = on 但这次什么都没读回来时，在屏上留一行说明 —— 免得「没恢复」和
 * 「功能没开」「快照读不认」三种情况长成同一个样子（前一轮就是被这个坑到的：判据全绿，
 * 用户那边只是「什么也没发生」）。以 SESS_NOTE_HEAD 开头的行存盘时同样剔掉。 */
#define SESS_NOTE_HEAD      "\xe2\x94\x80\xe2\x94\x80 \xe4\xbc\x9a\xe8\xaf\x9d\xef\xbc\x9a"
#define SESS_NOTE_NONE      SESS_NOTE_HEAD "\xe6\xb2\xa1\xe6\x9c\x89\xe5\x8f\xaf\xe8\xaf\xbb\xe7\x9a\x84\xe5\xbf\xab\xe7\x85\xa7\xef\xbc\x88\xe9\x80\x80\xe5\x87\xba\xe6\x97\xb6\xe4\xbc\x9a\xe5\x86\x99\xe5\x88\xb0 exe \xe5\x90\x8c\xe7\x9b\xae\xe5\xbd\x95 termux.session\xef\xbc\x89\xe2\x94\x80\xe2\x94\x80"
#define SESS_NOTE_BAD       SESS_NOTE_HEAD "\xe6\x89\xbe\xe5\x88\xb0\xe4\xba\x86\xe5\xbf\xab\xe7\x85\xa7\xe4\xbd\x86\xe8\xaf\xbb\xe4\xb8\x8d\xe8\xae\xa4\xef\xbc\x8c\xe6\x9c\xac\xe6\xac\xa1\xe6\x8c\x89\xe6\x97\xa0\xe5\xbf\xab\xe7\x85\xa7\xe5\xa4\x84\xe7\x90\x86 \xe2\x94\x80\xe2\x94\x80"
#define SESS_MARK_TAIL      " \xe8\xa1\x8c\xef\xbc\x9b\xe8\xbf\x9b\xe7\xa8\x8b\xe6\xb2\xa1\xe6\x9c\x89\xe7\x95\x99\xe5\x9c\xa8\xe5\x90\x8e\xe5\x8f\xb0\xef\xbc\x89\xe2\x94\x80\xe2\x94\x80" 

/* ---- 追加式小缓冲：写满上限就停止追加（宁少不错，绝不越界） ---- */
typedef struct { char *p; int len, cap; } SBuf;

static void sbuf_add(SBuf *b, const char *s, int n) {
    if (n <= 0 || b->len >= SESS_MAX_BYTES || b->len + n > SESS_MAX_BYTES) return;
    if (b->len + n > b->cap) {
        int nc = b->cap ? b->cap * 2 : 16384;
        while (nc < b->len + n && nc < SESS_MAX_BYTES) nc *= 2;
        if (nc > SESS_MAX_BYTES) nc = SESS_MAX_BYTES;
        if (nc < b->len + n) return;
        char *np = (char *)realloc(b->p, (size_t)nc + 1);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sbuf_str(SBuf *b, const char *s) { sbuf_add(b, s, (int)strlen(s)); }

static void sbuf_fmt(SBuf *b, const char *fmt, ...) {
    char tmp[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    sbuf_add(b, tmp, n);
}

/* 文本 → 快照里的一段记录：反斜杠写成 `\\`、ESC 写成 `\e`。为什么不放裸 ESC：
 * 文件格式是「一行一条记录」，把 ESC 原样塞进去以后，`grep` / 编辑器 / 逐行读代码
 * 的人都会看见控制字节，D 记录也不再是「能用肉眼看的历史」；转义只花一个字节。
 * 有了这套转义，正文里出现字面量 `\e`（比如 `cat` 了一个带转义的脚本）也不会被
 * 读回时当成真 ESC —— 那必须写成 `\\e`。 */
static void sbuf_esc(SBuf *b, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\') sbuf_str(b, "\\\\");
        else if (s[i] == 0x1B) sbuf_str(b, "\\e");
        else sbuf_add(b, s + i, 1);
    }
}

/* 快照里的一段正文 → 真正喂给屏幕的字节：`\e` 还原成 ESC、`\\` 还原成反斜杠。
 * ver_esc=0（v1 的老档）时原样拷贝 —— 老档里的反斜杠是正文而不是转义引导符，
 * 把 `C:\e` 当成 ESC 会把用户的 Windows 路径读花。 */
static void sess_unesc(SBuf *b, const char *s, int ver_esc) {
    if (!ver_esc) { sbuf_str(b, s); return; }
    const char *p = s;
    while (*p) {
        if (p[0] == '\\' && p[1] == 'e') { sbuf_add(b, "\x1b", 1); p += 2; }
        else if (p[0] == '\\' && p[1] == '\\') { sbuf_add(b, "\\", 1); p += 2; }
        else { sbuf_add(b, p, 1); p++; }
    }
}

/* 路径与 ini 用同一套定位（exe 旁边优先，其次主目录下的点文件），见 config_sibling_path。
 * 写永远只写这一支（「一次写盘」这条口径就看它）。 */
static void sess_path(WCHAR *out, int out_len, int for_write) {
    config_sibling_path(out, out_len, for_write, SESS_PATH_NAME, SESS_HOME_NAME);
}

#ifndef _WIN32                       /* Windows 侧全程宽字符（FindFirstFileW/stat 用不上），
                                      * 所以这支只在 POSIX 编出来，免得 -Wunused-function 撞上 -Werror */
/* 宽字符路径 → UTF-8（POSIX 侧 opendir/stat 只吃窄字符）。 */
static int sess_w2u(const WCHAR *w, char *dst, int dst_len) {
    if (!w || !w[0] || dst_len <= 0) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, dst_len, NULL, NULL);
    if (n <= 0) { dst[0] = 0; return 0; }
    dst[dst_len - 1] = 0;
    return 1;
}
#endif

/* v2.3.2 读档的「发现」：不止认 exe 旁边那一个文件名。
 *
 * 为什么要这一支：换版本的人多半是把 termux-2.3.x-windows-x64.exe 直接扔进 Downloads
 * 跑 —— 快照写在【上一次那个 exe 的名字】旁边，下一次换个名字启动就找不到，看着就像
 * 「根本没存」。所以下次启动时：exe 目录里所有 *.session + 主目录那份点文件，谁最新
 * 用谁（只读；写仍然只写规范名，不留一地文件）。
 * ---------------------------------------------------------------------- */
static time_t sess_mtime(const WCHAR *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return 0;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    /* FILETIME = 1601 起的 100ns 计数 ⇒ 换算成 Unix 秒（差值 11644473600） */
    unsigned long long t = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32)
                          | (unsigned long long)fa.ftLastWriteTime.dwLowDateTime;
    return (time_t)(t / 10000000ULL - 11644473600ULL);
#else
    char u[MAX_PATH * 4];
    if (!sess_w2u(path, u, (int)sizeof(u))) return 0;
    struct stat st;
    if (stat(u, &st) != 0 || S_ISDIR(st.st_mode)) return 0;
    return st.st_mtime;
#endif
}

/* 目录里挑一个最新修改的 *.session 出来（连自己那份规范名一起比）。
 * 找不到任何一份 ⇒ 返回 0，out 里放的是「本该写在哪」，给提示语用。 */
static int sess_pick_newest(WCHAR *out, int out_len) {
    WCHAR path[MAX_PATH] = { 0 };
    sess_path(path, MAX_PATH, 1);                    /* exe 旁的规范名（一定先建目录串） */
    wcsncpy(out, path, out_len - 1);
    WCHAR *sep = wcsrchr(out, TERMUX_PATH_SEP);
    if (!sep) return 0;
    *sep = 0;                                        /* out 现在是目录 */

    time_t best = 0;
    WCHAR bestname[MAX_PATH] = { 0 };

#ifdef _WIN32
    WCHAR pat[MAX_PATH * 2] = { 0 };
    _snwprintf(pat, (int)(sizeof(pat) / sizeof(pat[0])) - 1, L"%s" TERMUX_PATH_SEP_S L"*.session", out);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            WCHAR full[MAX_PATH * 2] = { 0 };
            _snwprintf(full, (int)(sizeof(full) / sizeof(full[0])) - 1,
                       L"%s" TERMUX_PATH_SEP_S L"%s", out, fd.cFileName);
            time_t t = sess_mtime(full);
            if (t > best) { best = t; wcsncpy(bestname, fd.cFileName, MAX_PATH - 1); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    char dir[MAX_PATH * 4];
    if (sess_w2u(out, dir, (int)sizeof(dir))) {
        DIR *d = opendir(dir);
        if (d) {
            size_t dlen = strlen(dir);
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                size_t nl = strlen(de->d_name);
                if (nl < 8 || strcmp(de->d_name + nl - 8, ".session") != 0) continue;
                char full[MAX_PATH * 6];
                if (dlen + nl + 2 >= sizeof(full)) continue;
                memcpy(full, dir, dlen);
                full[dlen] = '/';
                memcpy(full + dlen + 1, de->d_name, nl + 1);
                struct stat st;
                if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                if (st.st_mtime > best) {
                    best = st.st_mtime;
                    for (size_t i = 0; i <= nl && i < MAX_PATH - 1; i++)
                        bestname[i] = (WCHAR)(unsigned char)de->d_name[i];
                }
            }
            closedir(d);
        }
    }
#endif

    /* 主目录那份也参评 */
    const WCHAR *prof = plat_user_home();
    if (prof) {
        WCHAR alt[MAX_PATH] = { 0 };
        _snwprintf(alt, MAX_PATH - 1, L"%s" TERMUX_PATH_SEP_S SESS_HOME_NAME, prof);
        time_t t = sess_mtime(alt);
        if (t > best) {
            wcsncpy(out, alt, out_len - 1);
            return 1;
        }
    }
    if (!best || !bestname[0]) return 0;
    WCHAR full[MAX_PATH * 2] = { 0 };
    _snwprintf(full, (int)(sizeof(full) / sizeof(full[0])) - 1,
               L"%s" TERMUX_PATH_SEP_S L"%s", out, bestname);
    wcsncpy(out, full, out_len - 1);
    return 1;
}

/* =========================================================================
 * 存
 * ========================================================================= */

/* ---- v2.3.1：把「终端程序原本发的 ANSI」也存下来 ----------------------------------
 * 用户的原话是「恢复要保存 ANSI 代码」：只存纯文本的话，恢复出来是一片素色，`ls`
 * 的目录色、报错的红、进度条的底全没了 —— 看着就不算「恢复」。
 *
 * 一格的属性 = `cells[].Attributes`（16 色 + 下划线）+ 并行的 `fg_rgb/bg_rgb/rgb_valid`
 * （真彩，见 screen.c 的 cell_truecolor）。存进档里的是【索引色 + 真彩】这两类
 * 「程序自己发的」，刻意【不】复用 render.c 的 emit_attr16：那条会把 16 色按
 * `[theme] pane_*` 换成真彩，焊死在快照里就换不了主题了。存索引色 ⇒ 读回来由
 * 当前主题重新上色。默认属性（fg=7 / bg=0 / 无下划线 / 无真彩）一个字节都不发，
 * 否则满屏都是 `\e[0;37;40m`，文件胀十倍、历史也不能用肉眼看。
 * -------------------------------------------------------------------------- */

static void sess_snprintf_at(char *out, int bs, int *pos, const char *fmt, ...) {
    if (*pos < 0 || *pos >= bs - 1) { *pos = bs - 1 > 0 ? bs - 1 : 0; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, (size_t)(bs - *pos), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    /* vsnprintf 返回的是「本该写多长」，越界时不能直接加，否则 pos 会跑到 bs 外面，
     * 下一句就拿负数当剩余空间。 */
    *pos += (n < bs - *pos - 1) ? n : bs - *pos - 1;
}

/* 返回写进 out 的字节数；0 = 这一格是默认属性，什么都不用发。out 里是【裸 ESC 串】，
 * 由 sess_row_text 统一转义后再进档。 */
static int sess_attr_sgr(WORD attr, WORD frgb, WORD brgb, int fgv, int bgv,
                         char *out, int bs) {
    static const int m8[8] = {0,4,2,6,1,5,3,7};   /* Win32 位标志 → ANSI 颜色号（同 render.c） */
    int fg = attr & 0x0F, bg = (attr >> 4) & 0x0F;
    int ul = (attr & COMMON_LVB_UNDERSCORE) ? 1 : 0;
    if (!fgv && !bgv && !ul && fg == 7 && bg == 0) return 0;
    int pos = 0;
    sess_snprintf_at(out, bs, &pos, "\x1b[0");
    if (ul) sess_snprintf_at(out, bs, &pos, ";4");
    if (fgv) {
        int r, g, b;
        rgb565_split(frgb, &r, &g, &b);
        sess_snprintf_at(out, bs, &pos, ";38;2;%d;%d;%d", r, g, b);
    } else if (fg == 7) {
        sess_snprintf_at(out, bs, &pos, ";39");           /* 前景=默认：换主题也跟着走 */
    } else {
        sess_snprintf_at(out, bs, &pos, ";%d", (fg & 8) ? 90 + m8[fg & 7] : 30 + m8[fg & 7]);
    }
    if (bgv) {
        int r, g, b;
        rgb565_split(brgb, &r, &g, &b);
        sess_snprintf_at(out, bs, &pos, ";48;2;%d;%d;%d", r, g, b);
    } else if (bg == 0) {
        sess_snprintf_at(out, bs, &pos, ";49");
    } else {
        sess_snprintf_at(out, bs, &pos, ";%d", (bg & 8) ? 100 + m8[bg & 7] : 40 + m8[bg & 7]);
    }
    sess_snprintf_at(out, bs, &pos, "m");
    if (pos > 0 && pos < bs) out[pos] = 0;
    return pos;
}

/* 一个物理行的单元格 → 一行「带 ANSI、已转义」的文本（行尾空白去掉），追加到 out。
 * `*had_code` 置 1 = 这一行发过颜色码（收尾要补一个 `\e[m`，别把颜色漏给下一行）。
 * NUL 与 C0 控制符当空格；落单的代理项丢掉 —— 直接扔给 WideCharToMultiByte 会让
 * 整行转换失败或者变成一串 '?'（Windows 的 WCHAR 是 UTF-16，一个 emoji 占两格；
 * POSIX 侧 WCHAR 是 32 位，那两个判断自然不成立，不必 #ifdef）。
 * 返回追加的字节数（-1 = 装不下了，调用方按截断处理）。 */
static int sess_row_text(ScreenBuffer *s, int ph, SBuf *out, int cap, int *had_code,
                         int *cut) {
    ScreenLine *ln = &s->lines[ph];
    int used = ln->used;
    if (used > s->cols) used = s->cols;
    if (used > ln->len) used = ln->len;          /* len = cells[] 的容量（见 screen_init） */
    if (used <= 0) return 0;
    WCHAR *w = (WCHAR *)malloc((size_t)(used + 1) * sizeof(WCHAR));
    /* 每个【留下来的码点】对应回它的格子号：颜色是按格存的，正文是按码点取的，
     * 宽字符次格被跳掉后两者下标就错开了 ⇒ 必须带一张映射表。 */
    int *cell_at = (int *)malloc((size_t)(used + 1) * sizeof(int));
    if (!w || !cell_at) { free(w); free(cell_at); return 0; }
    int n = 0;
    for (int i = 0; i < used; i++) {
        WCHAR c = ln->cells[i].Char.UnicodeChar;
        WCHAR pv = (i > 0) ? ln->cells[i - 1].Char.UnicodeChar : 0;
        int prev_high = (pv >= 0xD800 && pv <= 0xDBFF);
        /* 宽字符的【次格】不是内容（BMP 宽字符的次格写 0、emoji 写低代理 —— 判据与
         * screen.c 的 cell_is_wide_trail 同一条）。v2.3.0 第一版把它当空格，于是
         * 「汉」存成「汉 」，中文历史每存一轮胀一倍、恢复出来还满屏夹空格
         * （实测：说明行被存成「上 次 会 话」）⇒ 必须跳过。 */
        if (c >= 0xDC00 && c <= 0xDFFF && prev_high) continue;
        if (c == 0 && !prev_high && is_wide_cp((unsigned int)pv)) continue;
        if (c == 0 || c < 0x20 || c == 0x7F) { cell_at[n] = i; w[n++] = L' '; continue; }
        if (c >= 0xD800 && c <= 0xDBFF) {        /* 高代理：后面紧跟低代理才成对留下 */
            WCHAR d = (i + 1 < used) ? ln->cells[i + 1].Char.UnicodeChar : 0;
            if (d >= 0xDC00 && d <= 0xDFFF) {
                cell_at[n] = i; w[n++] = c;
                cell_at[n] = i + 1; w[n++] = d;
                i++;
            } else { cell_at[n] = i; w[n++] = L' '; }
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) { cell_at[n] = i; w[n++] = L' '; continue; }   /* 落单的低代理 */
        cell_at[n] = i; w[n++] = c;
    }
    while (n > 0 && w[n - 1] == L' ') n--;       /* 行尾空白不算内容 */
    int start = out->len;
    int i = 0;
    while (i < n) {
        WORD attr = ln->cells[cell_at[i]].Attributes;
        WORD frgb = ln->fg_rgb ? ln->fg_rgb[cell_at[i]] : RGB565_WHITE;
        WORD brgb = ln->bg_rgb ? ln->bg_rgb[cell_at[i]] : RGB565_BLACK;
        unsigned char vv = ln->rgb_valid ? ln->rgb_valid[cell_at[i]] : 0;
        int fgv = (vv & 1) ? 1 : 0, bgv = (vv & 2) ? 1 : 0;
        int j = i + 1;
        while (j < n) {                          /* 同一串样式 = 同一格的五元组，见 render.c 的换色判据 */
            WORD a2 = ln->cells[cell_at[j]].Attributes;
            WORD f2 = ln->fg_rgb ? ln->fg_rgb[cell_at[j]] : RGB565_WHITE;
            WORD b2 = ln->bg_rgb ? ln->bg_rgb[cell_at[j]] : RGB565_BLACK;
            unsigned char v2 = ln->rgb_valid ? ln->rgb_valid[cell_at[j]] : 0;
            if (a2 != attr || f2 != frgb || b2 != brgb || ((v2 & 1) ? 1 : 0) != fgv
                || ((v2 >> 1) & 1) != bgv) break;
            j++;
        }
        char sgr[96];
        int slen = sess_attr_sgr(attr, frgb, brgb, fgv, bgv, sgr, (int)sizeof(sgr));
        if (slen > 0 && (slen >= (int)sizeof(sgr) - 1)) { slen = 0; }   /* 装不下的码宁可不发 */
        if (slen > 0 && out->len + slen + 2 > cap) { *cut = 1; break; }
        if (slen > 0) { sbuf_esc(out, sgr, slen); *had_code = 1; }
        int rl = j - i;
        int need = rl > 0 ? WideCharToMultiByte(CP_UTF8, 0, w + i, rl, NULL, 0, NULL, NULL) : 0;
        if (need > 0) {
            /* 缓冲区按【转换要的长度】开，再按剩余空间截：直接把 need 夹到装不下的
             * 长度再叫它转，WideCharToMultiByte 会返回 0（insufficient buffer），
             * 整行文本反而一个字都存不下来 —— 超长行应当截断，不该消失。 */
            char *dst = (char *)malloc((size_t)need + 1);
            if (!dst) { *cut = 1; break; }
            int g = WideCharToMultiByte(CP_UTF8, 0, w + i, rl, dst, need, NULL, NULL);
            if (g > 0) {
                if (dst[g - 1] == 0) g--;        /* 两侧的实现都可能把结尾 NUL 计进长度 */
                int room = cap - out->len;       /* 转义最坏一个字节胀成两个 ⇒ 只能塞一半 */
                if (g * 2 > room) {
                    if (room >= 2) sbuf_esc(out, dst, room / 2);
                    *cut = 1;
                } else {
                    sbuf_esc(out, dst, g);
                }
            }
            free(dst);
        }
        i = j;
    }
    free(w);
    free(cell_at);
    return out->len - start;                 /* ★ 返回【实际追加的字节数】：调用方拿这个长度
                                              * 去 sbuf_add，估大了就会把缓冲尾巴上的陈旧
                                              * 字节当正文抄进档里（v2.3.1 第一版就这样
                                              * 存出「REDMARK77\0DMARK77」）。 */
}

/* 从最后一行往前数逻辑行起点，最多 max 条，把起点（rel 行号）按正序写进 out[]。
 * 反向数 + 上限 ⇒ 代价与上限成正比，跟 scrollback 有多大无关（50 万行也一样快）。 */
static int sess_collect_starts(ScreenBuffer *s, int *out, int max) {
    int cnt = 0;
    for (int rel = s->rows - 1; rel >= -s->hist_lines && cnt < max; rel--)
        if (!s->line_wrap[screen_phys_row(s, rel)]) out[cnt++] = rel;
    for (int i = 0; i < cnt / 2; i++) {          /* 倒着数出来的，翻正 */
        int a = out[i];
        out[i] = out[cnt - 1 - i];
        out[cnt - 1 - i] = a;
    }
    return cnt;
}

/* 一个窗格的历史 → 若干条 `D ...`。三条剪枝：
 *  - 屏底部那几行空行不是「历史」（新窗格一屏空着，恢复出来只是让人以为上次会话真有
 *    那么多空行）⇒ 记下「最后一条非空行结束的位置」，收尾剪掉尾巴 ⇒ 空历史 = 零条 D
 *    记录 = 恢复时连说明行都不写；
 *  - 上一次恢复时那句说明行是【我们自己写的】，不是用户的历史 ⇒ 别再存回去，
 *    否则每开一次会话就多一行说明，档位会一直长；
 *  - v2.3.1：只有这一行真的发过颜色码，结尾才补 `\e[m` —— 默认色的行一个转义都不加，
 *    否则每行白涨四个字节、还会把「D 记录 = 能直接读的历史」弄脏。 */
static void sess_write_pane(ScreenBuffer *s, SBuf *b) {
    int *starts = (int *)malloc((size_t)(SESS_MAX_LINES + 1) * sizeof(int));
    if (!starts) return;
    int n = sess_collect_starts(s, starts, SESS_MAX_LINES);
    SBuf scr;
    memset(&scr, 0, sizeof(scr));
    int keep = b->len;
    int mlen = (int)strlen(SESS_MARK_HEAD);
    int nlen = (int)strlen(SESS_NOTE_HEAD);
    for (int i = 0; i < n; i++) {
        int to = (i + 1 < n) ? starts[i + 1] - 1 : s->rows - 1;
        int used = 0, head = 1, mark = 0, cut = 0, code = 0;   /* code 逐行重算：上一行的颜色不许替这一行补复位 */
        int line_at = b->len;
        for (int rel = starts[i]; rel <= to && !cut; rel++) {
            int pr = screen_phys_row(s, rel);
            scr.len = 0;                                   /* 复用容量，逐行重装 */
            int r = sess_row_text(s, pr, &scr, SESS_MAX_LINE_BYTES, &code, &cut);
            if (r <= 0) continue;                          /* 空行（或全空白）不进这一条 */
            if (head) {
                head = 0;
                if ((r >= mlen && memcmp(scr.p, SESS_MARK_HEAD, (size_t)mlen) == 0) ||
                    (r >= nlen && memcmp(scr.p, SESS_NOTE_HEAD, (size_t)nlen) == 0)) {
                    mark = 1;
                    break;
                }
                sbuf_str(b, "D ");                        /* 确认这条要留，才动 b */
            }
            if (used + r > SESS_MAX_LINE_BYTES) { cut = 1; continue; }
            sbuf_add(b, scr.p, r);
            used += r;
        }
        if (used > 0 && code) sbuf_str(b, "\\e[m");       /* 收尾复位，颜色不许漏给下一行 */
        if (mark) continue;                                /* 一条都没写出去，无需回滚 */
        if (used > 0) { sbuf_str(b, "\n"); keep = b->len; }
        else if (b->len > line_at) b->len = line_at;      /* 只写了 "D " 就被剪掉 ⇒ 回滚 */
    }
    free(scr.p);
    if (keep < b->len) { b->len = keep; if (b->p) b->p[keep] = 0; }
    free(starts);
}

static void sess_write_tree(SplitNode *nd, int node, SBuf *b, int depth) {
    if (node < 0 || depth > 24 || b->len >= SESS_MAX_BYTES) { sbuf_str(b, "p"); return; }
    if (nd[node].leaf) { sbuf_str(b, "p"); return; }
    sbuf_fmt(b, "%c%d", nd[node].dir == SPLIT_V ? 'v' : 'h', nd[node].frac_pct);
    sess_write_tree(nd, nd[node].a, b, depth + 1);
    sess_write_tree(nd, nd[node].b, b, depth + 1);
}

static void sess_leaf_list(SplitNode *nd, int node, int *out, int *cnt, int cap, int depth) {
    if (node < 0 || *cnt >= cap || depth > 24) return;
    if (nd[node].leaf) { out[(*cnt)++] = nd[node].pane_idx; return; }
    sess_leaf_list(nd, nd[node].a, out, cnt, cap, depth + 1);
    sess_leaf_list(nd, nd[node].b, out, cnt, cap, depth + 1);
}

/* 把【此刻还活着】的窗格与布局排成记录文本。返回 tab 数（0 = 此刻没东西可存）。
 * 「此刻还活着」很关键：窗格的 ScreenBuffer 是在 close_pane() 里 screen_free() 掉的，
 * 也就是说等主循环退出时历史早没了 —— 所以采集必须挂在窗格关闭的那一刻（见 session_note_now），
 * 退出时只做一次落盘。判据用 screen.lines != NULL 而不是 pane.active：reap 里 active 先被清 0，
 * 而那一刻屏还在。 */
static int sess_render(SBuf *b, int *leaves) {   /* b 必须是调用方新清零的 SBuf */
    sbuf_str(b, "s " SESS_VER_TEXT "\n");
    sbuf_fmt(b, "u %lld\n", (long long)time(NULL));

    int tabs = 0;
    if (leaves) *leaves = 0;
    EnterCriticalSection(&g_mux.cs);
    SplitNode *nd = split_nodes();
    for (int i = 0; i < g_mux.pane_count && b->len < SESS_MAX_BYTES; i++) {
        Pane *p = &g_mux.panes[i];
        if (!p->screen.lines || p->is_split_child || p->is_settings || p->is_about) continue;
        int root = split_root_for_tab(i);
        if (root < 0) continue;
        int lvs[MAX_PANES];
        int nl = 0;
        sess_leaf_list(nd, root, lvs, &nl, MAX_PANES, 0);
        if (nl < 1) continue;
        int focus = -1;
        for (int k = 0; k < nl; k++)
            if (lvs[k] == g_mux.active_pane) focus = k;
        sbuf_str(b, "T ");
        sbuf_fmt(b, "%d ", p->color);
        sess_write_tree(nd, root, b, 0);
        sbuf_str(b, "\n");
        for (int k = 0; k < nl; k++) {
            sbuf_str(b, "P ");
            sbuf_fmt(b, "%d\n", (k == focus) ? 1 : 0);
            if (lvs[k] >= 0 && lvs[k] < g_mux.pane_count)
                sess_write_pane(&g_mux.panes[lvs[k]].screen, b);
        }
        tabs++;
    }
    LeaveCriticalSection(&g_mux.cs);
    return tabs;
}

/* ---- 内存里留着「最富」的一份：窗格一个一个关掉时，后一次采集只会更少，
 *      所以只在 tab 数不减少、字节数不减少时才替换（同宽时新的赢：还活着的窗格
 *      可能又多打了几百行）。 ---- */
static SBuf g_sess_keep;
static int g_sess_keep_tabs;
static int g_sess_keep_leaves;
static int g_sess_suppress;        /* 恢复期间别顺手把半成品记进 keep */
static int g_sess_exit_flush;      /* 退出路径上已经存过 ⇒ 钩子那一次不再写盘 */
static int g_sess_flushing;        /* 落盘正在进行 ⇒ 不许重入（信号打在落盘中间） */

/* 先数一遍「现在有几个 tab、几个窗格」（只看结构，不碰文本），便宜到可以每次调用都做。
 * 作用是给下面的采集把关：主循环退出时是【一个窗格一个窗格关过来】的，每次都全量
 * 重排一遍文本就是 O(窗格²)，16 格 ×4000 行能卡住退出；结构已经比留着的那份少了，
 * 内容不可能更「富」⇒ 直接不看文本。 */
static void sess_count_live(int *tabs, int *leaves) {
    SplitNode *nd = split_nodes();
    *tabs = 0;
    *leaves = 0;
    for (int i = 0; i < g_mux.pane_count; i++) {
        Pane *p = &g_mux.panes[i];
        if (!p->screen.lines || p->is_split_child || p->is_settings || p->is_about) continue;
        int root = split_root_for_tab(i);
        if (root < 0) continue;
        int lvs[MAX_PANES], nl = 0;
        sess_leaf_list(nd, root, lvs, &nl, MAX_PANES, 0);
        if (nl < 1) continue;
        (*tabs)++;
        *leaves += nl;
    }
}

void session_note_now(void) {
    if (!g_session_persist || g_sess_suppress) return;
    if (g_sess_keep.p) {
        int tabs, leaves;
        EnterCriticalSection(&g_mux.cs);
        sess_count_live(&tabs, &leaves);
        LeaveCriticalSection(&g_mux.cs);
        if (tabs < g_sess_keep_tabs || leaves < g_sess_keep_leaves) return;
    }
    SBuf b;
    memset(&b, 0, sizeof(b));
    int nl = 0;
    int tabs = sess_render(&b, &nl);
    if (tabs < 1 || b.len <= 0 || !b.p) { free(b.p); return; }
    /* 「更富」= tab 数不少、字节数不少（同分时新的一份赢：还活着的窗格可能又多打了几百行）。 */
    if (g_sess_keep.p &&
        (tabs < g_sess_keep_tabs || b.len < g_sess_keep.len)) { free(b.p); return; }
    free(g_sess_keep.p);
    g_sess_keep = b;
    g_sess_keep_tabs = tabs;
    g_sess_keep_leaves = nl;
}

/* 把内存里那份快照写出去（一次 f*open + 一次 fwrite ⇒ 「退出只写一次盘」这条口径
 * 就看这一支被调用几次）。落盘后 free 掉，所以第二次调用没有东西可写。 */
static int sess_write_keep(void) {
    SBuf *b = &g_sess_keep;
    if (!b->p || b->len <= 0) return 0;

    WCHAR path[MAX_PATH] = { 0 };
    sess_path(path, MAX_PATH, 1);
    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        /* exe 旁边写不了（Program Files 之类）⇒ 退到主目录，和 ini 同一个兜底 */
        const WCHAR *prof = plat_user_home();
        if (prof) {
            WCHAR alt[MAX_PATH] = { 0 };
            _snwprintf(alt, MAX_PATH - 1, L"%s" TERMUX_PATH_SEP_S SESS_HOME_NAME, prof);
            f = _wfopen(alt, L"wb");
        }
    }
    int ok = 0;
    if (f) {
        ok = (fwrite(b->p, 1, (size_t)b->len, f) == (size_t)b->len) ? 1 : 0;
        if (fclose(f) != 0) ok = 0;
    }
    free(g_sess_keep.p);                 /* 落盘后就地松开（别等进程退出，窗格多的时候这份东西不小） */
    g_sess_keep.p = NULL;
    g_sess_keep.len = g_sess_keep.cap = 0;
    g_sess_keep_tabs = g_sess_keep_leaves = 0;
    return ok;
}

int session_save(void) {
    if (!g_session_persist || g_sess_exit_flush) return 0;
    session_note_now();      /* 窗格还活着的话此刻再采一次（多半已是同一份） */
    return sess_write_keep();
}

/* v2.3.1：给「进程马上就没了」的场合用 —— Windows 的控制台关闭事件、POSIX 的
 * SIGTERM/SIGHUP。这两条路都走不到退出钩子：
 *   - Windows 的 ctrl_handler 只做 `running = 0` 就 return TRUE，系统在那之后【立刻】
 *     终止进程（主循环还阻塞在 ReadConsoleInput 里）⇒ 按 X 关窗口时一次盘都没落，
 *     下次启动自然「没有上次会话」；v2.3.0 就是这样，也是用户报的「没有成功恢复」。
 *   - POSIX 的 TERM/HUP 靠主循环退回后那句 session_save()，能存，但要绕一圈；
 *     这里顺手在同一处兜住。
 * for_exit_path = 1 ⇒ 记一笔「这次退出已经存过了」，退出钩子那一次跳过（口径仍是
 *                     【一次写盘】，不能因为多挂了个钩子变成两次）。
 * for_exit_path = 0 ⇒ 只存当下这一份，进程继续跑，退出时照旧再存（POSIX 的 SIGUSR1
 *                     手动存盘用这个）。
 * 拿锁只等有限时间（TryEnter 200 次 ≈ 400ms）：信号/事件可能打在别的线程持锁中间，
 * 死等等于把退出路径挂在锁上。等不到就用上一次采集的那份落盘（宁少不错）。 */
int session_flush_now(int for_exit_path) {
    if (!g_session_persist || g_sess_flushing) return 0;
    g_sess_flushing = 1;
    int locked = 0;
    for (int t = 0; t < 200 && !locked; t++) {
        if (TryEnterCriticalSection(&g_mux.cs)) locked = 1;
        else Sleep(2);
    }
    if (locked) {
        LeaveCriticalSection(&g_mux.cs);
        session_note_now();              /* 它自己会拿锁（可重入），这里先明着放掉 */
    }
    int ok = sess_write_keep();
    if (ok && for_exit_path) g_sess_exit_flush = 1;
    g_sess_flushing = 0;
    return ok;
}

/* =========================================================================
 * 恢复
 * ========================================================================= */

static int sess_default_shell(WCHAR *out, int out_cells) {
    const WCHAR *d;
#ifdef _WIN32
    d = TERMUX_DEFAULT_SHELL_W;
#else
    d = plat_default_shell();
#endif
    if (!d || !d[0]) return 0;
    int i = 0;                       /* 手抄一遍：MSVC 把 wcsncpy 标了 C4996 */
    while (d[i] && i < out_cells - 1) { out[i] = d[i]; i++; }
    out[i] = 0;
    return 1;
}

/* 数树里有几个叶子，顺带验语法（先验再动手，免得半路上创建出一堆空标签页）。
 * 返回叶子数；0 = 语法不对 / 超上限。budget 是「还容得下几个窗格」。 */
static int sess_tree_count(const char **pp, int depth, int budget) {
    if (depth > 24 || budget <= 0) return 0;
    char c = **pp;
    if (c == 'p') { (*pp)++; return 1; }
    if (c != 'v' && c != 'h') return 0;
    (*pp)++;
    int pct = 0, any = 0;
    while (**pp >= '0' && **pp <= '9') { pct = pct * 10 + (**pp - '0'); (*pp)++; any = 1; }
    if (!any || pct < 5 || pct > 95) return 0;
    int a = sess_tree_count(pp, depth + 1, budget);
    if (a <= 0) return 0;
    int b = sess_tree_count(pp, depth + 1, budget - a);
    if (b <= 0) return 0;
    return a + b;
}

/* 按树把 node（当前持有 cur_pane 的叶子）展开；新建出来的窗格依次记进 leaves[]。 */
static int sess_expand(int node, int cur_pane, const char **pp, int *leaves, int *nl,
                       SplitNode *nd) {
    char c = **pp;
    if (c == 'p') {
        (*pp)++;
        if (*nl >= MAX_PANES) return 0;
        leaves[(*nl)++] = cur_pane;
        return 1;
    }
    if (c != 'v' && c != 'h') return 0;
    (*pp)++;
    int pct = 0, any = 0;
    while (**pp >= '0' && **pp <= '9') { pct = pct * 10 + (**pp - '0'); (*pp)++; any = 1; }
    if (!any || pct < 5 || pct > 95) return 0;
    if (g_mux.pane_count >= MAX_PANES) return 0;
    WCHAR sh[256];
    memset(sh, 0, sizeof(sh));
    if (!sess_default_shell(sh, (int)(sizeof(sh) / sizeof(sh[0])))) return 0;
    int np = create_pane_shell(sh);
    if (np < 0) return 0;
    if (split_do(node, (c == 'v') ? SPLIT_V : SPLIT_H, np) < 0) { close_pane(np); return 0; }
    g_mux.panes[np].is_split_child = 1;
    nd[node].frac_pct = pct;
    if (!sess_expand(nd[node].a, cur_pane, pp, leaves, nl, nd)) return 0;
    if (!sess_expand(nd[node].b, np, pp, leaves, nl, nd)) return 0;
    return 1;
}

/* 文件先切成一行一条记录：解析时前进/退回都只是动下标，不用跟游标较劲。 */
typedef struct { char type; const char *body; } SRec;

static int sess_index_lines(char *raw, SRec **out) {
    int cap = 64, n = 0;
    SRec *r = (SRec *)malloc((size_t)cap * sizeof(*r));
    if (!r) { *out = NULL; return -1; }
    for (char *p = raw; *p; ) {
        char *s = p;
        while (*p && *p != '\n') p++;
        char *e = p;
        if (*p == '\n') *p++ = 0;
        while (e > s && e[-1] == '\r') *--e = 0;
        if (e == s) continue;
        const char *body = (const char *)s + 1;
        if (body < e && *body == ' ') body++;
        if (n >= cap) {
            int nc = cap * 2;
            SRec *nr = (SRec *)realloc(r, (size_t)nc * sizeof(*r));
            if (!nr) break;
            r = nr;
            cap = nc;
        }
        r[n].type = s[0];
        r[n].body = body;
        n++;
    }
    *out = r;
    return n;
}

/* 把某个窗格的快照文本灌回屏幕缓冲。锁只在这一句上（create_pane_shell 内部也要拿
 * 锁，见 wincompat.h 里那句「必须可重入」—— 可重入不等于该套着拿）。 */
static void sess_feed(Pane *p, SBuf *hist, int lines) {
    if (!p || !p->active || (lines <= 0 && hist->len <= 0)) return;
    SBuf f;
    memset(&f, 0, sizeof(f));
    sbuf_str(&f, "\x1b[m");                      /* 先清掉残留样式，别把整段历史染色 */
    sbuf_add(&f, hist->p ? hist->p : "", hist->len);
    if (lines > 0) sbuf_fmt(&f, "%s%d%s\r\n", SESS_MARK_HEAD, lines, SESS_MARK_TAIL);
    /* v2.3.3：说明行从「历史前面」挪到「历史末尾」。放前面时，长历史（例：32 行）会把
     * 它顶出视口 —— 用户看到的就是「要能滚动才看得到这行说明」（实测）。挪到末尾之后，
     * 它是恢复出来的最后一行、紧贴 shell 的提示符，第一屏必定看得见；更早的那些行本来就该
     * 在滚动缓冲里（滚轮 / PgUp 回看），这也和本仓库「历史归 scrollback」的口径一致。
     * 注：不用 scroll_offset 去「启动就摊开到历史开头」—— 那条路要跨过 render 的 vo 夹取
     * （screen_scroll_limit）与 switch_pane 的复位，实测会把画面停在半中间，得不偿失。 */
    EnterCriticalSection(&g_mux.cs);
    screen_process_output(&p->screen, f.p, f.len);
    LeaveCriticalSection(&g_mux.cs);
    p->scroll_offset = 0;
}

/* 一行说明，只往当前窗格打（lines=0 ⇒ sess_feed 不会补「上次会话的历史」那行）。 */
static void sess_notice(const char *msg) {
    int ai = g_mux.active_pane;
    if (ai < 0 || ai >= g_mux.pane_count) return;
    SBuf b;
    memset(&b, 0, sizeof(b));
    /* 两头都要换行：此刻游标正停在 shell 已经打出来的那行提示符后面，贴着它写 ⇒ 这行
     * 从第二列才开始，存盘时「按前缀剔掉说明行」那条判据就抓不到它（实测会被当历史
     * 存下去，于是下一轮又多一行）。先 \r\n 起新行、写完再 \r\n 让提示符回到新行。 */
    sbuf_str(&b, "\r\n");
    sbuf_str(&b, msg);
    sbuf_str(&b, "\r\n");
    sess_feed(&g_mux.panes[ai], &b, 0);
    free(b.p);
}

int session_restore(void) {
    if (!g_session_persist) return 0;

    WCHAR path[MAX_PATH] = { 0 };
    /* v2.3.2：不再只认 exe 旁那个固定名字 —— 目录里所有 *.session 与主目录那份一起
     * 比时间，取最新的（换了 exe 文件名就等于换了快照目录，上一版栽在这上面）。 */
    int have = sess_pick_newest(path, MAX_PATH);
    FILE *f = have ? _wfopen(path, L"rb") : NULL;
    int why = have ? 2 : 1;                       /* 1 = 一份快照都没有；2 = 有但读不认 */
    if (!f) {
        sess_notice(why == 1 ? SESS_NOTE_NONE : SESS_NOTE_BAD);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > SESS_MAX_BYTES) { fclose(f); sess_notice(SESS_NOTE_BAD); return 0; }
    char *raw = (char *)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); sess_notice(SESS_NOTE_BAD); return 0; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;
    if (rd == 0) { free(raw); sess_notice(SESS_NOTE_BAD); return 0; }

    /* 从现在起会动窗格（建 tab / 失败回滚都走 close_pane）⇒ 这段时间不许采集，
     * 免得把「恢复到一半」当成一份快照存下去。suppress 必须和这里的每一条 return
     * 一一对应地解开：v2.3.0 第一版把它设在上面对 `done:` 之外的好几个 return 之前，
     * 结果「本次没有快照」就把标志永久卡住，退出时的采集全被吞掉（实测存不出文件）。 */
    g_sess_suppress = 1;
    /* 声明必须全部排在第一条 goto 之前：跳进作用域会绕过初始化（gcc 的
     * -Wmaybe-uninitialized 就是抓这个的），done: 里读到的是栈垃圾。 */
    SRec *recs = NULL;
    int nrec = 0, ri = 0, tabs = 0, focus_pane = -1, ver_esc = 0;
    nrec = sess_index_lines(raw, &recs);
    if (nrec < 1 || !recs) goto done;
    /* 版本：只认 1（纯文本）与 2（带 ANSI 转义）。老档照样恢复，只是没有颜色、
     * 而且正文里的反斜杠不能当转义引导符看。认不出的版本 ⇒ 整档不认。 */
    if (recs[ri].type != 's') goto done;
    if (recs[ri].body[0] != '1' && recs[ri].body[0] != '2') goto done;
    ver_esc = (recs[ri].body[0] >= '2') ? 1 : 0;
    ri++;
    while (ri < nrec) {
        if (recs[ri].type != 'T') { ri++; continue; }             /* u / 认不出的：跳过 */
        const char *tk = recs[ri].body;
        int color = 0;
        while (*tk >= '0' && *tk <= '9') { color = color * 10 + (*tk - '0'); tk++; }
        if (color < 0 || color > 8) color = 0;
        while (*tk == ' ') tk++;
        const char *chk = tk;
        if (sess_tree_count(&chk, 0, MAX_PANES) < 1 || *chk) { ri++; continue; }  /* 树坏了：这个 tab 不要 */
        ri++;

        int anchor;
        if (tabs == 0) {
            anchor = g_mux.active_pane;                            /* 启动时已经建好的那一个 */
            if (anchor < 0 || anchor >= g_mux.pane_count) goto done;
            SplitNode *nd = split_nodes();
            int root0 = split_root_for_tab(anchor);
            if (root0 < 0 || !nd[root0].leaf) goto done;           /* tab 0 不该有分屏 */
        } else {
            if (g_mux.pane_count >= MAX_PANES) goto done;
            WCHAR sh[256];
            memset(sh, 0, sizeof(sh));
            if (!sess_default_shell(sh, (int)(sizeof(sh) / sizeof(sh[0])))) goto done;
            anchor = create_pane_shell(sh);
            if (anchor < 0) goto done;
            split_init_tab(anchor);
        }
        g_mux.panes[anchor].color = color;
        int root = split_root_for_tab(anchor);
        if (root < 0) goto done;

        int leaves[MAX_PANES];
        int nl = 0;
        const char *tc = tk;
        if (!sess_expand(root, anchor, &tc, leaves, &nl, split_nodes()) || nl < 1) goto done;

        for (int k = 0; k < nl && ri < nrec; k++) {
            int flag = 0;
            if (recs[ri].type == 'P') {
                flag = recs[ri].body[0] == '1' ? 1 : 0;
                ri++;
            }
            if (flag) focus_pane = leaves[k];
            SBuf h;
            memset(&h, 0, sizeof(h));
            int lines = 0;
            while (ri < nrec && recs[ri].type == 'D') {
                if (recs[ri].body[0]) { sess_unesc(&h, recs[ri].body, ver_esc); sbuf_str(&h, "\r\n"); }
                else sbuf_str(&h, "\r\n");                        /* 空行也要占一行 */
                lines++;
                ri++;
            }
            sess_feed(&g_mux.panes[leaves[k]], &h, lines);
            free(h.p);
        }
        tabs++;
    }
done:
    g_sess_suppress = 0;
    free(recs);
    free(raw);
    /* 文件在、也打开了，但一条都没恢复出来（版本不认 / 结构坏了 / 空快照）⇒ 同样要
     * 说一声：「有档」和「有档但读不认」在用户那边长成同一个样子，不能靠猜。 */
    if (why && tabs < 1) sess_notice(SESS_NOTE_BAD);
    if (tabs < 1) return 0;
    if (focus_pane >= 0 && focus_pane < g_mux.pane_count) switch_pane(focus_pane);
    g_mux.needs_redraw = 1;
    return tabs;
}
