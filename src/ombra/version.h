// The build's version and commit, for the log header and (later) the About page and updater.
// The version is CMake's project VERSION (generated ombra/version_defs.h, also in VERSIONINFO); the
// commit is read at every build (cmake/build_info.cmake) and only version.cpp sees it, so a new
// commit recompiles one file.
#pragma once

namespace ombra {

// "0.1.0"
const char* AppVersion();
// "d70fdce", "d70fdce-dirty" (tracked changes at build time) or "unknown" (no git).
const char* BuildCommit();
// Set only for release builds (CMake OMBRA_SENTRY_DSN); empty otherwise.
const char* SentryDsn();

// The project's home: issues, releases (update.h's feed is its API twin).
inline constexpr const char* kRepoUrl = "https://github.com/Zyrecx/ombra";

}  // namespace ombra
