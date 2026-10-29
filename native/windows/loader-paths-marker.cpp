// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
extern "C" __declspec(dllexport) int LoaderPathMarker(){return 0x5744504d;}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
