# UI fonts

Embedded in `ombra-ui.exe` (and `ombra-tests.exe`) as RCDATA (`src/ombra-ui/fonts.rc`), loaded
in memory by `src/ombra-ui/ui/fonts.cpp`; nothing is installed. Unmodified static files, Regular
and SemiBold only (the only weights `SpecFor` uses). SIL Open Font License 1.1, see each `OFL.txt`;
neither names a Reserved Font Name. Subsetting would be a modification under the OFL.

- `inter/`: Inter 4.1, `extras/ttf/` of https://github.com/rsms/inter/releases/tag/v4.1
- `geist/`: Geist 1.9.1, `Geist/ttf/` of https://github.com/vercel/geist-font/releases/tag/v1.9.1
