# Regenerate the multi-resolution Windows icon using only built-in .NET drawing.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$workspace = Split-Path -Parent $PSScriptRoot
$resourceDirectory = Join-Path $workspace 'res'
[void][IO.Directory]::CreateDirectory($resourceDirectory)

function New-RoundedRectangle([single]$X, [single]$Y, [single]$Width, [single]$Height, [single]$Radius) {
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return ,$path
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$images = [Collections.Generic.List[byte[]]]::new()
$navy = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#172D46'))
$white = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#F4F8FC'))
$green = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#008D81'))
$background = New-RoundedRectangle 8 8 240 240 52
$bubble = New-RoundedRectangle 45 51 166 127 26
$tail = [Drawing.Drawing2D.GraphicsPath]::new()
$tail.AddPolygon([Drawing.PointF[]]@([Drawing.PointF]::new(63, 160), [Drawing.PointF]::new(63, 200), [Drawing.PointF]::new(108, 169)))
$line = [Drawing.Pen]::new($navy, 12)
$line.StartCap = $line.EndCap = [Drawing.Drawing2D.LineCap]::Round
$cross = [Drawing.Pen]::new($white, 10)
$cross.StartCap = $cross.EndCap = [Drawing.Drawing2D.LineCap]::Round
try {
    foreach ($size in $sizes) {
        # Draw large then downsample so the smallest title-bar icons stay smooth.
        $canvas = [Drawing.Bitmap]::new(1024, 1024)
        $graphics = [Drawing.Graphics]::FromImage($canvas)
        $bitmap = [Drawing.Bitmap]::new($size, $size)
        $output = [Drawing.Graphics]::FromImage($bitmap)
        $stream = [IO.MemoryStream]::new()
        try {
            $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
            $graphics.ScaleTransform(4, 4)
            $graphics.FillPath($navy, $background)
            $graphics.FillPath($white, $tail)
            $graphics.FillPath($white, $bubble)
            $graphics.DrawLine($line, 77, 92, 174, 92)
            $graphics.DrawLine($line, 77, 124, 134, 124)
            $graphics.FillEllipse($navy, 140, 140, 92, 92)
            $graphics.FillEllipse($green, 146, 146, 80, 80)
            $graphics.DrawLine($cross, 173, 173, 199, 199)
            $graphics.DrawLine($cross, 199, 173, 173, 199)
            $output.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $output.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $output.DrawImage($canvas, 0, 0, $size, $size)
            $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
            $images.Add($stream.ToArray())
            if ($size -eq 256) {
                $bitmap.Save((Join-Path $resourceDirectory 'app.png'), [Drawing.Imaging.ImageFormat]::Png)
            }
        } finally {
            $stream.Dispose()
            $output.Dispose()
            $bitmap.Dispose()
            $graphics.Dispose()
            $canvas.Dispose()
        }
    }
    $file = [IO.File]::Create((Join-Path $resourceDirectory 'app.ico'))
    $writer = [IO.BinaryWriter]::new($file)
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$sizes.Count)
        $offset = 6 + 16 * $sizes.Count
        for ($index = 0; $index -lt $sizes.Count; $index++) {
            $dimension = if ($sizes[$index] -eq 256) { 0 } else { $sizes[$index] }
            $writer.Write([byte]$dimension)
            $writer.Write([byte]$dimension)
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$images[$index].Length)
            $writer.Write([uint32]$offset)
            $offset += $images[$index].Length
        }
        foreach ($bytes in $images) { $writer.Write([byte[]]$bytes) }
    } finally { $writer.Dispose() }
} finally {
    $cross.Dispose()
    $line.Dispose()
    $tail.Dispose()
    $bubble.Dispose()
    $background.Dispose()
    $green.Dispose()
    $white.Dispose()
    $navy.Dispose()
}
Write-Output 'Generated res/app.ico (16-256 px) and res/app.png.'
