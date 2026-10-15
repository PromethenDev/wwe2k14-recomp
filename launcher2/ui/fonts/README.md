# UI fonts

This directory contains static font faces from the Google Fonts repository:

- Oswald Medium (500), SemiBold (600), and Bold (700)
- Barlow Regular (400), Medium (500), and SemiBold (600)
- Noto Sans (Latin, Greek, Cyrillic fallback) and Noto Sans Symbols 2 (symbol fallback)

The included family-specific OFL files are the SIL Open Font License for the corresponding fonts. At
configure/build time `generate_fonts.py` invokes Dear ImGui's `binary_to_compressed_c` helper and
produces a compressed C++ font-data include under the build directory; the launcher embeds those
arrays and does not load fonts from disk at runtime. Unavailable glyphs fall back to a visible square;
CJK fonts are not bundled.
