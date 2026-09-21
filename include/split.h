#ifndef WIN_TERMUX_SPLIT_H
#define WIN_TERMUX_SPLIT_H

/* ---------------------------------------------------------------------------
 * 分屏（split panes）：在一个标签页内把终端区域切成多个可见窗格。
 *
 * 分屏树（SplitNode）：叶子节点 leaf=1 持有一个 pane 索引（pane_idx，指向
 * g_mux.panes[]）；内部节点 leaf=0 按 dir（横向=上下分 / 纵向=左右分）与
 * fraction 把子矩形分给 a/b 两个孩子。两个孩子之间留 1 行/列画分隔边框。
 *
 * 设计要点（低风险）：
 *  - 分屏节点池是固定数组（g_split_nodes[]），每个标签页一棵树，树的根
 *    节点下标存在 tab 上；切标签页只是换 root，panes 数组本身不变。
 *  - split_layout() 是【纯函数】：只根据树 + 外接矩形算出每个叶子 pane 的
 *    屏幕矩形（PaneRect），不碰 Win32 / g_mux，便于在 Linux 上单测。
 *  - 单窗格（无分屏）时本模块完全不介入，走原来的整屏渲染路径。
 * ------------------------------------------------------------------------- */

#include "common.h"

#define MAX_SPLIT_NODES 32
#define SPLIT_MIN_COLS  4    /* 每个窗格至少 4 列 */
#define SPLIT_MIN_ROWS  3    /* 每个窗格至少 3 行。原来是 2：上下切分只要 5 行
                              * 就放行，切完每格 2 行基本没法用（2026-09-20 用户报
                              * 「太矮的时候，不应该可以拆分」）。改成 3 后上下切分
                              * 需要 2*3+1 = 7 行。 */

/* 分隔方向：SPLIT_H = 水平分隔线 -> 上下两个窗格；SPLIT_V = 垂直分隔线 -> 左右。 */
enum { SPLIT_H = 0, SPLIT_V = 1 };

typedef struct SplitNode SplitNode;
struct SplitNode {
    int used;      /* 1 = 节点已分配 */
    int leaf;      /* 1 = 叶子（持有 pane），0 = 内部（持有孩子） */
    int pane_idx;  /* 叶子：g_mux.panes[] 下标 */
    int dir;       /* 内部：SPLIT_H / SPLIT_V */
    int frac_pct;  /* 内部：第一个孩子占的百分比 5..95（默认 50） */
    int a, b;      /* 内部：孩子节点下标（a=上/左，b=下/右） */
    int parent;    /* 父节点下标，-1 = 根 */
};

/* 一个叶子窗格在【终端内容区】（不含第 0 行标签栏）的像素/字符矩形。
 * 行列均为 0 基、相对内容区（内容区第 0 行 = 终端第 2 行/1 基）。
 * c0/r0/cols/rows 是【内容矩形】（相对分配区域四周内缩 1 格，窗格内容与分隔线
 * 之间留 1 格空白）；oc0/or0/ocols/orows 是【外接分配矩形】（分隔线位置、鼠标
 * 边框命中、窗格点选用外接边界判定）。 */
typedef struct {
    int c0, r0;   /* 内容矩形左上角（列, 行），内容区 0 基 */
    int cols;     /* 内容宽（列数） */
    int rows;     /* 内容高（行数） */
    int oc0, or0; /* 外接分配矩形左上角 */
    int ocols, orows; /* 外接分配宽/高 */
    int valid;    /* 1 = 该 pane 本帧可见 */
} PaneRect;

/* ---- 节点池生命周期（split.c，会读写 g_split_*，其余布局函数纯） ---- */
void split_reset(void);
SplitNode *split_nodes(void);                      /* 全局节点池（生产路径传给 split_layout） */
int  split_new_leaf(int pane_idx);                 /* 分配一个叶子，返回节点下标，-1 失败 */
int  split_count_leaves(int root);
int  split_first_leaf(int root);
/* 找 pane_idx 所在叶子节点；返回节点下标，-1 表示不在树里。 */
int  split_find_leaf(int root, int pane_idx);

/* 在 node 处切分：新建持有 new_pane_idx 的叶子作为兄弟，原叶子【原地】变成
 * 内部节点（节点下标不变）。dir=SPLIT_H（上下）/ SPLIT_V（左右）。返回内部
 * 节点下标（== node），失败 -1。 */
int  split_do(int node, int dir, int new_pane_idx);

/* 删除叶子节点：其父收缩，兄弟顶替父的位置。返回顶替上来的节点下标（树根可能
 * 变），整树删光返回 -1。*removed_pane 回填被删 pane 索引。 */
int  split_remove_leaf(int root, int node, int *removed_pane);

/* ---- 标签页 <-> 树根注册表（split.c 维护，随 pane 创建/关闭调用） ---- */
void split_init_tab(int anchor_pane);          /* 新标签页：单叶子树根 */
int  split_root_for_tab(int anchor_pane);      /* 取某标签页（锚点 pane）的树根 */
void split_forget_pane(int pane);              /* pane 关闭后清理注册表 */
/* 关闭/切换后，用当前活动 pane 反查它属于哪个标签锚点（返回锚点 pane）。 */
int  split_tab_of_pane(int pane);

/* 高层操作（针对当前活动标签页）：
 * split_split_active：在活动 pane 上切分，new_pane 已创建。返回 1 成功。
 * split_close_active_pane：关闭活动 pane；若为 tab 内唯一 pane 返回 0（走关 tab），
 *   否则树收缩并把焦点交给 *survivor，返回 1。
 * split_active_root：当前活动 tab 的树根（无分屏也返回单叶子根），-1 无。 */
int  split_split_active(int dir, int new_pane);
int  split_close_active_pane(int *survivor);
int  split_active_root(void);
/* 当前活动 tab 是否真正分了屏（叶子数 >= 2）。 */
int  split_is_split(void);
/* 枚举某标签页（锚点 pane）分屏树里所有存活 pane，按视觉左->右/上->下次序填入
 * out（最多 max 个），返回数量。无分屏树时仅返回锚点自己。标签栏据此把一个分屏
 * 标签画成连排的多个 [pane] 段，可直接点标签切换窗格。 */
int  split_tab_panes(int anchor, int *out, int max);
/* 统一的「某 pane 即将关闭」处理：若它在某棵多叶子分屏树里，就把它的叶子从树中
 * 摘除、树收缩；若关掉的恰好是该 tab 的锚点 pane，则把存活的兄弟提升为新锚点
 * （清掉 is_split_child，保证标签页不丢）。*survivor 回填一个存活兄弟（用于接管
 * 焦点），返回 1 表示发生了树收缩（调用方不应再把它当普通整 tab 关闭）。
 * 单叶子树（独立标签页/关于/设置）不在树里，返回 0，走原有关闭流程。 */
int  split_remove_pane(int pane, int *survivor);

int  split_neighbor_pane(int root, int from_pane, char where);
int  split_next_pane(int root, int from_pane, int forward);
int  split_resize_pane(int root, int from_pane, char where, int delta_pct);
/* 直接把锚点 pane 所在某方向分隔的 frac 设为 pct（鼠标拖边框用）。 */
void split_resize_set_frac(int root, int anchor_pane, char dir, int pct);

/* ---- 拖动分隔条的定位（2026-09-20，bug #27） --------------------------------
 *
 * 「先左右分 -> 左半上下分三格 -> 中间那格再左右分」之后，拖中间格的【右侧】
 * 分隔线，被拖动的却是里层那条（中间格自己的 左|右），而且跳一下。两个原因
 * 叠在一起：
 *
 *  1) 改错了节点。拖动路径原来把锚点 pane 直接交给 split_resize_set_frac()，
 *     而它取的是锚点向上【第一个】方向匹配的祖先。嵌套分屏下中间格的右邻落在
 *     那个祖先的子树之外 —— 用户抓的那条线其实属于【更外层】的祖先。
 *  2) 分母对不上。百分比原来按「锚点外接宽 + 1 + 屏幕上的右邻宽」算，而 frac
 *     是按【被改的那个节点】的整棵子树宽度解释的。两者只有在锚点与右邻恰好是
 *     同一个节点的直接兄弟时才一致。
 *
 * 修法：先按「a/b 子树的分界线正好等于用户抓的那条线」挑出正确节点
 * （split_drag_pick_node），再按那个节点自己的子树跨度算百分比
 * （split_drag_pct），最后直接设该节点（split_set_frac_node）。
 * 下面几个都是纯函数（只读节点池 + 传进来的外接矩形），不碰 Win32 / g_mux。
 * -------------------------------------------------------------------------- */

/* 枚举 node 某一侧子树里的叶子 pane 下标。side: 0 = a（上/左），1 = b（下/右）。
 * 返回个数；node 不是内部节点时返回 0。 */
int  split_subtree_leaves(int node, int side, int *out, int cap);

/* 从 anchor 的叶子向上收集所有【分隔方向匹配】的祖先，由内向外写进 out
 * （out[0] 最靠近叶子）。返回个数。 */
int  split_dir_ancestors(int root, int anchor_pane, char dir, int *out, int cap);

/* 直接设某个节点的 frac_pct（a 子树占比）。anchor 落在 b 子树时自动取 100-pct，
 * 与 split_resize_set_frac() 的语义一致。 */
void split_set_frac_node(int node, int anchor_pane, int pct);

/* 抓到 grab_pane 的右沿（dir='V'）/底沿（dir='H'），而那条线就在 grab_pos 这一
 * 列/行时，返回【应该被拖动】的节点下标；找不到返回 -1。 */
int  split_drag_pick_node(const PaneRect *rects, int root, int grab_pane, char dir,
                          int grab_pos);

/* 按 node 自己的子树跨度，把鼠标位置换算成 a 子树占比（已夹 5..95）。
 * mouse_pos = 当前鼠标所在列（'V'）/行（'H'）。返回 1 成功并写 *out_pct。 */
int  split_drag_pct(const PaneRect *rects, int node, char dir, int mouse_pos, int *out_pct);

/* ---- 纯布局：给定树与外接矩形，填每个 pane 的 PaneRect（按 pane 索引存） ---- */
void split_layout(int root, int c0, int r0, int cols, int rows,
                  SplitNode *nodes, PaneRect *rects);

#endif /* WIN_TERMUX_SPLIT_H */
