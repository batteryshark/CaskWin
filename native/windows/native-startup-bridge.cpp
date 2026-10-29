// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
extern "C" __declspec(dllimport) int WinedowsNativeMarker();
extern "C" int WinedowsBridgeMarker(){return WinedowsNativeMarker();}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
