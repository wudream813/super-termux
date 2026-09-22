# super-termux

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

终端复用器（Terminal Multiplexer）—— 模块化 C 架构，单文件可执行。
在一个终端窗口里管理多个 shell 会话，像 tmux 一样分标签页、分屏、搜历史。

当前版本：**v2.0.2**（正式支持 Windows / Linux / macOS 三个系统）

## 平台支持

| 系统 | 后端 | 产物 | 状态 |
|---|---|---|---|
| Windows 10 1809+ | ConPTY + `CreateProcessW` | `termux.exe` | ✅ CI 每次构建 |
| Linux (glibc) | `forkpty` + termios raw | `termux-linux` | ✅ CI 每次构建 |
| macOS (Apple silicon / Intel) | `forkpty`（libSystem） | `termux-macos` | ✅ CI 每次构建 |

三个系统共用同一份引擎代码（screen / render / input / split / vt / theme / config …），
只有平台相关的部分抽在平台层：

```
Windows : src/main.c + src/platform_win.c + src/conpty_loader.c
POSIX   : src/main_posix.c + src/platform_posix.c + src/term_input_posix.c
接口    : include/platform.h
```

后端映射：ConPTY ↔ `forkpty`；`ResizePseudoConsole` ↔ `TIOCSWINSZ`；
`WriteConsoleA` ↔ `write(1)`；`ReadConsoleInputW` ↔ termios raw 自解析；
`GetConsoleScreenBufferInfo` ↔ `TIOCGWINSZ`；`WINDOW_BUFFER_SIZE_EVENT` ← `SIGWINCH`；
剪贴板 ↔ OSC 52 + `pbcopy` / `wl-copy` / `xclip`。

> ⚠️ **警告 / 注意事项**：
> 控制台终端**必须配置使用等宽字体**（Monospace Font，例如 *Cascadia Code*、*Consolas*、*JetBrains Mono*、*Fira Code* 等）。
> 使用非等宽字体会导致字符宽度计算偏差、边框排版错位及界面渲染故障。

## 特性

- **多标签页**：每个标签一个独立的 cmd / PowerShell 会话（ConPTY 后端，需 Win10 1809+）
- **GitHub-Dark 风格 UI**：真彩色（24bit）渲染，中文界面
- **标签页染色**：8 色调色板，右键标签 → 改颜色 / 改标题，或 `Ctrl+B t` 轮换
- **鼠标支持**：点击/中键/右键标签操作、滚轮滚动、hover 高亮
- **滚动历史**：10000 行环形滚动缓冲，PgUp/PgDn/滚轮查看
- **Alt 屏幕支持**：nano / vim 等全屏编辑器正常使用，退出后历史完整保留
- **内置帮助**：点击左上角 `termux` 徽标查看
- **分层命令面板**：仅通过 `Ctrl+B :` 打开（同时兼容中文全角冒号 `：`）；普通终端直接进入“操作命令面板”，设置页直接进入“设置命令面板”。每次最多显示 9 个结果，当前窗口始终重新编号为 `1`～`9`（序号用普通文字色，不上色块）；滚轮直接滚动结果列表，选中项跟着页面等量平移、在窗口中的相对位置不变；Tab 在搜索输入框与结果选择框之间切换，输入焦点下数字会继续搜索，结果焦点下数字选择当前可见项。操作面板支持新建终端（子面板内可按名称/命令搜索）、自定义命令行、标题/颜色、历史搜索、panel 切换（按编号或标题）、复制模式、分屏（左右/上下切分、切换窗格、全屏缩放、关闭窗格）、热重载、打开图形化设置、独立 About panel、关闭当前 panel、退出整个 termux，并可切换到设置命令面板；设置面板包含打开操作命令面板、默认启动项、打开 `.ini`、添加 panel 条目（预设/自定义后继续编辑）与菜单项设置。菜单项设置子面板只管理已有条目：Enter 编辑、`Ctrl+↑/↓` 调整位置（搜索期间禁用）、无查询时结果焦点下 `X` 删除；搜索输入焦点下 `U/D` 仍是普通查询字符，有查询时使用 `Ctrl+X` 删除；添加 panel 仍从设置面板的独立入口执行。Esc 返回子面板时会恢复父面板的选择、滚动位置和查询上下文。
- **可配置的键位**：前缀键（默认 `Ctrl+B`）与每个动作的按键都能在 `termux.ini` 的 `[keys]` 段里改，每个动作还能单独设为「不带前缀的直接键」（`noprefix`，设置页键位表里按 `P` 或点 `[前缀]/[直接]` 列切换）；帮助页显示的快捷键跟着配置实时变化
- **右上角状态徽章**：搜索输入框与复制/搜索状态全部收在右上角，不再占用任何一整行终端内容；复制模式与历史搜索都折叠成右上角一枚小徽章（`[复制模式 行选区]` / `[搜索 "err" 3/12]`），鼠标悬停时才向左展开完整操作提示，不再长期占据一整行
- **配色主题**：内置 `github-dark` / `one-dark` / `nord` / `gruvbox-dark` / `dracula`，也可在 `[theme]` 段按语义角色单独覆盖任意一种颜色；`Ctrl+B` → 设置命令面板 → 切换配色主题 可即时轮换
- **行为开关**：`scrollback` 滚动行数、`mouse` 鼠标开关、`copy_on_select` 拖选自动复制、`confirm_on_exit` 退出二次确认、`confirm_on_close` 关闭窗格/标签二次确认
- **每个菜单项可配启动默认颜色**：菜单项详情页（以及命令面板的 panel 编辑器）多了一条颜色选择条，`←/→`、数字 `0-8` 或鼠标点选即可指定这一项新建标签页时的颜色，写回 `termux.ini` 的 `color=` 字段
- **图形化设置页**：`Ctrl+B s` 打开，侧栏含「启动 / 菜单项 / 外观·主题 / 键位 / 行为」五类；主题可即时预览切换、语义色可改十六进制、键位支持**按键录制**，改完即时写回 `termux.ini`
- **诊断日志**：`TERMUX_DUMP=1` 时输出原始 ConPTY 流 / 渲染输出 / 鼠标事件

## 快捷键

| 快捷键 | 功能 |
|---|---|
| `Ctrl+B :` | 打开命令面板 (Command Palette) |
| `Ctrl+B c` | 新建默认 pane |
| `Ctrl+B +` | 新建 pane 菜单（选已配置项目 / 自定义命令行） |
| `Ctrl+B [` | 进入复制模式（方向键/hjkl 移动、Space/v 选区、`Shift+方向` 行选、`Alt+方向` 块选、`b` 切换行/块、Enter/`Ctrl+C`/y 复制、Esc 退出） |
| `Shift+点击两点` | 直接进入复制模式并选中两点之间（行选）；`Alt+点击两点` 为矩形框选。`Ctrl+C`/Enter 复制并关闭，Esc 退出，其它键退出并把该键发给终端 |
| `Ctrl+B /` | 搜索滚动历史（`U`/`D` 或 `n`/`N` 跳转上一个/下一个匹配，Esc 退出；右上角徽章上的 `[U 上] [D 下] [×]` 也可鼠标点） |
| `Ctrl+B s` | 打开图形化设置页面 (`termux.ini`) |
| `Ctrl+B r` | 热重载配置文件 (`termux.ini`) |
| `Ctrl+B ?` / `h` | 打开 / 关闭帮助 |
| `Ctrl+B n / p` | 下一个 / 上一个 pane |
| `Ctrl+B x` | 关闭当前 panel |
| `Ctrl+B d` | 退出 termux |
| `Ctrl+B t` / `Shift+t` | 轮换标签颜色 |
| `Ctrl+B w` | 打开「切换 panel」可视化面板（按编号或标题选择切换 pane；数字直跳已移除） |
| `Ctrl+B -` / `_` | 分屏：左右切分 / 上下切分（同一标签内多个终端同屏） |
| `Ctrl+B Tab` / `Shift+Tab` | 分屏：切换到下一个 / 上一个窗格 |
| `Ctrl+B ↑/↓/←/→` | 分屏：切换到对应方向的相邻窗格 |
| `Ctrl+B Shift+方向` | 分屏：拖动分隔线调整窗格大小 |
| `Ctrl+B q` | 分屏：关闭当前窗格（同标签只剩一个窗格时关闭整个标签页；可在设置里开启关闭二次确认） |
| `Ctrl+B z` | 分屏：当前窗格全屏缩放 / 还原 |

> 上表是默认键位；前缀键与每个动作的按键都可以在 `termux.ini` 的 `[keys]` 段里改，见下文。

## 鼠标操作

| 操作 | 功能 |
|---|---|
| 左键点击标签 | 切换 pane |
| 左键点击 `×` | 关闭该 pane |
| 右键点击标签 | 改颜色 / 改标题 |
| 中键点击标签 | 关闭该 pane |
| 鼠标左键拖拽 | 框选终端文字，松开自动复制到剪贴板 |
| 点击 `[+]` | 新建 pane（选已配置项目 / 自定义命令行） |
| 点击 `[*]` | 打开图形化设置页面 |
| 点击 `termux` | 打开 / 关闭帮助 |
| 滚轮 | 滚动历史（未开鼠标追踪时） |

## 编译

**Windows**（需要 MinGW-w64；MSVC 也可）

```bat
:: MSVC
cl /O2 /Iinclude src\*.c /Fe:termux.exe /link user32.lib shell32.lib

:: MinGW-w64 (GCC / Make)
make
:: 或手动执行
x86_64-w64-mingw32-gcc -O2 -s -Wall -Wextra -Iinclude src/*.c -o termux.exe -luser32 -lshell32
```

**Linux**

```sh
make linux          # -> ./termux-linux（需要 -lutil 提供 forkpty）
```

**macOS**（必须在 macOS 上编：`forkpty` 在 libSystem、没有 `-lutil`；
`_NSGetExecutablePath` 在 `<mach-o/dyld.h>`。Makefile 里有 `uname -s` 护栏，
在别的系统上跑 `make darwin` 会直接 exit 1，不会产出一个改名的 Linux 二进制冒充）

```sh
make darwin         # -> ./termux-macos
```

**跑测试**

```sh
make check-posix    # POSIX 侧一把梭：编译 + 79+ 条移植不变量 + 4 个单测 + 真 pty 冒烟
make unittest       # 主题 / 键位 / 配置（-Werror）
python3 verify_all.py
```

三个系统的完整检查见 [`.github/workflows/ci.yml`](.github/workflows/ci.yml)。

## 运行

```bat
termux.exe          :: Windows
set TERMUX_DUMP=1   :: 启用诊断日志（termux_dump.log / render_dump.log / mouse_dump.log）
```

```sh
./termux-linux      # Linux
./termux-macos      # macOS
TERMUX_DUMP=1 ./termux-linux
```

## ⚙️ 配置文件 (termux.ini)

- **配置文件路径**：优先读取 `termux.exe` 所在目录下的 `termux.ini`；若不存在则读取 `%USERPROFILE%\.termux.ini`。首次启动会自动生成默认配置文件。
- **热重载**：`Ctrl+B r`（或命令面板的“热重载”）即时生效。`scrollback` 只对之后新建的 pane 生效，其余项立即生效。
- 配置文件分四段：`[general]` 行为、`[theme]` 配色、`[keys]` 键位、`[menu]` 新建菜单。

```ini
# super-termux 配置文件 (UTF-8)

[general]
theme = github-dark        # github-dark | one-dark | nord | gruvbox-dark | dracula
prefix = C-b               # 前缀键：C- = Ctrl，M- = Alt，S- = Shift
scrollback = 10000         # 每个 pane 的滚动历史行数 (200 - 500000)
mouse = true               # 关掉后标签点击 / 拖选 / 滚轮全部停用
copy_on_select = true      # 鼠标拖选松开时自动复制到剪贴板
confirm_on_exit = false    # 退出 termux 前弹出 Y/N 二次确认
confirm_on_close = false   # 关闭窗格 / 标签前弹出 Y/N 二次确认
search_case_sensitive = false  # 历史搜索是否锁定大小写（false = 忽略大小写）
default_startup = 0        # 0 = 启动进终端，1 = 启动显示帮助

[theme]
# 按语义角色覆盖任意颜色，只写想改的那几行即可
# accent = #58a6ff
# background = #0d1117

[keys]
# 动作名 = 键位[ noprefix]
# 默认要先按前缀键；加上 noprefix（或 direct）就是不带前缀的直接键
# new-pane = c
# next-theme = T
# copy-mode = F8 noprefix

[menu]
# 序号 = 菜单显示名称, 启动命令行, 启动目录(可选), color=启动默认颜色(可选 1-8)
# 特殊命令 ":custom" 表示打开自定义命令行输入框
# color 省略或写 0 表示跟随默认蓝色
1 = cmd, cmd.exe
2 = PowerShell, powershell.exe, , color=2
3 = 自定义命令行, :custom
# 4 = WSL Ubuntu, wsl.exe -d Ubuntu
# 5 = Git Bash, "C:\Program Files\Git\bin\bash.exe" --login -i
# 6 = 项目终端, cmd.exe, D:\work\myproject
```

### `[keys]` 可绑定的动作

| 动作名 | 说明 | 默认键位 |
|---|---|---|
| `send-prefix` | 把前缀键本身发给当前 pane | 连按两次前缀键 |
| `command-palette` | 打开命令面板 | `:` |
| `new-pane` | 新建默认 pane | `c` |
| `new-pane-menu` | 新建 pane 菜单 | `+` |
| `copy-mode` | 进入复制模式 | `[` |
| `search` | 搜索滚动历史 | `/` |
| `settings` | 打开图形化设置 | `s` |
| `reload-config` | 热重载配置 | `r` |
| `help` | 打开 / 关闭帮助 | `?` / `h` |
| `next-pane` / `prev-pane` | 下一个 / 上一个 pane | `n` / `p` |
| `close-pane` | 关闭当前 pane | `x` |
| `quit` | 退出 termux | `d` |
| `tab-color-next` / `tab-color-prev` | 轮换标签颜色 | `t` / `Shift+t` |
| `switch-panel` | 切换 panel（打开可视化面板，按编号或标题选择） | `w` |
| `next-theme` | 切换下一个配色主题 | 未绑定 |
| `split-horizontal` | 分屏：上下切分 | `_`（Shift+-） |
| `split-vertical` | 分屏：左右切分 | `-` / `|` |
| `split-next` / `split-prev` | 分屏：下一个 / 上一个窗格 | `Tab` / `Shift+Tab` |
| `split-up` / `split-down` / `split-left` / `split-right` | 分屏：按方向切换窗格 | `↑` / `↓` / `←` / `→` |
| `split-resize-up` / `split-resize-down` / `split-resize-left` / `split-resize-right` | 分屏：拖分隔线调整大小 | `Shift+↑/↓/←/→` |
| `split-close` | 分屏：关闭当前窗格 | `q` |
| `split-zoom` | 分屏：当前窗格全屏缩放 / 还原 | `z` |

键位写法：`C-` = Ctrl，`M-`（或 `A-`）= Alt，`S-` = Shift；键名支持单个字符、`F1`-`F24`、
`Space` / `Tab` / `Enter` / `Esc` / `Backspace` / `Up` / `Down` / `Left` / `Right` /
`Home` / `End` / `PgUp` / `PgDn` / `Ins` / `Del`。例如 `prefix = C-a`、`new-pane = F2`、
`close-pane = M-w`。给某个动作写了绑定，它的默认键位就会被顶掉。

### 图形化设置页（`Ctrl+B s`）

不想手写 ini 的话，所有这些都能在设置页里点：

| 侧栏分类 | 能做什么 | 快捷键 |
|---|---|---|
| 启动 (Startup) | 默认启动项、`[+]` 菜单项的顺序 / 增删改 | 启动页按 `↑/↓ Enter X + P` |
| 菜单项 `[1]`-`[9]` | 单个条目的名称 / 命令行 / 启动目录 | `Tab` 切换输入框，`Enter` 保存 |
| **外观 / 主题** | 上下选择内置主题，`Enter`/点击**立即应用并写盘**；下方 16 个语义色带色块与十六进制值，`Enter` 进入编辑（6 位 hex），`R` 复位当前项，`Ctrl+R` 清除全部自定义 | 启动页按 `F2` |
| **键位设置** | 第一行是前缀键，下面是全部 17 个动作；`Enter` 或点击 `[改]` 进入**按键录制**（直接按你想要的组合键即可），`R` 或 `[复位]` 恢复默认，`Ctrl+R` 全部复位 | 启动页按 `F3` / `K` |
| **行为开关** | `mouse` / `copy_on_select` / `confirm_on_exit` / `confirm_on_close` 四个开关，`scrollback` 用 `←/→` 或 `[-] [+]` 按 1000 步进调整 | 启动页按 `F4` / `B` |

侧栏用鼠标点，或在任意分类页按 `Ctrl+↑ / Ctrl+↓` 依次切换；`Esc` 从分类页返回启动页。
自定义过的语义色行尾会带 `*`，自定义过的键位 `[复位]` 按钮会变红。

### `[theme]` 的 16 个语义角色

| 角色名 | 用途 | 角色名 | 用途 |
|---|---|---|---|
| `background` | 最深背景 / 亮底上的文字 | `accent` | 主强调色（活动标签、边框） |
| `tabbar` | 标签栏背景 | `cyan` | 信息色 / 浅蓝 |
| `panel` | 面板与弹窗背景 | `green` / `green_dark` | 成功、绿色标签 |
| `foreground` | 主文字 | `red` | 危险、关闭按钮 |
| `foreground_dim` | 次要文字 | `orange` / `yellow` | 提示、当前匹配 |
| `white` | 高亮文字 | `purple` / `pink` | 品牌色与彩色标签 |
| `selection` | 选区底色 | | |

界面里所有派生色（各种暗色标签底、hover 底色）都由这 16 个角色按固定比例混合得出，
所以只改 `accent` 一行，活动标签、边框、选中行会一起跟着变。

## 系统要求

- **Windows**：Windows 10 1809 (RS5) 或更高（ConPTY 支持）
- **Linux**：任何带 `forkpty`（libutil / glibc）的发行版
- **macOS**：Apple silicon 或 Intel 均可
- 从真实终端窗口运行（Windows Terminal / cmd / ConEmu / iTerm2 / kitty / GNOME Terminal …）
- **必须使用等宽字体**（如 Cascadia Code, Consolas, JetBrains Mono, Menlo 等），
  否则会出现渲染故障与排版错位

## 开发说明

源码采用模块化架构（`include/` 与 `src/`），无第三方依赖，仅链接 `user32`。
每次改动可一键运行仓库内的全部 Python 回归验证：

```bash
python3 verify_all.py
```

主题与键位模块不依赖任何 Win32 调用，可以直接在本机（含 Linux CI）编译执行真正的 C 单元测试：

```bash
make unittest
# 等价于
gcc -Wall -Wextra -Werror -Itests/stub -Iinclude src/theme.c src/keymap.c tests/test_config.c -o test_config && ./test_config
```

也可以单独运行验证脚本：

```bash
python3 verify_picker.py        # 选色器几何 / 点击 / hover 命中
python3 verify_flow.py          # 弹窗端到端流程
python3 verify_mouse53.py       # 鼠标按钮优先级 / 兜底 / 渲染用色
python3 verify_color8.py        # color=8 渲染用色回归
python3 verify_emoji.py         # Emoji 与字素簇边界测试
python3 verify_ringbuf_asan.py  # 环形缓冲区局部滚动 ASAN 内存安全
python3 verify_screen_state.py  # alt 屏 resize 保留真彩色 / 搜索当前项落点
python3 verify_html_clipboard.py # 复制保留颜色：HTML Format 偏移量 / 真彩色 / Campbell 16 色 / RLE
python3 verify_dirty_render.py   # 脏区渲染：整帧切行比对，未变行 0 输出
python3 verify_search.py          # 滚动历史搜索行为验证
python3 verify_item_color.py       # 菜单项启动默认颜色选择条的渲染/热区一致性
python3 verify_search_box.py       # 搜索输入框只画右上角一小框、不吃整行
python3 verify_palette_search.py   # 新建终端短查询/名称优先搜索回归
python3 verify_input_layout.py     # 输入框背景/末尾字符/光标列回归
python3 verify_cursor_render.py    # 终端输出区域最后一格光标回归
python3 verify_menu_settings.py    # 菜单项设置排序/搜索禁用/删除/模态输入回归
python3 verify_palette_interaction.py # 命令面板焦点/9 项编号/Esc 快照/颜色单元格回归
python3 verify_config_theme.py     # 配置体系：主题参考色板完整性 + keymap/theme C 单元测试
```


## 版本历史

**v2.0.2** —— 仓库改名 `win-termux` → **`super-termux`**（已经支持三个系统，
名字里的 `win-` 不再合适）。关于页的仓库链接、生成的 `termux.ini` 头部注释、
README 标题同步更新；`history.md` 和描述历史 bug 的代码注释**保持原样**，
不改写过去的记录。

这一版还补上了 Windows 侧缺失的运行时测试。CI 的 `windows` 作业以前只有
「构建 + 静态检查」，一个运行时测试都没有：

- **主机侧回归**：`make unittest`、`make verify-loader`、`verify_all.py`。这些用
  `tests/stub/windows.h` 替身，不需要真终端。为此把两个只在 Windows 上出现的
  坑收进了 `tests/hbuild.py` 和 Makefile 的 `HOST_EXE_EXT`：MinGW 会给【无扩展名】
  的 `-o` 自动补 `.exe`（`-o foo` 产出 `foo.exe`，脚本随后找 `foo` 就挂），
  以及 MinGW 不支持 `-fsanitize`（现在显式降级并打 `[SKIP-SANITIZER]`，不静默）。
- **真 ConPTY 冒烟测试**：新增 `tests/conpty_drive.py`（`CreatePseudoConsole` +
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`，接口与 `tests/drive.py` 对齐，读帧的
  方法直接复用）和 `tests/win_smoke.py`。验的是**运行期**证据：关于页/帮助页的
  平台文案（bug #5/#9 以前只做到 `gcc -E` 预处理级）、分屏真的多出窗格、
  resize 之后历史还在 —— 最后一项正是最初那个「resize 丢历史」bug 的老家，
  此前从没有过 Windows 上的自动化复现手段。

**v2.0.1** —— 修正「单文件 C」这个措辞。关于页副标题原来写「基于 …… 的高性能单文件 C
终端复用多标签环境」，「单文件 C」很容易被读成「源码只有一个 C 文件」，而实际是
20 个 `.c` + 18 个 `.h`、约 1.9 万行。真正成立的是**构建产物**是单个可执行文件
（Linux 只依赖 libc，Windows 只链 `user32`/`shell32`）。现在副标题拆成两行：
「基于 …… 的高性能终端复用多标签环境」+「纯 C 实现，单文件可执行」。
GitHub 仓库的 About 与 Topics 同步改为跨平台。

**v2.0.0** —— 正式支持 Windows / Linux / macOS 三个系统。引擎代码三系统共用，
平台相关部分抽到平台层；CI 在三个系统上各跑一遍完整检查并发布二进制。
移植过程中修掉的 7 个真 bug（剪贴板死路、配置路径两层错、宽字符多字搜索、
`plat_write_fd` EAGAIN 丢输入 41%、关于页写死 Windows 文案、命令行不认引号、
启动目录不展开且静默失败）都有验红的回归钉住。

更早的版本详见 [history.md](history.md)。

## 开源协议

本项目基于 [MIT 许可证](LICENSE) 开源。
