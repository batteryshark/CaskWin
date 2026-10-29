// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#ifndef WINEDOWS_MARKER_VALUE
#define WINEDOWS_MARKER_VALUE 0x50524956
#endif
static bool initialized;
extern "C" int WinedowsNativeMarker(){return initialized?WINEDOWS_MARKER_VALUE:0;}
extern "C" void WinedowsBuiltinEntry(){}
BOOL WINAPI DllMain(HINSTANCE,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)initialized=true;return TRUE;}
