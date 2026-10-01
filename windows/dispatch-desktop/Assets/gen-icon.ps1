<#
.SYNOPSIS
  관제조작반 앱 아이콘(Assets/dispatch.ico)을 다시 그린다 — 앱 로고(남색 둥근 사각 + CIMS, 레일 맨 위 로고와 같은 색·글자).

.DESCRIPTION
  창 제목 표시줄·작업 표시줄·Alt+Tab(Window.Icon — Themes/Styles.xaml `Window.Base`)과 실행 파일(csproj ApplicationIcon)이 이 파일을 쓴다.
  크기 = 16·20·24·32·40·48·64(32bpp BMP 항목) + 256(PNG 항목). 24 이하는 글자가 뭉개져 «C» 한 글자만 그린다.
  고친 뒤에는 이 스크립트를 다시 돌리고 ico 를 커밋한다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File windows/dispatch-desktop/Assets/gen-icon.ps1
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-LogoBitmap([int] $size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    [single] $pad = [Math]::Max(0.5, $size * 0.03)
    [single] $side = $size - 2 * $pad
    [single] $r = $side * 0.23
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($pad, $pad, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($pad + $side - 2 * $r, $pad, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($pad + $side - 2 * $r, $pad + $side - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($pad, $pad + $side - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $fill = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 79, 70, 229))      # Brush.Fill(밝게) #4F46E5
    $g.FillPath($fill, $path)
    $text = if ($size -le 24) { 'C' } else { 'CIMS' }
    [single] $em = if ($size -le 24) { $size * 0.72 } else { $size * 0.30 }
    $font = New-Object System.Drawing.Font 'Segoe UI', $em, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat ([System.Drawing.StringFormat]::GenericTypographic)
    $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
    $g.DrawString($text, $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, ($size * -0.02), $size, $size), $sf)
    $g.Dispose(); $font.Dispose(); $fill.Dispose(); $path.Dispose()
    return $bmp
}

# ICO 항목 — 256 은 PNG, 그 아래는 32bpp DIB(BITMAPINFOHEADER 높이 2배 + 픽셀 아래→위 + AND 마스크 0)
function Get-IconImageBytes([System.Drawing.Bitmap] $bmp) {
    $n = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    if ($n -ge 256) { $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); return , $ms.ToArray() }
    $bw = New-Object System.IO.BinaryWriter $ms
    $bw.Write([uint32]40); $bw.Write([int32]$n); $bw.Write([int32](2 * $n)); $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]0); $bw.Write([uint32]($n * $n * 4)); $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $n, $n), 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $row = New-Object byte[] ($n * 4)
    for ($y = $n - 1; $y -ge 0; $y--) {
        [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $row, 0, $row.Length)
        $bw.Write($row)
    }
    $bmp.UnlockBits($data)
    $mask = New-Object byte[] ([int]([Math]::Ceiling($n / 32.0)) * 4)
    for ($y = 0; $y -lt $n; $y++) { $bw.Write($mask) }
    $bw.Flush()
    return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) { $b = New-LogoBitmap $s; , (Get-IconImageBytes $b); $b.Dispose() }
$out = Join-Path $PSScriptRoot 'dispatch.ico'
$fs = [System.IO.File]::Create($out); $w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Close(); $fs.Close()
Write-Host "wrote $out ($((Get-Item $out).Length) bytes, sizes: $($sizes -join ', '))"
