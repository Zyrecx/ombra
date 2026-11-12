// The product's name, in one place. Only #defines of narrow string literals: rc.exe can include this, and a literal
// concatenates into wide text too (L"Launch " OMBRA_PRODUCT_NAME " at sign-in" is a wide string).
// The mark is brand_mark.h. Code names (namespace ombra, file names, CMake targets) aren't brand.
#pragma once

// Shown to the user: prose, tooltips, dialog titles, toasts, the MP4 handler name, the default
// clip folder and file names.
#define OMBRA_PRODUCT_NAME "Ombra"
// Beside the mark in the overlay's header.
#define OMBRA_WORDMARK "OMBRA"

// The executables. Must match the CMake targets' output names.
#define OMBRA_ENGINE_EXE "ombra.exe"
#define OMBRA_UI_EXE "ombra-ui.exe"

// The installation's identity: the %APPDATA%/%LOCALAPPDATA% folder, the Run-key value, the engine's
// pipe and mutex. Changing it once releases exist is a migration, not a rename: an existing install's
// config, logs and autostart entry stay under the old id, and an old and a new engine would each own
// their own mutex and run side by side. Also spelled out in tests/instance_tests.cpp (pinned on purpose) and
// scripts/telemetry.py.
#define OMBRA_INSTALL_ID "Ombra"
