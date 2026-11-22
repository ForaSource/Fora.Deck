param(
    [switch]$svg_only,
    [string]$browser_path = 'C:/Program Files/Google/Chrome/Application/chrome.exe'
)

# Fonts become outlines so SVG viewers do not need an installed font or network access.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore
$asset_path = $PSScriptRoot
$repository_path = Split-Path $asset_path -Parent
$build_path = Join-Path $repository_path 'build-brand-assets'
$culture = [Globalization.CultureInfo]::InvariantCulture
$semibold = [System.Windows.Media.GlyphTypeface]::new([uri](Join-Path $asset_path 'fonts/inter-semibold.ttf'))
$regular = [System.Windows.Media.GlyphTypeface]::new([uri](Join-Path $asset_path 'fonts/inter-regular.ttf'))
$mark_xml = [xml](Get-Content -LiteralPath (Join-Path $asset_path 'fora_deck_mark.svg') -Raw)
$mark_group = @($mark_xml.DocumentElement.ChildNodes | Where-Object LocalName -eq 'g')
if ($mark_group.Count -ne 1) { throw 'Expected one canonical mark group.' }
$mark_fragment = $mark_group[0].OuterXml

function number_text([double]$value) { $value.ToString('0.####', $culture) }

function glyph_text([string]$text, [double]$x, [double]$baseline, [double]$size,
                    [string]$color, $typeface, [switch]$wordmark) {
    $result = [Text.StringBuilder]::new()
    foreach ($character in $text.ToCharArray()) {
        if (!$typeface.CharacterToGlyphMap.ContainsKey([int]$character)) { throw "Missing glyph: $character" }
        $glyph = $typeface.CharacterToGlyphMap[[int]$character]
        $outline = $typeface.GetGlyphOutline($glyph, $size, $size).ToString($culture)
        $fill_rule = if ($outline.StartsWith('F1')) { 'nonzero' } else { 'evenodd' }
        $outline = $outline -replace '^F[01]', ''
        $fill = if ($wordmark -and $character -eq '.') { '#F16647' } else { $color }
        if ($outline) {
            [void]$result.Append("<path fill=`"$fill`" fill-rule=`"$fill_rule`" transform=`"translate($(number_text $x) $(number_text $baseline))`" d=`"$outline`"/>")
        }
        $x += $typeface.AdvanceWidths[$glyph] * $size
    }
    $result.ToString()
}

function brand_mark([double]$x, [double]$y, [double]$size) {
    "<g transform=`"translate($(number_text $x) $(number_text $y)) scale($(number_text ($size / 44)))`">$mark_fragment</g>"
}

function svg_document([int]$width, [int]$height, [string]$title, [string]$body) {
    $escaped_title = [Security.SecurityElement]::Escape($title)
    "<svg xmlns=`"http://www.w3.org/2000/svg`" width=`"$width`" height=`"$height`" viewBox=`"0 0 $width $height`" role=`"img`"><title>$escaped_title</title>$body</svg>"
}

$themes = @(
    @{ name = 'dark'; background = '#181A1D'; foreground = '#F2F3F5'; muted = '#AEB5BF' },
    @{ name = 'light'; background = '#F5F6F8'; foreground = '#20242B'; muted = '#56606D' }
)
foreach ($theme in $themes) {
    $body = "<rect width=`"1600`" height=`"480`" fill=`"$($theme.background)`"/>"
    $body += brand_mark 280 90 256
    $body += glyph_text 'Fora.Deck' 568 245 112 $theme.foreground $semibold -wordmark
    $body += glyph_text 'Your apps, organized your way.' 568 310 30 $theme.muted $regular
    $body += glyph_text 'Windows · Linux' 568 365 20 $theme.muted $regular
    $body += glyph_text 'By ForaSource' 1100 365 20 $theme.muted $regular
    $banner = svg_document 1600 480 'Fora.Deck — Your apps, organized your way. Windows and Linux. By ForaSource.' $body
    Set-Content -LiteralPath (Join-Path $asset_path "github_header_$($theme.name).svg") -Value $banner -Encoding utf8

    $body = brand_mark 230 40 320
    $body += glyph_text 'Fora.Deck' 550 270 160 $theme.foreground $semibold -wordmark
    $wordmark_svg = svg_document 1600 400 'Fora.Deck' $body
    Set-Content -LiteralPath (Join-Path $asset_path "fora_deck_wordmark_$($theme.name).svg") -Value $wordmark_svg -Encoding utf8
}
$avatar_body = '<rect width="1024" height="1024" rx="192" fill="#181A1D"/>'
$avatar_body += brand_mark 80 80 864
Set-Content -LiteralPath (Join-Path $asset_path 'fora_deck_avatar.svg') -Encoding utf8 `
    -Value (svg_document 1024 1024 'Fora.Deck product mark' $avatar_body)
$canonical_mark = Get-Content -LiteralPath (Join-Path $asset_path 'fora_deck_mark.svg') -Raw
Set-Content -LiteralPath (Join-Path $asset_path 'fora_deck_mark_mono_dark.svg') -Encoding utf8 `
    -Value $canonical_mark.Replace('#F16647', '#181A1D').TrimEnd()
Set-Content -LiteralPath (Join-Path $asset_path 'fora_deck_mark_mono_light.svg') -Encoding utf8 `
    -Value $canonical_mark.Replace('#F16647', '#FFFFFF').TrimEnd()
if ($svg_only) { return }
if (!(Test-Path -LiteralPath $browser_path -PathType Leaf)) { throw 'Pass -browser_path for a local Chromium browser.' }
New-Item -ItemType Directory -Path $build_path -Force | Out-Null
$exports = @(
    @{ source = 'github_header_dark.svg'; target = 'github_header_dark.png'; width = 1600; height = 480 },
    @{ source = 'github_header_light.svg'; target = 'github_header_light.png'; width = 1600; height = 480 },
    @{ source = 'fora_deck_wordmark_light.svg'; target = 'xlibrary_logo.png'; width = 1600; height = 400 },
    @{ source = 'fora_deck_avatar.svg'; target = 'github_avatar.png'; width = 1024; height = 1024 },
    @{ source = 'fora_deck_avatar.svg'; target = 'github_avatar_256.png'; width = 256; height = 256 },
    @{ source = 'fora_deck_mark.svg'; target = 'xlibrary_mark.png'; width = 1254; height = 1254 },
    @{ source = 'fora_deck_mark.svg'; target = 'mark-256.png'; width = 256; height = 256; intermediate = $true }
)
foreach ($export in $exports) {
    # Keep the viewBox and replace only the viewport, avoiding a cropped large source at small sizes.
    $source = Get-Content -LiteralPath (Join-Path $asset_path $export.source) -Raw
    $source = [regex]::Replace($source, '^<svg([^>]*?) width="\d+" height="\d+"',
                              "<svg`$1 width=`"$($export.width)`" height=`"$($export.height)`"")
    $render_path = Join-Path $build_path $export.source
    Set-Content -LiteralPath $render_path -Value $source -Encoding utf8
    $target_path = Join-Path $(if ($export.intermediate) { $build_path } else { $asset_path }) $export.target
    & $browser_path --headless --disable-gpu --no-sandbox --hide-scrollbars --default-background-color=00000000 `
        "--user-data-dir=$build_path/chrome-profile" "--screenshot=$target_path" `
        "--window-size=$($export.width),$($export.height)" ([uri]$render_path).AbsoluteUri 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "Raster export failed: $($export.target)" }
}

Add-Type -AssemblyName System.Drawing
$icon_sizes = @(16, 20, 24, 32, 48, 64, 128, 256)
$source_image = [System.Drawing.Image]::FromFile((Join-Path $build_path 'mark-256.png'))
try {
    foreach ($size in $icon_sizes | Where-Object { $_ -ne 256 }) {
        $bitmap = [System.Drawing.Bitmap]::new($size, $size)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.DrawImage($source_image, 0, 0, $size, $size)
            $bitmap.Save((Join-Path $build_path "mark-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
} finally { $source_image.Dispose() }
$frames = @($icon_sizes | ForEach-Object { ,([IO.File]::ReadAllBytes((Join-Path $build_path "mark-$_.png"))) })
$stream = [IO.File]::Create((Join-Path $asset_path 'xlibrary.ico'))
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$icon_sizes.Count)
    $offset = 6 + 16 * $icon_sizes.Count
    for ($index = 0; $index -lt $icon_sizes.Count; ++$index) {
        $dimension = if ($icon_sizes[$index] -eq 256) { 0 } else { $icon_sizes[$index] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$index].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$index].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
} finally { $writer.Dispose(); $stream.Dispose() }
Write-Output 'Exported brand images and eight-frame ICO.'
