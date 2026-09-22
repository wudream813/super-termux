#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脏区渲染下的「整帧 vs 增量」终端语义差分回归 (v1.8.14)。

背景
----
framediff 把整帧 VT 流按绝对光标定位（CUP ``ESC[r;cH``）切成「每行一段」，
与上一帧影子逐行 memcmp，没变的行整段不发。这个优化对「行内容」成立，但
渲染器的整帧里有两类字节不是行内容：

1. **帧尾光标序列**（``ESC[r;cH ESC[?25h`` 显光标 / ``ESC[?25l`` 隐光标）。
   它出现在所有内容行之后，framediff 扫描时帧尾那个 CUP 会把它折进「光标
   所在行」的 chunk。当那一行的 *内容* 没变（别处正在刷输出、或滚动时隐藏
   光标、滚回底部重现）时，整段 chunk 与影子相同就 **不发** —— 光标定位
   被吞掉，终端光标停在上一个发出的 CUP（往往落在滚动条列），表现为
   「滚动滚动条 / 刷新时光标位置不对」。修复：渲染器让 framediff 只扫描
   光标序列之前的 body（标签栏 + 内容行 + 弹层），光标序列逐帧 **无条件**
   追加到增量帧末尾。

2. **SGR 颜色跨行携带**（v1.8.13 已修）：渲染器一行第一个 cell 若颜色与
   「整帧顺序的上一行末尾」相同就不发 SGR；但增量帧只发变化行，终端实际
   的 SGR 状态停在上一帧最后发出的行，不重发就串色（colortool 背景丢失）。

本脚本编译 **真实的 src/framediff.c**，配一个最小 VT 终端模拟器，对同一
系列帧跑两条路径：A) 逐帧喂整帧；B) 喂真实 framediff 增量（+ 光标尾策略），
逐格比对屏幕（字符 + 真彩/16 色背景）与光标（位置 + 显隐），任何不一致即
失败。断言：

* 正确形态（行首 SGR 复位 + 光标尾无条件追加）在随机数百帧下逐格/逐光标
  全一致；
* 变异 1：把光标尾折回 body 一起扫描（模拟 v1.8.14 修复前）→ 光标错位，
  必须失败；
* 变异 2：去掉行首 SGR 复位、让颜色哨兵跨整帧携带（模拟 v1.8.13 修复前）
  → 背景格发散，必须失败。
"""

import subprocess
import sys
import tempfile
from pathlib import Path
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 无 -fsanitize）
#   统一收在 tests/hbuild.py；降级时会自己往 stderr 打 [SKIP-SANITIZER]。
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402


ROOT = Path(__file__).resolve().parent

TERM_H = r"""
#ifndef TERM_H
#define TERM_H
#include <string.h>
#include <stdlib.h>
#define MAXROW 96
#define MAXCOL 220
typedef struct { int ch, fr,fg,fb, br,bg,bb, fv,bv, f16,b16; } Cell;
typedef struct {
    Cell g[MAXROW][MAXCOL];
    int rows, cols, crow, ccol, cur_vis;
    int fr,fg,fb,br,bg,bb,fv,bv,f16,b16;
} Term;
static void term_init(Term*t,int r,int c){ memset(t,0,sizeof(*t)); t->rows=r;t->cols=c;t->cur_vis=1;t->f16=7; }
static void term_paint(Term*t,int r,int c,int ch){
    Cell*u=&t->g[r][c]; u->ch=ch;
    if(t->fv){u->fr=t->fr;u->fg=t->fg;u->fb=t->fb;}else u->f16=t->f16; u->fv=t->fv;
    if(t->bv){u->br=t->br;u->bg=t->bg;u->bb=t->bb;}else u->b16=t->b16; u->bv=t->bv;
}
static void term_putc(Term*t,int ch){
    if(t->crow<0||t->crow>=t->rows||t->ccol<0||t->ccol>=t->cols)return;
    term_paint(t,t->crow,t->ccol,ch); t->ccol++; if(t->ccol>=t->cols)t->ccol=t->cols-1;
}
static const int M16[8]={0,4,2,6,1,5,3,7};
static void term_sgr(Term*t,const char*p,int n){
    int args[96],na=0,bl=0; char buf[8];
    for(int i=0;i<=n;i++){
        if(i<n&&p[i]>='0'&&p[i]<='9'){ if(bl<7)buf[bl++]=(char)p[i]; }
        else { if(bl){buf[bl]=0;args[na++]=atoi(buf);bl=0;} }
    }
    if(na==0)args[na++]=0;
    for(int i=0;i<na;i++){ int a=args[i];
        if(a==0){t->fv=t->bv=0;t->f16=7;t->b16=0;}
        else if(a==1||a==4){}
        else if(a==38&&i+1<na&&args[i+1]==2){t->fr=args[i+2];t->fg=args[i+3];t->fb=args[i+4];t->fv=1;i+=4;}
        else if(a==48&&i+1<na&&args[i+1]==2){t->br=args[i+2];t->bg=args[i+3];t->bb=args[i+4];t->bv=1;i+=4;}
        else if(a>=30&&a<=37){t->fv=0;t->f16=M16[a-30];}
        else if(a>=40&&a<=47){t->bv=0;t->b16=M16[a-40];}
        else if(a>=90&&a<=97){t->fv=0;t->f16=8+M16[a-90];}
        else if(a>=100&&a<=107){t->bv=0;t->b16=8+M16[a-100];}
    }
}
static void term_el(Term*t){
    for(int c=t->ccol;c<t->cols;c++){ Cell*u=&t->g[t->crow][c]; u->ch=' ';
        if(t->bv){u->br=t->br;u->bg=t->bg;u->bb=t->bb;}else u->b16=t->b16; u->bv=t->bv;
        if(t->fv){u->fr=t->fr;u->fg=t->fg;u->fb=t->fb;}else u->f16=t->f16; u->fv=t->fv; }
}
static void term_feed(Term*t,const char*s,size_t len){
    size_t i=0;
    while(i<len){ unsigned char ch=(unsigned char)s[i];
        if(ch!=0x1B){term_putc(t,(int)ch);i++;continue;}
        if(i+1>=len){break;} char c2=s[i+1];
        if(c2=='['){ size_t j=i+2; while(j<len&&!((unsigned char)s[j]>=0x40&&(unsigned char)s[j]<=0x7E))j++;
            if(j>=len){break;} char fi=s[j]; const char*b=s+i+2; int bl=(int)(j-(i+2));
            if(fi=='H'||fi=='f'){ int row=0,col=0,k=0,seen=0;
                while(k<bl&&b[k]>='0'&&b[k]<='9'){row=row*10+b[k]-'0';k++;seen=1;}
                if(k<bl&&b[k]==';'){k++;while(k<bl&&b[k]>='0'&&b[k]<='9'){col=col*10+b[k]-'0';k++;}}
                if(!seen){row=1;} if(col==0){col=1;} t->crow=row-1;t->ccol=col-1;
                if(t->crow<0){t->crow=0;} if(t->ccol<0){t->ccol=0;}
            } else if(fi=='m') term_sgr(t,b,bl);
            else if(fi=='K') term_el(t);
            else if(fi=='h'||fi=='l'){ int h25=0; for(int k=0;k+1<bl;k++) if(b[k]=='2'&&b[k+1]=='5')h25=1; if(h25)t->cur_vis=(fi=='h'); }
            i=j+1;
        } else if(c2==']'){ size_t j=i+2; while(j<len&&s[j]!=0x07){ if(s[j]==0x1B){j++;if(j<len&&s[j]=='\\'){j++;break;}} j++; } i=(j<len)?j+1:len; }
        else i+=2;
    }
}
static int term_equal(const Term*a,const Term*b,int*pr,int*pc,const char**what){
    for(int r=0;r<a->rows;r++)for(int c=0;c<a->cols;c++){ const Cell*x=&a->g[r][c],*y=&b->g[r][c];
        if(x->ch!=y->ch){*pr=r;*pc=c;*what="ch";return 0;}
        if(x->bv!=y->bv){*pr=r;*pc=c;*what="bg-valid";return 0;}
        if(x->bv&&(x->br!=y->br||x->bg!=y->bg||x->bb!=y->bb)){*pr=r;*pc=c;*what="bg-truecolor";return 0;}
        if(!x->bv&&x->b16!=y->b16){*pr=r;*pc=c;*what="bg-16";return 0;}
        if(x->fv!=y->fv){*pr=r;*pc=c;*what="fg-valid";return 0;}
        if(x->fv&&(x->fr!=y->fr||x->fg!=y->fg||x->fb!=y->fb)){*pr=r;*pc=c;*what="fg-truecolor";return 0;}
        if(!x->fv&&x->f16!=y->f16){*pr=r;*pc=c;*what="fg-16";return 0;}
    }
    return 1;
}
#endif
"""

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "framediff.h"
#include "term.h"

#define RR 12
#define CC 30
#define TOTAL (RR+1)

typedef struct { int ch; int br,bg,bb,bv; } CellSpec;
typedef struct { CellSpec row[RR][CC]; int cur_y, cur_x; } Scene;

/* body：OSC + 内容行（行首 SGR 复位与否由 reset_mode 决定）+ 滚动条 +
 * ESC[0m ESC[1;1H 标签栏。不含帧尾光标序列。返回 body 长度。
 * reset_mode=1：每行首 ESC[0m 且哨兵每行重置（v1.8.13）；
 * reset_mode=0：哨兵跨整帧携带、行首不复位（v1.8.13 前）。 */
static size_t emit_body(char*out,size_t cap,const Scene*sc,int reset_mode){
    static int ls,lbr,lbg,lbb; static int inited=0;
    if(!inited){ls=0;lbr=lbg=lbb=0;inited=1;}
    size_t pos=0;
    pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b]0;colortool\x07");
    for(int y=0;y<RR;y++){
        pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[%d;1H",y+2);
        if(reset_mode){ pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[0m"); ls=0; }
        for(int x=0;x<CC-1;x++){
            const CellSpec*c=&sc->row[y][x];
            if(c->ch==0)continue;
            int same = c->bv ? (ls==1&&lbr==c->br&&lbg==c->bg&&lbb==c->bb) : (ls==0);
            if(!same){
                if(c->bv){ pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[0;48;2;%d;%d;%dm",c->br,c->bg,c->bb); ls=1; lbr=c->br;lbg=c->bg;lbb=c->bb; }
                else { pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[0m"); ls=0; }
            }
            out[pos++]=(char)c->ch;
        }
        int thumb=(y>=3&&y<=5);
        pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[%d;%dH",y+2,CC);
        if(thumb) pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[0;48;2;180;190;220m \x1b[0m");
        else pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[0;48;2;40;44;60m\x1b[38;2;120;130;160m\xe2\x94\x82\x1b[0m");
        ls=0; /* 滚动条末自带 ESC[0m */
    }
    pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[0m\x1b[1;1H");
    pos+=(size_t)snprintf(out+pos,cap-pos,"\x1b[0;48;2;20;25;35m\x1b[38;2;200;210;230m colortool  tab1  ");
    out[pos]=0; return pos;
}
static size_t emit_cursor(char*out,size_t cap,const Scene*sc){
    if(sc->cur_y<0) return (size_t)snprintf(out,cap,"\x1b[?25l");
    return (size_t)snprintf(out,cap,"\x1b[%d;%dH\x1b[?25h",sc->cur_y+2,sc->cur_x+1);
}

static unsigned long RNG;
static int rnd(int n){ RNG=RNG*1103515245u+12345u; return (int)((RNG>>16)%(unsigned)n); }

/* 跑 FRAMES 帧随机场景。
 *   reset_mode   : 1 = 行首 SGR 复位；0 = 跨帧携带
 *   cursor_split : 1 = 光标尾无条件追加（修复）；0 = 光标尾折回 body 一起 scan（bug）
 * 返回不一致帧计数（格不一致 或 光标位置/显隐不一致）。 */
static int run(int reset_mode,int cursor_split,unsigned long seed,int*first){
    RNG=seed;
    Term full,diff; FrameDiff fd;
    term_init(&full,TOTAL,CC); term_init(&diff,TOTAL,CC); framediff_init(&fd);
    Scene sc; memset(&sc,0,sizeof(sc));
    static char body[1<<16], cur[64], dlt[1<<17], whole[1<<17];
    int bad=0;
    for(int f=1;f<=240;f++){
        int mode=f%9;
        if(mode<4){ /* colortool 式刷色块：随机格随机背景/默认色 */
            for(int k=0;k<5;k++){ int y=rnd(RR), x=1+rnd(CC-3);
                if(rnd(3)){ sc.row[y][x].ch='#'; sc.row[y][x].bv=1; sc.row[y][x].br=rnd(256);sc.row[y][x].bg=rnd(140);sc.row[y][x].bb=rnd(256); }
                else { sc.row[y][x].ch='.'; sc.row[y][x].bv=0; }
            }
            sc.cur_y=8; sc.cur_x=5;
        } else if(mode==5){ sc.cur_y=-1; }               /* 滚动：隐藏光标 */
        else if(mode==6){ sc.cur_y=8; sc.cur_x=5; }       /* 滚回底部：重现 */
        else { if(rnd(2)&&sc.cur_y>=0){ sc.cur_x++; if(sc.cur_x>=CC-3)sc.cur_x=5; } }

        size_t bl=emit_body(body,sizeof(body),&sc,reset_mode);
        size_t cl=emit_cursor(cur,sizeof(cur),&sc);
        /* 路径 A：整帧 */
        term_feed(&full,body,bl); term_feed(&full,cur,cl);
        /* 路径 B：framediff 增量 */
        size_t scan_len;
        if(cursor_split){ framediff_begin_frame(&fd,TOTAL); framediff_scan(&fd,body,bl); scan_len=bl; }
        else { memcpy(whole,body,bl); memcpy(whole+bl,cur,cl); framediff_begin_frame(&fd,TOTAL); framediff_scan(&fd,whole,bl+cl); scan_len=bl+cl; }
        framediff_emit(&fd,NULL,0);
        size_t n=framediff_emit(&fd,dlt,sizeof(dlt));
        if(cursor_split){ memcpy(dlt+n,cur,cl); n+=cl; }
        term_feed(&diff,dlt,n);
        (void)scan_len;

        int pr,pc; const char*what=NULL;
        int cell_bad=!term_equal(&full,&diff,&pr,&pc,&what);
        int cur_bad=(full.cur_vis!=diff.cur_vis)||
                    (full.cur_vis&&diff.cur_vis&&(full.crow!=diff.crow||full.ccol!=diff.ccol));
        if(cell_bad||cur_bad){
            if(*first==0){
                if(cell_bad) printf("    帧%d 格不一致 r%d c%d: %s (full bv=%d %d,%d,%d | diff bv=%d %d,%d,%d)\n",
                    f,pr,pc,what, full.g[pr][pc].bv,full.g[pr][pc].br,full.g[pr][pc].bg,full.g[pr][pc].bb,
                    diff.g[pr][pc].bv,diff.g[pr][pc].br,diff.g[pr][pc].bg,diff.g[pr][pc].bb);
                else printf("    帧%d 光标不一致: full(vis=%d r%d c%d) diff(vis=%d r%d c%d)\n",
                    f,full.cur_vis,full.crow,full.ccol,diff.cur_vis,diff.crow,diff.ccol);
                *first=1;
            }
            bad++;
        }
    }
    framediff_free(&fd);
    return bad;
}

int main(void){
    int f;
    /* 1) 修复形态：行首复位 + 光标尾拆分 —— 必须 0 不一致 */
    int good=0; f=0;
    for(unsigned long s=1;s<=6;s++) good+=run(1,1,s*977,&f);
    printf("修复形态(行首复位+光标尾无条件追加): %d 不一致帧\n", good);
    /* 2) 变异：光标尾折回 body（v1.8.14 前）—— 必须有光标不一致 */
    int mut_cur=0; f=0;
    for(unsigned long s=1;s<=6;s++) mut_cur+=run(1,0,s*977,&f);
    printf("变异(光标尾折进行 chunk):         %d 不一致帧\n", mut_cur);
    /* 3) 变异：去掉行首 SGR 复位（v1.8.13 前）—— 必须有背景格不一致 */
    int mut_sgr=0; f=0;
    for(unsigned long s=1;s<=6;s++) mut_sgr+=run(0,1,s*977,&f);
    printf("变异(去掉行首 SGR 复位):           %d 不一致帧\n", mut_sgr);

    int ok = (good==0 && mut_cur>0 && mut_sgr>0);
    if(ok) printf("[OK] 整帧 vs framediff 增量终端语义差分通过；两处变异均被捕获。\n");
    else printf("[FAIL] good=%d mut_cur=%d mut_sgr=%d\n", good, mut_cur, mut_sgr);
    return ok?0:1;
}
"""


def main():
    print("=== 脏区光标/颜色终端语义差分 (verify_dirty_cursor.py) ===")
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "term.h").write_text(TERM_H, encoding="utf-8")
        (td / "harness.c").write_text(HARNESS, encoding="utf-8")
        exe = td / "t"
        cmd = ["gcc", "-O1", "-g", *hbuild.sanitize_flags(),
               "-Wall", "-Wextra", "-Werror",
               "-I", str(ROOT / "include"), "-I", str(td),
               str(ROOT / "src" / "framediff.c"),
               str(td / "harness.c"), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            sys.exit("FAIL: compile error")
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout)
        if r.returncode != 0:
            print(r.stderr)
            sys.exit(1)

    # 源码不变量：渲染器必须把光标序列与 framediff 差分的 body 分开（cursor_pos 切分）。
    print("--- 源码不变量: 光标尾与帧差 body 分离 (render.c) ---")
    src = (ROOT / "src" / "render.c").read_text(encoding="utf-8")
    assert "int cursor_pos = pos;" in src, "render.c: 缺少光标段起点 cursor_pos"
    assert "framediff_scan(&g_frame_diff, out, (size_t)cursor_pos);" in src, (
        "render.c: framediff 必须只扫描光标序列之前的 body（out, cursor_pos），"
        "否则帧尾光标序列会被折进行 chunk 吞掉")
    assert "memcpy(g_diff_buf + blen, out + cursor_pos, cursor_len)" in src, (
        "render.c: 光标序列必须无条件追加到增量帧末尾")
    assert 'pos += snprintf(out + pos, bs - pos, "\\x1b[0m");' in src, (
        "render.c: 行首 SGR 复位缺失（v1.8.13 背景修复）")
    print("[OK] render.c 光标尾分离 + 行首 SGR 复位在位。")

    # BUG-10 分支覆盖：body 里每个模态弹层分支，帧尾光标段都必须有对应处理，
    # 否则缺失的模式会掉进 active_pane 终端分支发 ?25h，把弹窗本应隐藏的光标
    # 重新点亮（退出确认弹窗上游离光标）。
    print("--- 源码不变量: body 弹层模式 ⊆ 帧尾光标分支 (BUG-10) ---")
    body_i = src.index("if (g_mux.confirm_exit_mode) {")
    body_seg = src[body_i:src.index("pos += snprintf(out + pos, bs - pos, \"\\x1b[0m\\x1b[1;1H\");", body_i)]
    cur_i = src.index("int cursor_pos = pos;")
    # 光标段到 framediff_scan 调用为止（之后是差分/写出逻辑）。
    cur_end = src.index("framediff_scan(&g_frame_diff, out, (size_t)cursor_pos);", cur_i)
    cur_seg = src[cur_i:cur_end]

    def modes_in(seg):
        out = set()
        for m in ["g_mux.confirm_exit_mode", "g_mux.confirm_close_mode",
                  "g_search_mode", "g_mux.palette_mode",
                  "g_mux.chooser_mode", "g_mux.ctx_mode", "g_mux.rename_mode",
                  "g_mux.custom_cmd_mode", "g_copy_mode"]:
            if m in seg:
                out.add(m)
        return out

    body_modes = modes_in(body_seg)
    cursor_modes = modes_in(cur_seg)
    # chooser/ctx/help 共用帧尾的藏光标分支（?25l），ctx_mode 只要出现即可。
    missing = body_modes - cursor_modes
    assert not missing, (
        "render.c: 帧尾光标段漏了 body 弹层模式 %s —— 这些弹窗会掉进终端 "
        "active_pane 分支发出 ?25h，弹窗上出现游离光标（BUG-10）" % sorted(missing))
    # 确认弹窗（退出 confirm_exit / 关闭窗格 confirm_close）必须显式藏光标（不能
    # 只靠兜底），且 body 里的弹窗渲染函数内不再各自发 ?25l（会被脏区按行差分吞掉）。
    conf_marker = "if (g_mux.confirm_exit_mode || g_mux.confirm_close_mode)"
    assert conf_marker in cur_seg, "帧尾光标段缺 confirm_exit/confirm_close 藏光标分支"
    conf_branch = cur_seg[cur_seg.index(conf_marker):cur_seg.index("else if", cur_seg.index(conf_marker))]
    assert "?25l" in conf_branch, "帧尾确认弹窗分支必须发 ?25l 隐藏光标"
    rc_start = src.index("void render_confirm_dialog(")
    rc_end = src.index("void render_confirm_exit(", rc_start)
    rc_body = src[rc_start:rc_end]
    # 只看实际输出语句（snprintf 里的 ?25l），注释里提到不算。
    rc_emits_hide = any("?25l" in line and "snprintf" in line for line in rc_body.splitlines())
    assert not rc_emits_hide, (
        "render_confirm_exit() 内部不应再 snprintf 发 ?25l：它在 body 里会被脏区"
        "按行差分跳过，光标显隐统一由帧尾光标段处理")
    print("[OK] body 弹层模式 %s 在帧尾光标段都有对应分支；confirm_exit 帧尾藏光标、"
          "body 内不发 ?25l。" % sorted(body_modes))


if __name__ == "__main__":
    main()
