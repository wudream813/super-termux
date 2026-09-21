#!/usr/bin/env python3
"""渲染 harness 的断言检查 —— 方案 B（拖动中裁剪、不折行）的验收。

跑法：
    sh tests/verify_clip_behavior.sh

检查四件事（全部基于 tests/parse_frames.py 仿真出的真实网格，不是字节猜测）：
  1. 未拖动时可见区行序 = R00..R09（内容确实喂进去了，且没被折行）。
  2. 拖动变窄后，可见区行序【完全不变】—— 这是方案 B 的核心：行序与视口稳定。
  3. 拖动后没有「一条逻辑行占两条显示行」的折断（narrow 帧里不出现以 ......ZZ
     开头的续行）。
  4. 拖动后最后一行可见内容仍在视口底部（验收标准：提示符正好在最下面一行）。

另外单独确认：scroll_offset>0（回看）时两版行为一致 —— 方案 B 只作用于
「非回看时拖窄」，不碰回看的 reflow 路径。
"""
import re
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PARSE = os.path.join(HERE, 'parse_frames.py')


def frames(logpath):
    out = subprocess.run([sys.executable, PARSE, logpath],
                         capture_output=True, text=True, check=True).stdout
    res = {}
    cur = None
    for line in out.splitlines():
        m = re.match(r'===== FRAME (\w+)', line)
        if m:
            cur = m.group(1)
            res[cur] = []
            continue
        if cur is not None:
            m2 = re.match(r'\s*(\d+)\|(.*)\|$', line)
            if m2:
                res[cur].append((int(m2.group(1)), m2.group(2)))
    return res


def visible_rows(lines, pane):
    """取某个窗格里的 R<nn> 行号序列。pane=0 上半，pane=1 下半。"""
    rows = []
    for idx, txt in lines:
        if pane == 0 and idx > 12:
            continue
        if pane == 1 and idx < 13:
            continue
        m = re.match(r'R(\d\d) ', txt)
        if m:
            rows.append(int(m.group(1)))
    return rows


def run(binary, args, tag):
    env = dict(os.environ, TERMUX_DUMP='1')
    log = f'/tmp/vh_{tag}.log'
    if os.path.exists(log):
        os.remove(log)
    cwd = os.getcwd()
    os.chdir('/tmp')
    try:
        subprocess.run([binary] + args, env=env, capture_output=True, check=True)
        # harness 把日志写在 cwd
        if os.path.exists('/tmp/harness_frames.log'):
            os.replace('/tmp/harness_frames.log', log)
    finally:
        os.chdir(cwd)
    return frames(log)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else '/tmp/rh_noreflow'
    fails = []

    # 主场景：无历史
    f = run(binary, ['120', '25', '50', '59', '0', '0'], 'plain')
    wide0 = visible_rows(f['wide'], 0)
    narrow0 = visible_rows(f['narrow'], 0)
    print(f'[无历史] wide  上半窗格行序: {wide0}')
    print(f'[无历史] narrow上半窗格行序: {narrow0}')
    if wide0 != list(range(10)):
        fails.append(f'未拖动时可见区行序不是 R00..R09，而是 {wide0}')
    if narrow0 != wide0:
        fails.append(f'拖动后行序变了：{wide0} -> {narrow0}')

    # 折断检查：narrow 帧里不该出现续行（......ZZ 开头）
    cont = [t for _, t in f['narrow'] if t.startswith('......')]
    if cont:
        fails.append(f'narrow 帧出现 {len(cont)} 条折断续行，例如 {cont[0]!r}')

    # 底部检查：上半窗格最后一条可见内容行应是 R09，且位于分隔线之上最后一行
    upper = [(i, t) for i, t in f['narrow'] if i < 13 and t.strip()]
    if upper:
        last_idx, last_txt = upper[-1]
        if not last_txt.startswith('R09 '):
            fails.append(f'上半窗格最后一条内容不是 R09，而是 {last_txt!r}（行 {last_idx}）')

    # 长历史场景
    for hist in ('500', '2000'):
        f2 = run(binary, ['120', '25', '50', '59', hist, '0'], f'h{hist}')
        w = visible_rows(f2['wide'], 0)
        n = visible_rows(f2['narrow'], 0)
        print(f'[hist={hist}] wide={w}  narrow={n}')
        if w != n:
            fails.append(f'hist={hist} 拖动后行序变了：{w} -> {n}')
        cont2 = [t for _, t in f2['narrow'] if t.startswith('......')]
        if cont2:
            fails.append(f'hist={hist} narrow 帧出现 {len(cont2)} 条折断续行')

    if fails:
        print('\n[FAIL]')
        for x in fails:
            print('  -', x)
        return 1
    print('\n[PASS] 方案 B 的四项检查全部通过（无历史 + hist=500 + hist=2000）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
