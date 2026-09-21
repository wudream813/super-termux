#include "split.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * 分屏树：节点池 + 纯布局。
 *
 * 节点池是固定数组，由 split_reset 清零、split_new_leaf 分配。为了让
 * split_layout 成为纯函数（Linux 侧单测可直接构造树调用），它不直接引用
 * 全局 g_split_nodes，而是由调用方把 nodes 数组传进来；生产路径传全局池。
 * ------------------------------------------------------------------------- */

static SplitNode g_split_nodes[MAX_SPLIT_NODES];

/* 标签页 <-> 树根注册表（纯布局部分不引用；高层操作 split_*tab* 使用）。
 * 静态初值全 0 会让空槽误指向 pane 0，split_reset() 启动时会填成 -1。 */
static int g_tab_anchor[MAX_PANES];
static int g_tab_root[MAX_PANES];

void split_reset(void) {
    memset(g_split_nodes, 0, sizeof(g_split_nodes));
    for (int i = 0; i < MAX_PANES; i++) { g_tab_anchor[i] = -1; g_tab_root[i] = -1; }
}

SplitNode *split_nodes(void) { return g_split_nodes; }

static int alloc_node(void) {
    for (int i = 0; i < MAX_SPLIT_NODES; i++) {
        if (!g_split_nodes[i].used) return i;
    }
    return -1;
}

int split_new_leaf(int pane_idx) {
    int n = alloc_node();
    if (n < 0) return -1;
    memset(&g_split_nodes[n], 0, sizeof(g_split_nodes[n]));
    g_split_nodes[n].used = 1;
    g_split_nodes[n].leaf = 1;
    g_split_nodes[n].pane_idx = pane_idx;
    g_split_nodes[n].parent = -1;
    g_split_nodes[n].a = g_split_nodes[n].b = -1;
    return n;
}

/* 布局递归用的纯树遍历（在任意 nodes 数组上）。 */
int split_count_leaves(int root) {
    if (root < 0 || !g_split_nodes[root].used) return 0;
    SplitNode *nd = &g_split_nodes[root];
    if (nd->leaf) return 1;
    return split_count_leaves(nd->a) + split_count_leaves(nd->b);
}

static int first_leaf_in(SplitNode *nodes, int n) {
    if (n < 0 || !nodes[n].used) return -1;
    if (nodes[n].leaf) return n;
    return first_leaf_in(nodes, nodes[n].a);
}

int split_first_leaf(int root) {
    int n = first_leaf_in(g_split_nodes, root);
    return (n >= 0) ? g_split_nodes[n].pane_idx : -1;
}

int split_find_leaf(int root, int pane_idx) {
    SplitNode *nodes = g_split_nodes;
    if (root < 0 || !nodes[root].used) return -1;
    int st[MAX_SPLIT_NODES];
    int top = 0;
    st[top++] = root;
    while (top > 0) {
        int n = st[--top];
        if (n < 0 || !nodes[n].used) continue;
        if (nodes[n].leaf) {
            if (nodes[n].pane_idx == pane_idx) return n;
        } else {
            if (nodes[n].b >= 0) st[top++] = nodes[n].b;
            if (nodes[n].a >= 0) st[top++] = nodes[n].a;
        }
    }
    return -1;
}

int split_do(int node, int dir, int new_pane_idx) {
    if (node < 0 || !g_split_nodes[node].used || !g_split_nodes[node].leaf) return -1;
    /* 旧叶子【原地】变成内部节点：它的节点下标不变，父/祖父对它的引用继续有效；
       * 它原有的 pane 落到新叶子 a（保留原位），新建的 pane 叶子作为 b。
     * SPLIT_H（上下）新窗格在下（b）；SPLIT_V（左右）新窗格在右（b）。 */
    int old_pane = g_split_nodes[node].pane_idx;
    int old_parent = g_split_nodes[node].parent;

    int leaf_new = split_new_leaf(new_pane_idx);
    int leaf_old = split_new_leaf(old_pane);
    if (leaf_new < 0 || leaf_old < 0) return -1;

    g_split_nodes[node].leaf = 0;
    g_split_nodes[node].pane_idx = -1;
    g_split_nodes[node].dir = dir;
    g_split_nodes[node].frac_pct = 50;
    g_split_nodes[node].a = leaf_old;   /* 上/左：原 pane */
    g_split_nodes[node].b = leaf_new;   /* 下/右：新 pane */
    g_split_nodes[node].parent = old_parent;
    g_split_nodes[leaf_old].parent = node;
    g_split_nodes[leaf_new].parent = node;
    return node;
}

/* 把节点 child 从其父中摘除，用 survivor 顶替父在祖父中的位置。
 * 返回顶替后的根节点（可能变化）。 */
static int replace_node_with(int root, int parent, int survivor) {
    int gp = g_split_nodes[parent].parent;
    g_split_nodes[survivor].parent = gp;
    g_split_nodes[parent].used = 0;
    if (gp < 0) return survivor;   /* parent 是根 */
    if (g_split_nodes[gp].a == parent) g_split_nodes[gp].a = survivor;
    else if (g_split_nodes[gp].b == parent) g_split_nodes[gp].b = survivor;
    return root;
}

int split_remove_leaf(int root, int node, int *removed_pane) {
    if (node < 0 || !g_split_nodes[node].used || !g_split_nodes[node].leaf) return root;
    if (removed_pane) *removed_pane = g_split_nodes[node].pane_idx;
    int par = g_split_nodes[node].parent;
    g_split_nodes[node].used = 0;
    if (par < 0) return -1;   /* 删的是根叶子：整树空了 */

    int other = (g_split_nodes[par].a == node) ? g_split_nodes[par].b : g_split_nodes[par].a;
    return replace_node_with(root, par, other);
}

/* ---------------------------- 纯布局 ------------------------------------- */

/* 最后一道闸：无论 frac 是怎么来的（拖分隔条 / 键盘 prefix+方向键 / 窗口变小后
 * 旧 frac 变得太极端），都不许把某一侧压到最小尺寸以下。
 *
 * 为什么必须有这道闸而不只靠 split_drag_pct() 夹百分比：键盘 resize 走
 * split_resize_pane()，它只按百分比加减、手里根本没有像素尺寸，夹不了；窗口
 * 拖窄之后同一个 frac 对应的列数也会缩到 1。（2026-09-20 用户报「纵向压缩可以
 * 把一个终端压缩到 <3 行/列」。）
 *
 * 空间连 2*min 都不够时不动手 —— 那种情况 layout_rec 的均分兜底已经接管了。 */
static void clamp_side(int *side, int total, int min_side) {
    if (total < min_side * 2) return;
    if (*side < min_side) *side = min_side;
    if (total - *side < min_side) *side = total - min_side;
}

static void layout_rec(SplitNode *nodes, int n, int c0, int r0, int cols, int rows,
                       PaneRect *rects) {
    if (n < 0 || !nodes[n].used) return;
    if (nodes[n].leaf) {
        int pi = nodes[n].pane_idx;
        if (pi >= 0) {
            /* 窗格内容矩形就是分配到的矩形（内容与分隔线相邻，之间不留空白）。
             * 内缩/外接两套坐标相等：渲染、ConPTY 尺寸、鼠标命中、分隔线定位都用
             * 同一套字段，避免两套坐标不一致。 */
            rects[pi].oc0 = c0;  rects[pi].or0 = r0;
            rects[pi].ocols = cols; rects[pi].orows = rows;
            rects[pi].c0 = c0;  rects[pi].r0 = r0;
            rects[pi].cols = cols; rects[pi].rows = rows;
            rects[pi].valid = 1;
        }
        return;
    }
    int frac = nodes[n].frac_pct;
    if (frac < 5) frac = 5;
    if (frac > 95) frac = 95;
    if (nodes[n].dir == SPLIT_V) {
        /* 左右分：中间留 1 列边框。 */
        int total = cols - 1;               /* 扣掉 1 列边框 */
        if (total < 2) total = 2;
        int left = total * frac / 100;
        int right = total - left;
        /* 保证两边都不小于最小宽；空间不够时均分。 */
        if (cols < SPLIT_MIN_COLS * 2 + 1) { left = total / 2; right = total - left; }
        clamp_side(&left, total, SPLIT_MIN_COLS);
        right = total - left;
        layout_rec(nodes, nodes[n].a, c0, r0, left, rows, rects);
        layout_rec(nodes, nodes[n].b, c0 + left + 1, r0, right, rows, rects);
    } else {
        /* 上下分：中间留 1 行边框。 */
        int total = rows - 1;
        if (total < 2) total = 2;
        int top = total * frac / 100;
        int bot = total - top;
        if (rows < SPLIT_MIN_ROWS * 2 + 1) { top = total / 2; bot = total - top; }
        clamp_side(&top, total, SPLIT_MIN_ROWS);
        bot = total - top;
        layout_rec(nodes, nodes[n].a, c0, r0, cols, top, rects);
        layout_rec(nodes, nodes[n].b, c0, r0 + top + 1, cols, bot, rects);
    }
}

void split_layout(int root, int c0, int r0, int cols, int rows,
                  SplitNode *nodes, PaneRect *rects) {
    if (root < 0 || !nodes || !rects) return;
    layout_rec(nodes, root, c0, r0, cols, rows, rects);
}

/* --------------------------- 树内导航 ------------------------------------ */

/* 中序收集叶子 pane 索引（左->右、上->下的视觉次序）。 */
static int collect_leaves(SplitNode *nodes, int n, int *out, int cap, int cnt) {
    if (n < 0 || !nodes[n].used || cnt >= cap) return cnt;
    if (nodes[n].leaf) { out[cnt++] = nodes[n].pane_idx; return cnt; }
    cnt = collect_leaves(nodes, nodes[n].a, out, cap, cnt);
    cnt = collect_leaves(nodes, nodes[n].b, out, cap, cnt);
    return cnt;
}

int split_next_pane(int root, int from_pane, int forward) {
    int leaves[MAX_PANES];
    int n = collect_leaves(g_split_nodes, root, leaves, MAX_PANES, 0);
    if (n <= 0) return from_pane;
    int idx = -1;
    for (int i = 0; i < n; i++) if (leaves[i] == from_pane) { idx = i; break; }
    if (idx < 0) return leaves[0];
    if (forward) idx = (idx + 1) % n;
    else         idx = (idx - 1 + n) % n;
    return leaves[idx];
}

/* 区间重叠长度（0 表示不重叠）。 */
static int span_overlap(int a0, int a1, int b0, int b1) {
    int lo = a0 > b0 ? a0 : b0;
    int hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0;
}

/* 用纯布局算出各 pane 矩形，再按「重叠优先 + 最近边界」选相邻叶子。
 * 这与 tmux 的方向切换一致：优先选与源窗格在【垂直于移动方向】上有重叠、
 * 且在移动方向上最近的窗格；没有重叠的再按中心距离兜底。 */
int split_neighbor_pane(int root, int from_pane, char where) {
    PaneRect rects[MAX_PANES];
    memset(rects, 0, sizeof(rects));
    split_layout(root, 0, 0, 200, 200, g_split_nodes, rects);
    if (from_pane < 0 || !rects[from_pane].valid) return from_pane;

    PaneRect f = rects[from_pane];
    int horizontal = (where == 'L' || where == 'R');  /* 左右移动：比较列 */
    int sign = (where == 'R' || where == 'D') ? 1 : -1;

    int best = from_pane;
    int best_overlap = -1;
    long long best_gap = (long long)1 << 60;
    long long best_center = (long long)1 << 60;

    for (int pi = 0; pi < MAX_PANES; pi++) {
        if (!rects[pi].valid || pi == from_pane) continue;
        PaneRect t = rects[pi];
        int gap, overlap, cross_center;
        if (horizontal) {
            /* 目标必须在源的右侧(sign>0: t.c0 >= f 右沿) 或左侧。 */
            if (sign > 0) { if (t.c0 < f.c0 + f.cols - 1) continue; gap = t.c0 - (f.c0 + f.cols - 1); }
            else          { if (t.c0 + t.cols - 1 > f.c0) continue; gap = f.c0 - (t.c0 + t.cols - 1); }
            overlap = span_overlap(f.r0, f.r0 + f.rows, t.r0, t.r0 + t.rows);
            cross_center = (t.r0 + t.rows/2) - (f.r0 + f.rows/2);
        } else {
            if (sign > 0) { if (t.r0 < f.r0 + f.rows - 1) continue; gap = t.r0 - (f.r0 + f.rows - 1); }
            else          { if (t.r0 + t.rows - 1 > f.r0) continue; gap = f.r0 - (t.r0 + t.rows - 1); }
            overlap = span_overlap(f.c0, f.c0 + f.cols, t.c0, t.c0 + t.cols);
            cross_center = (t.c0 + t.cols/2) - (f.c0 + f.cols/2);
        }
        long long cc = cross_center < 0 ? -(long long)cross_center : cross_center;
        /* 重叠多者优先；重叠相同取边界间距小；再相同取横向偏移小。 */
        if (overlap > best_overlap ||
            (overlap == best_overlap && gap < best_gap) ||
            (overlap == best_overlap && gap == best_gap && cc < best_center)) {
            best_overlap = overlap; best_gap = gap; best_center = cc; best = pi;
        }
    }
    return best;
}

int split_resize_pane(int root, int from_pane, char where, int delta_pct) {
    if (root < 0 || delta_pct == 0) return 0;
    int leaf = split_find_leaf(root, from_pane);
    if (leaf < 0) return 0;
    int par = g_split_nodes[leaf].parent;
    /* 向上找第一个分隔方向与拖动方向一致的祖先。 */
    while (par >= 0) {
        int dir = g_split_nodes[par].dir;
        int match_h = (where == 'U' || where == 'D') && dir == SPLIT_H;
        int match_v = (where == 'L' || where == 'R') && dir == SPLIT_V;
        if (match_h || match_v) {
            /* 判断当前 pane 落在 a（上/左）还是 b（下/右）子树。 */
            int leaves[MAX_PANES];
            int in_a = 0;
            int na = collect_leaves(g_split_nodes, g_split_nodes[par].a, leaves, MAX_PANES, 0);
            for (int i = 0; i < na; i++) if (leaves[i] == from_pane) { in_a = 1; break; }
            /* 向右/向下：若在 a 则增大 a 占比，若在 b 则减小 a 占比；
             * 向左/向上相反。 */
            int grow_positive = (where == 'R' || where == 'D');
            int step = delta_pct < 0 ? -delta_pct : delta_pct;
            int adj = (in_a == grow_positive) ? step : -step;
            int nv = g_split_nodes[par].frac_pct + adj;
            if (nv < 5) nv = 5;
            if (nv > 95) nv = 95;
            g_split_nodes[par].frac_pct = nv;
            return 1;
        }
        par = g_split_nodes[par].parent;
    }
    return 0;
}

int split_subtree_leaves(int node, int side, int *out, int cap) {
    if (node < 0 || node >= MAX_SPLIT_NODES || !out || cap <= 0) return 0;
    if (!g_split_nodes[node].used || g_split_nodes[node].leaf) return 0;
    int child = side ? g_split_nodes[node].b : g_split_nodes[node].a;
    if (child < 0) return 0;
    return collect_leaves(g_split_nodes, child, out, cap, 0);
}

int split_dir_ancestors(int root, int anchor_pane, char dir, int *out, int cap) {
    if (root < 0 || !out || cap <= 0) return 0;
    int leaf = split_find_leaf(root, anchor_pane);
    if (leaf < 0) return 0;
    int want_dir = (dir == 'V') ? SPLIT_V : SPLIT_H;
    int n = 0;
    int par = g_split_nodes[leaf].parent;
    while (par >= 0 && n < cap) {
        if (g_split_nodes[par].dir == want_dir) out[n++] = par;
        par = g_split_nodes[par].parent;
    }
    return n;
}

void split_set_frac_node(int node, int anchor_pane, int pct) {
    if (node < 0 || node >= MAX_SPLIT_NODES || !g_split_nodes[node].used) return;
    if (g_split_nodes[node].leaf) return;
    if (pct < 5) pct = 5;
    if (pct > 95) pct = 95;
    /* anchor 在 a 子树时 pct 就是 a 占比；在 b 子树时 a 占比 = 100-pct。 */
    int leaves[MAX_PANES];
    int in_a = 0;
    int na = collect_leaves(g_split_nodes, g_split_nodes[node].a, leaves, MAX_PANES, 0);
    for (int i = 0; i < na; i++) if (leaves[i] == anchor_pane) { in_a = 1; break; }
    g_split_nodes[node].frac_pct = in_a ? pct : (100 - pct);
}

/* 语义保持不变：取【最内层】那个方向匹配的祖先。拖分隔条的路径不再走这里，
 * 改用 split_drag_pick_node() 挑出用户真正抓住的那条线所属的节点。 */
void split_resize_set_frac(int root, int anchor_pane, char dir, int pct) {
    if (root < 0) return;
    int anc[MAX_SPLIT_NODES];
    int n = split_dir_ancestors(root, anchor_pane, dir, anc, MAX_SPLIT_NODES);
    if (n > 0) split_set_frac_node(anc[0], anchor_pane, pct);
}

/* 子树在屏幕上的跨度：'V' 取列区间 [lo,hi)，'H' 取行区间。hi 是【排他】的右沿/
 * 底沿 —— 也就是这个子树与它兄弟之间那条分隔线所在的列/行。lo/hi 可传 NULL。
 * 返回 0 表示算不出来（子树空 / 有 pane 矩形无效）。 */
static int subtree_span(const PaneRect *rects, int node, int side, char dir,
                        int *lo, int *hi) {
    int ls[MAX_PANES];
    int n = split_subtree_leaves(node, side, ls, MAX_PANES);
    if (n <= 0) return 0;
    int l = 0x7fffffff, h = -0x7fffffff;
    for (int i = 0; i < n; i++) {
        const PaneRect *r = &rects[ls[i]];
        if (!r->valid) return 0;
        int rl = (dir == 'V') ? r->oc0 : r->or0;
        int rh = (dir == 'V') ? r->oc0 + r->ocols : r->or0 + r->orows;
        if (rl < l) l = rl;
        if (rh > h) h = rh;
    }
    if (l >= h) return 0;
    if (lo) *lo = l;
    if (hi) *hi = h;
    return 1;
}

int split_drag_pick_node(const PaneRect *rects, int root, int grab_pane, char dir,
                         int grab_pos) {
    if (!rects || root < 0) return -1;
    int anc[MAX_SPLIT_NODES];
    int nanc = split_dir_ancestors(root, grab_pane, dir, anc, MAX_SPLIT_NODES);
    for (int k = 0; k < nanc; k++) {
        int alo, ahi, blo;
        if (!subtree_span(rects, anc[k], 0, dir, &alo, &ahi)) continue;
        if (!subtree_span(rects, anc[k], 1, dir, &blo, NULL)) continue;
        /* a 子树的排他右沿/底沿 == 用户抓的那条线，且 b 子树紧贴其后 —— 就是它。 */
        if (ahi == grab_pos && blo == grab_pos + 1) return anc[k];
    }
    /* 兜底：位置对不上任何该方向的分界线。抓得到分隔线就一定对得上（该 pane 必是
     * 某个子树 a 侧的最外沿），走到这里说明矩形还没算好；退回最内层祖先，至少不
     * 比改动前更差。 */
    return nanc > 0 ? anc[0] : -1;
}

int split_drag_pct(const PaneRect *rects, int node, char dir, int mouse_pos, int *out_pct) {
    if (!rects || !out_pct || node < 0) return 0;
    int alo, ahi, blo, bhi;
    if (!subtree_span(rects, node, 0, dir, &alo, &ahi)) return 0;
    if (!subtree_span(rects, node, 1, dir, &blo, &bhi)) return 0;
    /* 这个节点自己的总跨度（含中间那 1 格分隔线）—— 分母必须用它，不是屏幕上
     * 「锚点 + 右邻」的宽度。 */
    int total = (ahi - alo) + 1 + (bhi - blo);
    if (total <= 1) return 0;
    int span = total - 1;                 /* layout_rec 里真正参与分配的格数 */
    int pct = ((mouse_pos - alo) * 100) / span;
    /* 夹到两侧都不小于最小尺寸：只夹 5..95 是不够的 —— 窗格小的时候 5% 就是
     * 0 行/0 列，一路拖到底能把一个终端压没。（2026-09-20 用户报。）
     * 阈值与 layout_rec 的均分兜底保持一致：span >= 2*min 才谈得上满足最小尺寸。 */
    int min_side = (dir == 'V') ? SPLIT_MIN_COLS : SPLIT_MIN_ROWS;
    if (span >= min_side * 2) {
        int lo = (min_side * 100 + span - 1) / span;   /* ceil  -> left  >= min_side */
        int hi = ((span - min_side) * 100) / span;     /* floor -> right >= min_side */
        if (lo < 5) lo = 5;
        if (hi > 95) hi = 95;
        if (lo <= hi) {
            if (pct < lo) pct = lo;
            if (pct > hi) pct = hi;
        }
    }
    if (pct < 5) pct = 5;
    if (pct > 95) pct = 95;
    *out_pct = pct;
    return 1;
}

/* ---------------------------------------------------------------------------
 * 标签页 <-> 树根注册表 + 高层操作。
 *
 * 每个标签页由它的「锚点 pane」（创建该 tab 的第一个 pane，出现在标签栏）
 * 标识，持有一棵分屏树的根。分屏新增的 pane 不是锚点（不在标签栏单独成 tab，
 * 通过 pane.is_split_child 标记，由 render/input 处理）。
 * 注册表用固定数组：tab_root_anchor[k] = 锚点 pane，tab_root_node[k] = 树根节点。
 * ------------------------------------------------------------------------- */
#include "pane.h"

/* g_tab_anchor / g_tab_root 定义在文件顶部（纯布局段之前），split_reset 初始化。 */

void split_init_tab(int anchor_pane) {
    /* 已注册（作为锚点或某 tab 树里的 pane）则不重复建。 */
    if (split_tab_of_pane(anchor_pane) >= 0) return;
    for (int i = 0; i < MAX_PANES; i++) {
        if (g_tab_anchor[i] < 0) {
            g_tab_anchor[i] = anchor_pane;
            g_tab_root[i] = split_new_leaf(anchor_pane);
            return;
        }
    }
}

int split_root_for_tab(int anchor_pane) {
    for (int i = 0; i < MAX_PANES; i++)
        if (g_tab_anchor[i] == anchor_pane) return g_tab_root[i];
    return -1;
}

void split_forget_pane(int pane) {
    (void)pane;
    /* tab 锚点在 close_pane 后由高层 split_close_active_pane / 切 tab 逻辑处理；
     * 这里不直接删除注册项（关闭分屏子 pane 不改锚点）。 */
}

int split_tab_of_pane(int pane) {
    /* 返回该 pane 所属标签页的锚点 pane：在该锚点的树里能找到它。 */
    for (int i = 0; i < MAX_PANES; i++) {
        if (g_tab_anchor[i] < 0) continue;
        if (g_tab_anchor[i] == pane) return pane;
        if (split_find_leaf(g_tab_root[i], pane) >= 0) return g_tab_anchor[i];
    }
    return -1;
}

static void set_tab_root(int anchor, int root) {
    for (int i = 0; i < MAX_PANES; i++)
        if (g_tab_anchor[i] == anchor) { g_tab_root[i] = root; return; }
}

/* 把 pane 从它所属 tab 的分屏树中摘除（多叶子树）。返回 1 发生了树收缩；
 * *survivor 回填存活兄弟（视觉次序第一个叶子）。处理锚点提升。 */
static int remove_pane_from_tree(int pane, int *survivor) {
    if (survivor) *survivor = -1;
    int anchor = split_tab_of_pane(pane);
    if (anchor < 0) return 0;
    int slot = -1;
    for (int i = 0; i < MAX_PANES; i++)
        if (g_tab_anchor[i] == anchor) { slot = i; break; }
    if (slot < 0) return 0;
    int root = g_tab_root[slot];
    if (root < 0 || split_count_leaves(root) < 2) return 0;   /* 单叶子：整 tab 关闭 */
    int leaf = split_find_leaf(root, pane);
    if (leaf < 0) return 0;
    int removed = -1;
    int new_root = split_remove_leaf(root, leaf, &removed);
    (void)removed;
    g_tab_root[slot] = new_root;

    /* 选存活兄弟（视觉次序第一个叶子）接管焦点 / 锚点。 */
    int surv = split_first_leaf(new_root);
    if (survivor) *survivor = surv;

    /* 关掉的是锚点 pane：把存活兄弟提升为新锚点，保证标签页不从标签栏消失。 */
    if (pane == anchor && surv >= 0 && surv < g_mux.pane_count) {
        g_mux.panes[surv].is_split_child = 0;
        g_tab_anchor[slot] = surv;
    }
    g_mux.needs_redraw = 1;
    return 1;
}

int split_remove_pane(int pane, int *survivor) {
    return remove_pane_from_tree(pane, survivor);
}

int split_active_root(void) {
    if (g_mux.active_pane < 0) return -1;
    int anchor = split_tab_of_pane(g_mux.active_pane);
    if (anchor < 0) {
        /* 活动 pane 还没注册成 tab（兼容：直接以它为锚点建一棵单叶子树）。 */
        split_init_tab(g_mux.active_pane);
        anchor = g_mux.active_pane;
    }
    return split_root_for_tab(anchor);
}

int split_is_split(void) {
    int root = split_active_root();
    return root >= 0 && split_count_leaves(root) >= 2;
}

int split_tab_panes(int anchor, int *out, int max) {
    if (!out || max <= 0) return 0;
    int root = split_root_for_tab(anchor);
    if (root < 0) {
        /* 锚点没有分屏树（关于/设置页等）：单独成一段。 */
        out[0] = anchor;
        return 1;
    }
    int leaves[MAX_PANES];
    int n = collect_leaves(g_split_nodes, root, leaves, MAX_PANES, 0);
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        int p = leaves[i];
        if (p >= 0 && p < g_mux.pane_count && g_mux.panes[p].active)
            out[cnt++] = p;
    }
    if (cnt == 0) { out[0] = anchor; return 1; }
    return cnt;
}

int split_split_active(int dir, int new_pane) {
    if (new_pane < 0 || new_pane >= g_mux.pane_count) return 0;
    int root = split_active_root();
    if (root < 0) return 0;
    int leaf = split_find_leaf(root, g_mux.active_pane);
    if (leaf < 0) return 0;

    /* 空间不足检查：算出活动窗格当前矩形，若按该方向切分后两边放不下最小尺寸
     * （左右切需要 2*SPLIT_MIN_COLS+1 列、上下切需要 2*SPLIT_MIN_ROWS+1 行），
     * 不进行分割并提示（v1.8.43：窗格太小时再分割应警告、且最终不分）。 */
    {
        PaneRect rects[MAX_PANES];
        for (int i = 0; i < MAX_PANES; i++) rects[i].valid = 0;
        split_layout(root, 0, 0, g_mux.host_cols, g_mux.host_rows, g_split_nodes, rects);
        int ap = g_mux.active_pane;
        if (rects[ap].valid) {
            if (dir == SPLIT_V) {
                if (rects[ap].cols < SPLIT_MIN_COLS * 2 + 1) {
                    toast_show("窗格太窄，无法左右切分", 2200);
                    return 0;
                }
            } else {
                if (rects[ap].rows < SPLIT_MIN_ROWS * 2 + 1) {
                    toast_show("窗格太矮，无法上下切分", 2200);
                    return 0;
                }
            }
        }
    }

    /* 标记新 pane 为分屏子窗格（不单独出现在标签栏）。 */
    g_mux.panes[new_pane].is_split_child = 1;
    /* split_do 把活动叶子【原地】变成内部节点（节点下标不变），整棵树的根索引
     * 不会变。第一次切分时活动叶子就是根，之后的切分作用在嵌套叶子上，split_do
     * 返回的是那个子树节点——绝不能拿它覆盖标签根，否则真正的根丢失，新窗格不在
     * 同一棵树里（表现为「一个标签打不开 3 个及以上窗格」）。标签根始终是这里
     * 切分前取得的 root。 */
    int did = split_do(leaf, dir, new_pane);
    if (did < 0) { g_mux.panes[new_pane].is_split_child = 0; return 0; }
    int anchor = split_tab_of_pane(g_mux.active_pane);
    if (anchor >= 0) set_tab_root(anchor, root);
    /* 焦点交给新窗格。 */
    g_mux.active_pane = new_pane;
    g_mux.panes[new_pane].scroll_offset = 0;
    g_mux.needs_redraw = 1;
    return 1;
}

int split_close_active_pane(int *survivor) {
    return split_remove_pane(g_mux.active_pane, survivor);
}
