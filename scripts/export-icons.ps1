# Re-export the checked-in artwork without external image packages.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repoRoot = Split-Path -Parent $PSScriptRoot
$source = [Drawing.Bitmap]::new((Join-Path $repoRoot 'assets/app-icon.png'))
try {
	$frames = @()
	foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
		$bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
		try {
			$graphics = [Drawing.Graphics]::FromImage($bitmap)
			try {
				$graphics.Clear([Drawing.Color]::Transparent)
				$graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
				$graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
				$graphics.DrawImage($source, [Drawing.Rectangle]::new(0, 0, $size, $size))
			} finally { $graphics.Dispose() }
			$stream = [IO.MemoryStream]::new()
			try {
				$bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
				$bytes = $stream.ToArray()
				$frames += @{ Size = $size; Bytes = $bytes }
				if ($size -in @(16, 32, 48, 128)) {
					[IO.File]::WriteAllBytes((Join-Path $repoRoot "extension/icons/icon-$size.png"), $bytes)
				}
			} finally { $stream.Dispose() }
		} finally { $bitmap.Dispose() }
	}
	$stream = [IO.File]::Create((Join-Path $repoRoot 'native/resources/app.ico'))
	$writer = [IO.BinaryWriter]::new($stream)
	try {
		$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
		$offset = 6 + 16 * $frames.Count
		foreach ($frame in $frames) {
			$dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
			$writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
			$writer.Write([byte]0); $writer.Write([byte]0)
			$writer.Write([uint16]1); $writer.Write([uint16]32)
			$writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset)
			$offset += $frame.Bytes.Length
		}
		foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
	} finally { $writer.Dispose(); $stream.Dispose() }
	Write-Output 'Exported four Chrome PNG sizes and a seven-size Windows ICO.'
} finally { $source.Dispose() }
