# Side by side: ROTH's frame and ours at the same pose, scaled to a common
# height so the framing difference is visible rather than hidden by a resize.
param(
    [string]$Oracle,
    [string]$Rema,
    [string]$Out,
    [int]$H = 480
)
Add-Type -AssemblyName System.Drawing

$o = [System.Drawing.Bitmap]::FromFile($Oracle)
$r = [System.Drawing.Bitmap]::FromFile($Rema)

$ow = [int]($o.Width * $H / $o.Height)
$rw = [int]($r.Width * $H / $r.Height)
$gap = 8

$canvas = New-Object System.Drawing.Bitmap (($ow + $gap + $rw), ($H + 18))
$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.Clear([System.Drawing.Color]::FromArgb(20, 20, 24))
$g.DrawImage($o, (New-Object System.Drawing.Rectangle 0, 18, $ow, $H))
$g.DrawImage($r, (New-Object System.Drawing.Rectangle ($ow + $gap), 18, $rw, $H))

$font = New-Object System.Drawing.Font "Consolas", 11
$brush = [System.Drawing.Brushes]::White
$g.DrawString("ROTH (oracle)", $font, $brush, 4, 2)
$g.DrawString("REMAROTH", $font, $brush, ($ow + $gap + 4), 2)

$canvas.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $canvas.Dispose(); $o.Dispose(); $r.Dispose()
Write-Output "$Out written"

