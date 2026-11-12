// ombra-ui.exe --drive (driver.cpp).
#pragma once

#include <string>

namespace ombra_ui {

// %TEMP%\ombra-drive\home: the isolated instance (OMBRA_HOME) every --drive run uses.
std::wstring DriveHomeDir();
int RunDrive(int argc, wchar_t** argv);

}  // namespace ombra_ui
