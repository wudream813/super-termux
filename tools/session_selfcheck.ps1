<#
  session_selfcheck.ps1 —— 「上次会话没恢复出来」的一次性定位脚本（Windows）

  干什么：把三件只有你机器上才知道的事实一次列出来，别的什么都不改（只读）：
    1) 你手上这个 exe 是哪一个版本（v2.3.0 没有「关窗口也存盘」，v2.3.1 才有）
    2) 它读的是哪份 termux.ini，里面 session 到底是不是开着
    3) 快照 termux.session 在不在、多大、什么时候写的、里面有没有内容
  用法（PowerShell）：
    powershell -ExecutionPolicy Bypass -File .\session_selfcheck.ps1
    powershell -ExecutionPolicy Bypass -File .\session_selfcheck.ps1 -Exe .\termux.exe
#>
param(
    [string]$Exe = "",
    [switch]$ShowBody
)

$ErrorActionPreference = "Continue"
if (-not $Exe) {
    $cand = @("$PSScriptRoot\termux.exe", "$PWD\termux.exe")
    $Exe = $cand | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Exe -or -not (Test-Path $Exe)) {
    Write-Host "找不到 termux.exe。用 -Exe 指定完整路径，例：-Exe C:\Users\me\Downloads\termux.exe" -ForegroundColor Red
    return
}
$Exe = (Resolve-Path $Exe).Path
$ExeDir = Split-Path $Exe -Parent

Write-Host ""
Write-Host "=== 1) exe ===" -ForegroundColor Cyan
Write-Host "路径  : $Exe"
Write-Host "大小  : $((Get-Item $Exe).Length) 字节   修改: $((Get-Item $Exe).LastWriteTime)"
# 版本串就在二进制里（"v2.3.1 | Windows Terminal Multiplexer"），不用真的跑起来
$raw = [System.IO.File]::ReadAllBytes($Exe)
$txt = [System.Text.Encoding]::ASCII.GetString($raw)
$m = [regex]::Match($txt, "v\d+\.\d+\.\d+ \| Windows Terminal Multiplexer")
if ($m.Success) {
    Write-Host "版本  : $($m.Value)" -ForegroundColor Green
    if ($m.Value -match "v2\.3\.0") {
        Write-Host "        ↑ v2.3.0 只在「Ctrl+B d / shell 里 exit」这两条路存盘：点 X 关窗口 = 什么都没写" -ForegroundColor Yellow
    } elseif ($m.Value -notmatch "v2\.3\.[1-9]") {
        Write-Host "        ↑ 早于 v2.3.1：连会话存盘这个功能都还没有" -ForegroundColor Yellow
    }
} else {
    Write-Host "版本  : 没在二进制里找到版本串（不是本项目的 exe？）" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== 2) ini（读哪份 / session 开没开）===" -ForegroundColor Cyan
$iniCandidates = @("$ExeDir\termux.ini", "$env:USERPROFILE\.termux.ini")
foreach ($p in $iniCandidates) {
    if (Test-Path $p) {
        $line = (Select-String -Path $p -Pattern "^\s*session\s*=" | Select-Object -First 1)
        Write-Host "存在  : $p"
        if ($line) {
            Write-Host "        $($line.Line.Trim())   ← 行 $($line.LineNumber)" -ForegroundColor Green
        } else {
            Write-Host "        里面没有 session 这一行 ⇒ 按默认 off 处理（不读不写快照）" -ForegroundColor Yellow
        }
        if ($p -eq $iniCandidates[0]) { Write-Host "        （exe 旁边这份优先，主目录那份会被忽略）" -ForegroundColor DarkGray }
    } else {
        Write-Host "没有  : $p"
    }
}

Write-Host ""
Write-Host "=== 3) 快照 ===" -ForegroundColor Cyan
$snapCandidates = @("$ExeDir\termux.session", "$env:USERPROFILE\.termux.session")
$found = $false
foreach ($p in $snapCandidates) {
    if (Test-Path $p) {
        $found = $true
        $it = Get-Item $p
        $lines = @(Get-Content $p -Encoding UTF8)
        $dcount = @($lines | Where-Object { $_ -like "D *" }).Count
        $ver = ($lines | Select-Object -First 1)
        Write-Host "存在  : $p"
        Write-Host "        大小=$($it.Length) 字节  最后写入=$($it.LastWriteTime)"
        Write-Host "        版本行=$ver   D 记录（正文行）=$dcount 条"
        if ($it.Length -lt 40) { Write-Host "        ↑ 太小：等于没存到东西（多半是没开 session 或窗格刚建就退）" -ForegroundColor Yellow }
        elseif ($dcount -eq 0) { Write-Host "        ↑ 只有布局没有正文行 ⇒ 存的那一刻屏上没有内容" -ForegroundColor Yellow }
        else { Write-Host "        ⇒ 有内容。下次启动读不读得回来，看下面「4)」" -ForegroundColor Green }
        if ($ShowBody) { $lines | Select-Object -First 12 | ForEach-Object { Write-Host "        |$_" -ForegroundColor DarkGray } }
    } else {
        Write-Host "没有  : $p"
    }
}
if (-not $found) {
    Write-Host "⇒ 两处都没有快照：问题在【写】，不在读。按这个顺序查：" -ForegroundColor Yellow
    Write-Host "   a) session 开关是否真的开着（上面 2) 的结论）"
    Write-Host "   b) exe 旁边写不写得动：New-Item \"$ExeDir\_probe\" -ItemType File; Remove-Item \"$ExeDir\_probe\""
    Write-Host "   c) 你退出用的是哪一种：Ctrl+B d / 在 shell 里 exit / 点 X 关窗口。"
    Write-Host "      v2.3.0 点 X 关窗口 = 一次都不写；v2.3.1 起才会在关闭事件里存。"
}

Write-Host ""
Write-Host "=== 4) 30 秒往返自测（想验证就照做，脚本不代跑）===" -ForegroundColor Cyan
Write-Host "  1) 启动它，敲一行能认出来的字：  echo PSMARKER123"
Write-Host "  2) 用你平时那种方式退出（这次请【点 X 关窗口】）"
Write-Host "  3) 再跑一次本脚本 —— 看 3) 里 D 记录条数是不是 >= 1"
Write-Host "  4) 再启动一次 termux：屏上应有一行「── 上次会话的历史（共 N 行；进程没有留在后台）──」"
Write-Host "     · 3) 有内容、4) 屏上没有  ⇒ 读回的问题，把本脚本输出贴回来"
Write-Host "     · 3) 一直是空的           ⇒ 写下的问题（版本 / 目录权限 / 退出方式），把输出贴回来"
Write-Host ""
