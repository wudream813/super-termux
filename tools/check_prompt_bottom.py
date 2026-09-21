#!/usr/bin/env python3
"""末态判据（2026-09-20，bug #21「左窗格历史被截断」）：

  A. NEEDLES 里每条记录都必须在末态的【某一行】里完整出现（抓「整段列表消失」）
  B. 可见区最后一行必须非空（用户的硬要求：有历史时提示符正好在最下面一行、
     更下面没有东西）
  C. 可见区最后一行必须含 BOTTOM_NEEDLE（默认提示符片段）

用法:
  python3 tools/check_prompt_bottom.py <probe> <stream.bin> <cols> <rows> \
      STEPS=... [NEEDLES=a,b,c] [BOTTOM_NEEDLE=Downloads>] [EXTRA_ENV=k=v ...]

退出码 0 = 全过；非 0 = 有判据不满足（并打印实际末态）。
"""
import os
import re
import subprocess
import sys

LF = chr(10)


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    probe, stream, cols, rows = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    env = dict(os.environ)
    needles, bottom = [], "Downloads>"
    for kv in sys.argv[5:]:
        k, _, v = kv.partition("=")
        if k == "NEEDLES":
            needles = [x for x in v.split(",") if x]
        elif k == "BOTTOM_NEEDLE":
            bottom = v
        else:
            env[k] = v
    env["DUMP_FINAL"] = "1"
    env.setdefault("ALIGN", "1")

    p = subprocess.run([probe, stream, cols, rows], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode("utf-8", "replace")

    rows_n = int(rows)
    # 末态逐行："   +28: w=0 used=25 |C:...Downloads>|"
    pat = re.compile(r"^\s*([+-])(\d+): w=\d+ used=\s*(\d+) \|(.*?)\|\s*$")
    vis = {}
    hist = 0
    for line in out.split(LF):
        m = pat.match(line)
        if not m:
            continue
        sign, idx, used, text = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        if sign == "-":
            hist = max(hist, idx)
        else:
            vis[idx] = (used, text)

    bad = []
    if len(vis) != rows_n:
        bad.append("可见区行数=%d，应为 %d（末态解析失败？）" % (len(vis), rows_n))

    # A. NEEDLES
    alltext = [t for _, t in vis.values()]
    # 历史区也算：列表可能刚好被顶进滚动缓冲
    for nd in needles:
        if not any(nd in t for t in alltext):
            bad.append("记录消失：%s 不在可见区任何一行里" % nd)

    # B/C. 提示符必须正好在最后一行
    lu, lt = vis.get(rows_n - 1, (0, ""))
    if lu == 0:
        bad.append("可见区最后一行(rel+%d)是空的 —— 提示符没落到最下面一行" % (rows_n - 1))
    elif bottom and bottom not in lt:
        bad.append("可见区最后一行不是提示符：%r" % lt)

    print("  末态: 历史 %d 行 + 可见区 %d 行；非空可见行 %d 行" %
          (hist, len(vis), sum(1 for u, _ in vis.values() if u > 0)))
    print("  最后一行 rel+%d: %r" % (rows_n - 1, lt))
    if bad:
        for b in bad:
            print("  [FAIL] %s" % b)
        print("  --- 实际末态 ---")
        for i in sorted(vis):
            print("   +%02d used=%2d |%s|" % (i, vis[i][0], vis[i][1]))
        return 1
    print("  [ok] NEEDLES 全在 + 提示符正好在最下面一行、下面没有东西")
    return 0


if __name__ == "__main__":
    sys.exit(main())
