#!/usr/bin/env python3
"""把 render_dump.log 的某一帧真 VT 解码成字符网格。
用法: python3 tools/frame2txt.py <render_dump.log> <帧号> [列数]
帧格式: 每帧以一行 "[frame N host WxH]" 开头（见 src/main.c 的 render dump）。"""
import re, sys

path, want = sys.argv[1], int(sys.argv[2])
data = open(path, 'rb').read()
# 找帧头
heads = [(m.start(), m.group(0)) for m in re.finditer(rb'\[render len \d+ model \d+x\d+ host \d+x\d+\]\n', data)]
if not heads:
    print('找不到帧头，前 200 字节:', data[:200]); sys.exit(1)
print('共 %d 帧' % len(heads), file=sys.stderr)
if want >= len(heads): want = len(heads) - 1
st = heads[want][0]
en = heads[want+1][0] if want+1 < len(heads) else len(data)
print('帧头: %r  字节 %d..%d' % (heads[want][1].decode('latin1').strip(), st, en), file=sys.stderr)
body = data[st:en]
body = body[body.index(b'\n')+1:]

COLS = int(sys.argv[3]) if len(sys.argv) > 3 else None
ROWS = 40
cols = COLS or 200
grid = [[' ']*cols for _ in range(ROWS)]
x = y = 0
i = 0
n = len(body)
def put(ch):
    global x
    if x < cols and 0 <= y < ROWS: grid[y][x] = ch
    x += 1
while i < n:
    b = body[i]
    if b == 0x1b and i+1 < n and body[i+1] == 0x5b:
        j = i+2; params = b''
        while j < n and not (0x40 <= body[j] <= 0x7e): params += body[j:j+1]; j += 1
        if j >= n: break
        fin = body[j:j+1]; j += 1
        # 私有模式序列（ESC[?25h / ESC[<1;2;3M / ESC[>c / ESC[=…）的参数前带私有标记，
        # 不剥掉就会在 int() 上抛 ValueError（2026-09-20 实机日志踩到 ESC[?25h）。
        for _m in (b'?', b'<', b'>', b'='):
            if params.startswith(_m): params = params[len(_m):]
        ps = [int(t) if t else 0 for t in params.split(b';')] if params else [0]
        if fin == b'H' or fin == b'f':
            y = (ps[0] or 1) - 1; x = (ps[1] if len(ps) > 1 and ps[1] else 1) - 1
        elif fin == b'A': y -= ps[0] or 1
        elif fin == b'B': y += ps[0] or 1
        elif fin == b'C': x += ps[0] or 1
        elif fin == b'D': x = max(0, x - (ps[0] or 1))
        elif fin == b'K':
            mode = ps[0] or 0
            rng = range(x, cols) if mode == 0 else (range(0, x+1) if mode == 1 else range(0, cols))
            for xx in rng:
                if 0 <= y < ROWS: grid[y][xx] = ' '
        elif fin == b'J':
            mode = ps[0] or 0
            if mode == 2 or mode == 3:
                grid = [[' ']*cols for _ in range(ROWS)]
        i = j; continue
    if b == 0x1b and i+1 < n and body[i+1] == 0x5d:
        j = i+2
        while j < n and body[j] != 0x07: j += 1
        i = j+1; continue
    if b == 0x1b: i += 2; continue
    if b == 0x0d: x = 0; i += 1; continue
    if b == 0x0a:
        y += 1; i += 1; continue
    if b == 0x08: x = max(0, x-1); i += 1; continue
    # UTF-8 解码一个字符
    ln = 1
    if b >= 0xf0: ln = 4
    elif b >= 0xe0: ln = 3
    elif b >= 0xc0: ln = 2
    try: ch = body[i:i+ln].decode('utf-8')
    except Exception: ch = '?'; ln = 1
    if ch.isprintable() and ch != ' ': put(ch)
    elif ch == ' ': put(' ')
    i += ln
last = max((yy for yy in range(ROWS) if any(c != ' ' for c in grid[yy])), default=-1)
print('# cursor=(%d,%d) 非空行=%d 末非空行=%d' % (x, y, sum(1 for yy in range(ROWS) if any(c != ' ' for c in grid[yy])), last), file=sys.stderr)
for yy in range(0, min(ROWS, last+2)):
    print('%3d|%s' % (yy, ''.join(grid[yy]).rstrip()))
