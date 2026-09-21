#include "framediff.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* 动态字节块                                                                 */
/* ------------------------------------------------------------------------- */

static void chunk_append(FrameChunk *c, const char *p, size_t n) {
    if (n == 0) return;
    if (c->len + n + 1 > c->cap) {
        size_t cap = c->cap > 0 ? c->cap : 256;
        while (cap < c->len + n + 1) {
            if (cap > (size_t)1 << 30) { cap = c->len + n + 1; break; }
            cap *= 2;
        }
        char *np = (char *)realloc(c->data, cap);
        if (!np) return;
        c->data = np;
        c->cap = cap;
    }
    memcpy(c->data + c->len, p, n);
    c->len += n;
    c->data[c->len] = 0;
}

static void chunk_reset(FrameChunk *c) { c->len = 0; if (c->data) c->data[0] = 0; }

static void chunk_free(FrameChunk *c) {
    free(c->data);
    c->data = NULL;
    c->len = c->cap = 0;
}

static int chunk_equal(const FrameChunk *a, const FrameChunk *b) {
    if (a->len != b->len) return 0;
    return a->len == 0 || memcmp(a->data, b->data, a->len) == 0;
}

static void chunk_copy(FrameChunk *dst, const FrameChunk *src) {
    chunk_reset(dst);
    if (src->len > 0) chunk_append(dst, src->data, src->len);
}

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

void framediff_init(FrameDiff *fd) {
    memset(fd, 0, sizeof(*fd));
}

void framediff_free(FrameDiff *fd) {
    chunk_free(&fd->always);
    for (int i = 0; i < fd->rows_cap; i++) chunk_free(&fd->rows[i]);
    for (int i = 0; i < fd->shadow_cap; i++) chunk_free(&fd->shadow[i]);
    free(fd->rows);
    free(fd->shadow);
    free(fd->dirty);
    fd->rows = NULL;
    fd->shadow = NULL;
    fd->dirty = NULL;
    fd->rows_cap = fd->shadow_cap = 0;
    fd->row_count = fd->shadow_count = 0;
}

void framediff_invalidate(FrameDiff *fd) {
    fd->force_all = 1;
}

static FrameChunk *row_chunk(FrameDiff *fd, int row) {
    if (row < 0) return &fd->always;
    if (row >= fd->row_count) return NULL;
    return &fd->rows[row];
}

void framediff_begin_frame(FrameDiff *fd, int host_rows) {
    if (host_rows < 1) host_rows = 1;
    if (host_rows != fd->row_count) {
        /* 终端高度变化：影子尺寸不再匹配，强制整帧。 */
        fd->force_all = 1;
    }
    if (host_rows > fd->rows_cap) {
        int newcap = fd->rows_cap > 0 ? fd->rows_cap : 32;
        while (newcap < host_rows) newcap *= 2;
        FrameChunk *nr = (FrameChunk *)realloc(fd->rows, (size_t)newcap * sizeof(FrameChunk));
        char *nd = (char *)realloc(fd->dirty, (size_t)newcap);
        if (nr) {
            for (int i = fd->rows_cap; i < newcap; i++) memset(&nr[i], 0, sizeof(nr[i]));
            fd->rows = nr;
        }
        if (nd) fd->dirty = nd;
        /* rows 与 dirty 共用 rows_cap 容量，只有两者都扩容成功才提升容量：
         * 若 nr 成功而 nd 失败（OOM），rows_cap 提前提升会让 row_count 超过
         * dirty 旧数组长度，紧接着的 fd->dirty[i]=0 就越界写（ASAN 实测）。
         * 容量不提升时本帧维持旧行数，安全退化，下一帧重试扩容。 */
        if (nr && nd) fd->rows_cap = newcap;
    }
    fd->row_count = (fd->rows_cap >= host_rows) ? host_rows : fd->row_count;
    chunk_reset(&fd->always);
    for (int i = 0; i < fd->row_count; i++) {
        chunk_reset(&fd->rows[i]);
        if (fd->dirty) fd->dirty[i] = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* VT 流扫描：按绝对光标定位（CUP）切成逐行字节块                              */
/* ------------------------------------------------------------------------- */

void framediff_scan(FrameDiff *fd, const char *buf, size_t len) {
    enum { ST_NORMAL = 0, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC } st = ST_NORMAL;
    FrameChunk *cur = &fd->always;
    char csi[64];
    size_t csi_len = 0;
    size_t run_start = 0;   /* 当前普通字节段起点，攒成批写入，避免逐字节 realloc */

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        switch (st) {
            case ST_NORMAL:
                if (ch == 0x1B) {
                    if (i > run_start) chunk_append(cur, buf + run_start, i - run_start);
                    run_start = i + 1;
                    st = ST_ESC;
                }
                break;

            case ST_ESC:
                if (ch == '[') {
                    st = ST_CSI;
                    csi_len = 0;
                    if (csi_len < sizeof(csi)) csi[csi_len++] = (char)0x1B;
                    if (csi_len < sizeof(csi)) csi[csi_len++] = '[';
                } else if (ch == ']') {
                    /* OSC 整段（标题等）归 always，不属于任何一行。 */
                    st = ST_OSC;
                    chunk_append(&fd->always, "\x1b]", 2);
                } else {
                    /* 其它 ESC 序列（ESC c 等）：保留在当前行。 */
                    char pair[2] = { (char)0x1B, (char)ch };
                    chunk_append(cur, pair, 2);
                    run_start = i + 1;
                    st = ST_NORMAL;
                }
                break;

            case ST_CSI: {
                if (csi_len < sizeof(csi) - 1) csi[csi_len++] = (char)ch;
                if (ch >= 0x40 && ch <= 0x7E) {
                    csi[csi_len] = 0;
                    FrameChunk *dst = cur;
                    if (ch == 'H' || ch == 'f') {
                        /* CUP: \x1b[<row>;<col>H，参数缺省为 1。 */
                        long row = 0;
                        int seen = 0;
                        for (size_t k = 2; k < csi_len; k++) {
                            char p = csi[k];
                            if (p >= '0' && p <= '9') { row = row * 10 + (p - '0'); seen = 1; }
                            else break;
                        }
                        long target = (seen ? row : 1) - 1;
                        FrameChunk *rc = row_chunk(fd, (int)target);
                        if (rc) dst = rc;
                        cur = dst;
                    }
                    chunk_append(dst, csi, csi_len);
                    run_start = i + 1;
                    st = ST_NORMAL;
                }
                break;
            }

            case ST_OSC:
                if (ch == 0x07) {
                    chunk_append(&fd->always, "\x07", 1);
                    /* OSC 不属于任何一行；结束后普通字节先归 always，
                     * 直到下一个 CUP 再切到具体行。 */
                    cur = &fd->always;
                    run_start = i + 1;
                    st = ST_NORMAL;
                } else if (ch == 0x1B) {
                    st = ST_OSC_ESC;
                } else {
                    char one = (char)ch;
                    chunk_append(&fd->always, &one, 1);
                }
                break;

            case ST_OSC_ESC:
                if (ch == '\\') {
                    chunk_append(&fd->always, "\x1b\\", 2);
                } else {
                    char one = (char)0x1B;
                    chunk_append(&fd->always, &one, 1);
                    one = (char)ch;
                    chunk_append(&fd->always, &one, 1);
                }
                cur = &fd->always;
                run_start = i + 1;
                st = ST_NORMAL;
                break;
        }
    }
    if (st == ST_NORMAL && len > run_start)
        chunk_append(cur, buf + run_start, len - run_start);
}

/* ------------------------------------------------------------------------- */
/* 与影子比对，产出增量帧                                                      */
/* ------------------------------------------------------------------------- */

size_t framediff_emit(FrameDiff *fd, char *out, size_t out_cap) {
    size_t pos = 0;

    /* 第一次调用（out == NULL）：规划脏行并更新影子；第二次（out != NULL）：
     * 按已标记的脏行输出，不再比对。 */
    int planning = (out == NULL);
    if (planning) {
        /* 影子容量随当前帧数伸缩。 */
        if (fd->row_count > fd->shadow_cap) {
            int newcap = fd->shadow_cap > 0 ? fd->shadow_cap : 32;
            while (newcap < fd->row_count) newcap *= 2;
            FrameChunk *ns = (FrameChunk *)realloc(fd->shadow, (size_t)newcap * sizeof(FrameChunk));
            if (ns) {
                for (int i = fd->shadow_cap; i < newcap; i++) memset(&ns[i], 0, sizeof(ns[i]));
                fd->shadow = ns;
                fd->shadow_cap = newcap;
            }
        }
        for (int i = 0; i < fd->row_count; i++) {
            FrameChunk *r = &fd->rows[i];
            fd->dirty[i] = fd->force_all ||
                           i >= fd->shadow_count ||
                           !chunk_equal(r, &fd->shadow[i]);
        }
    }

    /* always 每帧都发。 */
    if (fd->always.len > 0) {
        if (out && pos + fd->always.len <= out_cap)
            memcpy(out + pos, fd->always.data, fd->always.len);
        pos += fd->always.len;
    }

    for (int i = 0; i < fd->row_count; i++) {
        FrameChunk *r = &fd->rows[i];
        if (!fd->dirty[i] || r->len == 0) continue;
        if (out && r->data && pos + r->len <= out_cap)
            memcpy(out + pos, r->data, r->len);
        pos += r->len;
    }

    if (planning && fd->shadow) {
        for (int i = 0; i < fd->row_count; i++) chunk_copy(&fd->shadow[i], &fd->rows[i]);
        fd->shadow_count = fd->row_count;
    }
    if (planning) fd->force_all = 0;
    return pos;
}

size_t framediff_diff_test(FrameDiff *fd, const char *buf, size_t len,
                           char *out, size_t out_cap) {
    framediff_scan(fd, buf, len);
    size_t need = framediff_emit(fd, NULL, 0);
    if (out && need <= out_cap) framediff_emit(fd, out, need);
    return need;
}
