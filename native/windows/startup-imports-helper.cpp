// SPDX-License-Identifier: LGPL-2.1-or-later
// Isolated test helper only; no filesystem/registry/loader hooks.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "detours.h"
BOOL WINAPI DllMain(HINSTANCE,DWORD reason,LPVOID){
    if(DetourIsHelperProcess())return TRUE;
    if(reason==DLL_PROCESS_ATTACH)DetourRestoreAfterWith();
    return TRUE;
}
