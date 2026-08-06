<#
.SYNOPSIS
    Draws the application icon and writes src/app/XiaomiViewer.ico.

.DESCRIPTION
    The icon is drawn here rather than kept as artwork, so this script is its
    source and the .ico beside app.rc is the built result. It is committed
    because the build must not depend on GDI+ being able to draw the same thing
    twice, and running this again is only needed when the design changes.

    Every size is drawn at its own resolution instead of being scaled down from
    one large bitmap: a 16-pixel lens whose ring survives the downscale is the
    whole reason the small sizes are in the file at all.

.PARAMETER OutputPath
    Where to write the icon. Defaults to src/app/XiaomiViewer.ico.

.PARAMETER PreviewDir
    If given, also write each size as a .png there, to look at the result
    without an icon viewer.
#>
[CmdletBinding()]
param(
    [string]$OutputPath,
    [string]$PreviewDir
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputPath) {
    $OutputPath = Join-Path $RepoRoot 'src\app\XiaomiViewer.ico'
}

# Sizes Windows actually asks for: the shell, the taskbar, Alt+Tab and the
# various DPI scalings of each. Anything missing here gets scaled by Windows,
# which is what makes an icon look soft.
$Sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)

# Above this, entries are stored as PNG. Vista and newer read both, and a 256
# pixel uncompressed entry alone would be a quarter of a megabyte.
$PngFrom = 128

# The application palette: the dark surfaces the UI is built from, and the one
# accent it uses. See src/app/Theme.h.
$TileTop = [System.Drawing.Color]::FromArgb(255, 34, 45, 66)
$TileBottom = [System.Drawing.Color]::FromArgb(255, 12, 14, 20)
$Accent = [System.Drawing.Color]::FromArgb(255, 79, 153, 240)
$GlassInner = [System.Drawing.Color]::FromArgb(255, 26, 46, 78)
$GlassOuter = [System.Drawing.Color]::FromArgb(255, 6, 9, 15)

function New-RoundedRectPath([single]$x, [single]$y, [single]$size, [single]$radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2.0
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $size - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $size - $d, $y + $size - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $size - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconBitmap([int]$size) {
    $s = [single]$size
    $bitmap = New-Object System.Drawing.Bitmap($size, $size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # --- The tile ---
    $margin = $s * 0.03
    $tileSize = $s - $margin * 2.0
    $tile = New-RoundedRectPath $margin $margin $tileSize ($s * 0.21)
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($margin, $margin)),
        (New-Object System.Drawing.PointF(($margin + $tileSize), ($margin + $tileSize))),
        $TileTop, $TileBottom)
    $g.FillPath($fill, $tile)
    $fill.Dispose()

    # A dark tile on the dark taskbar has no silhouette of its own, so it is
    # given a rim just bright enough to find its edge against.
    $rim = New-Object System.Drawing.Pen(
        [System.Drawing.Color]::FromArgb(38, 255, 255, 255), [single][Math]::Max(1.0, $s * 0.012))
    $g.DrawPath($rim, $tile)
    $rim.Dispose()

    # --- The lens ---
    # The ring is the whole of the icon at 16 pixels, where a proportion that
    # looks right at 256 comes out as two grey pixels, so the small sizes get a
    # wider, heavier one rather than a faithfully scaled one.
    $centre = $s * 0.5
    if ($size -le 24) {
        $ringRadius = $s * 0.33
        $ringWidth = [Math]::Max(1.6, $s * 0.125)
    } else {
        $ringRadius = $s * 0.295
        $ringWidth = $s * 0.09
    }

    $glassRadius = $ringRadius - $ringWidth * 0.5
    $glassPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glassPath.AddEllipse(($centre - $glassRadius), ($centre - $glassRadius),
        ($glassRadius * 2.0), ($glassRadius * 2.0))
    $glass = New-Object System.Drawing.Drawing2D.PathGradientBrush($glassPath)
    # Off-centre, so the glass looks lit from the same corner as the tile.
    $glass.CenterPoint = New-Object System.Drawing.PointF(($centre - $glassRadius * 0.35),
        ($centre - $glassRadius * 0.35))
    $glass.CenterColor = $GlassInner
    $glass.SurroundColors = @($GlassOuter)
    $g.FillPath($glass, $glassPath)
    $glass.Dispose()
    $glassPath.Dispose()

    $pen = New-Object System.Drawing.Pen($Accent, [single]$ringWidth)
    $g.DrawEllipse($pen, ($centre - $ringRadius), ($centre - $ringRadius),
        ($ringRadius * 2.0), ($ringRadius * 2.0))
    $pen.Dispose()

    # --- The catchlight ---
    # Left out below 20 pixels: at 16 it is one grey pixel that only muddies the
    # glass.
    if ($size -ge 20) {
        $spotRadius = $s * 0.05
        $spotX = $centre - $glassRadius * 0.40
        $spotY = $centre - $glassRadius * 0.44
        $spot = New-Object System.Drawing.SolidBrush(
            [System.Drawing.Color]::FromArgb(190, 235, 243, 255))
        $g.FillEllipse($spot, ($spotX - $spotRadius), ($spotY - $spotRadius),
            ($spotRadius * 2.0), ($spotRadius * 2.0))
        $spot.Dispose()
    }

    $g.Dispose()
    $tile.Dispose()
    return $bitmap
}

function Get-PngBytes($bitmap) {
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $stream.ToArray()
    $stream.Dispose()
    return $bytes
}

# An uncompressed icon entry: a BITMAPINFOHEADER whose height covers the colour
# rows and a mask that follows them, then bottom-up BGRA rows. The mask is all
# zeroes because the alpha channel already carries the shape, but it still has
# to be there or the entry is malformed.
function Get-DibBytes($bitmap) {
    $width = $bitmap.Width
    $height = $bitmap.Height

    $rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
    $locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = New-Object byte[] ($locked.Stride * $height)
    [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $pixels, 0, $pixels.Length)
    $stride = $locked.Stride
    $bitmap.UnlockBits($locked)

    $stream = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($stream)
    $writer.Write([uint32]40)               # biSize
    $writer.Write([int32]$width)            # biWidth
    $writer.Write([int32]($height * 2))     # biHeight: colour rows plus mask rows
    $writer.Write([uint16]1)                # biPlanes
    $writer.Write([uint16]32)               # biBitCount
    $writer.Write([uint32]0)                # biCompression: BI_RGB
    $writer.Write([uint32]($width * $height * 4))
    $writer.Write([int32]0)                 # biXPelsPerMeter
    $writer.Write([int32]0)                 # biYPelsPerMeter
    $writer.Write([uint32]0)                # biClrUsed
    $writer.Write([uint32]0)                # biClrImportant

    for ($y = $height - 1; $y -ge 0; $y--) {
        $writer.Write($pixels, $y * $stride, $width * 4)
    }

    $maskStride = [int](([int](($width + 31) / 32)) * 4)
    $writer.Write((New-Object byte[] ($maskStride * $height)), 0, $maskStride * $height)

    $writer.Flush()
    $bytes = $stream.ToArray()
    $writer.Dispose()
    return $bytes
}

$images = @()
foreach ($size in $Sizes) {
    $bitmap = New-IconBitmap $size
    if ($PreviewDir) {
        New-Item -ItemType Directory -Force -Path $PreviewDir | Out-Null
        $bitmap.Save((Join-Path $PreviewDir "icon-$size.png"),
            [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $images += , @{
        Size  = $size
        Bytes = if ($size -ge $PngFrom) { Get-PngBytes $bitmap } else { Get-DibBytes $bitmap }
    }
    $bitmap.Dispose()
}

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([uint16]0)                    # reserved
$writer.Write([uint16]1)                    # type: icon
$writer.Write([uint16]$images.Count)

# Entries come first and every one names the offset of its image, so the images
# start after the whole directory.
$offset = 6 + 16 * $images.Count
foreach ($image in $images) {
    # 256 is stored as 0: the field is one byte.
    $dimension = if ($image.Size -ge 256) { 0 } else { $image.Size }
    $writer.Write([byte]$dimension)
    $writer.Write([byte]$dimension)
    $writer.Write([byte]0)                  # palette entries: none, this is 32-bit
    $writer.Write([byte]0)                  # reserved
    $writer.Write([uint16]1)                # colour planes
    $writer.Write([uint16]32)               # bits per pixel
    $writer.Write([uint32]$image.Bytes.Length)
    $writer.Write([uint32]$offset)
    $offset += $image.Bytes.Length
}
foreach ($image in $images) {
    $writer.Write($image.Bytes, 0, $image.Bytes.Length)
}
$writer.Flush()

[System.IO.File]::WriteAllBytes($OutputPath, $stream.ToArray())
$writer.Dispose()

Write-Host "Wrote $OutputPath ($($images.Count) sizes, $([int]((Get-Item $OutputPath).Length / 1024)) KB)"
