<#
.SYNOPSIS
  관제 태블릿 앱 아이콘(적응형 아이콘의 전경)을 다시 그린다 — Windows 관제 앱 아이콘과 같은 로고(남색 면 + 흰 «CIMS»).

.DESCRIPTION
  적응형 아이콘(minSdk 26 — res/mipmap-anydpi-v26/ic_launcher.xml)은 배경(남색 #4F46E5, values/colors.xml `ic_launcher_background`)과
  전경(이 스크립트가 그리는 흰 글자) 두 층이다. 전경 캔버스는 108dp(xxxhdpi = 432px)이고 런처가 모양(원·둥근 사각)으로 잘라 내므로
  글자는 가운데 안전 구역(지름 66dp = 264px) 안에 둔다. 글꼴·굵기는 windows/dispatch-desktop/Assets/gen-icon.ps1 과 같다(Segoe UI Bold).
  고친 뒤에는 이 스크립트를 다시 돌리고 png 를 커밋한다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File android/dispatch-tablet/gen-icon.ps1
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$size = 432                      # 108dp x 4 (xxxhdpi)
$safe = 232                      # 글자 폭 — 안전 구역(264px) 안쪽
$bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
$g.Clear([System.Drawing.Color]::Transparent)

$sf = New-Object System.Drawing.StringFormat ([System.Drawing.StringFormat]::GenericTypographic)
$sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
# 글자 폭이 $safe 가 되는 크기를 잰다.
[single] $em = 100
$probe = New-Object System.Drawing.Font 'Segoe UI', $em, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
$w = $g.MeasureString('CIMS', $probe, 4000, $sf).Width
$probe.Dispose()
$em = $em * $safe / $w
$font = New-Object System.Drawing.Font 'Segoe UI', $em, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
$g.DrawString('CIMS', $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, ($size * -0.012), $size, $size), $sf)
$g.Dispose(); $font.Dispose()

$dir = Join-Path $PSScriptRoot 'src\main\res\mipmap-xxxhdpi'
New-Item -ItemType Directory -Force $dir | Out-Null
$out = Join-Path $dir 'ic_launcher_foreground.png'
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "wrote $out ($((Get-Item $out).Length) bytes, em $([Math]::Round($em, 1)) px)"
