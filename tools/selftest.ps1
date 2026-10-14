#Requires -Version 7
<#
    tools\selftest.ps1

    把编译好的两个产物（x32 / x64）用【全部命令行参数】各跑一遍，收证据、截图。

    设计要点
    --------
    1. 参数分两类，处理方式完全不同：

       [A] headless —— 程序自己"导完就退出"的模式（--ui-preview / --setup-ui /
           --notice-ui / --fx-dump / --face-dump / --audio-dump / --theme-dump /
           --clean-gold / --restore / --pay）。它们会写出 PNG / WAV 后自己 return，
           有明确的退出码，可以直接当断言用。

       [B] interactive —— 真正"开窗跑演出"的模式（--face-demo / --phase）。
           它们进消息循环后**永远不会自己退出**，只能定时截屏再强杀。
           跑这类必须带 --no-guardian（否则强杀会被看门狗判成"被强杀"，
           立刻再拉一场惩罚演出，没完没了）和 --no-lockdown
           （否则会把别的窗口最小化，可能连 runner 自己一起搞掉）。

    2. 为什么"程序自己导出的 PNG"也算截图：作者在 entity_main.cpp 里写得很清楚,
       UI 那几屏盖在桌面上截屏会被壁纸和其它窗口污染，只有单独导出才能按像素核对。
       所以对 [A] 类，PNG 就是最好的证据；对 [B] 类才补一张真实桌面截屏。

    3. 全程不 throw 掉证据：任何一个用例失败都只记进报告，脚本最后统一汇总，
       有失败就 exit 1。CI 里靠 `continue-on-error` 放行后续步骤，让产物和截图
       一定先传完，最后再用 `steps.selftest.outcome` 把整个 job 判红。
       -AllowFailures 是给手工排查用的（无论成败都 exit 0）。

    用法
    ----
        pwsh -NoProfile -File tools\selftest.ps1 -OutDir selftest
#>
[CmdletBinding()]
param(
    # 证据输出根目录（相对仓库根）
    [string]$OutDir = 'selftest',
    # [B] 类用例：启动后等几秒再截屏
    [int]$SettleSec = 6,
    # [A] 类用例：单个用例最多跑多久（秒），超时算挂住 = FAIL
    [int]$HeadlessTimeoutSec = 90,
    # 加了它：无论有没有失败，都 exit 0（只给手工排查用；CI 不要加，
    # 否则 steps.selftest.outcome 永远是 success，job 就红不了了）
    [switch]$AllowFailures
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# 0. 定位仓库根、拿到两个产物
# ---------------------------------------------------------------------------
$scriptDir = $PSScriptRoot
$repo      = Split-Path -Parent $scriptDir
Set-Location -LiteralPath $repo

if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $repo $OutDir
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$exes = [ordered]@{
    x32 = 'build\Win32\Release\Ransom_dev.exe'
    x64 = 'build\x64\Release\Ransom_dev.exe'
}

foreach ($k in $exes.Keys) {
    $p = Join-Path $repo $exes[$k]
    if (-not (Test-Path -LiteralPath $p)) {
        throw "找不到 $k 产物：$($exes[$k])（先跑编译步骤）"
    }
    $size = (Get-Item -LiteralPath $p).Length
    Write-Host ("产物 {0}: {1}  ({2:N0} bytes)" -f $k, $exes[$k], $size)
}

# ---------------------------------------------------------------------------
# 1. Windows Defender 排除
#    这是个"勒索软件模拟器"，产物被 Defender 当可疑样本隔离是完全可能的
#    （尤其是 x32 那份，行为启发式对老的 32 位 PE 更敏感）。
#    runner 是管理员，排除一般能设上；设不上也不致命，只记一行。
# ---------------------------------------------------------------------------
try {
    Add-MpPreference -ExclusionPath $repo -ErrorAction Stop
    Add-MpPreference -ExclusionProcess 'Ransom_dev.exe' -ErrorAction Stop
    Write-Host 'Defender: 已排除工作目录 + Ransom_dev.exe'
} catch {
    Write-Host "Defender: 排除设置失败（继续跑，不影响测试）：$($_.Exception.Message)"
}

# ---------------------------------------------------------------------------
# 2. 桌面截屏helper
#    故意用 Windows PowerShell 5.1（$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe）
#    来跑：5.1 是 .NET Framework，System.Drawing / System.Windows.Forms 一定在；
#    pwsh 7 是 .NET (Core)，这两个程序集未必装了，免得赌。
#    截图同时算一个平均亮度 —— 无头桌面的典型症状是整张纯黑，
#    光看"截到图了"判断不出来，得看亮度。
# ---------------------------------------------------------------------------
$captureScript = Join-Path $OutDir '_capture.ps1'
$captureBody = @'
param([string]$Path, [string]$Report)
$ErrorActionPreference = 'Stop'
try {
    Add-Type -AssemblyName System.Drawing
    Add-Type -AssemblyName System.Windows.Forms
    $vs  = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $vs.Width, $vs.Height
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $sum = 0.0; $n = 0
    for ($y = 0; $y -lt $bmp.Height; $y += 24) {
        for ($x = 0; $x -lt $bmp.Width; $x += 24) {
            $c = $bmp.GetPixel($x, $y)
            $sum += ($c.R + $c.G + $c.B) / 3.0
            $n++
        }
    }
    $br = [math]::Round($sum / $n, 2)
    $bmp.Dispose()
    "OK $($vs.Width)x$($vs.Height) brightness=$br" | Set-Content -LiteralPath $Report -Encoding ascii
} catch {
    "FAIL $($_.Exception.Message)" | Set-Content -LiteralPath $Report -Encoding ascii
}
'@
Set-Content -LiteralPath $captureScript -Value $captureBody -Encoding utf8BOM

$winPs = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path -LiteralPath $winPs)) { $winPs = 'powershell.exe' }

function Save-Desktop {
    param([string]$Path)
    $report = "$Path.txt"
    $r = [pscustomobject]@{ ok = $false; info = 'not-run'; brightness = $null; width = $null; height = $null }
    try {
        & $winPs -NoProfile -ExecutionPolicy Bypass -File $captureScript -Path $Path -Report $report | Out-Null
        $line = (Get-Content -LiteralPath $report -Raw -ErrorAction Stop).Trim()
        if ($line -like 'OK *') {
            $r.ok   = $true
            $r.info = $line
            if ($line -match '(\d+)x(\d+)') { $r.width = [int]$Matches[1]; $r.height = [int]$Matches[2] }
            if ($line -match 'brightness=([\d.]+)') { $r.brightness = [double]$Matches[1] }
        } else {
            $r.info = $line
        }
    } catch {
        $r.info = "FAIL $($_.Exception.Message)"
    }
    return $r
}

# ---------------------------------------------------------------------------
# 3. 进程清理
# ---------------------------------------------------------------------------
function Stop-AllRansom {
    $alive = @(Get-Process -Name 'Ransom_dev' -ErrorAction SilentlyContinue)
    foreach ($proc in $alive) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch { }
    }
    if ($alive.Count -gt 0) { Start-Sleep -Milliseconds 400 }
    return $alive.Count
}

# 传给 Start-Process 之前给带空格的参数补引号。
# 现在 runner 的工作目录是没有空格的，但这条保险很便宜 —— 万一以后仓库名
# 或路径带上空格，缺了引号会被拆成两个参数，程序会报一个莫名其妙的自检错误。
function Get-QuotedArgs {
    param([string[]]$Items)
    $out = @()
    foreach ($s in $Items) {
        if ($s -match '\s' -and $s -notmatch '^".*"$') { $out += ('"' + $s + '"') }
        else { $out += $s }
    }
    return $out
}

# 算一个目录里产出了什么（文件数 + 总字节），用来写进报告
function Get-FileSummary {
    param([string]$Dir)
    if (-not (Test-Path -LiteralPath $Dir)) {
        return [pscustomobject]@{ count = 0; bytes = 0; names = @() }
    }
    $items = @(Get-ChildItem -LiteralPath $Dir -File -Recurse -ErrorAction SilentlyContinue)
    $bytes = 0
    foreach ($i in $items) { $bytes += $i.Length }
    $names = @($items | Sort-Object Name | ForEach-Object { $_.Name })
    return [pscustomobject]@{ count = $items.Count; bytes = $bytes; names = $names }
}

# ---------------------------------------------------------------------------
# 4. 用例矩阵
# ---------------------------------------------------------------------------
# [A] 自己退出的模式。e = 期望退出码。
#     注意 --pay 在没有实体运行时是【故意】返回 3 的（entity_main.cpp 里
#     SendPayment 找不到 RansomDevIpcWnd 窗口就返回 3），所以期望值写 3，
#     不是 0 —— 这个用例是在验证"没有实体时点击金币会安全作废"。
$headless = @(
    @{ n = 'face-dump';        a = @('--face-dump', '{d}\faces');                            e = 0 }
    @{ n = 'audio-dump';       a = @('--audio-dump', '{d}\audio-all.wav');                   e = 0 }
    @{ n = 'theme-dump';       a = @('--theme-dump', '{d}\theme.wav');                       e = 0 }
    @{ n = 'ui-preview';       a = @('--ui-preview', '{d}\ui.png');                          e = 0 }
    @{ n = 'ui-preview-grid';  a = @('--ui-preview', '{d}\ui-grid.png', '--ui-grid');        e = 0 }
    @{ n = 'ui-preview-payup'; a = @('--ui-preview', '{d}\ui-payup.png', '--payup');         e = 0 }
    @{ n = 'setup-ui';         a = @('--setup-ui', '{d}\setup.png');                         e = 0 }
    @{ n = 'setup-ui-grid';    a = @('--setup-ui', '{d}\setup-grid.png', '--setup-grid');    e = 0 }
    @{ n = 'setup-ui-scroll';  a = @('--setup-ui', '{d}\setup-scroll.png', '--setup-scroll', '240'); e = 0 }
    @{ n = 'notice-ui';        a = @('--notice-ui', '{d}\notice.png');                       e = 0 }
    @{ n = 'notice-ui-grid';   a = @('--notice-ui', '{d}\notice-grid.png', '--setup-grid');  e = 0 }
    @{ n = 'fx-glow';          a = @('--fx-demo', 'glow',  '--fx-dump', '{d}\fx-glow.png');  e = 0 }
    @{ n = 'fx-black';         a = @('--fx-demo', 'black', '--fx-dump', '{d}\fx-black.png'); e = 0 }
    @{ n = 'fx-stop';          a = @('--fx-demo', 'stop',  '--fx-dump', '{d}\fx-stop.png');  e = 0 }
    @{ n = 'image-dir';        a = @('--ui-preview', '{d}\ui-imagedir.png', '--image-dir', 'assets\image'); e = 0 }
    @{ n = 'audio-dir';        a = @('--audio-dump', '{d}\audio-dir.wav', '--audio-dir', 'assets\audio');   e = 0 }
    @{ n = 'tolerance';        a = @('--ui-preview', '{d}\ui-tol.png', '--tolerance', '42'); e = 0 }
    @{ n = 'clean-gold';       a = @('--clean-gold');                                        e = 0 }
    @{ n = 'restore';          a = @('--restore');                                           e = 0 }
    @{ n = 'pay-no-entity';    a = @('--pay', '100', '--token', '1');                        e = 3 }
)

# [B] 会开窗挂着不退的模式。统一由脚本定时截屏后强杀。
#     序号 0-7 对应 director 的潜伏/浮现/瞬移/停牌/避开/被抓/付清/惩罚。
$interactive = @(
    @{ n = 'face-demo-idle';    a = @('--face-demo', 'idle') }
    @{ n = 'face-demo-stop';    a = @('--face-demo', 'stop') }
    @{ n = 'face-demo-attack';  a = @('--face-demo', 'attack'); pay = $true }
    @{ n = 'face-demo-loading'; a = @('--face-demo', 'loading') }
    @{ n = 'phase-0-idle';      a = @('--phase', '0', '--no-auto') }
    @{ n = 'phase-1-face';      a = @('--phase', '1', '--no-auto') }
    @{ n = 'phase-2-center';    a = @('--phase', '2', '--no-auto') }
    @{ n = 'phase-3-stop';      a = @('--phase', '3', '--no-auto') }
    @{ n = 'phase-5-caught';    a = @('--phase', '5', '--no-auto') }
    @{ n = 'phase-6-paid';      a = @('--phase', '6', '--no-auto') }
    @{ n = 'phase-7-punish';    a = @('--phase', '7', '--no-auto') }
)

# [B] 类统一前置的安全开关 —— 每一条都是为了防止它把 runner 自己搞死：
#   --no-guardian   不拉看门狗（我们一定会强杀它，否则会被判成"被强杀"再演一场）
#   --no-lockdown   勒索时不最小化别的窗口（保护 runner 自己的窗口/agent）
#   --no-block-menu 不拦截桌面图标的右键菜单
#   --no-setup      不弹设置窗口（模态，会一直卡住）
#   --no-audio      不出声（无头声卡，省得报错刷屏）
$safety = @('--no-guardian', '--no-lockdown', '--no-block-menu', '--no-setup', '--no-audio')

# ---------------------------------------------------------------------------
# 5. 跑
# ---------------------------------------------------------------------------
$results = @()
$totalStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host ''
Write-Host '=================================================================='
Write-Host " 自测开始：$([datetime]::UtcNow.ToString('u'))"
Write-Host '=================================================================='

foreach ($arch in $exes.Keys) {

    $exe = Join-Path $repo $exes[$arch]
    $archDir = Join-Path $OutDir $arch

    # ---------------- [A] headless ----------------
    foreach ($t in $headless) {

        $dir = Join-Path $archDir $t.n
        New-Item -ItemType Directory -Force -Path $dir | Out-Null

        $argList = @()
        foreach ($x in $t.a) { $argList += ($x -replace '\{d\}', $dir) }
        $argList += @('--diag', (Join-Path $dir 'diag.log'))

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $exit = $null
        $note = ''
        $timedOut = $false

        try {
            $proc = Start-Process -FilePath $exe -ArgumentList (Get-QuotedArgs $argList) `
                        -WorkingDirectory $repo -PassThru -ErrorAction Stop
            if (-not $proc.WaitForExit($HeadlessTimeoutSec * 1000)) {
                $timedOut = $true
                try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch { }
                $note = "超过 $HeadlessTimeoutSec 秒没退出（挂住了）"
            } else {
                $exit = $proc.ExitCode
            }
        } catch {
            $note = "启动失败：$($_.Exception.Message)"
        }
        $sw.Stop()

        # 强杀之后可能留下孩子进程，一并清掉
        if ($timedOut) { Stop-AllRansom | Out-Null }

        $fs = Get-FileSummary -Dir $dir
        # diag.log 是脚本自己加的，不算"产出"
        $produced = @($fs.names | Where-Object { $_ -ne 'diag.log' })
        $producedBytes = $fs.bytes
        if ($produced.Count -eq 0 -and -not $timedOut) {
            $note = ($note + ' 没有产出任何文件').Trim()
        }

        $verdict = if ($timedOut) { 'FAIL' }
                   elseif ($exit -eq $t.e) { 'PASS' }
                   else { 'FAIL' }

        $results += [pscustomobject]@{
            arch    = $arch
            kind    = 'headless'
            name    = $t.n
            args    = ($argList -join ' ')
            expect  = ($t.e.ToString())
            actual  = if ($timedOut) { 'TIMEOUT' } elseif ($null -eq $exit) { 'ERROR' } else { $exit.ToString() }
            verdict = $verdict
            ms      = $sw.ElapsedMilliseconds
            files   = $produced.Count
            bytes   = $producedBytes
            shot    = ''
            bright  = ''
            note    = $note
        }

        Write-Host ('[{0}] {1,-20} exit={2,-8} expect={3,-3} -> {4}  {5} 个文件' -f `
            $arch, $t.n, $(if ($timedOut) { 'TIMEOUT' } else { $exit }), $t.e, $verdict, $produced.Count)
    }

    # ---------------- [B] interactive ----------------
    foreach ($t in $interactive) {

        $dir = Join-Path $archDir "screen-$($t.n)"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null

        $argList = @()
        $argList += $safety
        $argList += ($t.a -replace '\{d\}', $dir)
        $argList += @('--diag', (Join-Path $dir 'diag.log'))

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $note = ''
        $payResult = ''
        $started = $false

        try {
            $proc = Start-Process -FilePath $exe -ArgumentList (Get-QuotedArgs $argList) `
                        -WorkingDirectory $repo -PassThru -ErrorAction Stop
            $started = $true

            # 让它把窗开出来、把第一帧画出来
            Start-Sleep -Seconds $SettleSec

            $alive = -not $proc.HasExited

            # 真实桌面截屏
            $shotPath = Join-Path $dir 'desktop.png'
            $shot = Save-Desktop -Path $shotPath

            # 可选：实体正在跑的时候点一枚金币，验证 --pay 的 IPC 通路
            # （这时应该返回 0，因为 RansomDevIpcWnd 窗口确实存在）
            if ($t.pay -and $alive) {
                $payProc = Start-Process -FilePath $exe `
                                -ArgumentList @('--pay', '100', '--token', '1') `
                                -WorkingDirectory $repo -PassThru
                $payProc.WaitForExit(15000) | Out-Null
                # ExitCode 在进程还没退出时会抛，被强杀之后也可能读到竞态，包一层
                if ($payProc.HasExited) {
                    try { $payCode = $payProc.ExitCode } catch { $payCode = "读取失败($($_.Exception.Message))" }
                } else {
                    try { Stop-Process -Id $payProc.Id -Force } catch { }
                    $payCode = '超时未退出'
                }
                $payResult = "运行中点金币 -> 退出码 $payCode"
            }

            if ($alive) {
                try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop; $note = '按预期仍在运行，已强杀' }
                catch { $note = "强杀失败：$($_.Exception.Message)" }
            } else {
                $note = "意外自行退出，退出码 $($proc.ExitCode)"
            }

            # 判定：能开起来、撑过 settle、截到图 = PASS
            $verdict = if (-not $alive) { 'FAIL' }
                       elseif (-not $shot.ok) { 'PASS' }   # 截图失败不算用例失败，另记
                       else { 'PASS' }

            $results += [pscustomobject]@{
                arch    = $arch
                kind    = 'interactive'
                name    = $t.n
                args    = ($argList -join ' ')
                expect  = 'timeout'
                actual  = if ($alive) { 'still-running' } else { 'exited-early' }
                verdict = $verdict
                ms      = $sw.ElapsedMilliseconds
                files   = (Get-FileSummary -Dir $dir).count
                bytes   = (Get-FileSummary -Dir $dir).bytes
                shot    = if ($shot.ok) { 'desktop.png' } else { "截图失败: $($shot.info)" }
                bright  = if ($null -ne $shot.brightness) { $shot.brightness.ToString() } else { '' }
                note    = ($note + ' ' + $payResult).Trim()
            }

            Write-Host ('[{0}] {1,-20} {2,-16} -> {3}  brightness={4}  {5}' -f `
                $arch, $t.n, $(if ($alive) { 'still-running' } else { 'exited-early' }), `
                $verdict, $shot.brightness, $note)

        } catch {
            $results += [pscustomobject]@{
                arch = $arch; kind = 'interactive'; name = $t.n; args = ($argList -join ' ')
                expect = 'timeout'; actual = 'error'; verdict = 'FAIL'
                ms = $sw.ElapsedMilliseconds; files = 0; bytes = 0; shot = ''; bright = ''
                note = "启动失败：$($_.Exception.Message)"
            }
            Write-Host ('[{0}] {1,-20} -> FAIL  启动失败: {2}' -f $arch, $t.n, $_.Exception.Message)
        } finally {
            $sw.Stop()
        }

        # 每个交互用例之间彻底清场
        Stop-AllRansom | Out-Null
    }

    # 交互用例跑完会撒/留金币 .lnk，收个尾
    try {
        $cp = Start-Process -FilePath $exe -ArgumentList @('--clean-gold') `
                  -WorkingDirectory $repo -PassThru
        $cp.WaitForExit(30000) | Out-Null
        if (-not $cp.HasExited) { try { Stop-Process -Id $cp.Id -Force } catch { } }
    } catch { }
}

$totalStopwatch.Stop()
Stop-AllRansom | Out-Null

# ---------------------------------------------------------------------------
# 6. 报告
# ---------------------------------------------------------------------------
$passCount = @($results | Where-Object { $_.verdict -eq 'PASS' }).Count
$failCount = @($results | Where-Object { $_.verdict -eq 'FAIL' }).Count
$shotCount = @($results | Where-Object { $_.shot -eq 'desktop.png' }).Count

$md = New-Object System.Collections.Generic.List[string]
$md.Add('# 自测报告 —— 全部命令行参数 × 产物')
$md.Add('')
$md.Add("- 生成时间 (UTC): ``$([datetime]::UtcNow.ToString('yyyy-MM-dd HH:mm:ss'))``")
$md.Add("- 提交: ``$env:GITHUB_SHA``")
$md.Add("- 工作流运行: #$env:GITHUB_RUN_NUMBER (id $env:GITHUB_RUN_ID)")
$md.Add("- 用例总数: **$($results.Count)** ｜ PASS: **$passCount** ｜ FAIL: **$failCount**")
$md.Add("- 桌面截屏: **$shotCount** 张")
$md.Add("- 总耗时: $([math]::Round($totalStopwatch.Elapsed.TotalSeconds,1)) 秒")
$md.Add('')
$md.Add('> 判定说明：[A] headless 类用例断言退出码等于期望值；')
$md.Add('> [B] interactive 类用例期望"启动后一直跑不掉"，由脚本定时截屏后强杀，')
$md.Add('> 能撑过观察期即 PASS。``--pay`` 在无实体运行时故意返回 3，属正常。')
$md.Add('')

foreach ($arch in $exes.Keys) {
    $rows = @($results | Where-Object { $_.arch -eq $arch })
    $p = @($rows | Where-Object { $_.verdict -eq 'PASS' }).Count
    $md.Add("## $arch  ($p/$($rows.Count) PASS)")
    $md.Add('')
    $md.Add('| 用例 | 类别 | 期望 | 实际 | 判定 | 耗时 | 产出文件 | 截图亮度 | 备注 |')
    $md.Add('|---|---|---|---|---|---|---|---|---|')
    foreach ($r in $rows) {
        $mark = if ($r.verdict -eq 'PASS') { 'PASS' } else { '**FAIL**' }
        $noteEsc = ($r.note -replace '\|', '\|')
        $md.Add("| ``$($r.name)`` | $($r.kind) | $($r.expect) | $($r.actual) | $mark | $([math]::Round($r.ms/1000.0,1))s | $($r.files) ($('{0:N0}' -f $r.bytes) B) | $($r.bright) | $noteEsc |")
    }
    $md.Add('')
    $md.Add('<details><summary>完整参数</summary>')
    $md.Add('')
    $md.Add('```')
    foreach ($r in $rows) { $md.Add("$($r.name): $($r.args)") }
    $md.Add('```')
    $md.Add('')
    $md.Add('</details>')
    $md.Add('')
}

$mdPath = Join-Path $OutDir 'selftest-report.md'
Set-Content -LiteralPath $mdPath -Value $md -Encoding utf8

$results | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $OutDir 'selftest-results.json') -Encoding utf8

# CI 的 job 摘要页
if ($env:GITHUB_STEP_SUMMARY) {
    Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value ($md -join "`n")
    Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value ''
}

Write-Host ''
Write-Host '=================================================================='
Write-Host " 自测结束：$passCount PASS / $failCount FAIL（共 $($results.Count) 个用例，$([math]::Round($totalStopwatch.Elapsed.TotalSeconds,1)) 秒）"
Write-Host " 报告: $mdPath"
Write-Host '=================================================================='

if ($failCount -gt 0) {
    foreach ($r in ($results | Where-Object { $_.verdict -eq 'FAIL' })) {
        Write-Host "::error title=自测失败 [$($r.arch)/$($r.name)]::$($r.args) -> $($r.actual)（期望 $($r.expect)） $($r.note)"
    }
}

if ($failCount -gt 0 -and -not $AllowFailures) {
    exit 1
}
exit 0
