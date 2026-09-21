#ifndef WIN_TERMUX_CLIPHTML_H
#define WIN_TERMUX_CLIPHTML_H

/* ---------------------------------------------------------------------------
 * 复制时保留颜色：构造 Windows 剪贴板 "HTML Format" 载荷。
 *
 * 这个模块不依赖 Win32（只用标准 C），所以可以在 Linux 侧直接编译跑
 * ASAN 回归。input.c 负责从屏幕取 cell、调 RegisterClipboardFormat /
 * SetClipboardData；HTML 的全部字节在这里生成。
 * ------------------------------------------------------------------------- */

#include <stddef.h>

typedef struct {
    unsigned short ch;      /* UTF-16 单元；0 = 空 cell（按空格处理） */
    unsigned char r, g, b;  /* 前景 RGB（fg_valid 时有效） */
    unsigned char br, bg, bb; /* 背景 RGB（bg_valid 时有效） */
    unsigned char fg_valid;
    unsigned char bg_valid;
    unsigned char bold;
    unsigned char underline;
    unsigned char skip;     /* 1 = 宽字符（中文/全角/emoji BMP 占位）的次格，输出时跳过 */
} ClipHtmlCell;

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} ClipHtmlBuf;

void cliphtml_init(ClipHtmlBuf *b);
void cliphtml_free(ClipHtmlBuf *b);

/* 片段构建：begin 一次，每行先 frag_break 再 frag_row（首行除外），最后 finalize。
 *
 * ★ 契约：调用 cliphtml_frag_begin() 之前【必须】先 cliphtml_init()。
 *   frag_begin 直接就往缓冲区里写，它不负责初始化；漏了 init 的话 b->data 是
 *   调用方栈上的垃圾值，buf_reserve 会去 realloc 那个指针 —— Linux 上新栈页
 *   恰好是 0（realloc(NULL,..) 等价 malloc）所以看不出来，换个平台就是
 *   "attempting free on address which was not malloc()-ed"。 */
void cliphtml_frag_begin(ClipHtmlBuf *b);
void cliphtml_frag_break(ClipHtmlBuf *b);   /* 行间换行（<pre> 内即换行） */
/* 追加一行；cells[x0..x1] 为有效区间，x1 < x0 表示空行。同色相邻 cell 自动
 * 合并成一个 <span>；UTF-16 代理对合成 4 字节 UTF-8；特殊字符转义。 */
void cliphtml_frag_row(ClipHtmlBuf *b, const ClipHtmlCell *cells, int x0, int x1);

/* 用 HTML Format 头/尾包裹片段并补 4 个偏移量。成功返回 1，
 * 之后 b->data / b->len 就是可直接 SetClipboardData 的 UTF-8 载荷。 */
int  cliphtml_finalize(ClipHtmlBuf *b);

/* console attr 颜色 nibble（0..15，Windows 控制台 BGR 顺序）→
 * Windows Terminal 默认 Campbell 调色板 RGB。 */
void cliphtml_palette16(int idx, int *r, int *g, int *b);

#endif /* WIN_TERMUX_CLIPHTML_H */
