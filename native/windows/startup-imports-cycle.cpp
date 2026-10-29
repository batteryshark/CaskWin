// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
extern "C" __declspec(dllimport) int WinedowsBridgeMarker();
extern "C" int WinedowsNativeMarker(){return WinedowsBridgeMarker();}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
