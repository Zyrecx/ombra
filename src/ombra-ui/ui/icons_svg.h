#pragma once

// Icon glyph sources for widget.cpp's DrawIcon.
//
// Provenance: lucide-static v1.38.0 (https://lucide.dev), ISC license -- permissive, GPL-
// compatible, no attribution requirement beyond this note. Fetched via
// https://cdn.jsdelivr.net/npm/lucide-static@latest/icons/<name>.svg and embedded verbatim below
// as raw string literals (not loaded from disk at runtime -- keeps the overlay a single
// self-contained binary, matching this project's "single-digit MB, no external asset dependency"
// posture, and sidesteps the cwd-relative-path class of bug already documented for
// out/snapshots).
//
// Each source has `stroke="currentColor"` replaced with a literal `#FFFFFF` placeholder --
// Direct2D's SVG 1.1-subset parser (ID2D1DeviceContext5::CreateSvgDocument) does not understand
// the `currentColor` keyword and CreateSvgDocument fails outright if it's present. The placeholder
// is never actually seen: widget.cpp's GetOrCreateSvgIcon overwrites the root element's `stroke`
// attribute via ID2D1SvgElement::SetAttributeValue(..., D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, ...)
// with the caller's real brush color on every draw, before the document is drawn. All ten icons
// share the same 24x24 viewBox, 2px stroke, round cap/join -- Lucide's default style, unmodified.
//
// IconType::Toggle has no entry here (the on/off pill is drawn by SwitchControl directly, not as an
// icon glyph -- this IconType value is effectively unused today) and falls back to widget.cpp's
// hand-drawn switch, same as any icon whose SVG fails to parse on an older Windows version.

#include "ombra-ui/ui/widget.h"

namespace ombra_ui {

// IconType::Save -- literal floppy-disk glyph (e.g. a "Save Replay" button), distinct from Replay.
inline constexpr const char* kIconSvgSave =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z" /><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7" /><path d="M7 3v4a1 1 0 0 0 1 1h7" /></svg>)svg";

// IconType::Replay -- two opposing arrows (lucide-static's "refresh-cw" arcs) + a filled center
// dot, Home's Instant Replay tile. Two arrows (matching the
// reference crop), but each arrowhead is trimmed from Lucide's stock two-line chevron (an "L"-
// shaped corner: one arm pointing radially outward, one pointing inward toward center) down to
// just its outward arm -- a single short tick, not a full corner. Concretely: arrowhead 1 was
// `M21 3v5h-5` (outward vertical arm `21,3`->`21,8`, then an inward horizontal arm `21,8`->`16,8`
// that's now dropped); arrowhead 2 is its 180-degree mirror, same trim. Dot is r=4, matching
// kIconSvgRecord's dot size exactly (visual consistency between the two tiles' medallions) and is
// a separate element (explicit fill, stroke="none", not inherited from root) so widget.cpp's
// DrawIcon can retint it independently of the ring -- same "dot" convention as kIconSvgRecord below,
// unused here today but free to share code.
inline constexpr const char* kIconSvgReplay =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8" /><path d="M21 3v5" /><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16" /><path d="M3 16v5" /><circle id="dot" cx="12" cy="12" r="4" fill="#FFFFFF" stroke="none" /></svg>)svg";

// IconType::Camera
inline constexpr const char* kIconSvgCamera =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M13.997 4a2 2 0 0 1 1.76 1.05l.486.9A2 2 0 0 0 18.003 7H20a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2h1.997a2 2 0 0 0 1.759-1.048l.489-.904A2 2 0 0 1 10.004 4z" /><circle cx="12" cy="13" r="3" /></svg>)svg";

// IconType::Folder -- folder with an up-arrow, matching reference_design.jpg's "Open recordings"
// glyph (lucide-static calls this "folder-up").
inline constexpr const char* kIconSvgFolder =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z" /><path d="M12 10v6" /><path d="m9 13 3-3 3 3" /></svg>)svg";

// IconType::Settings -- gear
inline constexpr const char* kIconSvgSettings =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915" /><circle cx="12" cy="12" r="3" /></svg>)svg";

// IconType::Close -- X
inline constexpr const char* kIconSvgClose =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18" /><path d="m6 6 12 12" /></svg>)svg";

// IconType::Check
inline constexpr const char* kIconSvgCheck =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 6 9 17l-5-5" /></svg>)svg";

// IconType::Alert -- triangle-alert
inline constexpr const char* kIconSvgAlert =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3" /><path d="M12 9v4" /><path d="M12 17h.01" /></svg>)svg";

// IconType::Back -- chevron-left
inline constexpr const char* kIconSvgBack =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m15 18-6-6 6-6" /></svg>)svg";

// IconType::Record -- hand-authored ring + filled center dot (not stock Lucide -- no Lucide icon
// has an independently-tintable dot). The `id="dot"` circle is what lets widget.cpp's DrawIcon
// recolor just the dot to theme.color.recording (red) when recording state is available.
// ActionTileControl::dot_active is always false today since Record has no backend yet.
inline constexpr const char* kIconSvgRecord =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9" /><circle id="dot" cx="12" cy="12" r="4" fill="#FFFFFF" stroke="none" /></svg>)svg";

// IconType::Palette -- Settings rail's Appearance icon (lucide-static "palette").
inline constexpr const char* kIconSvgPalette =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22a1 1 0 0 1 0-20 10 9 0 0 1 10 9 5 5 0 0 1-5 5h-2.25a1.75 1.75 0 0 0-1.4 2.8l.3.4a1.75 1.75 0 0 1-1.4 2.8z" /><circle cx="13.5" cy="6.5" r="0.5" fill="#FFFFFF" /><circle cx="17.5" cy="10.5" r="0.5" fill="#FFFFFF" /><circle cx="6.5" cy="12.5" r="0.5" fill="#FFFFFF" /><circle cx="8.5" cy="7.5" r="0.5" fill="#FFFFFF" /></svg>)svg";

// IconType::Keyboard -- Settings rail's Hotkeys icon (lucide-static "keyboard").
inline constexpr const char* kIconSvgKeyboard =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 8h.01" /><path d="M12 12h.01" /><path d="M14 8h.01" /><path d="M16 12h.01" /><path d="M18 8h.01" /><path d="M6 8h.01" /><path d="M7 16h10" /><path d="M8 12h.01" /><rect width="20" height="16" x="2" y="4" rx="2" /></svg>)svg";

// IconType::Sliders -- Settings rail's Advanced icon (lucide-static "sliders-horizontal").
inline constexpr const char* kIconSvgSliders =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="21" x2="14" y1="4" y2="4" /><line x1="10" x2="3" y1="4" y2="4" /><line x1="21" x2="12" y1="12" y2="12" /><line x1="8" x2="3" y1="12" y2="12" /><line x1="21" x2="16" y1="20" y2="20" /><line x1="12" x2="3" y1="20" y2="20" /><line x1="14" x2="14" y1="2" y2="6" /><line x1="8" x2="8" y1="10" y2="14" /><line x1="16" x2="16" y1="18" y2="22" /></svg>)svg";

// IconType::Bell -- Settings rail's Notifications icon (lucide-static "bell").
inline constexpr const char* kIconSvgBell =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.268 21a2 2 0 0 0 3.464 0" /><path d="M3.262 15.326A1 1 0 0 0 4 17h16a1 1 0 0 0 .74-1.673C19.41 13.956 18 12.499 18 8A6 6 0 0 0 6 8c0 4.499-1.411 5.956-2.738 7.326" /></svg>)svg";

// IconType::Zap -- Settings rail's Quick Settings icon (lucide-static "zap").
inline constexpr const char* kIconSvgZap =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 14a1 1 0 0 1-.78-1.63l9.9-10.2a.5.5 0 0 1 .86.46l-1.92 6.02A1 1 0 0 0 13 10h7a1 1 0 0 1 .78 1.63l-9.9 10.2a.5.5 0 0 1-.86-.46l1.92-6.02A1 1 0 0 0 11 14z" /></svg>)svg";

// IconType::Gauge -- row icon for the Quality group (lucide-static "gauge").
inline constexpr const char* kIconSvgGauge =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m12 14 4-4" /><path d="M3.34 19a10 10 0 1 1 17.32 0" /></svg>)svg";

// IconType::Volume -- row icon for the Audio group (lucide-static "volume-2").
inline constexpr const char* kIconSvgVolume =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z" /><path d="M16 9a5 5 0 0 1 0 6" /><path d="M19.364 18.364a9 9 0 0 0 0-12.728" /></svg>)svg";

// IconType::Monitor -- Quick Settings' Capture-mode card, "Desktop" (lucide-static "monitor").
inline constexpr const char* kIconSvgMonitor =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="20" height="14" x="2" y="3" rx="2" /><line x1="8" x2="16" y1="21" y2="21" /><line x1="12" x2="12" y1="17" y2="21" /></svg>)svg";

// IconType::Gamepad2 -- Quick Settings' Capture-mode card, "Game" (lucide-static "gamepad-2").
inline constexpr const char* kIconSvgGamepad2 =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="6" x2="10" y1="11" y2="11" /><line x1="8" x2="8" y1="9" y2="13" /><line x1="15" x2="15.01" y1="12" y2="12" /><line x1="18" x2="18.01" y1="10" y2="10" /><path d="M17.32 5H6.68a4 4 0 0 0-3.978 3.59c-.006.052-.01.101-.017.152C2.604 9.416 2 14.456 2 16a3 3 0 0 0 3 3c1 0 1.5-.5 2-1l1.414-1.414A2 2 0 0 1 9.828 16h4.344a2 2 0 0 1 1.414.586L17 18c.5.5 1 1 2 1a3 3 0 0 0 3-3c0-1.545-.604-6.584-.685-7.258-.007-.05-.011-.1-.017-.151A4 4 0 0 0 17.32 5z" /></svg>)svg";

// IconType::Eye -- Monitors grid's per-tile "hide from this list" button (lucide-static "eye"),
// Monitors grid tile.
inline constexpr const char* kIconSvgEye =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0" /><circle cx="12" cy="12" r="3" /></svg>)svg";

// IconType::Mic -- Audio panel's Simple-mode Mic square tile (lucide-static "mic").
inline constexpr const char* kIconSvgMic =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z" /><path d="M19 10v2a7 7 0 0 1-14 0v-2" /><line x1="12" x2="12" y1="19" y2="22" /></svg>)svg";

// IconType::Play -- Clips player: play (lucide-static "play").
inline constexpr const char* kIconSvgPlay =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <path d="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z" /> </svg>)svg";

// IconType::Pause -- Clips player: pause (lucide-static "pause").
inline constexpr const char* kIconSvgPause =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <rect x="14" y="3" width="5" height="18" rx="1" /> <rect x="5" y="3" width="5" height="18" rx="1" /> </svg>)svg";

// IconType::Trash -- Clips: delete to the Recycle Bin (lucide-static "trash-2").
inline constexpr const char* kIconSvgTrash =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <path d="M10 11v6" /> <path d="M14 11v6" /> <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6" /> <path d="M3 6h18" /> <path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" /> </svg>)svg";

// IconType::Copy -- Clips: copy the file to the clipboard (lucide-static "copy").
inline constexpr const char* kIconSvgCopy =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <rect width="14" height="14" x="8" y="8" rx="2" ry="2" /> <path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2" /> </svg>)svg";

// IconType::Film -- Home: the Clips tile (lucide-static "film").
inline constexpr const char* kIconSvgFilm =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <rect width="18" height="18" x="3" y="3" rx="2" /> <path d="M7 3v18" /> <path d="M3 7.5h4" /> <path d="M3 12h18" /> <path d="M3 16.5h4" /> <path d="M17 3v18" /> <path d="M17 7.5h4" /> <path d="M17 16.5h4" /> </svg>)svg";

// IconType::VolumeOff -- Clips player: muted (lucide-static "volume-x").
inline constexpr const char* kIconSvgVolumeOff =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <path d="M11 4.702a.7.7 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.7.7 0 0 0 11 19.298z" /> <path d="m16.5 14.5 5-5" /> <path d="m16.5 9.5 5 5" /> </svg>)svg";

// IconType::Scissors -- Clips: save the trimmed range (lucide-static "scissors").
inline constexpr const char* kIconSvgScissors =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <circle cx="6" cy="6" r="3" /> <path d="M8.12 8.12 12 12" /> <path d="M20 4 8.12 15.88" /> <circle cx="6" cy="18" r="3" /> <path d="M14.8 14.8 20 20" /> </svg>)svg";

// IconType::Bug -- Setup: the Reports step (lucide-static v1.38.0 "bug").
inline constexpr const char* kIconSvgBug =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <path d="M12 20v-9" /> <path d="M14 7a4 4 0 0 1 4 4v3a6 6 0 0 1-12 0v-3a4 4 0 0 1 4-4z" /> <path d="M14.12 3.88 16 2" /> <path d="M21 21a4 4 0 0 0-3.81-4" /> <path d="M21 5a4 4 0 0 1-3.55 3.97" /> <path d="M22 13h-4" /> <path d="M3 21a4 4 0 0 1 3.81-4" /> <path d="M3 5a4 4 0 0 0 3.55 3.97" /> <path d="M6 13H2" /> <path d="m8 2 1.88 1.88" /> <path d="M9 7.13V6a3 3 0 1 1 6 0v1.13" /> </svg>)svg";

// IconType::Power -- Setup: Start with Windows (lucide-static "power").
inline constexpr const char* kIconSvgPower =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"> <path d="M12 2v10" /> <path d="M18.4 6.6a9 9 0 1 1-12.77.04" /> </svg>)svg";

// IconType -> embedded SVG source, or nullptr if this icon has no SVG (falls back to widget.cpp's
// hand-drawn switch in DrawIcon).
inline const char* IconSvgSource(IconType icon) {
    switch (icon) {
        case IconType::Save:     return kIconSvgSave;
        case IconType::Eye:      return kIconSvgEye;
        case IconType::Replay:   return kIconSvgReplay;
        case IconType::Camera:   return kIconSvgCamera;
        case IconType::Folder:   return kIconSvgFolder;
        case IconType::Settings: return kIconSvgSettings;
        case IconType::Close:    return kIconSvgClose;
        case IconType::Check:    return kIconSvgCheck;
        case IconType::Alert:    return kIconSvgAlert;
        case IconType::Back:     return kIconSvgBack;
        case IconType::Record:   return kIconSvgRecord;
        case IconType::Palette:  return kIconSvgPalette;
        case IconType::Keyboard: return kIconSvgKeyboard;
        case IconType::Sliders:  return kIconSvgSliders;
        case IconType::Bell:     return kIconSvgBell;
        case IconType::Zap:      return kIconSvgZap;
        case IconType::Gauge:    return kIconSvgGauge;
        case IconType::Volume:   return kIconSvgVolume;
        case IconType::Monitor:  return kIconSvgMonitor;
        case IconType::Gamepad2: return kIconSvgGamepad2;
        case IconType::Mic:      return kIconSvgMic;
        case IconType::Play:     return kIconSvgPlay;
        case IconType::Pause:    return kIconSvgPause;
        case IconType::Trash:    return kIconSvgTrash;
        case IconType::Copy:     return kIconSvgCopy;
        case IconType::Film:     return kIconSvgFilm;
        case IconType::VolumeOff: return kIconSvgVolumeOff;
        case IconType::Scissors: return kIconSvgScissors;
        case IconType::Power:    return kIconSvgPower;
        case IconType::Bug:      return kIconSvgBug;
        case IconType::None:
        case IconType::Toggle:
        default:
            return nullptr;
    }
}

}  // namespace ombra_ui
