#!/bin/sh
# 级联拼接回归（bug #16，2026-09-17）。
#
# 症状：拖动分屏（窄 -> 宽）松手后，历史区一整段 dir 记录被并成一条逻辑行、
# 按新宽度重折 —— 内容顺序完好、字节不丢，但词被从中间切开
# （Mu|sic、Saved Games20|26-07-25、.vs|code）。
#
# 根因：bug #11 加的「ConPTY 窄屏行内续写」CUP 判定把 line_wrap 标在了
# 【被续写的那一行】上，而 line_wrap[r]=1 的语义是「r 是 r-1 的续行」。
# 真机一次会话里误命中 47 次 => 24 列下整份 dir 列表被并成一条 1484 字符的
# 逻辑行。字节证据见 analysis/拖动错乱-根因与三种可选行为.md 第 11 节。
#
# 两条真机字节流都用【从 screen_resize_trace.log 推出的真实宽度链】回放：
#   v1（2026-09-15）：59 -> 19 -> 70     也是 bug #15 的回归
#   v2（2026-09-17）：59 -> 24 -> 82     本次的级联
# SPLIT 是宽度切换点在字节流里的偏移：v2 的 771 = dir 命令的位置
# （trace 显示 59->24 时 hist=0、height=7，即拖窄发生在敲 dir 之前）。
set -e
cd "$(dirname "$0")/.."

BIN=/tmp/verify_cascade_probe
gcc -O1 -Itests/stub -Iinclude tests/cascade_probe.c \
    src/screen.c src/vt.c src/utf8.c src/theme.c -o "$BIN"

V1=tests/fixtures_pane0_stream.bin
V2=tests/fixtures_pane0_stream_v2.bin
V3=tests/fixtures_pane0_stream_v3.bin
V6=tests/fixtures_pane0_stream_v6.bin
fail=0

# NEEDLES：这些记录必须在末态的【同一行】里完整出现。
# 级联判据（一行里 >=2 个日期）只能抓「并起来」，抓不到「拆开」——拆开后每行最多
# 一个日期，看起来跟正常折行一样。2026-09-17 用户报的「有些地方多出了空格」正是
# 拆开：`2026-09-07  12:50    <DIR>` + 一行纯空格（7 个）+ `   arena`，
# 26+7+8=41 正好等于真实记录长度，即按 33 列折的行拖宽后没被并回去。
NDL="termux_dump.log,typst-latex-studio.html,luogu-markdown-editor-1.2.19.vsix,Win11_25H2_Chinese_Simplified_x64_v2.iso,liquidbounce b100.jar,Ext2Fsd v0.68"

echo "=== 1a) v2 真机链 59 -> 24 -> 82：级联判据，全 SPLIT 扫描 ==="
for k in 760 765 771 776 780 1000 1600 2400 3200 4000; do
    r=$(SPLIT=$k MID=24 SEQ=82 "$BIN" "$V2" 59 29 2>&1 | tail -1)
    case "$r" in
        *PASS*) echo "  [ok] SPLIT=$k" ;;
        *) echo "  [FAIL] SPLIT=$k -> $r"; fail=1 ;;
    esac
done

# NEEDLES 只在【忠实切点】上有意义：SPLIT 是宽度切换点在字节流里的偏移，选错了
# 等于把记录从中间截断喂进不同宽度，末态必然碎，那是在测我的切点不是在测代码。
# 忠实区间 = 771..780（dir 命令回显之后、输出之前）。
echo "=== 1b) v2 忠实切点：NEEDLES 判据（记录不得被拆开）==="
for k in 771 776 780; do
    r=$(NEEDLES="$NDL" SPLIT=$k MID=24 SEQ=82 "$BIN" "$V2" 59 29 2>&1 | tail -1)
    case "$r" in
        *PASS*) echo "  [ok] SPLIT=$k" ;;
        *) echo "  [FAIL] SPLIT=$k -> $r"; fail=1 ;;
    esac
done

echo "=== 2) v2 其它宽度链（来回拖动 / 只窄 / 只宽）==="
for seq in "24,82,24,82" "24" "82" "59,24,82,37" "24,82,120"; do
    r=$(SEQ=$seq "$BIN" "$V2" 59 29 2>&1 | tail -1)
    case "$r" in
        *PASS*) echo "  [ok] SEQ=$seq" ;;
        *) echo "  [FAIL] SEQ=$seq -> $r"; fail=1 ;;
    esac
done

echo "=== 3) v1 真机链 59 -> 19 -> 70（bug #15 不回归）==="
for k in 0 1500 2500 3500 3800 4000 4200 4400 4600 4800 5000 5300; do
    r=$(SPLIT=$k MID=19 SEQ=70 "$BIN" "$V1" 59 32 2>&1 | tail -1)
    case "$r" in
        *PASS*) echo "  [ok] SPLIT=$k" ;;
        *) echo "  [FAIL] SPLIT=$k -> $r"; fail=1 ;;
    esac
done

# bug #18（2026-09-17，用户第三次报「多余空格」）：conhost 折行续写的【形态 (2)】
#   <写满一行> ESC[<末行>;<越界列>H CR LF <剩余内容>          （真机偏移 1589/1899/3258）
# 那个 CR LF 在底行触发整屏滚动，screen_scroll_up 把滚出来的新底行 line_wrap 清 0，
# 于是记录被拆成两半、名字那半前面挂一串空格。级联判据和 NEEDLES 都抓不到它
# （拆开后每行最多一个日期），必须直接看末态逐行 —— 见 tools/check_dir_records.py。
echo "=== 3b) dir 记录不得被拆开（末态逐行判据）==="
for k in 771 776 780; do
    if python3 ../tools/check_dir_records.py "$BIN" "$V2" 59 29 \
            SPLIT=$k MID=24 SEQ=82 > /tmp/dircheck.txt 2>&1; then
        sed 's/^/  /' /tmp/dircheck.txt; echo "  [ok] SPLIT=$k"
    else
        sed 's/^/  /' /tmp/dircheck.txt; echo "  [FAIL] SPLIT=$k"; fail=1
    fi
done

# v1（2026-09-15）流的忠实切点 = 1247：偏移 1247 起 conhost 才开始发 19 列排版的
# 字节（第一个 ESC[31;19H），之前是 59 列时代。修复前这个切点上末态有 9 处问题
# （4 条 <DIR> 记录被拆成「<DIR> 行 + 一行纯空格 + 名字行」三行，正是用户 2026-09-17
# 晚上报的「还是有多余空格」），修复后 0 处。
echo "=== 3c) v1 忠实切点：dir 记录不得被拆开 ==="
for k in 1247 1260 1280 1300; do
    if python3 ../tools/check_dir_records.py "$BIN" "$V1" 59 32 \
            SPLIT=$k MID=19 SEQ=70 > /tmp/dircheck.txt 2>&1; then
        sed 's/^/  /' /tmp/dircheck.txt; echo "  [ok] SPLIT=$k"
    else
        sed 's/^/  /' /tmp/dircheck.txt; echo "  [FAIL] SPLIT=$k"; fail=1
    fi
done

# v3 = 2026-09-17 14:10 那批（用户装 v15 后仍报「多余空格」时抓的）。
# 真机 resize 链从 screen_resize_trace.log 读出：两个窗格交错，共 16 帧；带滚动
# 历史（hist 244 -> 57 -> 51）的那条链是 120 -> 59 -> 19 -> 80 -> 103，就是 pane0。
# SPLIT/MID 只能表达一步，喂错宽度等于在测切点，所以这里用 STEPS 做多步交错回放：
# 偏移 212=conhost 自报 59 列(ESC[8;29;59t)、1153=开始发 19 列排版、
# 7589/8238=两次 ESC[H 整屏重绘（最长文本段 80 / 73 列）。
echo "=== 3d) v3 多步交错回放（真机链 120->59->19->80->103）==="
for st in "212:59,1153:19,7589:80,8238:103" "212:59,1146:19,7589:80,8238:103" \
          "0:59,1153:19,7589:80,8238:103"; do
    if python3 ../tools/check_dir_records.py "$BIN" "$V3" 120 29 \
            STEPS="$st" > /tmp/dircheck.txt 2>&1; then
        sed 's/^/  /' /tmp/dircheck.txt; echo "  [ok] STEPS=$st"
    else
        sed 's/^/  /' /tmp/dircheck.txt; echo "  [FAIL] STEPS=$st"; fail=1
    fi
done

echo "=== 4) 探测器自检（对已知坏代码必须报 FAIL，否则上面的 PASS 不可信）==="
TMPVT=/tmp/vt_cup_regress.c
python3 ../tools/make_cup_regress_vt.py src/vt.c "$TMPVT"
BADBIN=/tmp/verify_cascade_probe_bad
gcc -O1 -Itests/stub -Iinclude tests/cascade_probe.c \
    src/screen.c "$TMPVT" src/utf8.c src/theme.c -o "$BADBIN"
r=$(SPLIT=771 MID=24 SEQ=82 "$BADBIN" "$V2" 59 29 2>&1 | tail -1)
case "$r" in
    *FAIL*) echo "  [ok] 把 CUP 置 1 规则加回去后探测器报 FAIL（判据有效）" ;;
    *) echo "  [FAIL] 探测器对已知坏代码没报 FAIL"; fail=1 ;;
esac

echo "=== 5) reanchor 自检（快照丢 wrap 标志的坏变体必须报 FAIL）==="
TMPSC=/tmp/screen_reanchor_regress.c
python3 ../tools/make_reanchor_regress_screen.py src/screen.c "$TMPSC"
BADBIN2=/tmp/verify_cascade_probe_bad2
gcc -O1 -Itests/stub -Iinclude tests/cascade_probe.c \
    "$TMPSC" src/vt.c src/utf8.c src/theme.c -o "$BADBIN2"
r=$(NEEDLES="$NDL" SPLIT=771 MID=24 SEQ=82 "$BADBIN2" "$V2" 59 29 2>&1 | tail -1)
case "$r" in
    *FAIL*) echo "  [ok] 把 reanchor 恢复标志改回写死清 0 后报 FAIL（NEEDLES 判据有效）" ;;
    *) echo "  [FAIL] NEEDLES 判据对已知坏代码没报 FAIL"; fail=1 ;;
esac
rm -f "$TMPSC" "$BADBIN2"

echo "=== 6) bug#18 自检（去掉新底行续行规则后，末态判据必须报 FAIL）==="
TMPVT2=/tmp/vt_eolcont_regress.c
python3 ../tools/make_eolcont_regress_vt.py src/vt.c "$TMPVT2"
BADBIN3=/tmp/verify_cascade_probe_bad3
gcc -O1 -Itests/stub -Iinclude tests/cascade_probe.c \
    src/screen.c "$TMPVT2" src/utf8.c src/theme.c -o "$BADBIN3"
if python3 ../tools/check_dir_records.py "$BADBIN3" "$V2" 59 29 \
        SPLIT=771 MID=24 SEQ=82 > /tmp/eolcont_selfcheck.txt 2>&1; then
    echo "  [FAIL] 去掉修复后判据仍然 PASS —— 判据无效"
    cat /tmp/eolcont_selfcheck.txt
    fail=1
else
    echo "  [ok] 去掉修复后判据报 FAIL（末态判据有效）"
fi
rm -f "$TMPVT2" "$BADBIN3" /tmp/eolcont_selfcheck.txt /tmp/dircheck.txt

echo "=== 7) CSI 自检（把「CSI 一律作废判定窗口」改回去后，v3 必须报 FAIL）==="
TMPVT3=/tmp/vt_csi_regress.c
python3 ../tools/make_csi_regress_vt.py src/vt.c "$TMPVT3"
BADBIN4=/tmp/verify_cascade_probe_bad4
gcc -O1 -Itests/stub -Iinclude tests/cascade_probe.c \
    src/screen.c "$TMPVT3" src/utf8.c src/theme.c -o "$BADBIN4"
if python3 ../tools/check_dir_records.py "$BADBIN4" "$V3" 120 29 \
        STEPS="212:59,1153:19,7589:80,8238:103" > /tmp/csicheck.txt 2>&1; then
    echo "  [FAIL] 改回无条件作废后判据仍然 PASS —— 判据无效"
    cat /tmp/csicheck.txt
    fail=1
else
    echo "  [ok] 改回无条件作废后 v3 报 FAIL（判据有效）"
fi
rm -f "$TMPVT3" "$BADBIN4" /tmp/csicheck.txt

# v6 = 2026-09-20 那批（用户装 v18B 后报「左边窗格历史截断」时抓的）。
# 真机 resize 链从 screen_resize_trace.log 读出：左窗格 59->67->36->61->5->92，
# 右窗格 60->52->83->58->114->27。字节偏移取自 termux_dump.log 的 pane0 分块：
#   206=conhost 自报 59 列(ESC[8;29;59t)、2644/4179/4993=三次 ESC[H 整屏重绘、
#   5884=拖到 5 列后紧接着拖回 92 列（这两步之间没有字节，所以偏移相同）。
# 判据：整份 dir 列表一条不能少 + 提示符必须正好落在最下面一行。
V6STEPS="206:59,2644:67,4179:36,4993:61,5884:5,5884:92"
V6NDL="better_explorer.exe,conpty_source.log,dwm-topmost-x64.zip,EXPR.exe,liquidbounce b100.jar,luogu-markdown-editor-1.2.19.vsix,OpenArk64.exe,release_v18.zip,typst-latex-studio.html"
echo "=== 8) v6 两趟 resize 重绘（真机链 120->59->67->36->61->5->92）==="
if python3 ../tools/check_prompt_bottom.py "$BIN" "$V6" 120 29 \
        STEPS="$V6STEPS" NEEDLES="$V6NDL" > /tmp/v6check.txt 2>&1; then
    sed 's/^/  /' /tmp/v6check.txt; echo "  [ok] STEPS=$V6STEPS"
else
    sed 's/^/  /' /tmp/v6check.txt; echo "  [FAIL] STEPS=$V6STEPS"; fail=1
fi

echo "=== 9) 两趟重绘自检（把修复撤回后，v6 必须报 FAIL）==="
TMPVT4=/tmp/vt_repass_regress.c
python3 ../tools/make_repass_regress_vt.py src/vt.c "$TMPVT4"
BADBIN5=/tmp/verify_cascade_probe_bad5
gcc -O1 -Itests/stub -Iinclude tests/cascade_probe.c \
    src/screen.c "$TMPVT4" src/utf8.c src/theme.c -o "$BADBIN5"
if python3 ../tools/check_prompt_bottom.py "$BADBIN5" "$V6" 120 29 \
        STEPS="$V6STEPS" NEEDLES="$V6NDL" > /tmp/v6self.txt 2>&1; then
    echo "  [FAIL] 撤回修复后判据仍然 PASS —— 判据无效"
    cat /tmp/v6self.txt
    fail=1
else
    echo "  [ok] 撤回修复后 v6 报 FAIL（判据有效）"
    grep '  \[FAIL\]' /tmp/v6self.txt | head -3 | sed 's/^/      /'
fi
rm -f "$TMPVT4" "$BADBIN5" /tmp/v6check.txt /tmp/v6self.txt

rm -f "$TMPVT" "$BADBIN" "$BIN"

if [ "$fail" -ne 0 ]; then
    echo
    echo "[FAIL] 级联拼接回归未通过"
    exit 1
fi
echo
echo "[PASS] 级联拼接回归全部通过（v1/v2/v3/v6 四条真机链 + 末态判据 + 五个自检）"
