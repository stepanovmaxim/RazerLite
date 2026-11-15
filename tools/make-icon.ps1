# Draws icon.ico: a mouse silhouette in Razer green, PNG-compressed entries
# (supported by Windows Vista and newer at every size).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'icon.ico'
$sizes = 16, 20, 24, 32, 48, 64, 128, 256

function New-RoundedPath {
    param([single]$x, [single]$y, [single]$w, [single]$h, [single]$rTop, [single]$rBottom)
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2 * $rTop, 2 * $rTop, 180, 90)                                  # top-left
    $p.AddArc($x + $w - 2 * $rTop, $y, 2 * $rTop, 2 * $rTop, 270, 90)                 # top-right
    $p.AddArc($x + $w - 2 * $rBottom, $y + $h - 2 * $rBottom, 2 * $rBottom, 2 * $rBottom, 0, 90)
    $p.AddArc($x, $y + $h - 2 * $rBottom, 2 * $rBottom, 2 * $rBottom, 90, 90)
    $p.CloseFigure()
    return $p
}

$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    $bodyW = $s * 0.60
    $bodyH = $s * 0.86
    $x = ($s - $bodyW) / 2
    $y = ($s - $bodyH) / 2

    $body = New-RoundedPath $x $y $bodyW $bodyH ($bodyW * 0.50) ($bodyW * 0.42)
    $green = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 68, 214, 44))
    $g.FillPath($green, $body)

    # the split between the two main buttons, plus the wheel sitting in it
    $dark = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 20, 26, 18))
    $lineW = [Math]::Max(1.0, $s * 0.055)
    $g.FillRectangle($dark, [single]($s / 2 - $lineW / 2), [single]($y + $bodyH * 0.06),
                     [single]$lineW, [single]($bodyH * 0.30))
    $wheelW = [Math]::Max(2.0, $s * 0.12)
    $wheelH = [Math]::Max(3.0, $s * 0.20)
    $wheel = New-RoundedPath ([single]($s / 2 - $wheelW / 2)) ([single]($y + $bodyH * 0.16)) `
                             $wheelW $wheelH ($wheelW / 2) ($wheelW / 2)
    $g.FillPath($dark, $wheel)

    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $pngs += ,@($s, $ms.ToArray())
    $ms.Dispose()
}

# ICO container: 6-byte header, then one 16-byte directory entry per image.
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $size = $p[0]; $data = $p[1]
    $bw.Write([byte]$(if ($size -ge 256) { 0 } else { $size }))
    $bw.Write([byte]$(if ($size -ge 256) { 0 } else { $size }))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$data.Length)
    $bw.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($p in $pngs) { $bw.Write($p[1]) }
$bw.Close(); $fs.Close()

Write-Host "[ok] $out  $((Get-Item $out).Length) bytes"
