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

    # 采样步长 24px，同时算两样东西：
    #   brightness —— 全部采样点的平均亮度（无头桌面整张纯黑时会趋近 0）
    #   sig        —— 8x8 分块的平均亮度。只看 brightness 分辨不出"两张完全
    #                 不同的桌面恰好平均亮度相同"，分块签名可以。
    #                 后面用它判断各帧之间到底有没有真的变化。
    $step = 24
    $buckets = [double[]]::new(64)
    $counts  = [int[]]::new(64)
    $sum = 0.0; $n = 0
    for ($y = 0; $y -lt $bmp.Height; $y += $step) {
        $by = [int]($y * 8 / $bmp.Height); if ($by -gt 7) { $by = 7 }
        for ($x = 0; $x -lt $bmp.Width; $x += $step) {
            $c = $bmp.GetPixel($x, $y)
            $v = ($c.R + $c.G + $c.B) / 3.0
            $sum += $v; $n++
            $bx = [int]($x * 8 / $bmp.Width); if ($bx -gt 7) { $bx = 7 }
            $i = $by * 8 + $bx
            $buckets[$i] += $v
            $counts[$i]++
        }
    }
    $br = [math]::Round($sum / $n, 2)
    $sig = @()
    for ($i = 0; $i -lt 64; $i++) {
        if ($counts[$i] -gt 0) { $sig += [int][math]::Round($buckets[$i] / $counts[$i]) } else { $sig += 0 }
    }
    $bmp.Dispose()
    "OK $($vs.Width)x$($vs.Height) brightness=$br sig=$($sig -join ',')" | Set-Content -LiteralPath $Report -Encoding ascii
} catch {
    "FAIL $($_.Exception.Message)" | Set-Content -LiteralPath $Report -Encoding ascii
}
