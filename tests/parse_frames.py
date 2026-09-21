#!/usr/bin/env python3
"""把 harness_frames.log 里的 VT 字节流仿真成字符网格并打印。

为什么必须仿真：渲染输出是带 CUP/SGR 的字节流，按列字典重建会把铺底空格
留在宽字符的次格上，凭空造出「宽字符之间有空白」的假象（本项目已在此栽过
六次）。唯一可靠的做法是老老实实模拟终端：CUP 定位、宽字符占两格（次格写
None 并跳过）。
"""
import re, sys, unicodedata

def wide(ch):
    if ch is None:
        return 1
    return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1

def simulate(data, rows, cols):
    grid = [[' '] * cols for _ in range(rows)]
    r = c = 0
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b == '\x1b':
            if i + 1 < n and data[i+1] == '[':
                j = i + 2
                while j < n and data[j] not in 'ABCDEFGHJKSTfmnsulh':
                    j += 1
                if j >= n:
                    break
                final = data[j]
                params = data[i+2:j]
                if params.startswith('?'):
                    i = j + 1                # 私有序列（?25l 隐藏光标等）：跳过
                    continue
                nums = []
                for x in params.split(';'):
                    if x == '':
                        continue
                    try:
                        nums.append(int(x))
                    except ValueError:
                        nums.append(0)
                if final == 'H':
                    r = (nums[0] - 1) if nums else 0
                    c = (nums[1] - 1) if len(nums) > 1 else 0
                    r = max(0, min(rows - 1, r))
                    c = max(0, min(cols - 1, c))
                elif final == 'm':
                    pass                      # 颜色对内容判定无影响
                elif final == 'K':
                    mode = nums[0] if nums else 0
                    if mode == 0:
                        for x in range(c, cols): grid[r][x] = ' '
                    elif mode == 1:
                        for x in range(0, c + 1): grid[r][x] = ' '
                    elif mode == 2:
                        for x in range(cols): grid[r][x] = ' '
                i = j + 1
                continue
            else:
                i += 2
                continue
        elif b in ('\r', '\n', '\t'):
            i += 1
            continue
        # 普通字符：可能是多字节 UTF-8
        try:
            ch = data[i]
        except IndexError:
            break
        i += 1
        if 0 <= r < rows and 0 <= c < cols:
            w = wide(ch)
            grid[r][c] = ch
            if w == 2 and c + 1 < cols:
                grid[r][c+1] = None          # 宽字符次格：占位，不可写
                c += 1
        c += 1
    return grid

def render_grid(grid):
    """宽字符的次格（None）不补空格 —— 补了会造成「。 。 。」这种字间空白的
    假象，本项目已在此栽过多次。终端里次格本来就是占位、不显示。"""
    out = []
    for row in grid:
        out.append(''.join((ch if ch is not None else '') for ch in row).rstrip())
    return out

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'harness_frames.log'
    raw = open(path, 'rb').read().decode('utf-8', errors='replace')
    # 按 FRAME 头切分
    parts = re.split(r'(?m)^FRAME ', raw)
    for part in parts:
        if not part.strip():
            continue
        head, _, body = part.partition('\n')
        body = body.strip('\n')
        tag = re.search(r'tag=(\w+)', head)
        hcols = re.search(r'hcols=(\d+)', head)
        tag = tag.group(1) if tag else '?'
        hcols = int(hcols.group(1)) if hcols else 80
        # 宿主高度取 25（harness 默认），内容区去掉标签栏 1 行
        rows, cols = 25, hcols
        grid = simulate(body, rows, cols)
        lines = render_grid(grid)
        print(f'===== FRAME {tag} (hcols={hcols}) =====')
        for idx, ln in enumerate(lines):
            if ln.strip():
                print(f'{idx:2d}|{ln}|')
        print()

if __name__ == '__main__':
    main()
