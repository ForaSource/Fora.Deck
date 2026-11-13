# Fora.Deck brand assets

The product mark is a geometric D with a displaced panel, related to ForaSource's
visual direction. The parent and product remain separate marks. Use the exact
name **Fora.Deck**; in the graphical wordmark only the dot is coral `#F16647`.
`Fora` and `Deck` use the same color and weight.

## Sources and exports

| Asset | Purpose |
| --- | --- |
| [fora_deck_mark.svg](fora_deck_mark.svg) | Canonical color vector, with a transparent background. |
| [Dark ink](fora_deck_mark_mono_dark.svg), [white ink](fora_deck_mark_mono_light.svg) | Monochrome variants of the product mark. |
| [Dark-background wordmark](fora_deck_wordmark_dark.svg), [light-background wordmark](fora_deck_wordmark_light.svg) | Transparent logo compositions. |
| [Dark header](github_header_dark.svg), [light header](github_header_light.svg) | Vector originals of the two README banners. |
| [fora_deck_avatar.svg](fora_deck_avatar.svg) | Avatar/icon composition with a graphite background. |
| `github_header_dark.png`, `github_header_light.png` | README banners, 1600×480 each. |
| `github_avatar.png`, `github_avatar_256.png` | 1024×1024 avatar and 256×256 installed Linux icon. |
| `xlibrary_logo.png`, `xlibrary_mark.png` | Transparent light-background wordmark (1600×400) and color mark (1254×1254). |
| `xlibrary.ico` | Windows/resource icon with 16, 20, 24, 32, 48, 64, 128 and 256 pixel PNG frames. |

Existing PNG/ICO filenames are retained for their consumers. CMake installs
`github_avatar_256.png` under the existing Linux desktop icon identifier and embeds
`xlibrary.ico` for the application; Windows also uses it as the executable/installer icon.
The SVGs are design sources, not a new Qt SVG dependency.

The wordmarks and banner text use outlines from the bundled [Inter fonts](fonts/README.md).
They render without an installed font, external stylesheet or network request. Keep the
[original OFL notice](fonts/OFL.txt) with the font resources. Platform and author labels
are artwork, not a statement that every build or installer has passed acceptance.

## Regeneration

Run from the repository root in **PowerShell 7 on Windows**:

```powershell
./assets/generate_brand_assets.ps1
# Generate SVGs only:
./assets/generate_brand_assets.ps1 -svg_only
```

The script reads the canonical mark and local Inter fonts. Windows WPF produces
the glyph outlines; a local Chromium browser renders the PNGs. Use `-browser_path`
to select a different local Chrome/Chromium executable. Raster rendering can vary
slightly with the browser version. Review both themes after regeneration.

Temporary renders, a separate browser profile and the 16–256 pixel PNG icon frames
are written to the ignored `build-brand-assets/` directory. The script also packs
these frames into the existing ICO. Do not edit generated exports by hand; update
the canonical mark or generator instead. No build, application startup, font installation,
download or publication is part of regeneration. Linux users can edit/render the
self-contained SVGs with their preferred tools; the Windows script is optional
asset-authoring tooling and is not required to build or run Fora.Deck.
