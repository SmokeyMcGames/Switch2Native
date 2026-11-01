[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets'
$source = [Drawing.Image]::FromFile((Join-Path $assets 'controller.png'))
$sizes = @(16,20,24,32,40,48,64,128,256)
$frames = [Collections.Generic.List[byte[]]]::new()
try {
    foreach ($size in $sizes) {
        $bitmap = [Drawing.Bitmap]::new($size,$size)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $stream = [IO.MemoryStream]::new()
        try {
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.DrawImage($source,0,0,$size,$size)
            $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
            $frames.Add($stream.ToArray())
        } finally { $stream.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
    }
} finally { $source.Dispose() }
$file = [IO.File]::Create((Join-Path $assets 'controller.ico'))
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
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$index].Length)
        $writer.Write([uint32]$offset)
        $offset += $frames[$index].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
} finally { $writer.Dispose() }
Write-Output 'Created controller.ico with nine sizes from the original artwork.'
