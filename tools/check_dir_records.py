#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 cascade_probe 末态里 dir 记录有没有被【拆开】（bug #18，2026-09-17）。

用法:  check_dir_records.py <probe 可执行文件> <字节流> <start_cols> <rows> [env K=V ...]

判据（都是「多余空格」的直接指纹，不依赖具体文件名）:
  A  某行内容形如  <日期> ... <DIR> + 只有空格          -> <DIR> 记录被拦腰拆开
  B  某行 w=0（自成一条逻辑行）且内容非空但全是空格      -> 渲染出来就是一条空行
     （w=1 的纯空格行是长记录折行后的尾部填充，属于同一条逻辑行，不算）
  C  某行以 >=2 个空格开头、且上一行以 <DIR>+空格 结尾    -> 拆开的下半截（名字行）

为什么级联判据（一行里 >=2 个日期）不够：拆开后每行最多一个日期，看起来跟正常
折行一模一样，A/B/C 三变体全 PASS —— 必须直接看末态逐行。
"""
import os
import re
import subprocess
import sys

# 末态行格式:  "   -20: w=0 used=48 |内容|"
ROW = re.compile(r'^\s*([+-][0-9]+):\s*w=([01])\s*used=\s*([0-9]+)\s*\|(.*)\|$')
DIR_TAIL = re.compile(r'<DIR> *$')
# 日期分隔符【必须同时接受 / 和 -】：真机 dir 输出是 2026/09/17（字节 0x2f），
# 只写 - 会让 A 判据永远不触发（本次踩到：A 静默失效，只剩 C 在报）。
DATE_HEAD = re.compile(r'^[0-9]{4}[/-][0-9]{2}[/-][0-9]{2}')


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    probe, stream, cols, rows = sys.argv[1:5]
    env = dict(os.environ)
    for kv in sys.argv[5:]:
        k, _, v = kv.partition('=')
        env[k] = v
    env['DUMP_FINAL'] = '1'

    out = subprocess.run([probe, stream, cols, rows], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = out.stdout.decode('utf-8', 'replace')

    parsed = []
    for line in text.split('\n'):
        m = ROW.match(line)
        if m:
            parsed.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)))
    if len(parsed) < 5:
        print('  [FAIL] 末态逐行没解析出来（只拿到 %d 行），判据无法运行' % len(parsed))
        print(text[-800:])
        return 2

    bad = []
    for i, (rel, w, used, content) in enumerate(parsed):
        if content and content.strip() == '' and w == 0:
            bad.append((rel, 'B 纯空格行', content))
            continue
        if DATE_HEAD.match(content) and DIR_TAIL.search(content):
            bad.append((rel, 'A <DIR> 后被拆开', content))
            continue
        if i > 0 and content.startswith('  ') and content.strip():
            prev = parsed[i - 1][3]
            if DIR_TAIL.search(prev):
                bad.append((rel, 'C 拆开的名字行', content))

    for line in text.split('\n'):
        if line.startswith('[PASS]') or line.startswith('[FAIL]'):
            print('  probe: ' + line)

    if bad:
        print('  [FAIL] 末态有 %d 处 dir 记录被拆开 / 多余空格:' % len(bad))
        for rel, kind, content in bad[:12]:
            print('    rel=%s %s |%s|' % (rel, kind, content))
        return 1
    print('  [ok] 末态 %d 行，没有 <DIR> 被拆开、没有纯空格行' % len(parsed))
    return 0


if __name__ == '__main__':
    sys.exit(main())
