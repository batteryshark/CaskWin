// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "common.h"
#define WD_DESKTOP_MODE (WM_APP+0x371)
#define WD_DESKTOP_STOP (WM_APP+0x372)
#define WD_DESKTOP_WIDTH L"Winedows.Desktop.Width"
#define WD_DESKTOP_HEIGHT L"Winedows.Desktop.Height"
#define WD_DESKTOP_DEPTH L"Winedows.Desktop.Depth"
#define WD_DESKTOP_INITIAL_WIDTH L"Winedows.Desktop.InitialWidth"
#define WD_DESKTOP_INITIAL_HEIGHT L"Winedows.Desktop.InitialHeight"
// Existing menu-bearing roots retain native top-level style when embedded.
// The value is the exact physical desktop HWND, not a process-local boolean.
#define WD_DESKTOP_NATIVE_MENU_ROOT L"Winedows.NativeMenuRoot"
namespace wd {
str desktop_specification(const Prefix &,const str &program,const str &option);
class DesktopHost {
    struct State;
    State *state=nullptr;
public:
    DesktopHost(const Prefix &,const str &program,const str &option,HANDLE job);
    ~DesktopHost();
    HWND window() const;
    DesktopHost(const DesktopHost&)=delete;
    DesktopHost& operator=(const DesktopHost&)=delete;
};
void attach_desktop(const str &prefix_root);
HWND virtual_desktop();
bool desktop_mode(DEVMODEW *mode,bool initial=false);
LONG desktop_change(DEVMODEW *mode,DWORD flags);
}
