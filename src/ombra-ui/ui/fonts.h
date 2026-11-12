// The UI's font choice (ui.font): Inter (default) and Geist are embedded in the exe
// (third_party/fonts, src/ombra-ui/fonts.rc) and loaded in memory; System is Windows 11's
// Segoe UI Variable.
#pragma once

// RCDATA ids, shared with fonts.rc (rc.exe reads only the #defines).
#define OMBRA_RC_FONT_INTER_REGULAR   201
#define OMBRA_RC_FONT_INTER_SEMIBOLD  202
#define OMBRA_RC_FONT_GEIST_REGULAR   203
#define OMBRA_RC_FONT_GEIST_SEMIBOLD  204

#ifndef RC_INVOKED
#include <dwrite_3.h>
#include <wrl/client.h>

#include <string>

namespace ombra_ui {

enum class UiFont { Inter, System, Geist };

UiFont UiFontFromKey(const std::string& key);

// The collection holding an embedded font (Inter, Geist), built once per factory from this
// module's resources; null for System, or when the resources are missing (then DirectWrite falls
// back to a system font, which tests would notice).
IDWriteFontCollection* EmbeddedFontCollection(IDWriteFactory* factory, UiFont font);

}  // namespace ombra_ui
#endif
