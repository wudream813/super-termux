#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_split.py — 分屏树 / 纯布局回归（v1.8.33）。

分屏树（src/split.c）是纯逻辑（不碰 Win32/g_mux 的布局部分）：叶子 pane 经
split_do 原地变成内部节点（左右 SPLIT_V / 上下 SPLIT_H），split_layout 按外接
矩形递归算出每个叶子 pane 的 PaneRect，中间留 1 行/列边框。

本脚本用 tests/stub 的 windows.h 替身编译【真实 src/split.c】+ 测试驱动，断言：
  * 单叶子 / 切分后叶子数；
  * 左右切分：两 pane 宽之和 + 1 边框 = 外接宽、等高、右 pane 起点 = 左宽+1；
  * 二次（上下）切分：右侧 pane 占满高、左上下高之和 + 1 边框 = 外接高；
  * 邻接导航（重叠优先）：下邻/右邻正确，边界方向返回自身；
  * Tab 视觉次序循环覆盖所有 pane；
  * 关闭一个叶子后树收缩、剩余 pane 布局正确；
  * 拖分隔线（resize）改变对应 frac_pct。
变异：把 SPLIT_V 边框从 1 列改成 0（layout 不留边框），宽度断言立即失败。
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
INC = os.path.join(ROOT, "include")
STUB = os.path.join(ROOT, "tests", "stub")

# split.c 的高层操作会引用 g_mux / pane.h（Win32 类型），本测试只覆盖纯布局与
# 节点池（split_reset/split_new_leaf/split_do/split_remove_leaf/split_layout/
# neighbor/next/resize_set_frac 之前的部分）。因此测试驱动直接用节点池 + 纯
# split_layout，不链接 pane.c。
HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "split.h"
#include "types.h"

static int failures = 0;
static void ck(const char *n, int cond) {
    if (!cond) { printf("[FAIL] %s\n", n); failures++; }
    else       printf("[ok]   %s\n", n);
}

int main(void) {
    split_reset();
    SplitNode *N = split_nodes();

    int r0 = split_new_leaf(0);
    ck("单叶子根", r0 == 0);
    ck("叶子数=1", split_count_leaves(r0) == 1);
    ck("first leaf pane=0", split_first_leaf(r0) == 0);

    int p1 = split_do(r0, SPLIT_V, 1);
    ck("垂直切分原地变内部(根不变)", p1 == r0);
    ck("根变内部节点", N[p1].leaf == 0);
    ck("叶子数=2", split_count_leaves(p1) == 2);
    ck("pane0/pane1 在树中", split_find_leaf(p1,0)>=0 && split_find_leaf(p1,1)>=0);

    PaneRect rects[16];
    memset(rects, 0, sizeof(rects));
    split_layout(p1, 0, 0, 80, 24, N, rects);
    /* 位置/尺寸守恒按【外接分配矩形】（ocols/orows/oc0/or0）：分隔线在边界。 */
    ck("左 pane 外接原点(0,0)", rects[0].oc0==0 && rects[0].or0==0);
    ck("右 pane 外接起点=左外接宽+1", rects[1].oc0 == rects[0].ocols + 1);
    ck("左右外接宽+边框=80", rects[0].ocols + 1 + rects[1].ocols == 80);
    ck("左右外接等高24", rects[0].orows==24 && rects[1].orows==24);
    ck("两 pane valid", rects[0].valid && rects[1].valid);
    /* v1.8.41：窗格内容矩形与外接分配矩形一致（窗格之间不留空白，需求改在
     * 标签栏的 [标题 ×] 里加空格，不是窗格间）。 */
    ck("内容矩形=外接矩形(无内缩)", rects[0].c0==rects[0].oc0 && rects[0].r0==rects[0].or0 &&
       rects[0].cols==rects[0].ocols && rects[0].rows==rects[0].orows &&
       rects[1].c0==rects[1].oc0 && rects[1].cols==rects[1].ocols);
    ck("分隔线列=两窗格外接交界", rects[0].oc0+rects[0].ocols+1 == rects[1].oc0);

    int leaf0 = split_find_leaf(p1, 0);
    int p2 = split_do(leaf0, SPLIT_H, 2);
    ck("二次(上下)切分成功", p2 >= 0);
    ck("叶子数=3", split_count_leaves(p1) == 3);
    memset(rects, 0, sizeof(rects));
    split_layout(p1, 0, 0, 80, 24, N, rects);
    ck("3 pane valid", rects[0].valid && rects[1].valid && rects[2].valid);
    ck("右 pane 外接占满高24", rects[1].orows == 24);
    ck("左上下外接高+边框=24", rects[0].orows + 1 + rects[2].orows == 24);
    ck("左上左下外接同宽", rects[0].ocols == rects[2].ocols);
    ck("上下分隔行间=外接交界", rects[0].or0+rects[0].orows+1 == rects[2].or0);
    ck("左上内容=外接(无内缩)", rects[0].r0==rects[0].or0 && rects[0].c0==rects[0].oc0);

    ck("pane2(左下) 右邻=pane1(右)", split_neighbor_pane(p1,2,'R')==1);
    ck("pane0(左上) 下邻=pane2(左下)", split_neighbor_pane(p1,0,'D')==2);
    ck("pane1 左邻 在左列", split_neighbor_pane(p1,1,'L')!=1);
    ck("pane0 上邻=自身(边界)", split_neighbor_pane(p1,0,'U')==0);
    ck("pane1 右邻=自身(边界)", split_neighbor_pane(p1,1,'R')==1);

    int o0 = split_first_leaf(p1);
    int o1 = split_next_pane(p1,o0,1);
    int o2 = split_next_pane(p1,o1,1);
    int back = split_next_pane(p1,o2,1);
    ck("Tab 循环回起点", back==o0);
    ck("Tab 覆盖3个不同 pane", o0!=o1 && o1!=o2 && o0!=o2);
    ck("Tab 反向回退", split_next_pane(p1,o1,0)==o0);

    int leaf2 = split_find_leaf(p1, 2);
    int rem=-1;
    int root_after = split_remove_leaf(p1, leaf2, &rem);
    ck("关闭回填 pane=2", rem==2);
    ck("关闭后叶子数=2", split_count_leaves(root_after)==2);
    ck("pane2 已移除", split_find_leaf(root_after,2)<0);
    memset(rects,0,sizeof(rects));
    split_layout(root_after,0,0,80,24,N,rects);
    ck("关闭后 pane0 外接占满左列高24", rects[0].valid && rects[0].orows==24);

    int before = N[root_after].frac_pct;
    split_resize_pane(root_after, 0, 'R', 5);
    ck("向右扩 a 占比+5", N[root_after].frac_pct == before+5);
    split_resize_pane(root_after, 1, 'L', 5);   /* pane1 在 b，向左扩也增大 a */
    ck("b 向左扩 a 占比再+5", N[root_after].frac_pct == before+10);
    split_resize_set_frac(root_after, 0, 'V', 70);
    ck("set_frac 直接设 70", N[root_after].frac_pct == 70);
    split_resize_set_frac(root_after, 1, 'V', 70);  /* pane1 在 b：a 占比=30 */
    ck("set_frac 从 b 侧设 a=100-70", N[root_after].frac_pct == 30);

    /* ---- 高层：split_remove_pane（统一摘除 / 锚点提升） ---- */
    /* 新建一棵以 pane10 为锚点的标签树，切两刀得到 3 个叶子。 */
    split_reset();
    int anchor = 10;
    split_init_tab(anchor);
    g_mux.pane_count = 13;
    for (int i = 0; i < 13; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
    int ta = split_root_for_tab(anchor);
    int ta_leaf_a = split_find_leaf(ta, anchor);
    split_do(ta_leaf_a, SPLIT_V, 11);                 /* 左=10, 右=11 */
    g_mux.panes[11].is_split_child = 1;
    int leaf11 = split_find_leaf(split_root_for_tab(anchor), 11);
    split_do(leaf11, SPLIT_H, 12);                   /* 右侧再上下：右上=11, 右下=12 */
    g_mux.panes[12].is_split_child = 1;
    ck("锚点树叶子数=3", split_count_leaves(split_root_for_tab(anchor)) == 3);

    /* v1.8.38：标签栏据 split_tab_panes 把一个分屏标签画成连排 [pane×] 段，
     * 返回视觉左->右/上->下次序的存活 pane；无树时只返回锚点自己。 */
    {
        int out[16], gn;
        gn = split_tab_panes(anchor, out, 16);
        ck("tab_panes 3 窗格返回 3", gn == 3);
        ck("tab_panes 视觉次序 10,11,12", gn == 3 && out[0] == 10 && out[1] == 11 && out[2] == 12);
        /* 关掉 12 后（置 active=0 模拟已死）枚举只剩存活窗格。 */
        g_mux.panes[12].active = 0;
        gn = split_tab_panes(anchor, out, 16);
        ck("tab_panes 死掉的 12 被过滤", gn == 2 && out[0] == 10 && out[1] == 11);
        g_mux.panes[12].active = 1;
        /* max 截断。 */
        gn = split_tab_panes(anchor, out, 2);
        ck("tab_panes 受 max 截断", gn == 2 && out[0] == 10 && out[1] == 11);
        /* 无分屏树的 pane：单独返回它自己。 */
        gn = split_tab_panes(7, out, 16);
        ck("tab_panes 无树只返回锚点", gn == 1 && out[0] == 7);
    }

    /* 关掉一个非锚点子窗格(12)：树收缩为 2 叶子，锚点仍是 10。 */
    int surv12 = -1;
    ck("remove 子窗格12 返回1", split_remove_pane(12, &surv12) == 1);
    ck("remove 12 后叶子数=2", split_count_leaves(split_root_for_tab(10)) == 2);
    ck("remove 12 后 12 不在树中", split_find_leaf(split_root_for_tab(10), 12) < 0);
    ck("remove 12 后锚点仍=10", split_tab_of_pane(10) == 10);

    /* 关掉锚点 pane(10)：存活兄弟应被提升为新锚点（is_split_child 清 0）。 */
    int surv10 = -1;
    ck("remove 锚点10 返回1", split_remove_pane(10, &surv10) == 1);
    ck("remove 锚点后有存活兄弟", surv10 == 11);
    ck("兄弟11 被提升为新锚点", split_tab_of_pane(11) == 11);
    ck("新锚点 is_split_child 已清0", g_mux.panes[11].is_split_child == 0);
    ck("新锚点树叶子数=1", split_count_leaves(split_root_for_tab(11)) == 1);

    /* 单叶子（独立标签页）摘除返回 0（走整 tab 关闭流程）。 */
    split_init_tab(20);
    int surv20 = -1;
    ck("单叶子摘除返回0", split_remove_pane(20, &surv20) == 0);

    /* ---- 运行时模拟：连续 3 次分屏（一个标签里开 3 个窗格） ---- */
    split_reset();
    g_mux.pane_count = 8;
    for (int i = 0; i < 8; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
    g_mux.active_pane = 0;
    g_mux.host_cols = 120; g_mux.host_rows = 30;   /* v1.8.43：split_split_active 空间检查用 */
    split_init_tab(0);
    /* 第一次：0 -> 0|1，焦点到新窗格 1。 */
    ck("第1次分屏成功", split_split_active(SPLIT_V, 1) == 1);
    ck("第1次后焦点在1", g_mux.active_pane == 1);
    ck("第1次后 1 是子窗格", g_mux.panes[1].is_split_child == 1);
    /* 第二次：在焦点窗格 1 上再切，-> 0 | 1|2，焦点到 2。 */
    ck("第2次分屏成功", split_split_active(SPLIT_V, 2) == 1);
    ck("第2次后焦点在2", g_mux.active_pane == 2);
    ck("第2次后 2 是子窗格", g_mux.panes[2].is_split_child == 1);
    /* 第三次：在焦点窗格 2 上再切，-> 0 | 1 | 2|3，焦点到 3。 */
    ck("第3次分屏成功", split_split_active(SPLIT_V, 3) == 1);
    ck("第3次后焦点在3", g_mux.active_pane == 3);
    ck("第3次后 3 是子窗格", g_mux.panes[3].is_split_child == 1);
    /* 同一棵树、4 个叶子、锚点仍是 0。 */
    ck("3次分屏后同树叶子数=4", split_count_leaves(split_root_for_tab(0)) == 4);
    {
        int out[16], gn = split_tab_panes(0, out, 16);
        ck("3次分屏 tab_panes=4 且都在一个标签", gn == 4 && out[0]==0 && out[1]==1 && out[2]==2 && out[3]==3);
        PaneRect rs[MAX_PANES]; memset(rs, 0, sizeof(rs));
        split_layout(split_root_for_tab(0), 0, 0, 120, 30, split_nodes(), rs);
        ck("4 个窗格都有有效矩形", rs[0].valid && rs[1].valid && rs[2].valid && rs[3].valid);
        ck("窗格外接宽之和+边框=总宽", rs[0].ocols+rs[1].ocols+rs[2].ocols+rs[3].ocols+3 == 120);
        ck("内容=外接(无内缩)", rs[0].cols==rs[0].ocols && rs[3].c0==rs[3].oc0 && rs[0].c0==0);
    }

    /* ---- v1.8.43：窗格太小再分割应拒绝（返回 0，不产生新窗格） ---- */
    {
        split_reset();
        g_mux.pane_count = 8;
        for (int i = 0; i < 8; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
        g_mux.active_pane = 0;
        split_init_tab(0);
        /* 终端很窄（6 列）：左右切分（需 2*4+1=9 列）应失败。 */
        g_mux.host_cols = 6; g_mux.host_rows = 30;
        ck("窄到放不下左右切分 -> 拒绝", split_split_active(SPLIT_V, 1) == 0);
        ck("拒绝后 1 不是子窗格", g_mux.panes[1].is_split_child == 0);
        /* 终端很矮（3 行）：上下切分（需 2*2+1=5 行）应失败。 */
        g_mux.host_cols = 80; g_mux.host_rows = 3;
        ck("矮到放不下上下切分 -> 拒绝", split_split_active(SPLIT_H, 1) == 0);
        /* 空间足够时正常切分。 */
        g_mux.host_cols = 80; g_mux.host_rows = 24;
        ck("空间足够 -> 左右切分成功", split_split_active(SPLIT_V, 1) == 1);
    }

    /* ---- bug #25（2026-09-20）：「太矮的时候，不应该可以拆分」。
     *      SPLIT_MIN_ROWS 从 2 抬到 3，上下切分的门槛从 2*2+1=5 行变成 2*3+1=7 行。
     *      原来 5 行的终端就能上下切，切完每格 2 行，基本没法用。 ---- */
    {
        split_reset();
        g_mux.pane_count = 8;
        for (int i = 0; i < 8; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
        g_mux.active_pane = 0;
        split_init_tab(0);
        g_mux.host_cols = 80;
        g_mux.host_rows = 5;
        ck("5 行：旧门槛(5)会放行，新门槛(7)必须拒绝", split_split_active(SPLIT_H, 1) == 0);
        ck("拒绝后 1 不是子窗格", g_mux.panes[1].is_split_child == 0);
        g_mux.host_rows = 6;
        ck("6 行：仍然拒绝", split_split_active(SPLIT_H, 1) == 0);
        ck("6 行拒绝后 1 仍不是子窗格", g_mux.panes[1].is_split_child == 0);
        g_mux.host_rows = 7;
        ck("7 行：正好够（3+1+3）-> 允许", split_split_active(SPLIT_H, 1) == 1);
        ck("切出来的确是子窗格", g_mux.panes[1].is_split_child == 1);
    }
    /* 常量本身也钉住：以后改 include/split.h 必须同步改这里，免得门槛被无声改回去。 */
    ck("SPLIT_MIN_ROWS == 3", SPLIT_MIN_ROWS == 3);
    ck("SPLIT_MIN_COLS == 4（本次未动）", SPLIT_MIN_COLS == 4);


    /* ---- bug #27（2026-09-20）：「这种情况下时，调整正中间窗格的右边栏有bug」。
     *      用户澄清：现象 = 动的是另一条分隔条；布局 = 「先左右分，左窗格上下分
     *      3格，在左窗格的中间格再左右分」；中间格【左】侧那条分隔线正常，只有
     *      【右】侧坏。
     *
     *      根因（两个叠在一起）：
     *       1) 改错节点 —— 拖动路径把锚点 pane 交给 split_resize_set_frac()，它取
     *          锚点向上【第一个】方向匹配的祖先。中间格右半 pane 的第一个 V 祖先
     *          是【里层】的 2|4 节点，而用户抓的那条线属于【外层】祖先。
     *       2) 分母对不上 —— 百分比按「锚点宽 + 1 + 屏幕上右邻宽」算，frac 却按
     *          被改节点的整棵子树宽度解释。 ---- */
    {
        split_reset();
        g_mux.pane_count = 8;
        for (int i = 0; i < 8; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
        g_mux.active_pane = 0;
        g_mux.host_cols = 120; g_mux.host_rows = 30;
        split_init_tab(0);
        ck("#27 第1刀 左右分 0|1", split_split_active(SPLIT_V, 1) == 1);
        g_mux.active_pane = 0;
        ck("#27 第2刀 左窗格上下分 0/2", split_split_active(SPLIT_H, 2) == 1);
        g_mux.active_pane = 2;
        ck("#27 第3刀 下半再上下分 2/3", split_split_active(SPLIT_H, 3) == 1);
        g_mux.active_pane = 2;
        ck("#27 第4刀 中间格再左右分 2|4", split_split_active(SPLIT_V, 4) == 1);

        int rt = split_root_for_tab(0);
        ck("#27 树里 5 个叶子", split_count_leaves(rt) == 5);
        PaneRect rs[MAX_PANES]; memset(rs, 0, sizeof(rs));
        split_layout(rt, 0, 0, 120, 30, split_nodes(), rs);

        ck("#27 pane2 与 pane4 同一行（中间行）",
           rs[2].valid && rs[4].valid && rs[2].or0 == rs[4].or0 && rs[2].orows == rs[4].orows);
        ck("#27 pane2|pane4 相邻", rs[2].oc0 + rs[2].ocols + 1 == rs[4].oc0);
        int grab = rs[4].oc0 + rs[4].ocols;
        ck("#27 pane4 右沿就是外层分隔线（pane1 左沿-1）", grab + 1 == rs[1].oc0);
        ck("#27 上/中/下三格的右沿都压在这条线上",
           rs[0].oc0 + rs[0].ocols == grab && rs[3].oc0 + rs[3].ocols == grab);

        /* 里层 V 节点 = pane2 那个叶子的【父】节点。注意 split_find_leaf() 返回的
         * 是叶子本身，不是父节点。 */
        int leaf2 = split_find_leaf(rt, 2);
        int leaf24 = split_nodes()[leaf2].parent;
        ck("#27 里层 V 节点存在且不是根", leaf24 >= 0 && leaf24 != rt);
        ck("#27 里层节点确实是左右分且孩子就是 2|4",
           split_nodes()[leaf24].leaf == 0 && split_nodes()[leaf24].dir == SPLIT_V);

        /* 左侧那条（pane2 的右沿）：改动前后都该挑中里层节点 —— 对应用户说的
         * 「左边正常」。 */
        ck("#27 抓里层(pane2 右沿) -> 里层节点",
           split_drag_pick_node(rs, rt, 2, 'V', rs[2].oc0 + rs[2].ocols) == leaf24);
        /* 右侧那条（pane4 的右沿）：必须挑中根节点。旧行为挑中 leaf24，于是里层
         * 分隔线动、外层不动 = 用户看到的「动的是另一条分隔条」。 */
        int node = split_drag_pick_node(rs, rt, 4, 'V', grab);
        ck("#27 抓 pane4 右沿 -> 根节点（不是里层 2|4）", node == rt && node != leaf24);
        ck("#27 抓 pane0 右沿 -> 根节点", split_drag_pick_node(rs, rt, 0, 'V', grab) == rt);
        ck("#27 抓 pane3 右沿 -> 根节点", split_drag_pick_node(rs, rt, 3, 'V', grab) == rt);

        /* 端到端：把外层分隔线拖到右窗格中点，看它是不是真的动、且里层没被碰。 */
        int target = rs[1].oc0 + rs[1].ocols / 2;
        int pct = -1;
        ck("#27 外层 pct 可算", split_drag_pct(rs, node, 'V', target, &pct) == 1);
        /* 分母必须是【被改节点自己的子树跨度】。这棵树里根节点 = 左列 + 1 分隔 +
         * 右窗格 = 整个 120 列；旧的「锚点宽 + 1 + 屏幕上右邻宽」在这里是
         * 29 + 1 + 60 = 90，会把 pct 从 75 顶到 95（夹死），分隔线一下冲到最右。 */
        int span = rs[1].oc0 + rs[1].ocols;    /* 根子树总宽 */
        ck("#27 分母=节点子树跨度（不是锚点+右邻）", pct == (target * 100) / (span - 1));
        int f24 = split_nodes()[leaf24].frac_pct;
        split_set_frac_node(node, 4, pct);
        ck("#27 里层 2|4 的 frac 没被碰（用户没抓它）", split_nodes()[leaf24].frac_pct == f24);
        memset(rs, 0, sizeof(rs));
        split_layout(rt, 0, 0, 120, 30, split_nodes(), rs);
        int moved = rs[4].oc0 + rs[4].ocols;
        ck("#27 分隔线真的跟到鼠标位置(±1 量化)", moved >= target - 1 && moved <= target);
        ck("#27 pane1 左沿紧跟新分隔线", rs[1].oc0 == moved + 1);

        /* 反面对照：split_resize_set_frac() 语义【故意保持不变】（取最内层方向匹配
         * 祖先，别处还在用）。它改的确实是里层节点 —— 这就是当初的 bug，也说明
         * 拖动路径为什么必须改道走 split_drag_pick_node()。 */
        split_nodes()[rt].frac_pct = 50; split_nodes()[leaf24].frac_pct = 50;
        split_resize_set_frac(rt, 4, 'V', 63);
        ck("#27 对照：旧路径改的是里层节点", split_nodes()[leaf24].frac_pct != 50);
        ck("#27 对照：旧路径下根节点没动", split_nodes()[rt].frac_pct == 50);
    }

    /* ---- bug #27 的横方向同一个坑：先上下分 -> 上半左右分三格 -> 中间格再上下分。
     *      抓中间格下半 pane 的【底沿】= 外层横线。 ---- */
    {
        split_reset();
        g_mux.pane_count = 8;
        for (int i = 0; i < 8; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
        g_mux.active_pane = 0;
        g_mux.host_cols = 120; g_mux.host_rows = 60;
        split_init_tab(0);
        ck("#27H 第1刀 上下分 0/1", split_split_active(SPLIT_H, 1) == 1);
        g_mux.active_pane = 0;
        ck("#27H 第2刀 上半左右分 0|2", split_split_active(SPLIT_V, 2) == 1);
        g_mux.active_pane = 2;
        ck("#27H 第3刀 右半再左右分 2|3", split_split_active(SPLIT_V, 3) == 1);
        g_mux.active_pane = 2;
        ck("#27H 第4刀 中间格再上下分 2/4", split_split_active(SPLIT_H, 4) == 1);

        int rt = split_root_for_tab(0);
        PaneRect rs[MAX_PANES]; memset(rs, 0, sizeof(rs));
        split_layout(rt, 0, 0, 120, 60, split_nodes(), rs);
        int grabh = rs[4].or0 + rs[4].orows;
        ck("#27H pane4 底沿就是外层横线（pane1 顶沿-1）", grabh + 1 == rs[1].or0);
        int leafh = split_nodes()[split_find_leaf(rt, 2)].parent;
        ck("#27H 里层节点是上下分", split_nodes()[leafh].dir == SPLIT_H);
        int nodeh = split_drag_pick_node(rs, rt, 4, 'H', grabh);
        ck("#27H 抓 pane4 底沿 -> 根节点（不是里层 2/4）", nodeh == rt && nodeh != leafh);
        int tpct = -1;
        ck("#27H 外层 pct 可算", split_drag_pct(rs, nodeh, 'H', rs[1].or0 + rs[1].orows / 2, &tpct) == 1);
        int fh = split_nodes()[leafh].frac_pct;
        split_set_frac_node(nodeh, 4, tpct);
        ck("#27H 里层 2/4 的 frac 没被碰", split_nodes()[leafh].frac_pct == fh);
    }


    /* ---- bug #29（2026-09-20）：「纵向压缩可以把一个终端压缩到 <3 行/列」。
     *      frac 只夹 5..95。窗格小的时候 5% 就是 0 行 —— 一路拖到底能把一个终端
     *      压没。两道防线：split_drag_pct() 夹百分比（拖动跟手），layout_rec 里
     *      的 clamp_side() 兜底（覆盖键盘 resize 和窗口拖窄后的旧 frac）。 ---- */
    {
        split_reset();
        g_mux.pane_count = 8;
        for (int i = 0; i < 8; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
        g_mux.active_pane = 0;
        g_mux.host_cols = 80; g_mux.host_rows = 12;
        split_init_tab(0);
        ck("#29 上下分成功", split_split_active(SPLIT_H, 1) == 1);
        int rt = split_root_for_tab(0);
        PaneRect rs[MAX_PANES]; memset(rs, 0, sizeof(rs));
        split_layout(rt, 0, 0, 80, 12, split_nodes(), rs);
        ck("#29 初始两格都 >= 3 行",
           rs[0].orows >= SPLIT_MIN_ROWS && rs[1].orows >= SPLIT_MIN_ROWS);

        /* 一路拖到最上面（鼠标压在第 0 行）。12 行 -> span=11。 */
        int node = split_drag_pick_node(rs, rt, 0, 'H', rs[0].or0 + rs[0].orows);
        ck("#29 挑中根节点", node == rt);
        int pct = -1;
        ck("#29 pct 可算", split_drag_pct(rs, node, 'H', 0, &pct) == 1);
        /* pct 自己就要夹住：不夹的话会写成 5（对应 11*5/100 = 0 行），全靠
         * layout_rec 兜底，表现为「拖回来一大段分隔线都不动」。
         * 期望值 = ceil(3*100/11) = 28。 */
        ck("#29 pct 被夹到 28（=ceil(3*100/11)），不是 5", pct == 28);
        split_set_frac_node(node, 0, pct);
        memset(rs, 0, sizeof(rs));
        split_layout(rt, 0, 0, 80, 12, split_nodes(), rs);
        ck("#29 拖到最上后上格仍 >= 3 行", rs[0].orows >= SPLIT_MIN_ROWS);
        ck("#29 拖到最上后下格仍 >= 3 行", rs[1].orows >= SPLIT_MIN_ROWS);

        /* 反方向：一路拖到最下面。 */
        memset(rs, 0, sizeof(rs)); split_layout(rt, 0, 0, 80, 12, split_nodes(), rs);
        node = split_drag_pick_node(rs, rt, 0, 'H', rs[0].or0 + rs[0].orows);
        pct = -1;
        ck("#29 反向 pct 可算", split_drag_pct(rs, node, 'H', 11, &pct) == 1);
        ck("#29 反向 pct 被夹到 72（=floor(8*100/11)）", pct == 72);
        split_set_frac_node(node, 0, pct);
        memset(rs, 0, sizeof(rs)); split_layout(rt, 0, 0, 80, 12, split_nodes(), rs);
        ck("#29 拖到最下后上格仍 >= 3 行", rs[0].orows >= SPLIT_MIN_ROWS);
        ck("#29 拖到最下后下格仍 >= 3 行", rs[1].orows >= SPLIT_MIN_ROWS);

        /* 直接写极端 frac —— 模拟键盘 prefix+方向键（split_resize_pane 只按百分比
         * 加减、手里没有像素尺寸）和「窗口拖窄后旧 frac 变得太极端」。这时只有
         * layout_rec 里的 clamp_side() 能兜住。 */
        split_nodes()[rt].frac_pct = 5;
        memset(rs, 0, sizeof(rs)); split_layout(rt, 0, 0, 80, 12, split_nodes(), rs);
        ck("#29 frac=5 时上格仍 >= 3 行（layout 兜底）", rs[0].orows >= SPLIT_MIN_ROWS);
        ck("#29 frac=5 时下格没被撑爆", rs[1].orows >= SPLIT_MIN_ROWS);
        split_nodes()[rt].frac_pct = 95;
        memset(rs, 0, sizeof(rs)); split_layout(rt, 0, 0, 80, 12, split_nodes(), rs);
        ck("#29 frac=95 时下格仍 >= 3 行（layout 兜底）", rs[1].orows >= SPLIT_MIN_ROWS);
        ck("#29 frac=95 时上格没被压没", rs[0].orows >= SPLIT_MIN_ROWS);
    }

    /* ---- bug #29 的左右方向（SPLIT_MIN_COLS = 4） ---- */
    {
        split_reset();
        g_mux.pane_count = 8;
        for (int i = 0; i < 8; i++) { g_mux.panes[i].active = 1; g_mux.panes[i].is_split_child = 0; }
        g_mux.active_pane = 0;
        g_mux.host_cols = 20; g_mux.host_rows = 20;
        split_init_tab(0);
        ck("#29V 左右分成功", split_split_active(SPLIT_V, 1) == 1);
        int rv = split_root_for_tab(0);
        PaneRect rs[MAX_PANES]; memset(rs, 0, sizeof(rs));
        split_layout(rv, 0, 0, 20, 20, split_nodes(), rs);
        ck("#29V 初始两格都 >= 4 列",
           rs[0].ocols >= SPLIT_MIN_COLS && rs[1].ocols >= SPLIT_MIN_COLS);

        split_nodes()[rv].frac_pct = 5;
        memset(rs, 0, sizeof(rs)); split_layout(rv, 0, 0, 20, 20, split_nodes(), rs);
        ck("#29V frac=5 时左格仍 >= 4 列（layout 兜底）", rs[0].ocols >= SPLIT_MIN_COLS);
        split_nodes()[rv].frac_pct = 95;
        memset(rs, 0, sizeof(rs)); split_layout(rv, 0, 0, 20, 20, split_nodes(), rs);
        ck("#29V frac=95 时右格仍 >= 4 列（layout 兜底）", rs[1].ocols >= SPLIT_MIN_COLS);

        memset(rs, 0, sizeof(rs)); split_layout(rv, 0, 0, 20, 20, split_nodes(), rs);
        int nv = split_drag_pick_node(rs, rv, 0, 'V', rs[0].oc0 + rs[0].ocols);
        int pv = -1;
        ck("#29V 一路拖到最左 pct 可算", split_drag_pct(rs, nv, 'V', 0, &pv) == 1);
        ck("#29V pct 被夹到 22（=ceil(4*100/19)）", pv == 22);
        split_set_frac_node(nv, 0, pv);
        memset(rs, 0, sizeof(rs)); split_layout(rv, 0, 0, 20, 20, split_nodes(), rs);
        ck("#29V 拖到最左后两格都 >= 4 列",
           rs[0].ocols >= SPLIT_MIN_COLS && rs[1].ocols >= SPLIT_MIN_COLS);

        memset(rs, 0, sizeof(rs)); split_layout(rv, 0, 0, 20, 20, split_nodes(), rs);
        nv = split_drag_pick_node(rs, rv, 0, 'V', rs[0].oc0 + rs[0].ocols);
        pv = -1;
        split_drag_pct(rs, nv, 'V', 19, &pv);
        split_set_frac_node(nv, 0, pv);
        memset(rs, 0, sizeof(rs)); split_layout(rv, 0, 0, 20, 20, split_nodes(), rs);
        ck("#29V 拖到最右后两格都 >= 4 列",
           rs[0].ocols >= SPLIT_MIN_COLS && rs[1].ocols >= SPLIT_MIN_COLS);
    }

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nSPLIT TESTS PASSED\n");
    return 0;
}
"""


STUB_EXTRA = r"""
typedef wchar_t WCHAR;
typedef struct { union { WCHAR UnicodeChar; char AsciiChar; } Char; unsigned short Attributes; } CHAR_INFO;
typedef struct { long dummy; } CRITICAL_SECTION;
typedef struct { unsigned long dwEventMask; } MOUSE_EVENT_RECORD;
typedef void *HPCON;
#ifndef FOREGROUND_RED
#define FOREGROUND_RED 4
#define FOREGROUND_GREEN 2
#define FOREGROUND_BLUE 1
#define FOREGROUND_INTENSITY 8
#endif
#ifndef __stdcall
#define __stdcall
#endif
"""


def main() -> int:
    print("=== 分屏树 / 布局验证 (verify_split.py) ===")
    with tempfile.TemporaryDirectory() as td:
        # 组装增强 stub windows.h（tests/stub 内容 + 补类型）。
        stub_win = os.path.join(STUB, "windows.h")
        with open(stub_win, encoding="utf-8") as f:
            stub_txt = f.read()
        if "CHAR_INFO" not in stub_txt:
            stub_txt = stub_txt + "\n" + STUB_EXTRA
        for name in ("shellapi.h", "process.h"):
            sp = os.path.join(STUB, name)
            if os.path.exists(sp):
                with open(sp, encoding="utf-8") as f:
                    content = f.read()
                with open(os.path.join(td, name), "w", encoding="utf-8") as o:
                    o.write(content)
        with open(os.path.join(td, "windows.h"), "w", encoding="utf-8") as f:
            f.write(stub_txt)
        h = os.path.join(td, "h.c")
        exe = os.path.join(td, "h.bin")
        # 驱动需要 g_mux 等全局符号（split 高层函数引用），提供最小定义。
        globs = os.path.join(td, "globs.c")
        with open(globs, "w", encoding="utf-8") as f:
            f.write(
                '#include "common.h"\n#include "types.h"\n'
                "MuxState g_mux;\n"
                "int g_split_zoom_dummy;\n"
                "/* v1.8.43：split_split_active 空间不足时调用 toast_show，harness 提供空实现。 */\n"
                "void toast_show(const char *msg, unsigned int ms){(void)msg;(void)ms;}\n")
        open(h, "w", encoding="utf-8").write(HARNESS)

        builds = [0]

        def build_run(split_c_path):
            builds[0] += 1
            out = os.path.join(td, "h%d.bin" % builds[0])
            cp = subprocess.run(
                ["gcc", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-I" + td, "-I" + INC, h, globs, split_c_path, "-o", out, "-lm"],
                capture_output=True, text=True)
            if cp.returncode != 0:
                print(cp.stderr, file=sys.stderr)
                return None, cp.stderr
            r = subprocess.run([out], capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr

        rc, out = build_run(os.path.join(SRC, "split.c"))
        if rc is None:
            print("FAIL: split harness 编译失败", file=sys.stderr)
            return 1
        print(out)
        if rc != 0:
            return 1

        # ---- 自证（验红）：把「按分界线位置挑节点」退回旧行为（取最内层方向匹配
        #      祖先，即 bug #27 的病因），新增断言必须失败。 ----
        with open(os.path.join(SRC, "split.c"), encoding="utf-8") as f:
            split_src = f.read()
        needle = "        if (ahi == grab_pos && blo == grab_pos + 1) return anc[k];"
        assert split_src.count(needle) == 1, "自证锚点没找到，改 split.c 时要同步这里"
        mutant = os.path.join(td, "split_mutant.c")
        with open(mutant, "w", encoding="utf-8") as f:
            f.write(split_src.replace(
                needle,
                "        (void)grab_pos;  /* MUTANT: 退回旧行为 —— 不看抓的是哪条线，"
                "直接取最内层方向匹配祖先 */\n        if (k == 0) return anc[k];"))
        rc2, out2 = build_run(mutant)
        if rc2 == 0:
            print("FAIL: 自证失败 —— 退回旧行为后测试仍然全绿，说明判据没抓住 bug #27",
                  file=sys.stderr)
            return 1
        caught = [ln for ln in (out2 or "").splitlines() if ln.startswith("[FAIL]")]
        if not caught:
            print("自证编译/运行输出（应为 0 条 FAIL 时才打印）：\n" + (out2 or "")[-2000:],
                  file=sys.stderr)
        print("自证：退回旧行为后被抓住 %d 条，例如：" % len(caught))
        for ln in caught[:4]:
            print("      " + ln)
        if not any("#27" in ln for ln in caught):
            print("FAIL: 自证失败 —— 失败的都不是 #27 的断言", file=sys.stderr)
            return 1
        print("自证通过：#27 的判据确实钉住了「挑对节点」这件事。")

        # ---- 自证 2：bug #27 的另一半病因是【分母】。把分母里的 b 子树跨度丢掉，
        #      「分母=节点子树跨度」这条断言必须失败。 ----
        needle2 = "    int total = (ahi - alo) + 1 + (bhi - blo);"
        assert split_src.count(needle2) == 1, "自证 2 锚点没找到，改 split.c 时要同步这里"
        mutant2 = os.path.join(td, "split_mutant2.c")
        with open(mutant2, "w", encoding="utf-8") as f:
            f.write(split_src.replace(
                needle2,
                "    (void)bhi; (void)blo;\n"
                "    int total = (ahi - alo) + 1;  /* MUTANT: 分母丢掉 b 子树 */"))
        rc3, out3 = build_run(mutant2)
        if rc3 == 0:
            print("FAIL: 自证 2 失败 —— 分母改坏后测试仍然全绿", file=sys.stderr)
            return 1
        caught2 = [ln for ln in (out3 or "").splitlines() if ln.startswith("[FAIL]")]
        print("自证 2：分母改坏后被抓住 %d 条，例如：" % len(caught2))
        for ln in caught2[:3]:
            print("      " + ln)
        if not any("分母" in ln for ln in caught2):
            print("FAIL: 自证 2 失败 —— 没抓住「分母」那条断言", file=sys.stderr)
            return 1
        print("自证 2 通过：#27 的判据也钉住了「分母用节点自己的跨度」。")

        # ---- 自证 3：拆掉 layout_rec 里的最小尺寸兜底闸（clamp_side 失效）。 ----
        n3a = "    if (*side < min_side) *side = min_side;"
        n3b = "    if (total - *side < min_side) *side = total - min_side;"
        assert split_src.count(n3a) == 1 and split_src.count(n3b) == 1, \
            "自证 3 锚点没找到，改 split.c 时要同步这里"
        mutant3 = os.path.join(td, "split_mutant3.c")
        with open(mutant3, "w", encoding="utf-8") as f:
            f.write(split_src.replace(n3a, "    if (*side < -1) *side = min_side;  /* MUTANT */")
                             .replace(n3b, "    if (total - *side < -1) *side = total - min_side;  /* MUTANT */"))
        rc4, out4 = build_run(mutant3)
        if rc4 == 0:
            print("FAIL: 自证 3 失败 —— 拆掉 layout 兜底闸后测试仍然全绿", file=sys.stderr)
            return 1
        caught3 = [ln for ln in (out4 or "").splitlines() if ln.startswith("[FAIL]")]
        print("自证 3：拆掉 layout 兜底闸后被抓住 %d 条，例如：" % len(caught3))
        for ln in caught3[:3]:
            print("      " + ln)
        if not any("兜底" in ln for ln in caught3):
            print("FAIL: 自证 3 失败 —— 没抓住「layout 兜底」那几条断言", file=sys.stderr)
            return 1
        print("自证 3 通过：最小尺寸的最后一道闸确实被钉住了。")

        # ---- 自证 4：拆掉 split_drag_pct 的百分比夹取（只留 5..95）。 ----
        n4 = "    if (span >= min_side * 2) {"
        assert split_src.count(n4) == 1, "自证 4 锚点没找到，改 split.c 时要同步这里"
        mutant4 = os.path.join(td, "split_mutant4.c")
        with open(mutant4, "w", encoding="utf-8") as f:
            f.write(split_src.replace(n4, "    if (0 && span >= min_side * 2) {  /* MUTANT */"))
        rc5, out5 = build_run(mutant4)
        if rc5 == 0:
            print("FAIL: 自证 4 失败 —— 拆掉百分比夹取后测试仍然全绿", file=sys.stderr)
            return 1
        caught4 = [ln for ln in (out5 or "").splitlines() if ln.startswith("[FAIL]")]
        print("自证 4：拆掉百分比夹取后被抓住 %d 条，例如：" % len(caught4))
        for ln in caught4[:3]:
            print("      " + ln)
        if not any("pct 被夹到" in ln for ln in caught4):
            print("FAIL: 自证 4 失败 —— 没抓住「pct 被夹到」那几条断言", file=sys.stderr)
            return 1
        print("自证 4 通过：拖动路径的百分比夹取也被钉住了。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
